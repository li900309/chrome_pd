/*
 * Copyright 2015 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * USB Power Delivery protocol stack.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/wait.h>

#include "usb_mux.h"
#include "tcpm.h"
#include "usb_pd.h"

#if 0
/*
 * Debug log level - higher number == more log
 *   Level 0: Log state transitions
 *   Level 1: Level 0, plus packet info
 *   Level 2: Level 1, plus ping packet and packet dump on error
 *
 * Note that higher log level causes timing changes and thus may affect
 * performance.
 */
static int debug_level;

/*
 * PD communication enabled flag. When false, PD state machine still
 * detects source/sink connection and disconnection, and will still
 * provide VBUS, but never sends any PD communication.
 */
static u8 pd_comm_enabled = 1 /*TODO CONFIG_USB_PD_COMM_ENABLED*/;

#define DUAL_ROLE_IF_ELSE(port, sink_clause, src_clause) \
	(port->power_role == PD_ROLE_SINK ? (sink_clause) : (src_clause))

#define READY_RETURN_STATE(port) DUAL_ROLE_IF_ELSE(port, PD_STATE_SNK_READY, \
							 PD_STATE_SRC_READY)

/* Type C supply voltage (mV) */
#define TYPE_C_VOLTAGE	5000 /* mV */

/* PD counter definitions */
#define PD_MESSAGE_ID_COUNT 7
#define PD_HARD_RESET_COUNT 2
#define PD_CAPS_COUNT 50
#define PD_SNK_CAP_RETRIES 3

enum vdm_states {
	VDM_STATE_ERR_BUSY = -3,
	VDM_STATE_ERR_SEND = -2,
	VDM_STATE_ERR_TMOUT = -1,
	VDM_STATE_DONE = 0,
	/* Anything >0 represents an active state */
	VDM_STATE_READY = 1,
	VDM_STATE_BUSY = 2,
	VDM_STATE_WAIT_RSP_BUSY = 3,
};

/* Port dual-role state */
enum pd_dual_role_states drp_state = PD_DRP_TOGGLE_OFF;

/* Enable varible for Try.SRC states */
static u8 pd_try_src_enable;

struct pd_port {
	struct device *dev;
	/* Associated Type-C Port Controller */
	struct tcpc_dev	*tcpc;
	struct tcpm_ops *ops;
	/* associated policy */
	struct pd_policy policy;
	/* board specific power supply configuration */
	const struct pd_supply_cfg *cfg;
	/* USB muxes controlling the port */
	struct usb_mux_device *mux;

	/* current port power role (SOURCE or SINK) */
	u8 power_role;
	/* current port data role (DFP or UFP) */
	u8 data_role;
	/* port flags, see PD_FLAGS_* */
	u16 flags;
	/* Port polarity : 0 => CC1 is CC line, 1 => CC2 is CC line */
	/* PD state for port */
	enum pd_states task_state;
	/* PD state when we run state handler the last time */
	enum pd_states last_state;
	/* The state to go to after timeout */
	enum pd_states timeout_state;
	/* Timeout for the current state. Set to 0 for no timeout. */
	ktime_t timeout;
	/* Time for source recovery after hard reset */
	ktime_t src_recover;
	/* Time for CC debounce end */
	ktime_t cc_debounce;
	/* The cc state */
	enum pd_cc_states cc_state;
	/* status of last transmit */
	u8 tx_status;

	/* last requested voltage PDO index */
	int requested_idx;
	/* Current limit / voltage based on the last request message */
	u32 curr_limit;
	u32 supply_voltage;
	/* Signal charging update that affects the port */
	int new_power_request;
	/* Store previously requested voltage request */
	int prev_request_mv;
	/* Time for Try.SRC states */
	ktime_t try_src_marker;

	/* PD state for Vendor Defined Messages */
	enum vdm_states vdm_state;
	/* Timeout for the current vdm state.  Set to 0 for no timeout. */
	ktime_t vdm_timeout;
	/* next Vendor Defined Message to send */
	u32 vdo_data[VDO_MAX_SIZE];
	u8 vdo_count;
	/* VDO to retry if UFP responder replied busy. */
	u32 vdo_retry;

	/* Attached ChromeOS device id, RW hash, and current RO / RW image */
	u16 dev_id;
	//TODO u32 dev_rw_hash[PD_RW_HASH_SIZE/4];
	//TODO enum ec_current_image current_image;

	/* protocol counters */
	int hard_reset_count;
	int caps_count;
	int hard_reset_sent;
	int snk_cap_count;

	/* Last received source cap */
	u32 src_caps[PDO_MAX_OBJECTS];
	int src_cap_cnt;

	/* */
	struct task_struct *task;
	wait_queue_head_t wait;
	atomic_t events;
};

static const char * const pd_state_names[] = {
	"DISABLED", "SUSPENDED",
	"SNK_DISCONNECTED", "SNK_DISCONNECTED_DEBOUNCE",
	"SNK_HARD_RESET_RECOVER",
	"SNK_DISCOVERY", "SNK_REQUESTED", "SNK_TRANSITION", "SNK_READY",
	"SNK_SWAP_INIT", "SNK_SWAP_SNK_DISABLE",
	"SNK_SWAP_SRC_DISABLE", "SNK_SWAP_STANDBY", "SNK_SWAP_COMPLETE",
	"SRC_DISCONNECTED", "SRC_DISCONNECTED_DEBOUNCE", "SRC_ACCESSORY",
	"SRC_HARD_RESET_RECOVER", "SRC_STARTUP",
	"SRC_DISCOVERY", "SRC_NEGOCIATE", "SRC_ACCEPTED", "SRC_POWERED",
	"SRC_TRANSITION", "SRC_READY", "SRC_GET_SNK_CAP", "DR_SWAP",
	"SRC_SWAP_INIT", "SRC_SWAP_SNK_DISABLE", "SRC_SWAP_SRC_DISABLE",
	"SRC_SWAP_STANDBY",
	"VCONN_SWAP_SEND", "VCONN_SWAP_INIT", "VCONN_SWAP_READY",
	"SOFT_RESET", "HARD_RESET_SEND", "HARD_RESET_EXECUTE", "BIST_RX",
	"BIST_TX",
};
//TODO BUILD_ASSERT(ARRAY_SIZE(pd_state_names) == PD_STATE_COUNT);

static void pd_mux_set(struct pd_port *port, enum typec_mux mux_mode,
		       enum usb_switch usb_config, int polarity)
{
	dev_dbg(port->dev, "mux_set mode %d cfg %d polarity CC%d\n",
		mux_mode, usb_config, polarity + 1);
	if (port->mux)
		port->mux->set(port->mux, mux_mode, usb_config, polarity);
}

static inline void set_state_timeout(struct pd_port *port,
				     ktime_t timeout,
				     enum pd_states timeout_state)
{
	port->timeout = timeout;
	port->timeout_state = timeout_state;
}

/* Return flag for pd state is connected */
int pd_is_connected(struct pd_port *port)
{
	if (port->task_state == PD_STATE_DISABLED)
		return 0;

	return DUAL_ROLE_IF_ELSE(port,
		/* sink */
		port->task_state != PD_STATE_SNK_DISCONNECTED &&
		port->task_state != PD_STATE_SNK_DISCONNECTED_DEBOUNCE,
		/* source */
		port->task_state != PD_STATE_SRC_DISCONNECTED &&
		port->task_state != PD_STATE_SRC_DISCONNECTED_DEBOUNCE);
}

void pd_vbus_low(struct pd_port *port)
{
	port->flags &= ~PD_FLAGS_VBUS_NEVER_LOW;
}

static inline int pd_is_vbus_present(struct pd_port *port)
{
	return port->ops->get_vbus_level(port->tcpc);
	//TODO vbus gpio ?return pd_snk_is_vbus_provided(port);
}

static int pd_snk_debug_acc_toggle(struct pd_port *port)
{
	/*
	 * when we are in SNK_DISCONNECTED and we see VBUS appearing
	 * (without having seen Rp before), that might be a powered debug
	 * accessory, let's toggle to source to try to detect it.
	 */
	return port->cfg->use_debug_mode && pd_is_vbus_present(port);
}

static int pd_debug_acc_plugged(struct pd_port *port)
{
	return port->cfg->use_debug_mode &&
		port->task_state == PD_STATE_SRC_ACCESSORY;
}

static inline void set_state(struct pd_port *port, enum pd_states next_state)
{
	enum pd_states last_state = port->task_state;

	set_state_timeout(port, ktime_set(0, 0), 0);
	port->task_state = next_state;

	if (last_state == next_state)
		return;
	/* Ignore dual-role toggling between sink and source */
	if ((last_state == PD_STATE_SNK_DISCONNECTED &&
	     next_state == PD_STATE_SRC_DISCONNECTED) ||
	    (last_state == PD_STATE_SRC_DISCONNECTED &&
	     next_state == PD_STATE_SNK_DISCONNECTED))
		return;

	if (next_state == PD_STATE_SRC_DISCONNECTED ||
	    next_state == PD_STATE_SNK_DISCONNECTED) {
		/* Clear the input current limit */
		pd_set_input_current_limit(port, 0, 0);
		port->ops->set_vconn(port->tcpc, 0);
		/* If we are source, make sure VBUS is off */
		if (port->power_role == PD_ROLE_SOURCE)
			pd_power_supply_reset(port);

		port->dev_id = 0;
		port->flags &= ~PD_FLAGS_RESET_ON_DISCONNECT_MASK;
		pd_dfp_exit_mode(&port->policy, 0, 0);
		pd_mux_set(port, TYPEC_MUX_NONE, USB_SWITCH_DISCONNECT,
			   port->polarity);
		/* Disable TCPC RX */
		port->ops->set_rx_enable(port->tcpc, 0);
	}

	dev_info(port->dev, "st%d\n", next_state);
}

static void pd_update_roles(struct pd_port *port)
{
	/* Notify TCPC of role update */
	port->ops->set_msg_header(port->tcpc, port->power_role,
				  port->data_role);
}

static int send_control(struct pd_port *port, int type)
{
	int bit_len;
	u16 header = PD_HEADER(type, port->power_role,
			port->data_role, port->msg_id, 0);

	bit_len = pd_transmit(port, TCPC_TX_SOP, header, NULL);

	if (debug_level >= 1)
		dev_dbg(port->dev, "CTRL[%d]>%d\n", type, bit_len);

	return bit_len;
}

static int send_source_cap(struct pd_port *port)
{
	int bit_len;
	const u32 *src_pdo = port->cfg->src_pdo;
	const int src_pdo_cnt = port->cfg->src_pdo_cnt;
	u16 header;

	if (src_pdo_cnt == 0)
		/* No source capabilities defined, sink only */
		header = PD_HEADER(PD_CTRL_REJECT, port->power_role,
			port->data_role, port->msg_id, 0);
	else
		header = PD_HEADER(PD_DATA_SOURCE_CAP, port->power_role,
			port->data_role, port->msg_id, src_pdo_cnt);

	bit_len = pd_transmit(port, TCPC_TX_SOP, header, src_pdo);
	if (debug_level >= 1)
		dev_dbg(port->dev, "srcCAP>%d\n", bit_len);

	return bit_len;
}

static void send_sink_cap(struct pd_port *port)
{
	int bit_len;
	u16 header = PD_HEADER(PD_DATA_SINK_CAP, port->power_role,
			port->data_role, port->msg_id, port->cfg->snk_pdo_cnt);

	bit_len = pd_transmit(port, TCPC_TX_SOP, header, port->cfg->snk_pdo);
	if (debug_level >= 1)
		dev_dbg(port->dev, "snkCAP>%d\n", bit_len);
}

static int send_request(struct pd_port *port, u32 rdo)
{
	int bit_len;
	u16 header = PD_HEADER(PD_DATA_REQUEST, port->power_role,
			port->data_role, port->msg_id, 1);

	bit_len = pd_transmit(port, TCPC_TX_SOP, header, &rdo);
	if (debug_level >= 1)
		dev_dbg(port->dev, "REQ%d>\n", bit_len);

	return bit_len;
}

static int send_bist_cmd(struct pd_port *port)
{
	/* currently only support sending bist carrier 2 */
	u32 bdo = BDO(BDO_MODE_CARRIER2, 0);
	int bit_len;
	u16 header = PD_HEADER(PD_DATA_BIST, port->power_role,
			port->data_role, port->msg_id, 1);

	bit_len = pd_transmit(port, TCPC_TX_SOP, header, &bdo);
	dev_dbg(port->dev, "BIST>%d\n", bit_len);

	return bit_len;
}

static void queue_vdm(struct pd_port *port, u32 *header, const u32 *data,
			     int data_cnt)
{
	port->vdo_count = data_cnt + 1;
	port->vdo_data[0] = header[0];
	memcpy(&port->vdo_data[1], data, sizeof(u32) * data_cnt);
	/* Set ready, pd task will actually send */
	port->vdm_state = VDM_STATE_READY;
}

static void handle_vdm_request(struct pd_port *port, int cnt, u32 *payload)
{
	int rlen = 0;
	u32 *rdata;

	if (port->vdm_state == VDM_STATE_BUSY) {
		/* If UFP responded busy retry after timeout */
		if (PD_VDO_CMDT(payload[0]) == CMDT_RSP_BUSY) {
			port->vdm_timeout = ktime_add_us(ktime_get(),
				PD_T_VDM_BUSY);
			port->vdm_state = VDM_STATE_WAIT_RSP_BUSY;
			port->vdo_retry = (payload[0] & ~VDO_CMDT_MASK) |
				CMDT_INIT;
			return;
		} else {
			port->vdm_state = VDM_STATE_DONE;
		}
	}

	if (PD_VDO_SVDM(payload[0]))
		rlen = pd_svdm(&port->policy, cnt, payload, &rdata);
	else
		rlen = pd_custom_vdm(&port->policy, cnt, payload, &rdata);

	if (rlen > 0) {
		queue_vdm(port, rdata, &rdata[1], rlen - 1);
		return;
	}
	if (debug_level >= 1)
		dev_dbg(port->dev, "Unhandled VDM VID %04x CMD %04x\n",
			PD_VDO_VID(payload[0]), payload[0] & 0xFFFF);
}

void pd_execute_hard_reset(struct pd_port *port)
{
	if (port->last_state == PD_STATE_HARD_RESET_SEND)
		dev_info(port->dev, "HARD RESET TX\n");
	else
		dev_info(port->dev, "HARD RESET RX\n");

	port->msg_id = 0;
	pd_dfp_exit_mode(&port->policy, 0, 0);

	/*
	 * Fake set last state to hard reset to make sure that the next
	 * state to run knows that we just did a hard reset.
	 */
	port->last_state = PD_STATE_HARD_RESET_EXECUTE;

	/*
	 * If we are swapping to a source and have changed to Rp, restore back
	 * to Rd and turn off vbus to match our power_role.
	 */
	if (port->task_state == PD_STATE_SNK_SWAP_STANDBY ||
	    port->task_state == PD_STATE_SNK_SWAP_COMPLETE) {
		port->ops->set_cc(port->tcpc, TYPEC_CC_RD);
		pd_power_supply_reset(port);
	}

	if (port->power_role == PD_ROLE_SINK) {
		/* Clear the input current limit */
		pd_set_input_current_limit(port, 0, 0);

		set_state(port, PD_STATE_SNK_HARD_RESET_RECOVER);
		return;
	}

	/* We are a source, cut power */
	pd_power_supply_reset(port);
	port->src_recover = ktime_add_us(ktime_get(), PD_T_SRC_RECOVER);
	set_state(port, PD_STATE_SRC_HARD_RESET_RECOVER);
}

static void execute_soft_reset(struct pd_port *port)
{
	port->msg_id = 0;
	set_state(port, DUAL_ROLE_IF_ELSE(port, PD_STATE_SNK_DISCOVERY,
						PD_STATE_SRC_DISCOVERY));
	dev_dbg(port->dev, "Soft Reset\n");
}

static void pd_store_src_cap(struct pd_port *port, int cnt, u32 *src_caps)
{
	int i;

	port->src_cap_cnt = cnt;
	for (i = 0; i < cnt; i++)
		port->src_caps[i] = *src_caps++;
}

static void pd_send_request_msg(struct pd_port *port, int always_send_request)
{
	u32 rdo, curr_limit, supply_voltage;
	int res;
	const int charging = 1;
	const int max_request_allowed = 1;

	/* Clear new power request */
	port->new_power_request = 0;

	/* Build and send request RDO */
	/*
	 * If this port is not actively charging or we are not allowed to
	 * request the max voltage, then select vSafe5V
	 */
	res = pd_build_request(port->cfg, port->src_cap_cnt, port->src_caps,
			       &rdo, &curr_limit, &supply_voltage,
			       charging && max_request_allowed ?
					PD_REQUEST_MAX : PD_REQUEST_VSAFE5V);

	if (res)
		/*
		 * If fail to choose voltage, do nothing, let source re-send
		 * source cap
		 */
		return;

	/* Don't re-request the same voltage */
	if (!always_send_request && port->prev_request_mv == supply_voltage)
		return;

	dev_info(port->dev, "Req [%d] %dmV %dmA %s\n", RDO_POS(rdo),
		 supply_voltage, curr_limit,
		 (rdo & RDO_CAP_MISMATCH) ? "Mismatch" : "");

	port->curr_limit = curr_limit;
	port->supply_voltage = supply_voltage;
	port->prev_request_mv = supply_voltage;
	res = send_request(port, rdo);
	if (res >= 0)
		set_state(port, PD_STATE_SNK_REQUESTED);
	/* If fail send request, do nothing, let source re-send source cap */
}

static void pd_update_pdo_flags(struct pd_port *port, u32 pdo)
{
	/* can only parse PDO flags if type is fixed */
	if ((pdo & PDO_TYPE_MASK) != PDO_TYPE_FIXED)
		return;

	if (pdo & PDO_FIXED_DUAL_ROLE)
		port->flags |= PD_FLAGS_PARTNER_DR_POWER;
	else
		port->flags &= ~PD_FLAGS_PARTNER_DR_POWER;

	if (pdo & PDO_FIXED_EXTERNAL)
		port->flags |= PD_FLAGS_PARTNER_EXTPOWER;
	else
		port->flags &= ~PD_FLAGS_PARTNER_EXTPOWER;

	if (pdo & PDO_FIXED_COMM_CAP)
		port->flags |= PD_FLAGS_PARTNER_USB_COMM;
	else
		port->flags &= ~PD_FLAGS_PARTNER_USB_COMM;

	if (pdo & PDO_FIXED_DATA_SWAP)
		port->flags |= PD_FLAGS_PARTNER_DR_DATA;
	else
		port->flags &= ~PD_FLAGS_PARTNER_DR_DATA;
}

static void handle_data_request(struct pd_port *port, u16 head,
		u32 *payload)
{
	int type = PD_HEADER_TYPE(head);
	int cnt = PD_HEADER_CNT(head);

	switch (type) {
	case PD_DATA_SOURCE_CAP:
		if ((port->task_state == PD_STATE_SNK_DISCOVERY)
			|| (port->task_state == PD_STATE_SNK_TRANSITION)
			|| (port->task_state == PD_STATE_SNK_READY)) {
			/* Port partner is now known to be PD capable */
			port->flags |= PD_FLAGS_PREVIOUS_PD_CONN;

			pd_store_src_cap(port, cnt, payload);
			/* src cap 0 should be fixed PDO */
			pd_update_pdo_flags(port, payload[0]);

			pd_process_source_cap(port, port->cfg,
					      port->src_cap_cnt,
					      port->src_caps);
			pd_send_request_msg(port, 1);
		}
		break;
	case PD_DATA_REQUEST:
		if ((port->power_role == PD_ROLE_SOURCE) && (cnt == 1))
			if (!pd_check_requested_voltage(port->cfg, payload[0])){
				if (send_control(port, PD_CTRL_ACCEPT) < 0)
					/*
					 * if we fail to send accept, do
					 * nothing and let sink timeout and
					 * send hard reset
					 */
					return;

				/* explicit contract is now in place */
				port->flags |= PD_FLAGS_EXPLICIT_CONTRACT;
				port->requested_idx = RDO_POS(payload[0]);
				set_state(port, PD_STATE_SRC_ACCEPTED);
				return;
			}
		/* the message was incorrect or cannot be satisfied */
		send_control(port, PD_CTRL_REJECT);
		/* keep last contract in place (whether implicit or explicit) */
		set_state(port, PD_STATE_SRC_READY);
		break;
	case PD_DATA_BIST:
		/* If not in READY state, then don't start BIST */
		if (DUAL_ROLE_IF_ELSE(port,
				port->task_state == PD_STATE_SNK_READY,
				port->task_state == PD_STATE_SRC_READY)) {
			/* currently only support sending bist carrier mode 2 */
			if ((payload[0] >> 28) == 5) {
				/* bist data object mode is 2 */
				pd_transmit(port, TCPC_TX_BIST_MODE_2, 0,
					    NULL);
				/* Set to appropriate port disconnected state */
				set_state(port, DUAL_ROLE_IF_ELSE(port,
						PD_STATE_SNK_DISCONNECTED,
						PD_STATE_SRC_DISCONNECTED));
			}
		}
		break;
	case PD_DATA_SINK_CAP:
		port->flags |= PD_FLAGS_SNK_CAP_RECVD;
		/* snk cap 0 should be fixed PDO */
		pd_update_pdo_flags(port, payload[0]);
		if (port->task_state == PD_STATE_SRC_GET_SINK_CAP)
			set_state(port, PD_STATE_SRC_READY);
		break;
	case PD_DATA_VENDOR_DEF:
		handle_vdm_request(port, cnt, payload);
		break;
	default:
		dev_info(port->dev, "Unhandled data message type %d\n", type);
	}
}

void pd_request_power_swap(struct pd_port *port)
{
	if (port->task_state == PD_STATE_SRC_READY)
		set_state(port, PD_STATE_SRC_SWAP_INIT);
	else if (port->task_state == PD_STATE_SNK_READY)
		set_state(port, PD_STATE_SNK_SWAP_INIT);
	wake_up(&port->wait);
}

static void __maybe_unused pd_request_vconn_swap(struct pd_port *port)
{
	if (port->task_state == PD_STATE_SRC_READY ||
	    port->task_state == PD_STATE_SNK_READY)
		set_state(port, PD_STATE_VCONN_SWAP_SEND);
	wake_up(&port->wait);
}

void pd_request_data_swap(struct pd_port *port)
{
	if (DUAL_ROLE_IF_ELSE(port,
				port->task_state == PD_STATE_SNK_READY,
				port->task_state == PD_STATE_SRC_READY))
		set_state(port, PD_STATE_DR_SWAP);
	wake_up(&port->wait);
}

static void pd_set_data_role(struct pd_port *port, int role)
{
	int ss_on = (role == PD_ROLE_DFP) ||
		    (port->mux && !port->mux->dfp_only);

	port->data_role = role;

	/*
	 * Need to connect SS mux for if new data role is DFP.
	 * If new data role is UFP, then disconnect the SS mux.
	 */
	pd_mux_set(port, ss_on ? TYPEC_MUX_USB : TYPEC_MUX_NONE,
		   ss_on ?  USB_SWITCH_CONNECT : USB_SWITCH_DISCONNECT,
		   port->polarity);
	pd_update_roles(port);
}

static void pd_dr_swap(struct pd_port *port)
{
	pd_set_data_role(port, !port->data_role);
	port->flags |= PD_FLAGS_DATA_SWAPPED;
}

static void handle_ctrl_request(struct pd_port *port, u16 head,
		u32 *payload)
{
	int type = PD_HEADER_TYPE(head);
	int res;

	switch (type) {
	case PD_CTRL_GOOD_CRC:
		/* should not get it */
		break;
	case PD_CTRL_PING:
		/* Nothing else to do */
		break;
	case PD_CTRL_GET_SOURCE_CAP:
		res = send_source_cap(port);
		if ((res >= 0) &&
		    (port->task_state == PD_STATE_SRC_DISCOVERY))
			set_state(port, PD_STATE_SRC_NEGOCIATE);
		break;
	case PD_CTRL_GET_SINK_CAP:
		send_sink_cap(port);
		/* TODO if not dual role: send_control(port, PD_CTRL_REJECT); */
		break;
	case PD_CTRL_GOTO_MIN:
		break;
	case PD_CTRL_PS_RDY:
		if (port->task_state == PD_STATE_SNK_SWAP_SRC_DISABLE) {
			set_state(port, PD_STATE_SNK_SWAP_STANDBY);
		} else if (port->task_state == PD_STATE_SRC_SWAP_STANDBY) {
			/* reset message ID and swap roles */
			port->msg_id = 0;
			port->power_role = PD_ROLE_SINK;
			pd_update_roles(port);
			set_state(port, PD_STATE_SNK_DISCOVERY);
		} else if (port->task_state == PD_STATE_VCONN_SWAP_INIT) {
			/*
			 * If VCONN is on, then this PS_RDY tells us it's
			 * ok to turn VCONN off
			 */
			if (port->flags & PD_FLAGS_VCONN_ON)
				set_state(port, PD_STATE_VCONN_SWAP_READY);
		} else if (port->task_state == PD_STATE_SNK_DISCOVERY) {
			/* Don't know what power source is ready. Reset. */
			set_state(port, PD_STATE_HARD_RESET_SEND);
		} else if (port->task_state == PD_STATE_SNK_SWAP_STANDBY) {
			/* Do nothing, assume this is a redundant PD_RDY */
		} else if (port->power_role == PD_ROLE_SINK) {
			set_state(port, PD_STATE_SNK_READY);
			pd_set_input_current_limit(port, port->curr_limit,
						   port->supply_voltage);
		}
		break;
	case PD_CTRL_REJECT:
	case PD_CTRL_WAIT:
		if (port->task_state == PD_STATE_DR_SWAP)
			set_state(port, READY_RETURN_STATE(port));
		else if (port->task_state == PD_STATE_VCONN_SWAP_SEND)
			set_state(port, READY_RETURN_STATE(port));
		else if (port->task_state == PD_STATE_SRC_SWAP_INIT)
			set_state(port, PD_STATE_SRC_READY);
		else if (port->task_state == PD_STATE_SNK_SWAP_INIT)
			set_state(port, PD_STATE_SNK_READY);
		else if (port->task_state == PD_STATE_SNK_REQUESTED)
			/* no explicit contract */
			set_state(port, PD_STATE_SNK_READY);
		break;
	case PD_CTRL_ACCEPT:
		if (port->task_state == PD_STATE_SOFT_RESET) {
			execute_soft_reset(port);
		} else if (port->task_state == PD_STATE_DR_SWAP) {
			/* switch data role */
			pd_dr_swap(port);
			set_state(port, READY_RETURN_STATE(port));
		} else if (port->task_state == PD_STATE_VCONN_SWAP_SEND) {
			/* switch vconn */
			set_state(port, PD_STATE_VCONN_SWAP_INIT);
		} else if (port->task_state == PD_STATE_SRC_SWAP_INIT) {
			/* explicit contract goes away for power swap */
			port->flags &= ~PD_FLAGS_EXPLICIT_CONTRACT;
			set_state(port, PD_STATE_SRC_SWAP_SNK_DISABLE);
		} else if (port->task_state == PD_STATE_SNK_SWAP_INIT) {
			/* explicit contract goes away for power swap */
			port->flags &= ~PD_FLAGS_EXPLICIT_CONTRACT;
			set_state(port, PD_STATE_SNK_SWAP_SNK_DISABLE);
		} else if (port->task_state == PD_STATE_SNK_REQUESTED) {
			/* explicit contract is now in place */
			port->flags |= PD_FLAGS_EXPLICIT_CONTRACT;
			set_state(port, PD_STATE_SNK_TRANSITION);
		}
		break;
	case PD_CTRL_SOFT_RESET:
		execute_soft_reset(port);
		/* We are done, acknowledge with an Accept packet */
		send_control(port, PD_CTRL_ACCEPT);
		break;
	case PD_CTRL_PR_SWAP:
		if (pd_check_power_swap(port)) {
			send_control(port, PD_CTRL_ACCEPT);
			/*
			 * Clear flag for checking power role to avoid
			 * immediately requesting another swap.
			 */
			port->flags &= ~PD_FLAGS_CHECK_PR_ROLE;
			set_state(port,
				  DUAL_ROLE_IF_ELSE(port,
					PD_STATE_SNK_SWAP_SNK_DISABLE,
					PD_STATE_SRC_SWAP_SNK_DISABLE));
		} else {
			send_control(port, PD_CTRL_REJECT);
		}
		/* TODO if not dual role: send_control(port, PD_CTRL_REJECT); */
		break;
	case PD_CTRL_DR_SWAP:
		if (pd_check_data_swap(port, port->data_role)) {
			/*
			 * Accept switch and perform data swap. Clear
			 * flag for checking data role to avoid
			 * immediately requesting another swap.
			 */
			port->flags &= ~PD_FLAGS_CHECK_DR_ROLE;
			if (send_control(port, PD_CTRL_ACCEPT) >= 0)
				pd_dr_swap(port);
		} else {
			send_control(port, PD_CTRL_REJECT);
		}
		break;
	case PD_CTRL_VCONN_SWAP:
		if (port->task_state == PD_STATE_SRC_READY ||
		    port->task_state == PD_STATE_SNK_READY) {
			if (pd_check_vconn_swap(port)) {
				if (send_control(port, PD_CTRL_ACCEPT) > 0)
					set_state(port,
						  PD_STATE_VCONN_SWAP_INIT);
			} else {
				send_control(port, PD_CTRL_REJECT);
			}
		}
		/* TODO no VCONN_SWAP send_control(port, PD_CTRL_REJECT); */
		break;
	default:
		dev_info(port->dev, "Unhandled ctrl message type %d\n", type);
	}
}

static void handle_request(struct pd_port *port, u16 head,
		u32 *payload)
{
	int cnt = PD_HEADER_CNT(head);
	int p;

	/* dump received packet content (only dump ping at debug level 2) */
	if ((debug_level == 1 && PD_HEADER_TYPE(head) != PD_CTRL_PING) ||
	    debug_level >= 2) {
		dev_dbg(port->dev, "RECV %04x/%d ", head, cnt);
		for (p = 0; p < cnt; p++)
			dev_dbg(port->dev, "[%d]%08x ", p, payload[p]);
		dev_dbg(port->dev, "\n");
	}

	if (!pd_is_connected(port))
		set_state(port, PD_STATE_HARD_RESET_SEND);

	if (cnt)
		handle_data_request(port, head, payload);
	else
		handle_ctrl_request(port, head, payload);
}

void pd_send_vdm(struct pd_port *port, u32 vid, int cmd, const u32 *data,
		 int count)
{
	if (count > VDO_MAX_SIZE - 1) {
		dev_warn(port->dev, "VDM over max size\n");
		return;
	}

	/* set VDM header with VID & CMD */
	port->vdo_data[0] = VDO(vid, ((vid & USB_SID_PD) == USB_SID_PD) ?
				   1 : (PD_VDO_CMD(cmd) <= CMD_ATTENTION), cmd);
	queue_vdm(port, port->vdo_data, data, count);

	wake_up(&port->wait);
}

static inline int pdo_busy(struct pd_port *port)
{
	/*
	 * Note, main PDO state machine (pd_task) uses READY state exclusively
	 * to denote port partners have successfully negociated a contract.  All
	 * other protocol actions force state transitions.
	 */
	int rv = (port->task_state != PD_STATE_SRC_READY);
	rv &= (port->task_state != PD_STATE_SNK_READY);
	return rv;
}

static unsigned vdm_get_ready_timeout(u32 vdm_hdr)
{
	unsigned timeout;
	int cmd = PD_VDO_CMD(vdm_hdr);

	/* its not a structured VDM command */
	if (!PD_VDO_SVDM(vdm_hdr))
		return 500*MSEC;

	switch (PD_VDO_CMDT(vdm_hdr)) {
	case CMDT_INIT:
		if ((cmd == CMD_ENTER_MODE) || (cmd == CMD_EXIT_MODE))
			timeout = PD_T_VDM_WAIT_MODE_E;
		else
			timeout = PD_T_VDM_SNDR_RSP;
		break;
	default:
		if ((cmd == CMD_ENTER_MODE) || (cmd == CMD_EXIT_MODE))
			timeout = PD_T_VDM_E_MODE;
		else
			timeout = PD_T_VDM_RCVR_RSP;
		break;
	}
	return timeout;
}

static void pd_vdm_send_state_machine(struct pd_port *port)
{
	int res;
	u16 header;

	switch (port->vdm_state) {
	case VDM_STATE_READY:
		/* Only transmit VDM if connected. */
		if (!pd_is_connected(port)) {
			port->vdm_state = VDM_STATE_ERR_BUSY;
			break;
		}

		/*
		 * if there's traffic or we're not in PDO ready state don't send
		 * a VDM.
		 */
		if (pdo_busy(port))
			break;

		/* Prepare and send VDM */
		header = PD_HEADER(PD_DATA_VENDOR_DEF, port->power_role,
				   port->data_role, port->msg_id,
				   (int)port->vdo_count);
		res = pd_transmit(port, TCPC_TX_SOP, header,
				  port->vdo_data);
		if (res < 0) {
			port->vdm_state = VDM_STATE_ERR_SEND;
		} else {
			port->vdm_state = VDM_STATE_BUSY;
			port->vdm_timeout = ktime_add_us(ktime_get(),
				vdm_get_ready_timeout(port->vdo_data[0]));
		}
		break;
	case VDM_STATE_WAIT_RSP_BUSY:
		/* wait and then initiate request again */
		if (ktime_after(ktime_get(), port->vdm_timeout)) {
			port->vdo_data[0] = port->vdo_retry;
			port->vdo_count = 1;
			port->vdm_state = VDM_STATE_READY;
		}
		break;
	case VDM_STATE_BUSY:
		/* Wait for VDM response or timeout */
		if (port->vdm_timeout.tv64 &&
		    ktime_after(ktime_get(), port->vdm_timeout)) {
			port->vdm_state = VDM_STATE_ERR_TMOUT;
		}
		break;
	default:
		break;
	}
}

enum pd_dual_role_states pd_get_dual_role(void)
{
	return drp_state;
}

static int pd_is_power_swapping(struct pd_port *port)
{
	/* return true if in the act of swapping power roles */
	return  port->task_state == PD_STATE_SNK_SWAP_SNK_DISABLE ||
		port->task_state == PD_STATE_SNK_SWAP_SRC_DISABLE ||
		port->task_state == PD_STATE_SNK_SWAP_STANDBY ||
		port->task_state == PD_STATE_SNK_SWAP_COMPLETE ||
		port->task_state == PD_STATE_SRC_SWAP_SNK_DISABLE ||
		port->task_state == PD_STATE_SRC_SWAP_SRC_DISABLE ||
		port->task_state == PD_STATE_SRC_SWAP_STANDBY;
}

int pd_get_partner_data_swap_capable(struct pd_port *port)
{
	/* return data swap capable status of port partner */
	return port->flags & PD_FLAGS_PARTNER_DR_DATA;
}

static int pd_task(void *data)
{
	struct pd_port *port = data;
	int head;
	u32 payload[7];
	int timeout = 10*MSEC;
	int cc1, cc2;
	int res, incoming_packet = 0;
	ktime_t next_role_swap = ms_to_ktime(PD_T_DRP_SNK/1000);
	int snk_hard_reset_vbus_off = 0;
	enum pd_states this_state;
	enum pd_cc_states new_cc_state;
	ktime_t now;
	int evt;

	/* Ensure the power supply is in the default state */
	pd_power_supply_reset(port);

	/*
	 * If VBUS is high, then initialize flag for VBUS has always been
	 * present. This flag is used to maintain a PD connection after a
	 * reset by sending a soft reset.
	 */
	port->flags = pd_is_vbus_present(port) ? PD_FLAGS_VBUS_NEVER_LOW : 0;

	port->ops->init(port->tcpc);

	/* Disable TCPC RX until connection is established */
	port->ops->set_rx_enable(port->tcpc, 0);

	/* Initialize USB mux to its default state */
	pd_mux_set(port, TYPEC_MUX_NONE, USB_SWITCH_DISCONNECT, 0);

	/* Initialize PD protocol state variables for each port. */
	port->power_role = PD_ROLE_DEFAULT;
	port->vdm_state = VDM_STATE_DONE;
	set_state(port, port->cfg->default_state);
	port->ops->set_cc(port->tcpc, PD_ROLE_DEFAULT == PD_ROLE_SOURCE ?
				      TYPEC_CC_RP : TYPEC_CC_RD);

	/* Initialize PD Policy engine */
	pd_dfp_pe_init(&port->policy, port->dev, port);

	while (!kthread_should_stop()) {
		/* process VDM messages last */
		pd_vdm_send_state_machine(port);

		/* wait for next event/packet or timeout expiration */
		wait_event_timeout(port->wait, 0, usecs_to_jiffies(timeout));
		evt = atomic_read(&port->events);
		atomic_sub(evt, &port->events);

		/* if TCPC has reset, then need to initialize it again */
		if (evt & PD_EVENT_TCPC_RESET) {
			dev_info(port->dev, "[TCPC reset!]\n");
			port->ops->init(port->tcpc);

			/* Ensure CC termination is default */
			port->ops->set_cc(port->tcpc,
					  PD_ROLE_DEFAULT == PD_ROLE_SOURCE ?
					  TYPEC_CC_RP : TYPEC_CC_RD);

			/*
			 * If we have a stable contract in the default role,
			 * then simply update TCPC with some missing info
			 * so that we can continue without resetting PD comms.
			 * Otherwise, go to the default disconnected state
			 * and force renegotiation.
			 */
			if ((PD_ROLE_DEFAULT == PD_ROLE_SINK &&
			     port->task_state == PD_STATE_SNK_READY) ||
			    (PD_ROLE_DEFAULT == PD_ROLE_SOURCE &&
			     port->task_state == PD_STATE_SRC_READY)) {
				port->ops->set_polarity(port->tcpc,
							port->polarity);
				port->ops->set_msg_header(port->tcpc,
							  port->power_role,
							  port->data_role);
				port->ops->set_rx_enable(port->tcpc, 1);
			} else {
				set_state(port, port->cfg->default_state);
			}
		}

		/* process any potential incoming message */
		incoming_packet = evt & PD_EVENT_RX;
		if (incoming_packet) {
			port->ops->get_message(port->tcpc, payload, &head);
			if (head > 0)
				handle_request(port, head, payload);
		}
		/* if nothing to do, verify the state of the world in 500ms */
		this_state = port->task_state;
		timeout = 500*MSEC;
		switch (this_state) {
		case PD_STATE_DISABLED:
			/* Nothing to do */
			break;
		case PD_STATE_SRC_DISCONNECTED:
			timeout = 10*MSEC;
			port->ops->get_cc(port->tcpc, &cc1, &cc2);

			/* Vnc monitoring */
			if ((cc1 == TYPEC_CC_VOLT_RD ||
			     cc2 == TYPEC_CC_VOLT_RD) ||
			    (cc1 == TYPEC_CC_VOLT_RA &&
			     cc2 == TYPEC_CC_VOLT_RA)) {
				port->cc_state = PD_CC_NONE;
				set_state(port,
					PD_STATE_SRC_DISCONNECTED_DEBOUNCE);
			}
			/*
			 * Try.SRC state is embedded here. Wait for SNK
			 * detect, or if timer expires, transition to
			 * SNK_DISCONNETED.
			 *
			 * If Try.SRC state is not active, then this block
			 * handles the normal DRP toggle from SRC->SNK
			 */
			else if ((port->flags & PD_FLAGS_TRY_SRC &&
				 ktime_after(ktime_get(), port->try_src_marker))
				 || (!(port->flags & PD_FLAGS_TRY_SRC) &&
				 drp_state != PD_DRP_FORCE_SOURCE &&
				 ktime_after(ktime_get(), next_role_swap))) {
				port->power_role = PD_ROLE_SINK;
				set_state(port, PD_STATE_SNK_DISCONNECTED);
				port->ops->set_cc(port->tcpc, TYPEC_CC_RD);
				next_role_swap = ktime_add_us(ktime_get(),
							      PD_T_DRP_SNK);
				port->try_src_marker = ktime_add_us(ktime_get(),
								PD_T_TRY_WAIT);

				/* Swap states quickly */
				timeout = 2*MSEC;
			}
			break;
		case PD_STATE_SRC_DISCONNECTED_DEBOUNCE:
			timeout = 20*MSEC;
			port->ops->get_cc(port->tcpc, &cc1, &cc2);

			if (cc1 == TYPEC_CC_VOLT_RD &&
			    cc2 == TYPEC_CC_VOLT_RD) {
				/* Debug accessory */
				new_cc_state = PD_CC_DEBUG_ACC;
			} else if (cc1 == TYPEC_CC_VOLT_RD ||
				   cc2 == TYPEC_CC_VOLT_RD) {
				/* UFP attached */
				new_cc_state = PD_CC_UFP_ATTACHED;
			} else if (cc1 == TYPEC_CC_VOLT_RA &&
				   cc2 == TYPEC_CC_VOLT_RA) {
				/* Audio accessory */
				new_cc_state = PD_CC_AUDIO_ACC;
			} else {
				/* No UFP */
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				timeout = 5*MSEC;
				break;
			}
			/* If in Try.SRC state, then don't need to debounce */
			if (!(port->flags & PD_FLAGS_TRY_SRC)) {
				/* Debounce the cc state */
				if (new_cc_state != port->cc_state) {
					port->cc_debounce =
						ktime_add_us(ktime_get(),
						PD_T_CC_DEBOUNCE);
					port->cc_state = new_cc_state;
					break;
				} else if (ktime_before(ktime_get(),
					   port->cc_debounce)) {
					break;
				}
			}

			/* Debounce complete */
			/* UFP is attached */
			if (new_cc_state == PD_CC_UFP_ATTACHED) {
				port->polarity = (cc2 == TYPEC_CC_VOLT_RD);
				port->ops->set_polarity(port->tcpc,
							port->polarity);

				/* initial data role for source is DFP */
				pd_set_data_role(port, PD_ROLE_DFP);

				/* Enable VBUS */
				if (pd_set_power_supply_ready(port)) {
					pd_mux_set(port, TYPEC_MUX_NONE,
						   USB_SWITCH_DISCONNECT,
						   port->polarity);
					break;
				}
				/* If PD comm is enabled, enable TCPC RX */
				if (pd_comm_enabled)
					port->ops->set_rx_enable(port->tcpc, 1);

				port->ops->set_vconn(port->tcpc, 1);
				port->flags |= PD_FLAGS_VCONN_ON;

				port->flags |= PD_FLAGS_CHECK_PR_ROLE |
						  PD_FLAGS_CHECK_DR_ROLE;
				port->hard_reset_count = 0;
				timeout = 5*MSEC;
				set_state(port, PD_STATE_SRC_STARTUP);
			}
			/* Accessory is attached */
			else if (new_cc_state == PD_CC_AUDIO_ACC ||
				 new_cc_state == PD_CC_DEBUG_ACC) {
				/* Set the USB muxes and the default USB role */
				pd_set_data_role(port, port->cfg->debug_role);

				if (port->cfg->use_debug_mode &&
				    new_cc_state == PD_CC_DEBUG_ACC) {
					//ccd_set_mode(system_is_locked() ?
					//	     CCD_MODE_PARTIAL :
					//	     CCD_MODE_ENABLED);
					//typec_set_input_current_limit(
					//	port, 3000, TYPE_C_VOLTAGE);
					//charge_manager_update_dualrole(
					//	port, CAP_DEDICATED);
				}
				set_state(port, PD_STATE_SRC_ACCESSORY);
			}
			break;
		case PD_STATE_SRC_ACCESSORY:
			/* Combined audio / debug accessory state */
			timeout = 100*MSEC;

			port->ops->get_cc(port->tcpc, &cc1, &cc2);

			/* If accessory becomes detached */
			if ((port->cc_state == PD_CC_AUDIO_ACC &&
			     (cc1 != TYPEC_CC_VOLT_RA ||
			      cc2 != TYPEC_CC_VOLT_RA)) ||
			    (port->cc_state == PD_CC_DEBUG_ACC &&
			     (cc1 != TYPEC_CC_VOLT_RD ||
			      cc2 != TYPEC_CC_VOLT_RD))) {
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				//ccd_set_mode(CCD_MODE_DISABLED);
				timeout = 10*MSEC;
			}
			break;
		case PD_STATE_SRC_HARD_RESET_RECOVER:
			/* Do not continue until hard reset recovery time */
			if (ktime_before(ktime_get(), port->src_recover)) {
				timeout = 50*MSEC;
				break;
			}

			/* Enable VBUS */
			timeout = 10*MSEC;
			if (pd_set_power_supply_ready(port)) {
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				break;
			}
			set_state(port, PD_STATE_SRC_STARTUP);
			break;
		case PD_STATE_SRC_STARTUP:
			/* Wait for power source to enable */
			if (port->last_state != port->task_state) {
				/*
				 * fake set data role swapped flag so we send
				 * discover identity when we enter SRC_READY
				 */
				port->flags |= PD_FLAGS_DATA_SWAPPED;
				/* reset various counters */
				port->caps_count = 0;
				port->msg_id = 0;
				port->snk_cap_count = 0;
				set_state_timeout(
					port,
					ktime_add_ms(ktime_get(),
					port->cfg->turn_on_delay_ms),
					PD_STATE_SRC_DISCOVERY);
			}
			break;
		case PD_STATE_SRC_DISCOVERY:
			if (port->last_state != port->task_state) {
				/*
				 * If we have had PD connection with this port
				 * partner, then start NoResponseTimer.
				 */
				if (port->flags & PD_FLAGS_PREVIOUS_PD_CONN)
					set_state_timeout(port,
						ktime_add_us(ktime_get(),
						PD_T_NO_RESPONSE),
						port->hard_reset_count <
						  PD_HARD_RESET_COUNT ?
						    PD_STATE_HARD_RESET_SEND :
						    PD_STATE_SRC_DISCONNECTED);
			}

			/* Send source cap some minimum number of times */
			if (port->caps_count < PD_CAPS_COUNT) {
				/* Query capabilites of the other side */
				res = send_source_cap(port);
				/* packet was acked => PD capable device) */
				if (res >= 0) {
					set_state(port,
						  PD_STATE_SRC_NEGOCIATE);
					timeout = 10*MSEC;
					port->hard_reset_count = 0;
					port->caps_count = 0;
					/* Port partner is PD capable */
					port->flags |=
						PD_FLAGS_PREVIOUS_PD_CONN;
				} else { /* failed, retry later */
					timeout = PD_T_SEND_SOURCE_CAP;
					port->caps_count++;
				}
			}
			break;
		case PD_STATE_SRC_NEGOCIATE:
			/* wait for a "Request" message */
			if (port->last_state != port->task_state)
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  PD_STATE_HARD_RESET_SEND);
			break;
		case PD_STATE_SRC_ACCEPTED:
			/* Accept sent, wait for enabling the new voltage */
			if (port->last_state != port->task_state)
				set_state_timeout(
					port,
					ktime_add_us(ktime_get(),
					PD_T_SINK_TRANSITION),
					PD_STATE_SRC_POWERED);
			break;
		case PD_STATE_SRC_POWERED:
			/* Switch to the new requested voltage */
			if (port->last_state != port->task_state) {
				pd_transition_voltage(port->requested_idx);
				set_state_timeout(
					port,
					ktime_add_ms(ktime_get(),
					port->cfg->turn_on_delay_ms),
					PD_STATE_SRC_TRANSITION);
			}
			break;
		case PD_STATE_SRC_TRANSITION:
			/* the voltage output is good, notify the source */
			res = send_control(port, PD_CTRL_PS_RDY);
			if (res >= 0) {
				timeout = 10*MSEC;
				/* it'a time to ping regularly the sink */
				set_state(port, PD_STATE_SRC_READY);
			} else {
				/* The sink did not ack, cut the power... */
				set_state(port, PD_STATE_SRC_DISCONNECTED);
			}
			break;
		case PD_STATE_SRC_READY:
			timeout = PD_T_SOURCE_ACTIVITY;

			/*
			 * Don't send any PD traffic if we woke up due to
			 * incoming packet or if VDO response pending to avoid
			 * collisions.
			 */
			if (incoming_packet ||
			    (port->vdm_state == VDM_STATE_BUSY))
				break;

			/* Send get sink cap if haven't received it yet */
			if (port->last_state != port->task_state &&
			    !(port->flags & PD_FLAGS_SNK_CAP_RECVD)) {
				if (++(port->snk_cap_count) <=
				    PD_SNK_CAP_RETRIES) {
					send_control(port,
						     PD_CTRL_GET_SINK_CAP);
					set_state(port,
						  PD_STATE_SRC_GET_SINK_CAP);
					break;
				} else if (debug_level >= 1 &&
				  port->snk_cap_count == PD_SNK_CAP_RETRIES+1) {
					dev_dbg(port->dev, "ERR SNK_CAP\n");
				}
			}

			/* Check power role policy, which may trigger a swap */
			if (port->flags & PD_FLAGS_CHECK_PR_ROLE) {
				pd_check_pr_role(port, PD_ROLE_SOURCE,
						 port->flags);
				port->flags &= ~PD_FLAGS_CHECK_PR_ROLE;
				break;
			}

			/* Check data role policy, which may trigger a swap */
			if (port->flags & PD_FLAGS_CHECK_DR_ROLE) {
				pd_check_dr_role(port, port->data_role,
						 port->flags);
				port->flags &= ~PD_FLAGS_CHECK_DR_ROLE;
				break;
			}

			/* Send discovery SVDMs last */
			if (port->data_role == PD_ROLE_DFP &&
			    (port->flags & PD_FLAGS_DATA_SWAPPED)) {
				pd_send_vdm(port, USB_SID_PD,
					    CMD_DISCOVER_IDENT, NULL, 0);
				port->flags &= ~PD_FLAGS_DATA_SWAPPED;
				break;
			}

			if (!(port->flags & PD_FLAGS_PING_ENABLED))
				break;

			/* Verify that the sink is alive */
			res = send_control(port, PD_CTRL_PING);
			if (res >= 0)
				break;

			/* Ping dropped. Try soft reset. */
			set_state(port, PD_STATE_SOFT_RESET);
			timeout = 10 * MSEC;
			break;
		case PD_STATE_SRC_GET_SINK_CAP:
			if (port->last_state != port->task_state)
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  PD_STATE_SRC_READY);
			break;
		case PD_STATE_DR_SWAP:
			if (port->last_state != port->task_state) {
				res = send_control(port, PD_CTRL_DR_SWAP);
				if (res < 0) {
					timeout = 10*MSEC;
					/*
					 * If failed to get goodCRC, send
					 * soft reset, otherwise ignore
					 * failure.
					 */
					set_state(port, res == -1 ?
						   PD_STATE_SOFT_RESET :
						   READY_RETURN_STATE(port));
					break;
				}
				/* Wait for accept or reject */
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  READY_RETURN_STATE(port));
			}
			break;
		case PD_STATE_SRC_SWAP_INIT:
			if (port->last_state != port->task_state) {
				res = send_control(port, PD_CTRL_PR_SWAP);
				if (res < 0) {
					timeout = 10*MSEC;
					/*
					 * If failed to get goodCRC, send
					 * soft reset, otherwise ignore
					 * failure.
					 */
					set_state(port, res == -1 ?
						   PD_STATE_SOFT_RESET :
						   PD_STATE_SRC_READY);
					break;
				}
				/* Wait for accept or reject */
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  PD_STATE_SRC_READY);
			}
			break;
		case PD_STATE_SRC_SWAP_SNK_DISABLE:
			/* Give time for sink to stop drawing current */
			if (port->last_state != port->task_state)
				set_state_timeout(port,
						 ktime_add_us(ktime_get(),
						 PD_T_SINK_TRANSITION),
						 PD_STATE_SRC_SWAP_SRC_DISABLE);
			break;
		case PD_STATE_SRC_SWAP_SRC_DISABLE:
			/* Turn power off */
			if (port->last_state != port->task_state) {
				pd_power_supply_reset(port);
				set_state_timeout(port,
						  ktime_add_ms(ktime_get(),
						  port->cfg->turn_off_delay_ms),
						  PD_STATE_SRC_SWAP_STANDBY);
			}
			break;
		case PD_STATE_SRC_SWAP_STANDBY:
			/* Send PS_RDY to let sink know our power is off */
			if (port->last_state != port->task_state) {
				/* Send PS_RDY */
				res = send_control(port, PD_CTRL_PS_RDY);
				if (res < 0) {
					timeout = 10*MSEC;
					set_state(port,
						  PD_STATE_SRC_DISCONNECTED);
					break;
				}
				/* Switch to Rd and swap roles to sink */
				port->ops->set_cc(port->tcpc, TYPEC_CC_RD);
				port->power_role = PD_ROLE_SINK;
				/* Wait for PS_RDY from new source */
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_PS_SOURCE_ON),
						  PD_STATE_SNK_DISCONNECTED);
			}
			break;
		case PD_STATE_SUSPENDED:
			/*
			 * TODO: Suspend state only supported if we are also
			 * the TCPC.
			 */
			break;
		case PD_STATE_SNK_DISCONNECTED:
			/* Do not wake up too often when disconnected */
			timeout = drp_state != PD_DRP_TOGGLE_ON ? 1000*MSEC
								: 10*MSEC;
			port->ops->get_cc(port->tcpc, &cc1, &cc2);

			/* Source connection monitoring */
			if (cc1 != TYPEC_CC_VOLT_OPEN ||
			    cc2 != TYPEC_CC_VOLT_OPEN) {
				port->cc_state = PD_CC_NONE;
				port->hard_reset_count = 0;
				new_cc_state = PD_CC_DFP_ATTACHED;
				port->cc_debounce = ktime_add_us(ktime_get(),
							PD_T_CC_DEBOUNCE);
				set_state(port,
					PD_STATE_SNK_DISCONNECTED_DEBOUNCE);
				timeout = 10*MSEC;
				break;
			}

			/*
			 * If Try.SRC is active and failed to detect a SNK,
			 * then it transitions to TryWait.SNK. Need to prevent
			 * normal dual role toggle until tDRPTryWait timer
			 * expires.
			 */
			if (port->flags & PD_FLAGS_TRY_SRC) {
				if (ktime_after(ktime_get(),
						port->try_src_marker))
					port->flags &= ~PD_FLAGS_TRY_SRC;
				break;
			}

			/*
			 * If no source detected, check for role toggle.
			 * If VBUS is detected, and we are in the debug
			 * accessory toggle state, then allow toggling.
			 */
			if ((drp_state == PD_DRP_TOGGLE_ON &&
			     ktime_after(ktime_get(), next_role_swap)) ||
			    pd_snk_debug_acc_toggle(port)) {
				/* Swap roles to source */
				port->power_role = PD_ROLE_SOURCE;
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				port->ops->set_cc(port->tcpc, TYPEC_CC_RP);
				next_role_swap = ktime_add_us(ktime_get(),
							      PD_T_DRP_SRC);

				/* Swap states quickly */
				timeout = 2*MSEC;
			}
			break;
		case PD_STATE_SNK_DISCONNECTED_DEBOUNCE:
			port->ops->get_cc(port->tcpc, &cc1, &cc2);
			if (cc1 == TYPEC_CC_VOLT_OPEN &&
			    cc2 == TYPEC_CC_VOLT_OPEN) {
				/* No connection any more */
				set_state(port, PD_STATE_SNK_DISCONNECTED);
				timeout = 5*MSEC;
				break;
			}

			timeout = 20*MSEC;

			/* Wait for CC debounce and VBUS present */
			if (ktime_before(ktime_get(), port->cc_debounce) ||
			   !pd_is_vbus_present(port))
				break;

			if (pd_try_src_enable &&
			    !(port->flags & PD_FLAGS_TRY_SRC)) {
				/*
				 * If TRY_SRC is enabled, but not active,
				 * then force attempt to connect as source.
				 */
				port->try_src_marker = ktime_add_us(ktime_get(),
					PD_T_TRY_SRC);
				/* Swap roles to source */
				port->power_role = PD_ROLE_SOURCE;
				port->ops->set_cc(port->tcpc, TYPEC_CC_RP);
				timeout = 2*MSEC;
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				/* Set flag after the state change */
				port->flags |= PD_FLAGS_TRY_SRC;
				break;
			}

			/* We are attached */
			port->polarity = (cc2 != TYPEC_CC_VOLT_OPEN);
			port->ops->set_polarity(port->tcpc, port->polarity);
			/* reset message ID  on connection */
			port->msg_id = 0;
			/* initial data role for sink is UFP */
			pd_set_data_role(port, PD_ROLE_UFP);

			/* If PD comm is enabled, enable TCPC RX */
			if (pd_comm_enabled)
				port->ops->set_rx_enable(port->tcpc, 1);

			/*
			 * fake set data role swapped flag so we send
			 * discover identity when we enter SRC_READY
			 */
			port->flags |= PD_FLAGS_CHECK_PR_ROLE |
					  PD_FLAGS_CHECK_DR_ROLE |
					  PD_FLAGS_DATA_SWAPPED;
			set_state(port, PD_STATE_SNK_DISCOVERY);
			timeout = 10*MSEC;
			//TODO
			//hook_call_deferred(
			//	pd_usb_billboard_deferred,
			//	PD_T_AME);
			break;
		case PD_STATE_SNK_HARD_RESET_RECOVER:
			if (port->last_state != port->task_state)
				port->flags |= PD_FLAGS_DATA_SWAPPED;

			/* Wait for VBUS to go low and then high*/
			if (port->last_state != port->task_state) {
				snk_hard_reset_vbus_off = 0;
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SAFE_0V),
						  port->hard_reset_count <
						    PD_HARD_RESET_COUNT ?
						     PD_STATE_HARD_RESET_SEND :
						     PD_STATE_SNK_DISCOVERY);
			}

			if (!pd_is_vbus_present(port) &&
			    !snk_hard_reset_vbus_off) {
				/* VBUS has gone low, reset timeout */
				snk_hard_reset_vbus_off = 1;
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SRC_RECOVER_MAX +
						  PD_T_SRC_TURN_ON),
						  PD_STATE_SNK_DISCONNECTED);
			}
			if (pd_is_vbus_present(port) &&
			    snk_hard_reset_vbus_off) {
				/* VBUS went high again */
				set_state(port, PD_STATE_SNK_DISCOVERY);
				timeout = 10*MSEC;
			}

			/*
			 * Don't need to set timeout because VBUS changing
			 * will trigger an interrupt and wake us up.
			 */
			break;
		case PD_STATE_SNK_DISCOVERY:
			/* Wait for source cap expired only if we are enabled */
			if ((port->last_state != port->task_state)
			    && pd_comm_enabled) {
				/*
				 * If VBUS has never been low, and we timeout
				 * waiting for source cap, try a soft reset
				 * first, in case we were already in a stable
				 * contract before this boot.
				 */
				if (port->flags & PD_FLAGS_VBUS_NEVER_LOW) {
					port->flags &=
						~PD_FLAGS_VBUS_NEVER_LOW;
					set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SINK_WAIT_CAP),
						  PD_STATE_SOFT_RESET);
				}
				/*
				 * If we haven't passed hard reset counter,
				 * start SinkWaitCapTimer, otherwise start
				 * NoResponseTimer.
				 */
				else if (port->hard_reset_count <
					 PD_HARD_RESET_COUNT)
					set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SINK_WAIT_CAP),
						  PD_STATE_HARD_RESET_SEND);
				else if (port->flags &
					 PD_FLAGS_PREVIOUS_PD_CONN)
					/* ErrorRecovery */
					set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_NO_RESPONSE),
						  PD_STATE_SNK_DISCONNECTED);
			}

			break;
		case PD_STATE_SNK_REQUESTED:
			/* Wait for ACCEPT or REJECT */
			if (port->last_state != port->task_state) {
				port->hard_reset_count = 0;
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  PD_STATE_HARD_RESET_SEND);
			}
			break;
		case PD_STATE_SNK_TRANSITION:
			/* Wait for PS_RDY */
			if (port->last_state != port->task_state)
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_PS_TRANSITION),
						  PD_STATE_HARD_RESET_SEND);
			break;
		case PD_STATE_SNK_READY:
			timeout = 20*MSEC;

			/*
			 * Don't send any PD traffic if we woke up due to
			 * incoming packet or if VDO response pending to avoid
			 * collisions.
			 */
			if (incoming_packet ||
			    (port->vdm_state == VDM_STATE_BUSY))
				break;

			/* Check for new power to request */
			if (port->new_power_request) {
				pd_send_request_msg(port, 0);
				break;
			}

			/* Check power role policy, which may trigger a swap */
			if (port->flags & PD_FLAGS_CHECK_PR_ROLE) {
				pd_check_pr_role(port, PD_ROLE_SINK,
						 port->flags);
				port->flags &= ~PD_FLAGS_CHECK_PR_ROLE;
				break;
			}

			/* Check data role policy, which may trigger a swap */
			if (port->flags & PD_FLAGS_CHECK_DR_ROLE) {
				pd_check_dr_role(port, port->data_role,
						 port->flags);
				port->flags &= ~PD_FLAGS_CHECK_DR_ROLE;
				break;
			}

			/* If DFP, send discovery SVDMs */
			if (port->data_role == PD_ROLE_DFP &&
			     (port->flags & PD_FLAGS_DATA_SWAPPED)) {
				pd_send_vdm(port, USB_SID_PD,
					    CMD_DISCOVER_IDENT, NULL, 0);
				port->flags &= ~PD_FLAGS_DATA_SWAPPED;
				break;
			}

			/* Sent all messages, don't need to wake very often */
			timeout = 200*MSEC;
			break;
		case PD_STATE_SNK_SWAP_INIT:
			if (port->last_state != port->task_state) {
				res = send_control(port, PD_CTRL_PR_SWAP);
				if (res < 0) {
					timeout = 10*MSEC;
					/*
					 * If failed to get goodCRC, send
					 * soft reset, otherwise ignore
					 * failure.
					 */
					set_state(port, res == -1 ?
						   PD_STATE_SOFT_RESET :
						   PD_STATE_SNK_READY);
					break;
				}
				/* Wait for accept or reject */
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  PD_STATE_SNK_READY);
			}
			break;
		case PD_STATE_SNK_SWAP_SNK_DISABLE:
			/* Stop drawing power */
			pd_set_input_current_limit(port, 0, 0);
			set_state(port, PD_STATE_SNK_SWAP_SRC_DISABLE);
			timeout = 10*MSEC;
			break;
		case PD_STATE_SNK_SWAP_SRC_DISABLE:
			/* Wait for PS_RDY */
			if (port->last_state != port->task_state)
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_PS_SOURCE_OFF),
						  PD_STATE_HARD_RESET_SEND);
			break;
		case PD_STATE_SNK_SWAP_STANDBY:
			if (port->last_state != port->task_state) {
				/* Switch to Rp and enable power supply */
				port->ops->set_cc(port->tcpc, TYPEC_CC_RP);
				if (pd_set_power_supply_ready(port)) {
					/* Restore Rd */
					port->ops->set_cc(port->tcpc,
							  TYPEC_CC_RD);
					timeout = 10*MSEC;
					set_state(port,
						  PD_STATE_SNK_DISCONNECTED);
					break;
				}
				/* Wait for power supply to turn on */
				set_state_timeout(
					port,
					ktime_add_ms(ktime_get(),
					port->cfg->turn_on_delay_ms),
					PD_STATE_SNK_SWAP_COMPLETE);
			}
			break;
		case PD_STATE_SNK_SWAP_COMPLETE:
			/* Send PS_RDY and change to source role */
			res = send_control(port, PD_CTRL_PS_RDY);
			if (res < 0) {
				/* Restore Rd */
				port->ops->set_cc(port->tcpc, TYPEC_CC_RD);
				pd_power_supply_reset(port);
				timeout = 10 * MSEC;
				set_state(port, PD_STATE_SNK_DISCONNECTED);
				break;
			}

			/* Don't send GET_SINK_CAP on swap */
			port->snk_cap_count = PD_SNK_CAP_RETRIES+1;
			port->caps_count = 0;
			port->msg_id = 0;
			port->power_role = PD_ROLE_SOURCE;
			pd_update_roles(port);
			set_state(port, PD_STATE_SRC_DISCOVERY);
			timeout = 10*MSEC;
			break;
		case PD_STATE_VCONN_SWAP_SEND:
			if (port->last_state != port->task_state) {
				res = send_control(port, PD_CTRL_VCONN_SWAP);
				if (res < 0) {
					timeout = 10*MSEC;
					/*
					 * If failed to get goodCRC, send
					 * soft reset, otherwise ignore
					 * failure.
					 */
					set_state(port, res == -1 ?
						   PD_STATE_SOFT_RESET :
						   READY_RETURN_STATE(port));
					break;
				}
				/* Wait for accept or reject */
				set_state_timeout(port,
						  ktime_add_us(ktime_get(),
						  PD_T_SENDER_RESPONSE),
						  READY_RETURN_STATE(port));
			}
			break;
		case PD_STATE_VCONN_SWAP_INIT:
			if (port->last_state != port->task_state) {
				if (!(port->flags & PD_FLAGS_VCONN_ON)) {
					/* Turn VCONN on and wait for it */
					port->ops->set_vconn(port->tcpc, 1);
					set_state_timeout(port,
					  ktime_add_ms(ktime_get(),
					  port->cfg->vconn_swap_delay_ms),
					  PD_STATE_VCONN_SWAP_READY);
				} else {
					set_state_timeout(port,
					  ktime_add_us(ktime_get(),
					  PD_T_VCONN_SOURCE_ON),
					  READY_RETURN_STATE(port));
				}
			}
			break;
		case PD_STATE_VCONN_SWAP_READY:
			if (port->last_state != port->task_state) {
				if (!(port->flags & PD_FLAGS_VCONN_ON)) {
					/* VCONN is now on, send PS_RDY */
					port->flags |= PD_FLAGS_VCONN_ON;
					res = send_control(port,
							   PD_CTRL_PS_RDY);
					if (res == -1) {
						timeout = 10*MSEC;
						/*
						 * If failed to get goodCRC,
						 * send soft reset
						 */
						set_state(port,
							  PD_STATE_SOFT_RESET);
						break;
					}
					set_state(port,
						  READY_RETURN_STATE(port));
				} else {
					/* Turn VCONN off and wait for it */
					port->ops->set_vconn(port->tcpc, 0);
					port->flags &= ~PD_FLAGS_VCONN_ON;
					set_state_timeout(port,
					  ktime_add_ms(ktime_get(),
					  port->cfg->vconn_swap_delay_ms),
					  READY_RETURN_STATE(port));
				}
			}
			break;
		case PD_STATE_SOFT_RESET:
			if (port->last_state != port->task_state) {
				/* Message ID of soft reset is always 0 */
				port->msg_id = 0;
				res = send_control(port, PD_CTRL_SOFT_RESET);

				/* if soft reset failed, try hard reset. */
				if (res < 0) {
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
					timeout = 5*MSEC;
					break;
				}

				set_state_timeout(
					port,
					ktime_add_us(ktime_get(),
						     PD_T_SENDER_RESPONSE),
					PD_STATE_HARD_RESET_SEND);
			}
			break;
		case PD_STATE_HARD_RESET_SEND:
			port->hard_reset_count++;
			if (port->last_state != port->task_state)
				port->hard_reset_sent = 0;

			/* try sending hard reset until it succeeds */
			if (!port->hard_reset_sent) {
				if (pd_transmit(port, TCPC_TX_HARD_RESET,
						0, NULL) < 0) {
					timeout = 10*MSEC;
					break;
				}

				/* successfully sent hard reset */
				port->hard_reset_sent = 1;
				/*
				 * If we are source, delay before cutting power
				 * to allow sink time to get hard reset.
				 */
				if (port->power_role == PD_ROLE_SOURCE) {
					set_state_timeout(port,
					  ktime_add_us(ktime_get(),
						       PD_T_PS_HARD_RESET),
					  PD_STATE_HARD_RESET_EXECUTE);
				} else {
					set_state(port,
						  PD_STATE_HARD_RESET_EXECUTE);
					timeout = 10*MSEC;
				}
			}
			break;
		case PD_STATE_HARD_RESET_EXECUTE:
			/*
			 * If hard reset while in the last stages of power
			 * swap, then we need to restore our CC resistor.
			 */
			if (port->last_state == PD_STATE_SNK_SWAP_STANDBY)
				port->ops->set_cc(port->tcpc, TYPEC_CC_RD);

			/* reset our own state machine */
			pd_execute_hard_reset(port);
			timeout = 10*MSEC;
			break;
		case PD_STATE_BIST_RX:
			send_bist_cmd(port);
			/* Delay at least enough for partner to finish BIST */
			timeout = PD_T_BIST_RECEIVE + 20*MSEC;
			/* Set to appropriate port disconnected state */
			set_state(port, DUAL_ROLE_IF_ELSE(port,
						PD_STATE_SNK_DISCONNECTED,
						PD_STATE_SRC_DISCONNECTED));
			break;
		case PD_STATE_BIST_TX:
			pd_transmit(port, TCPC_TX_BIST_MODE_2, 0, NULL);
			/* Delay at least enough to finish sending BIST */
			timeout = PD_T_BIST_TRANSMIT + 20*MSEC;
			/* Set to appropriate port disconnected state */
			set_state(port, DUAL_ROLE_IF_ELSE(port,
						PD_STATE_SNK_DISCONNECTED,
						PD_STATE_SRC_DISCONNECTED));
			break;
		default:
			break;
		}

		port->last_state = this_state;

		/*
		 * Check for state timeout, and if not check if need to adjust
		 * timeout value to wake up on the next state timeout.
		 */
		now = ktime_get();
		if (port->timeout.tv64) {
			if (ktime_after(now, port->timeout)) {
				set_state(port, port->timeout_state);
				/* On a state timeout, run next state soon */
				timeout = timeout < 10*MSEC ? timeout : 10*MSEC;
			} else if (ktime_to_us(ktime_sub(port->timeout, now)) <
				   timeout) {
				timeout = ktime_to_us(ktime_sub(port->timeout,
						      now));
			}
		}

		/* Check for disconnection */
		if (!pd_is_connected(port) || pd_is_power_swapping(port))
			continue;

		if (port->power_role == PD_ROLE_SOURCE) {
			/* Source: detect disconnect by monitoring CC */
			port->ops->get_cc(port->tcpc, &cc1, &cc2);
			if (port->polarity)
				cc1 = cc2;
			if (cc1 == TYPEC_CC_VOLT_OPEN) {
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				/* Debouncing */
				timeout = 10*MSEC;
				/*
				 * If Try.SRC is configured, then ATTACHED_SRC
				 * needs to transition to TryWait.SNK. Change
				 * power role to SNK and start state timer.
				 */
				if (pd_try_src_enable) {
					/* Swap roles to sink */
					port->power_role = PD_ROLE_SINK;
					port->ops->set_cc(port->tcpc,
							  TYPEC_CC_RD);
					/* Set timer for TryWait.SNK state */
					port->try_src_marker =
						ktime_add_us(ktime_get(),
						PD_T_TRY_WAIT);
					/* Advance to TryWait.SNK state */
					set_state(port,
						  PD_STATE_SNK_DISCONNECTED);
					/* Mark state as TryWait.SNK */
					port->flags |= PD_FLAGS_TRY_SRC;
				}
			}
		}
		/*
		 * Sink disconnect if VBUS is low and we are not recovering
		 * a hard reset.
		 */
		if (port->power_role == PD_ROLE_SINK &&
		    !pd_is_vbus_present(port) &&
		    port->task_state != PD_STATE_SNK_HARD_RESET_RECOVER &&
		    port->task_state != PD_STATE_HARD_RESET_EXECUTE) {
			/* Sink: detect disconnect by monitoring VBUS */
			set_state(port, PD_STATE_SNK_DISCONNECTED);
			/* set timeout small to reconnect fast */
			timeout = 5*MSEC;
		}
	}
	return 0;
}

void pd_tcpm_event(struct pd_port *port, int events)
{
	atomic_or(events, &port->events);
	wake_up(&port->wait);
}
#endif
