/*
 * Copyright 2015-2016 Google, Inc
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

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec.h>
#include <linux/workqueue.h>

#include "tcpm.h"

enum tcpm_state {
	INVALID_STATE,

	SRC_UNATTACHED,
	SRC_ATTACH_WAIT,
	SRC_ATTACHED,
	SRC_STARTUP,
	SRC_SEND_CAPABILITIES,
	SRC_NEGOTIATE_CAPABILITIES,
	SRC_TRANSITION_SUPPLY,
	SRC_READY,
	SRC_WAIT_NEW_CAPABILITIES,

	SNK_UNATTACHED,
	SNK_ATTACH_WAIT,
	SNK_DEBOUNCED,
	SNK_ATTACHED,
	SNK_STARTUP,
	SNK_WAIT_CAPABILITIES,
	SNK_NEGOTIATE_CAPABILITIES,
	SNK_TRANSITION_SINK,
	SNK_READY,

	ACC_UNATTACHED,
	DEBUG_ACC_ATTACHED,
	AUDIO_ACC_ATTACHED,
	AUDIO_ACC_DEBOUNCE,

	GIVE_SINK_CAPS,
	GIVE_SOURCE_CAPS,

	HARD_RESET_SEND,
	HARD_RESET_START,
	SRC_HARD_RESET_VBUS_OFF,
	SRC_HARD_RESET_VBUS_ON,
	SNK_HARD_RESET_SINK_OFF,
	SNK_HARD_RESET_WAIT_VBUS,
	SNK_HARD_RESET_SINK_ON,

	SOFT_RESET,
	SOFT_RESET_SEND,

	DR_SWAP_ACCEPT,
	DR_SWAP_SEND,
	DR_SWAP_SEND_TIMEOUT,
	DR_SWAP_CANCEL,
	DR_SWAP_REJECT,
	DR_SWAP_WAIT,
	DR_SWAP_CHANGE_DR,

	PR_SWAP_ACCEPT,
	PR_SWAP_SEND,
	PR_SWAP_SEND_TIMEOUT,
	PR_SWAP_CANCEL,
	PR_SWAP_REJECT,
	PR_SWAP_WAIT,
	PR_SWAP_START,
	PR_SWAP_SRC_SNK_SOURCE_OFF,
	PR_SWAP_SRC_SNK_SINK_ON,
	PR_SWAP_SNK_SRC_SINK_OFF,
	PR_SWAP_SNK_SRC_SOURCE_ON,

	VCONN_SWAP_ACCEPT,
	VCONN_SWAP_SEND,
	VCONN_SWAP_SEND_TIMEOUT,
	VCONN_SWAP_CANCEL,
	VCONN_SWAP_REJECT,
	VCONN_SWAP_WAIT,
	VCONN_SWAP_START,
	VCONN_SWAP_WAIT_FOR_VCONN,
	VCONN_SWAP_TURN_ON_VCONN,
	VCONN_SWAP_TURN_OFF_VCONN,

	REQUEST_REJECT,
};

struct tcpm_port {
	struct device *dev;

	struct mutex lock;
	struct workqueue_struct *wq;

	struct typec_capability typec_caps;
	struct typec_port *typec_port;

	struct tcpc_dev	*tcpc;

	enum typec_cc_status cc1;
	enum typec_cc_status cc2;
	enum typec_cc_polarity polarity;

	bool attached;
	bool vbus_present;
	bool vconn_source;

	enum tcpm_state prev_state;
	enum tcpm_state state;
	enum tcpm_state delayed_state;
	struct delayed_work state_machine;
	bool state_machine_running;

	struct completion tx_complete;
	enum tcpm_transmit_status tx_status;

	struct completion swap_complete;
	int swap_status;

	unsigned int message_id;
	unsigned int caps_count;
	unsigned int hard_reset_count;
	bool pd_capable;
	bool explicit_contract;

	/* Partner capabilities/requests */
	u32 sink_request;
	u32 source_caps[PDO_MAX_OBJECTS];
	unsigned int nr_source_caps;
	u32 sink_caps[PDO_MAX_OBJECTS];
	unsigned int nr_sink_caps;
};

static int tcpm_pd_transmit(struct tcpm_port *port,
			    enum tcpm_transmit_type type,
			    const struct pd_message *msg)
{
	unsigned long timeout;
	int ret;

	if (msg)
		dev_info(port->dev, "PD TX, header: %#x\n", msg->header);
	else
		dev_info(port->dev, "PD TX, type: %#x\n", type);

	reinit_completion(&port->tx_complete);
	ret = port->tcpc->pd_transmit(port->tcpc, type, msg);
	if (ret < 0)
		return ret;

	mutex_unlock(&port->lock);
	timeout = wait_for_completion_timeout(&port->tx_complete,
				msecs_to_jiffies(PD_T_TCPC_TX_TIMEOUT));
	mutex_lock(&port->lock);
	if (!timeout)
		return -ETIMEDOUT;

	switch (port->tx_status) {
	case TCPC_TX_SUCCESS:
		port->message_id = (port->message_id + 1) & PD_HEADER_ID_MASK;
		return 0;
	case TCPC_TX_DISCARDED:
		return -EAGAIN;
	case TCPC_TX_FAILED:
	default:
		return -EIO;
	}
}

void tcpm_pd_transmit_complete(struct tcpm_port *port,
			       enum tcpm_transmit_status status)
{
	dev_info(port->dev, "PD TX complete, status: %u\n", status);
	port->tx_status = status;
	complete(&port->tx_complete);
}
EXPORT_SYMBOL_GPL(tcpm_pd_transmit_complete);

static int tcpm_set_roles(struct tcpm_port *port, enum typec_pwr_role pwr,
			  enum typec_data_role data)
{
	int ret;

	/* XXX: (Dis)connect mux? */

	ret = port->tcpc->set_pd_header(port->tcpc, pwr, data);
	if (ret < 0)
		return ret;

	port->typec_port->pwr_role = pwr;
	port->typec_port->data_role = data;

	return 0;
}

static int tcpm_pd_send_source_caps(struct tcpm_port *port)
{
	struct pd_message msg;

	memset(&msg, 0, sizeof(msg));
	if (!port->tcpc->config->nr_src_pdo) {
		/* No source capabilities defined, sink only */
		msg.header = PD_HEADER(PD_CTRL_REJECT,
				       port->typec_port->pwr_role,
				       port->typec_port->data_role,
				       port->message_id, 0);
	} else {
		msg.header = PD_HEADER(PD_DATA_SOURCE_CAP,
				       port->typec_port->pwr_role,
				       port->typec_port->data_role,
				       port->message_id,
				       port->tcpc->config->nr_src_pdo);
	}
	memcpy(&msg.payload, port->tcpc->config->src_pdo,
	       port->tcpc->config->nr_src_pdo * sizeof(u32));

	return tcpm_pd_transmit(port, TCPC_TX_SOP, &msg);
}

static int tcpm_pd_send_sink_caps(struct tcpm_port *port)
{
	struct pd_message msg;

	memset(&msg, 0, sizeof(msg));
	if (!port->tcpc->config->nr_snk_pdo) {
		/* No sink capabilities defined, source only */
		msg.header = PD_HEADER(PD_CTRL_REJECT,
				       port->typec_port->pwr_role,
				       port->typec_port->data_role,
				       port->message_id, 0);
	} else {
		msg.header = PD_HEADER(PD_DATA_SOURCE_CAP,
				       port->typec_port->pwr_role,
				       port->typec_port->data_role,
				       port->message_id,
				       port->tcpc->config->nr_snk_pdo);
	}
	memcpy(&msg.payload, port->tcpc->config->snk_pdo,
	       port->tcpc->config->nr_snk_pdo * sizeof(u32));

	return tcpm_pd_transmit(port, TCPC_TX_SOP, &msg);
}

static void tcpm_set_state(struct tcpm_port *port, enum tcpm_state state,
			      unsigned int delay_ms)
{
	if (delay_ms) {
		dev_info(port->dev, "delayed state change %u -> %u @ %u ms\n",
			 port->state, state, delay_ms);
		port->delayed_state = state;
		mod_delayed_work(port->wq, &port->state_machine,
				 msecs_to_jiffies(delay_ms));
	} else {
		dev_info(port->dev, "state change %u -> %u\n", port->state,
			 state);
		port->delayed_state = INVALID_STATE;
		port->prev_state = port->state;
		port->state = state;
		/*
		 * Don't re-queue the state machine work item if we're currently
		 * in the state machine and we're immediately changing states.
		 * tcpm_state_machine_work() will continue running the state
		 * machine.
		 */
		if (!port->state_machine_running)
			mod_delayed_work(port->wq, &port->state_machine, 0);
	}
}

static void tcpm_pd_data_request(struct tcpm_port *port,
				 const struct pd_message *msg)
{
        enum pd_data_msg_type type = pd_header_type(msg->header);
	unsigned int cnt = pd_header_cnt(msg->header);

	switch (type) {
	case PD_DATA_SOURCE_CAP:
		if (port->typec_port->pwr_role != TYPEC_PWR_SINK)
			break;
		memcpy(&port->source_caps, msg->payload, cnt * sizeof(u32));
		port->nr_source_caps = cnt;
		tcpm_set_state(port, SNK_NEGOTIATE_CAPABILITIES, 0);
		break;
	case PD_DATA_REQUEST:
		if (port->typec_port->pwr_role != TYPEC_PWR_SOURCE ||
		    cnt != 1) {
			tcpm_set_state(port, REQUEST_REJECT, 0);
			break;
		}
		port->sink_request = msg->payload[0];
		tcpm_set_state(port, SRC_NEGOTIATE_CAPABILITIES, 0);
		break;
	case PD_DATA_SINK_CAP:
		/* We don't do anything with this at the moment... */
		memcpy(&port->sink_caps, msg->payload, cnt * sizeof(u32));
		port->nr_sink_caps = cnt;
		break;
	case PD_DATA_BIST:
		/* TODO */
	case PD_DATA_VENDOR_DEF:
		/* TODO */
	default:
		dev_warn(port->dev, "Unhandled data message type %#x\n", type);
		break;
	}
}

static void tcpm_pd_ctrl_request(struct tcpm_port *port,
				 const struct pd_message *msg)
{
	enum pd_ctrl_msg_type type = pd_header_type(msg->header);

	switch (type) {
	case PD_CTRL_GOOD_CRC:
	case PD_CTRL_PING:
		break;
	case PD_CTRL_GET_SOURCE_CAP:
		switch (port->state) {
		case SRC_READY:
		case SNK_READY:
			tcpm_set_state(port, GIVE_SOURCE_CAPS, 0);
			break;
		default:
			tcpm_set_state(port, REQUEST_REJECT, 0);
			break;
		}
		break;
	case PD_CTRL_GET_SINK_CAP:
		switch (port->state) {
		case SRC_READY:
		case SNK_READY:
			tcpm_set_state(port, GIVE_SINK_CAPS, 0);
			break;
		default:
			tcpm_set_state(port, REQUEST_REJECT, 0);
			break;
		}
		break;
	case PD_CTRL_GOTO_MIN:
		break;
	case PD_CTRL_PS_RDY:
		switch (port->state) {
		case SNK_TRANSITION_SINK:
			tcpm_set_state(port, SNK_READY, 0);
			break;
		case PR_SWAP_SRC_SNK_SOURCE_OFF:
			tcpm_set_state(port, PR_SWAP_SRC_SNK_SINK_ON, 0);
			break;
		case PR_SWAP_SNK_SRC_SINK_OFF:
			tcpm_set_state(port, PR_SWAP_SNK_SRC_SOURCE_ON, 0);
			break;
		case VCONN_SWAP_WAIT_FOR_VCONN:
			tcpm_set_state(port, VCONN_SWAP_TURN_OFF_VCONN, 0);
			break;
		default:
			break;
		}
		break;
	case PD_CTRL_REJECT:
	case PD_CTRL_WAIT:
		switch (port->state) {
		case SNK_NEGOTIATE_CAPABILITIES:
			if (port->explicit_contract)
				tcpm_set_state(port, SNK_READY, 0);
			else
				tcpm_set_state(port,
						  SNK_WAIT_CAPABILITIES, 0);
			break;
		case DR_SWAP_SEND:
			tcpm_set_state(port, DR_SWAP_CANCEL, 0);
			break;
		case PR_SWAP_SEND:
			tcpm_set_state(port, PR_SWAP_CANCEL, 0);
			break;
		case VCONN_SWAP_SEND:
			tcpm_set_state(port, VCONN_SWAP_CANCEL, 0);
			break;
		default:
			break;
		}
		break;
	case PD_CTRL_ACCEPT:
		switch (port->state) {
		case SNK_NEGOTIATE_CAPABILITIES:
			tcpm_set_state(port, SNK_TRANSITION_SINK, 0);
			break;
		case SOFT_RESET_SEND:
			port->message_id = 0;
			if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE)
				tcpm_set_state(port,
						  SRC_SEND_CAPABILITIES, 0);
			else	
				tcpm_set_state(port,
						  SNK_WAIT_CAPABILITIES, 0);
			break;
		case DR_SWAP_SEND:
			tcpm_set_state(port, DR_SWAP_CHANGE_DR, 0);
			break;
		case VCONN_SWAP_SEND:
			tcpm_set_state(port, VCONN_SWAP_START, 0);
			break;
		default:
			break;
		}
		break;
	case PD_CTRL_SOFT_RESET:
		tcpm_set_state(port, SOFT_RESET, 0);
		break;
	case PD_CTRL_DR_SWAP:
		if (port->typec_caps.type != TYPEC_PORT_DRP) {
			tcpm_set_state(port, DR_SWAP_REJECT, 0);
			break;
		}
		switch (port->state) {
		case SRC_READY:
		case SNK_READY:
			tcpm_set_state(port, DR_SWAP_ACCEPT, 0);
			break;
		default:
			tcpm_set_state(port, DR_SWAP_WAIT, 0);
			break;
		}
		break;
	case PD_CTRL_PR_SWAP:
		if (port->typec_caps.type != TYPEC_PORT_DRP) {
			tcpm_set_state(port, PR_SWAP_REJECT, 0);
			break;
		}
		switch (port->state) {
		case SRC_READY:
		case SNK_READY:
			tcpm_set_state(port, PR_SWAP_ACCEPT, 0);
			break;
		default:
			tcpm_set_state(port, PR_SWAP_WAIT, 0);
			break;
		}
		break;
	case PD_CTRL_VCONN_SWAP:
		switch (port->state) {
		case SRC_READY:
		case SNK_READY:
			tcpm_set_state(port, VCONN_SWAP_ACCEPT, 0);
			break;
		default:
			tcpm_set_state(port, VCONN_SWAP_WAIT, 0);
			break;
		}
		break;
	default:
		dev_warn(port->dev, "Unhandled ctrl message type %#x\n", type);
		break;
	}
}

void tcpm_pd_receive(struct tcpm_port *port, const struct pd_message *msg)
{
	unsigned int cnt = pd_header_cnt(msg->header);

	mutex_lock(&port->lock);
	dev_info(port->dev, "PD RX, header: %#x\n", msg->header);
	if (!port->attached)
		goto out;

	if (cnt)
		tcpm_pd_data_request(port, msg);
	else
		tcpm_pd_ctrl_request(port, msg);
out:
	mutex_unlock(&port->lock);
}
EXPORT_SYMBOL_GPL(tcpm_pd_receive);

static int tcpm_pd_send_control(struct tcpm_port *port,
				enum pd_ctrl_msg_type type)
{
	struct pd_message msg;

	memset(&msg, 0, sizeof(msg));
	msg.header = PD_HEADER(type, port->typec_port->pwr_role,
			       port->typec_port->data_role,
			       port->message_id, 0);

	return tcpm_pd_transmit(port, TCPC_TX_SOP, &msg);
}

static int tcpm_pd_check_request(struct tcpm_port *port)
{
	u32 pdo, rdo = port->sink_request;
	unsigned int max, op, pdo_max, index;
	enum pd_pdo_type type;

	index = rdo_index(rdo);
	if (!index || index > port->tcpc->config->nr_src_pdo)
		return -EINVAL;

	pdo = port->tcpc->config->src_pdo[index - 1];
	type = pdo_type(pdo);
	switch (type) {
	case PDO_TYPE_FIXED:
	case PDO_TYPE_VAR:
		max = rdo_max_current(rdo);
		op = rdo_op_current(rdo);
		pdo_max = pdo_max_current(pdo);

		if (op > pdo_max)
			return -EINVAL;
		if (max > pdo_max && !(rdo & RDO_CAP_MISMATCH))
			return -EINVAL;

		if (type == PDO_TYPE_FIXED)
			dev_info(port->dev,
				 "Requested %u mV, %u mA for %u / %u mA",
				 pdo_fixed_voltage(pdo), pdo_max, op, max);
		else
			dev_info(port->dev,
				 "Requested %u -> %u mV, %u mA for %u / %u mA",
				 pdo_min_voltage(pdo), pdo_max_voltage(pdo),
				 pdo_max, op, max);
		break;
	case PDO_TYPE_BATT:
		max = rdo_max_power(rdo);
		op = rdo_op_power(rdo);
		pdo_max = pdo_max_power(pdo);

		if (op > pdo_max)
			return -EINVAL;
		if (max > pdo_max && !(rdo & RDO_CAP_MISMATCH))
			return -EINVAL;

		dev_info(port->dev,
			 "Requested %u -> %u mV, %u mW for %u / %u mW",
			 pdo_min_voltage(pdo), pdo_max_voltage(pdo),
			 pdo_max, op, max);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int tcpm_pd_select_pdo(struct tcpm_port *port)
{
	unsigned int i, max_mw = 0;
	int ret = -EINVAL;

	/*
	 * Select the source PDO providing the most power while staying within
	 * the board's voltage limits.
	 */
	for (i = 0; i < port->nr_source_caps; i++) {
		u32 pdo = port->source_caps[i];
		enum pd_pdo_type type = pdo_type(pdo);
		unsigned int mv, ma, mw;

		if (type == PDO_TYPE_FIXED)
			mv = pdo_fixed_voltage(pdo);
		else
			mv = pdo_min_voltage(pdo);

		if (type == PDO_TYPE_BATT) {
			mw = pdo_max_power(pdo);
		} else {
			ma = min(pdo_max_current(pdo),
				 port->tcpc->config->max_snk_ma);
			mw = ma * mv / 1000;
		}

		if (mw > max_mw && mv <= port->tcpc->config->max_snk_mv) {
			ret = i;
			max_mw = mw;
		}
	}

	return ret;
}

static int tcpm_pd_build_request(struct tcpm_port *port, u32 *rdo)
{
	unsigned int mv, ma, mw, flags;
	enum pd_pdo_type type;
	int index;
	u32 pdo;

	index = tcpm_pd_select_pdo(port);
	if (index < 0)
		return -EINVAL;
	pdo = port->source_caps[index];
	type = pdo_type(pdo);

	if (type == PDO_TYPE_FIXED)
		mv = pdo_fixed_voltage(pdo);
	else
		mv = pdo_min_voltage(pdo);

	/* Select maximum available current within the board's power limit */
	if (type == PDO_TYPE_BATT) {
		mw = pdo_max_power(pdo);
		ma = 1000 * min(mw, port->tcpc->config->max_snk_mw) / mv;
	} else {
		ma = min(pdo_max_current(pdo),
			 1000 * port->tcpc->config->max_snk_mw / mv);
	}
	ma = min(ma, port->tcpc->config->max_snk_ma);

	/* XXX: Any other flags need to be set? */
	flags = 0;

	/* Set mismatch bit if offered power is less than operating power */
	mw = ma * mv;
	if (mw < port->tcpc->config->operating_snk_mw)
		flags |= RDO_CAP_MISMATCH;

	if (type == PDO_TYPE_BATT) {
		*rdo = RDO_BATT(index + 1, mw, mw, flags);

		dev_info(port->dev, "Requesting PDO %d, %u mV, %u mW\n",
			 index, mv, mw);
	} else {
		*rdo = RDO_FIXED(index + 1, ma, ma, flags);

		dev_info(port->dev, "Requesting PDO %d, %u mV, %u mA\n",
			 index, mv, ma);
	}

	return 0;
}

static int tcpm_pd_send_request(struct tcpm_port *port)
{
	struct pd_message msg;
	int ret;
	u32 rdo;

	ret = tcpm_pd_build_request(port, &rdo);
	if (ret < 0)
		return ret;

	memset(&msg, 0, sizeof(msg));
	msg.header = PD_HEADER(PD_DATA_REQUEST,
			       port->typec_port->pwr_role,
			       port->typec_port->data_role,
			       port->message_id, 1);
	msg.payload[0] = rdo;

	return tcpm_pd_transmit(port, TCPC_TX_SOP, &msg);
}

static int tcpm_src_attach(struct tcpm_port *port)
{
	int ret;

	if (port->attached)
		return 0;

	if (port->cc1 == TYPEC_CC_RD)
		port->polarity = TYPEC_POLARITY_CC1;
	else
		port->polarity = TYPEC_POLARITY_CC2;
	ret = port->tcpc->set_polarity(port->tcpc, port->polarity);
	if (ret < 0)
		return ret;

	ret = tcpm_set_roles(port, TYPEC_PWR_SOURCE, TYPEC_HOST);
	if (ret < 0)
		return ret;

	/* XXX: regulator_enable(vbus) */

	ret = port->tcpc->set_pd_rx(port->tcpc, true);
	if (ret < 0)
		goto out_disable_mux;

	ret = port->tcpc->set_vconn(port->tcpc, true);
	if (ret < 0)
		goto out_disable_pd;
	port->vconn_source = true;

	port->pd_capable = false;
	port->hard_reset_count = 0;

	typec_connect(port->typec_port);
	port->attached = true;

	return 0;

out_disable_pd:
	port->tcpc->set_pd_rx(port->tcpc, false);
out_disable_mux:
	/* XXX: Disconnect SuperSpeed mux */
	return ret;
}

static void tcpm_src_detach(struct tcpm_port *port)
{
	if (!port->attached)
		return;

	typec_disconnect(port->typec_port);
	port->attached = false;

	if (port->vconn_source) {
		port->tcpc->set_vconn(port->tcpc, false);
		port->vconn_source = false;
	}

	/* XXX: regulator_disable(vbus) */

	/* XXX: Disconnect mux */

	port->tcpc->set_pd_rx(port->tcpc, false);
}

static int tcpm_snk_attach(struct tcpm_port *port)
{
	int ret;

	if (port->attached)
		return 0;

	if (port->cc1 == TYPEC_CC_OPEN)
		port->polarity = TYPEC_POLARITY_CC1;
	else
		port->polarity = TYPEC_POLARITY_CC2;
	ret = port->tcpc->set_polarity(port->tcpc, port->polarity);
	if (ret < 0)
		return ret;

	ret = tcpm_set_roles(port, TYPEC_PWR_SINK, TYPEC_DEVICE);
	if (ret < 0)
		return ret;

	ret = port->tcpc->set_pd_rx(port->tcpc, true);
	if (ret < 0)
		goto out_disable_mux;

	/* XXX: Start sinking power */

	port->vconn_source = false;
	port->pd_capable = false;
	port->hard_reset_count = 0;

	typec_connect(port->typec_port);
	port->attached = true;

	return 0;

out_disable_mux:
	/* XXX: (Dis)connect SuperSpeed mux? */
	return ret;
}

static void tcpm_snk_detach(struct tcpm_port *port)
{
	if (!port->attached)
		return;

	typec_disconnect(port->typec_port);
	port->attached = false;

	/* XXX: Stop sinking power */

	if (port->vconn_source) {
		port->tcpc->set_vconn(port->tcpc, false);
		port->vconn_source = false;
	}

	/* XXX: Disconnect mux */

	port->tcpc->set_pd_rx(port->tcpc, false);
}

static int tcpm_acc_attach(struct tcpm_port *port)
{
	int ret;

	if (!port->attached)
		return 0;

	ret = tcpm_set_roles(port, TYPEC_PWR_SOURCE, TYPEC_HOST);
	if (ret < 0)
		return ret;

	typec_connect(port->typec_port);
	port->attached = true;

	return 0;
}

static void tcpm_acc_detach(struct tcpm_port *port)
{
	typec_disconnect(port->typec_port);
	port->attached = false;

	/* XXX: Disconnect mux */
}

static inline enum tcpm_state hard_reset_state(struct tcpm_port *port)
{
	if (port->hard_reset_count < PD_N_HARD_RESET_COUNT)
		return HARD_RESET_SEND;
	return (port->typec_port->pwr_role == TYPEC_PWR_SOURCE) ?
		SRC_UNATTACHED : SNK_UNATTACHED;
}

static inline enum tcpm_state ready_state(struct tcpm_port *port)
{
	if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE)
		return SRC_READY;
	else
		return SNK_READY;
}

static void run_state_machine(struct tcpm_port *port)
{
	int ret;

	switch (port->state) {
	/* SRC states */
	case SRC_UNATTACHED:
		tcpm_src_detach(port);
		port->tcpc->set_cc(port->tcpc, TYPEC_CC_RP_DEF);
		break;
	case SRC_ATTACH_WAIT:
		if (port->cc1 == TYPEC_CC_RD &&
		    port->cc2 == TYPEC_CC_RD)
			tcpm_set_state(port, DEBUG_ACC_ATTACHED,
					  TYPEC_T_CC_DEBOUNCE);			
		else if (port->cc1 == TYPEC_CC_RA &&
			   port->cc2 == TYPEC_CC_RA)
			tcpm_set_state(port, AUDIO_ACC_ATTACHED,
					  TYPEC_T_CC_DEBOUNCE);			
		else if (port->cc1 == TYPEC_CC_RD ||
			 port->cc2 == TYPEC_CC_RD)
			tcpm_set_state(port, SRC_ATTACHED,
					  TYPEC_T_CC_DEBOUNCE);			
		break;
	case SRC_ATTACHED:
		ret = tcpm_src_attach(port);
		if (ret < 0)
			tcpm_set_state(port, SRC_UNATTACHED, 0);
		else
			tcpm_set_state(port, SRC_STARTUP, 0);
		break;
	case SRC_STARTUP:
		port->typec_port->pwr_opmode = TYPEC_PWR_MODE_USB;
		port->caps_count = 0;
		port->message_id = 0;
		port->explicit_contract = false;
		tcpm_set_state(port, SRC_SEND_CAPABILITIES, 0);
		break;
	case SRC_SEND_CAPABILITIES:
		port->caps_count++;
		if (port->caps_count > PD_N_CAPS_COUNT)
			break;
		ret = tcpm_pd_send_source_caps(port);
		if (ret < 0) {
			tcpm_set_state(port, SRC_SEND_CAPABILITIES,
					  PD_T_SEND_SOURCE_CAP);
		} else {
			port->hard_reset_count = 0;
			port->caps_count = 0;
			port->pd_capable = true;
			tcpm_set_state(port, hard_reset_state(port),
					  PD_T_SEND_SOURCE_CAP);
		}
		break;
	case SRC_NEGOTIATE_CAPABILITIES:
		ret = tcpm_pd_check_request(port);
		if (ret < 0) {
			tcpm_pd_send_control(port, PD_CTRL_REJECT);
			if (!port->explicit_contract) {
				tcpm_set_state(port,
						  SRC_WAIT_NEW_CAPABILITIES, 0);
			} else {
				tcpm_set_state(port, SRC_READY, 0);
			}
		} else {
			tcpm_pd_send_control(port, PD_CTRL_ACCEPT);
			tcpm_set_state(port, SRC_TRANSITION_SUPPLY,
					  PD_T_SRC_TRANSITION);
		}
		break;
	case SRC_TRANSITION_SUPPLY:
		/* XXX: regulator_set_voltage(vbus, ...) */
		tcpm_pd_send_control(port, PD_CTRL_PS_RDY);
		port->explicit_contract = true;
		port->typec_port->pwr_opmode = TYPEC_PWR_MODE_PD;
		tcpm_set_state(port, SRC_READY, 0);
		break;
	case SRC_READY:
		/* XXX: Send Discovery VDM? */
		tcpm_pd_send_control(port, PD_CTRL_PING);
		tcpm_set_state(port, SRC_READY, PD_T_SOURCE_ACTIVITY);
		break;
	case SRC_WAIT_NEW_CAPABILITIES:
		/* Nothing to do... */
		break;

	/* SNK states */
	case SNK_UNATTACHED:
		tcpm_snk_detach(port);
		port->tcpc->set_cc(port->tcpc, TYPEC_CC_RD);
		break;
	case SNK_ATTACH_WAIT:
		if ((port->cc1 == TYPEC_CC_OPEN &&
		     port->cc2 != TYPEC_CC_OPEN) ||
		    (port->cc1 != TYPEC_CC_OPEN &&
		     port->cc2 == TYPEC_CC_OPEN))
			tcpm_set_state(port, SNK_DEBOUNCED,
					  TYPEC_T_CC_DEBOUNCE);
		else if (port->cc1 == TYPEC_CC_OPEN &&
			 port->cc2 == TYPEC_CC_OPEN)
			tcpm_set_state(port, SNK_UNATTACHED,
					  TYPEC_T_PD_DEBOUNCE);
		break;
	case SNK_DEBOUNCED:
		if (port->vbus_present)
			tcpm_set_state(port, SNK_ATTACHED, 0);
		else if (port->cc1 == TYPEC_CC_OPEN &&
			 port->cc2 == TYPEC_CC_OPEN)
			tcpm_set_state(port, SNK_UNATTACHED,
					  TYPEC_T_PD_DEBOUNCE);
		break;
	case SNK_ATTACHED:
		ret = tcpm_snk_attach(port);
		if (ret < 0)
			tcpm_set_state(port, SNK_UNATTACHED, 0);
		else
			tcpm_set_state(port, SNK_STARTUP, 0);
		break;
	case SNK_STARTUP:
		/* XXX: Check monitored CC pin for actual current supplied? */
		port->typec_port->pwr_opmode = TYPEC_PWR_MODE_USB;
		port->message_id = 0;
		port->explicit_contract = false;
		tcpm_set_state(port, SNK_WAIT_CAPABILITIES, 0);
		break;
	case SNK_WAIT_CAPABILITIES:
		tcpm_set_state(port, hard_reset_state(port),
				  PD_T_SINK_WAIT_CAP);
		break;
	case SNK_NEGOTIATE_CAPABILITIES:
		port->pd_capable = true;
		ret = tcpm_pd_send_request(port);
		if (ret < 0) {
			/* Let the Source send capabilities again. */
			tcpm_set_state(port, SNK_WAIT_CAPABILITIES, 0);
		} else {
			tcpm_set_state(port, hard_reset_state(port),
					  PD_T_SENDER_RESPONSE);
		}
		break;
	case SNK_TRANSITION_SINK:
		tcpm_set_state(port, hard_reset_state(port),
				  PD_T_PS_TRANSITION);
		break;
	case SNK_READY:
		/* XXX: Send Discovery VDM? */
		port->explicit_contract = true;
		port->typec_port->pwr_opmode = TYPEC_PWR_MODE_PD;
		break;

	/* Accessory states */
	case ACC_UNATTACHED:
		tcpm_acc_detach(port);
		tcpm_set_state(port, SRC_UNATTACHED, 0);
		break;
	case DEBUG_ACC_ATTACHED:
	case AUDIO_ACC_ATTACHED:
		ret = tcpm_acc_attach(port);
		if (ret < 0)
			tcpm_set_state(port, ACC_UNATTACHED, 0);
		break;
	case AUDIO_ACC_DEBOUNCE:
		tcpm_set_state(port, ACC_UNATTACHED, TYPEC_T_CC_DEBOUNCE);
		break;

	/* Give_{Sink,Source}_Caps states */
	case GIVE_SINK_CAPS:
		tcpm_pd_send_sink_caps(port);
		tcpm_set_state(port, port->prev_state, 0);
		break;
	case GIVE_SOURCE_CAPS:
		tcpm_pd_send_source_caps(port);
		tcpm_set_state(port, port->prev_state, 0);
		break;

	/* Hard_Reset states */
	case HARD_RESET_SEND:
		port->hard_reset_count++;
		tcpm_pd_transmit(port, TCPC_TX_HARD_RESET, NULL);
		tcpm_set_state(port, HARD_RESET_START, 0);
		break;
	case HARD_RESET_START:
		if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE)
			tcpm_set_state(port, SRC_HARD_RESET_VBUS_OFF,
					  PD_T_PS_HARD_RESET);
		else
			tcpm_set_state(port, SNK_HARD_RESET_SINK_OFF, 0);
		break;
	case SRC_HARD_RESET_VBUS_OFF:
		/* XXX: regulator_disable(vbus) */
		tcpm_set_state(port, SRC_HARD_RESET_VBUS_ON,
				  PD_T_SRC_RECOVER);
		break;
	case SRC_HARD_RESET_VBUS_ON:
		/* XXX: regulator_enable(vbus) */
		tcpm_set_state(port, SRC_STARTUP, 0);
		break;
	case SNK_HARD_RESET_SINK_OFF:
		/* XXX: Stop sinking power */
		tcpm_set_state(port, hard_reset_state(port), PD_T_SAFE_0V);
		break;
	case SNK_HARD_RESET_WAIT_VBUS:
		/* Assume we're disconnected if VBUS doesn't come back. */
		tcpm_set_state(port, SNK_UNATTACHED,
				  PD_T_SRC_RECOVER_MAX + PD_T_SRC_TURN_ON);
		break;
	case SNK_HARD_RESET_SINK_ON:
		/* XXX: Start sinking power */
		tcpm_set_state(port, SNK_STARTUP, 0);
		break;

	/* Soft_Reset states */
	case SOFT_RESET:
		port->message_id = 0;
		tcpm_pd_send_control(port, PD_CTRL_ACCEPT);
		if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE)
			tcpm_set_state(port, SRC_SEND_CAPABILITIES, 0);
		else
			tcpm_set_state(port, SNK_WAIT_CAPABILITIES, 0);
		break;
	case SOFT_RESET_SEND:
		port->message_id = 0;
		tcpm_pd_send_control(port, PD_CTRL_SOFT_RESET);
		tcpm_set_state(port, hard_reset_state(port),
				  PD_T_SENDER_RESPONSE);
		break;

	/* DR_Swap states */
	case DR_SWAP_SEND:
		tcpm_pd_send_control(port, PD_CTRL_DR_SWAP);
		tcpm_set_state(port, DR_SWAP_SEND_TIMEOUT,
				  PD_T_SENDER_RESPONSE);
		break;
	case DR_SWAP_ACCEPT:
		tcpm_pd_send_control(port, PD_CTRL_ACCEPT);
		tcpm_set_state(port, DR_SWAP_CHANGE_DR, 0);
		break;
	case DR_SWAP_SEND_TIMEOUT:
		port->swap_status = -ETIMEDOUT;
		complete(&port->swap_complete);
		tcpm_set_state(port, ready_state(port), 0);
		break;
	case DR_SWAP_CHANGE_DR:
		if (port->typec_port->data_role == TYPEC_HOST)
			tcpm_set_roles(port, port->typec_port->pwr_role,
				       TYPEC_DEVICE);
		else
			tcpm_set_roles(port, port->typec_port->pwr_role,
				       TYPEC_HOST);
		port->swap_status = 0;
		complete(&port->swap_complete);
		tcpm_set_state(port, ready_state(port), 0);
		break;

	/* PR_Swap states */
	case PR_SWAP_ACCEPT:
		tcpm_pd_send_control(port, PD_CTRL_ACCEPT);
		tcpm_set_state(port, PR_SWAP_START, 0);
		break;
	case PR_SWAP_SEND:
		tcpm_pd_send_control(port, PD_CTRL_PR_SWAP);
		tcpm_set_state(port, PR_SWAP_SEND_TIMEOUT,
				  PD_T_SENDER_RESPONSE);
		break;
	case PR_SWAP_SEND_TIMEOUT:
		port->swap_status = -ETIMEDOUT;
		complete(&port->swap_complete);
		tcpm_set_state(port, ready_state(port), 0);
		break;
	case PR_SWAP_START:
		if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE)
			tcpm_set_state(port, PR_SWAP_SRC_SNK_SOURCE_OFF,
					  PD_T_SRC_TRANSITION);
		else
			tcpm_set_state(port, PR_SWAP_SNK_SRC_SINK_OFF, 0);
		break;
	case PR_SWAP_SRC_SNK_SOURCE_OFF:
		/* XXX: regulator_disable(vbus) */
		port->tcpc->set_cc(port->tcpc, TYPEC_CC_RD);
		tcpm_pd_send_control(port, PD_CTRL_PS_RDY);
		tcpm_set_state(port, SNK_UNATTACHED, PD_T_PS_SOURCE_ON);
		break;
	case PR_SWAP_SRC_SNK_SINK_ON:
		/* XXX: Start sinking power */
		tcpm_set_roles(port, TYPEC_PWR_SINK,
			       port->typec_port->data_role);
		tcpm_set_state(port, SNK_STARTUP, 0);
		break;
	case PR_SWAP_SNK_SRC_SINK_OFF:
		/* XXX: Stop sinking power */
		tcpm_set_state(port, hard_reset_state(port),
				  PD_T_PS_SOURCE_OFF);
		break;
	case PR_SWAP_SNK_SRC_SOURCE_ON:
		port->tcpc->set_cc(port->tcpc, TYPEC_CC_RP_DEF);
		/* XXX: regulator_enable(vbus) */
		tcpm_pd_send_control(port, PD_CTRL_PS_RDY);
		tcpm_set_roles(port, TYPEC_PWR_SOURCE,
			       port->typec_port->data_role);
		tcpm_set_state(port, SRC_STARTUP, 0);
		break;

	case VCONN_SWAP_ACCEPT:
		tcpm_pd_send_control(port, PD_CTRL_ACCEPT);
		tcpm_set_state(port, VCONN_SWAP_START, 0);
		break;
	case VCONN_SWAP_SEND:
		tcpm_pd_send_control(port, PD_CTRL_VCONN_SWAP);
		tcpm_set_state(port, VCONN_SWAP_SEND_TIMEOUT,
				  PD_T_SENDER_RESPONSE);
		break;
	case VCONN_SWAP_SEND_TIMEOUT:
		port->swap_status = -ETIMEDOUT;
		complete(&port->swap_complete);
		tcpm_set_state(port, ready_state(port), 0);
		break;
	case VCONN_SWAP_START:
		if (port->vconn_source)
			tcpm_set_state(port, VCONN_SWAP_WAIT_FOR_VCONN, 0);
		else
			tcpm_set_state(port, VCONN_SWAP_TURN_ON_VCONN, 0);
		break;
	case VCONN_SWAP_WAIT_FOR_VCONN:
		tcpm_set_state(port, hard_reset_state(port),
				  PD_T_VCONN_SOURCE_ON);
		break;
	case VCONN_SWAP_TURN_ON_VCONN:
		port->tcpc->set_vconn(port->tcpc, true);
		port->vconn_source = true;
		tcpm_pd_send_control(port, PD_CTRL_PS_RDY);
		tcpm_set_state(port, ready_state(port), 0);
		break;
	case VCONN_SWAP_TURN_OFF_VCONN:
		port->tcpc->set_vconn(port->tcpc, false);
		port->vconn_source = false;
		tcpm_set_state(port, ready_state(port), 0);
		break;

	case DR_SWAP_CANCEL:
	case PR_SWAP_CANCEL:
	case VCONN_SWAP_CANCEL:
		/* XXX: Distinguish between REJECT and WAIT */
		port->swap_status = -EAGAIN;
		complete(&port->swap_complete);
		if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE)
			tcpm_set_state(port, SRC_READY, 0);
		else
			tcpm_set_state(port, SNK_READY, 0);
		break;
	case REQUEST_REJECT:
	case DR_SWAP_REJECT:
	case PR_SWAP_REJECT:
	case VCONN_SWAP_REJECT:
		tcpm_pd_send_control(port, PD_CTRL_REJECT);
		tcpm_set_state(port, port->prev_state, 0);
		break;
	case DR_SWAP_WAIT:
	case PR_SWAP_WAIT:
	case VCONN_SWAP_WAIT:
		tcpm_pd_send_control(port, PD_CTRL_WAIT);
		tcpm_set_state(port, port->prev_state, 0);
		break;

	default:
		BUG_ON(1);
		break;
	}
}

static void tcpm_state_machine_work(struct work_struct *work)
{
	struct tcpm_port *port = container_of(work, struct tcpm_port,
					      state_machine.work);
	enum tcpm_state prev_state;

	mutex_lock(&port->lock);
	port->state_machine_running = true;

	/* If we were queued due to a delayed state change, update it now */
	if (port->delayed_state) {
		port->prev_state = port->state;
		port->state = port->delayed_state;
		port->delayed_state = INVALID_STATE;
	}

	/*
	 * Continue running as long as we have (non-delayed) state changes
	 * to make.
	 */
	do {
		prev_state = port->state;
		run_state_machine(port);
	} while (port->state != prev_state && !port->delayed_state);

	port->state_machine_running = false;
	mutex_unlock(&port->lock);
}

void tcpm_cc_change(struct tcpm_port *port, enum typec_cc_status cc1,
		    enum typec_cc_status cc2)
{
	enum typec_cc_status old_cc1, old_cc2;

	mutex_lock(&port->lock);

	old_cc1 = port->cc1;
	old_cc2 = port->cc2;
	port->cc1 = cc1;
	port->cc2 = cc2;
	dev_info(port->dev, "CC1: %u -> %u, CC2: %u -> %u\n", old_cc1, cc1,
		 old_cc2, cc2);

	/*
	 * TODO:
	 *  - DRP toggling
	 *  - Try.SRC and TryWait.SNK states
	 */

	switch (port->state) {
	case SRC_UNATTACHED:
	case ACC_UNATTACHED:
		if ((cc1 == TYPEC_CC_RD || cc2 == TYPEC_CC_RD) ||
		    (cc1 == TYPEC_CC_RA && cc2 == TYPEC_CC_RA))
			tcpm_set_state(port, SRC_ATTACH_WAIT, 0);
		break;
	case SRC_ATTACH_WAIT:
		if ((cc1 == TYPEC_CC_OPEN && cc2 == TYPEC_CC_OPEN) ||
		    (cc1 == TYPEC_CC_OPEN && cc2 == TYPEC_CC_RA) ||
		    (cc1 == TYPEC_CC_RA && cc2 == TYPEC_CC_OPEN))
			tcpm_set_state(port, SRC_UNATTACHED, 0);
		else if (cc1 != old_cc1 || cc2 != old_cc2)
			tcpm_set_state(port, SRC_ATTACH_WAIT, 0);
		break;
	case SRC_ATTACHED:
		if ((port->polarity == TYPEC_POLARITY_CC1 &&
		     cc1 == TYPEC_CC_OPEN) ||
		    (port->polarity == TYPEC_POLARITY_CC2 &&
		     cc2 == TYPEC_CC_OPEN))
			tcpm_set_state(port, SRC_UNATTACHED, 0);
		break;

	case SNK_UNATTACHED:
		if ((cc1 != TYPEC_CC_OPEN && cc2 == TYPEC_CC_OPEN) ||
		    (cc1 == TYPEC_CC_OPEN && cc2 != TYPEC_CC_OPEN))
			tcpm_set_state(port, SNK_ATTACH_WAIT, 0);
		break;
	case SNK_ATTACH_WAIT:
		tcpm_set_state(port, SNK_ATTACH_WAIT, 0);
		break;
	case SNK_DEBOUNCED:
		tcpm_set_state(port, SNK_DEBOUNCED, 0);
		break;

	case AUDIO_ACC_ATTACHED:
		if (cc1 == TYPEC_CC_OPEN || cc2 == TYPEC_CC_OPEN)
			tcpm_set_state(port, AUDIO_ACC_DEBOUNCE, 0);
		break;
	case AUDIO_ACC_DEBOUNCE:
		if (cc1 == TYPEC_CC_RA && cc2 == TYPEC_CC_RA)
			tcpm_set_state(port, AUDIO_ACC_ATTACHED, 0);
		break;

	case DEBUG_ACC_ATTACHED:
		if (cc1 == TYPEC_CC_OPEN || cc2 == TYPEC_CC_OPEN)
			tcpm_set_state(port, ACC_UNATTACHED, 0);
		break;

	default:
		if (port->typec_port->pwr_role == TYPEC_PWR_SOURCE &&
		    port->attached &&
		    ((port->polarity == TYPEC_POLARITY_CC1 &&
		      cc1 == TYPEC_CC_OPEN) ||
		     (port->polarity == TYPEC_POLARITY_CC2 &&
		      cc2 == TYPEC_CC_OPEN)))
			tcpm_set_state(port, SRC_UNATTACHED, 0);
		break;
	}
	mutex_unlock(&port->lock);
}
EXPORT_SYMBOL_GPL(tcpm_cc_change);

void tcpm_vbus_on(struct tcpm_port *port)
{
	mutex_lock(&port->lock);
	dev_info(port->dev, "VBUS on\n");
	port->vbus_present = true;
	switch (port->state) {
	case SNK_DEBOUNCED:
		tcpm_set_state(port, SNK_ATTACHED, 0);
		break;
	case SNK_HARD_RESET_WAIT_VBUS:
		tcpm_set_state(port, SNK_STARTUP, 0);
		break;
	default:
		break;
	}
	mutex_unlock(&port->lock);
}
EXPORT_SYMBOL_GPL(tcpm_vbus_on);

void tcpm_vbus_off(struct tcpm_port *port)
{
	mutex_lock(&port->lock);
	dev_info(port->dev, "VBUS off\n");
	port->vbus_present = false;
	switch (port->state) {
	case SNK_HARD_RESET_SINK_OFF:
		tcpm_set_state(port, SNK_HARD_RESET_WAIT_VBUS, 0);
		break;
	default:
		if (port->typec_port->pwr_role == TYPEC_PWR_SINK &&
		    port->attached)
			tcpm_set_state(port, SNK_UNATTACHED, 0);
		break;
	}
	mutex_unlock(&port->lock);
}
EXPORT_SYMBOL_GPL(tcpm_vbus_off);

void tcpm_pd_hard_reset(struct tcpm_port *port)
{
	mutex_lock(&port->lock);
	if (port->attached)
		tcpm_set_state(port, HARD_RESET_START, 0);
	mutex_unlock(&port->lock);
}
EXPORT_SYMBOL_GPL(tcpm_pd_hard_reset);

static inline struct tcpm_port *typec_to_tcpm(struct typec_port *typec)
{
	return container_of(typec->cap, struct tcpm_port, typec_caps);
}

static int tcpm_dr_swap(struct typec_port *typec)
{
	struct tcpm_port *port = typec_to_tcpm(typec);

	mutex_lock(&port->lock);
	if (port->typec_caps.type != TYPEC_PORT_DRP) {
		mutex_unlock(&port->lock);
		return -EINVAL;
	}
	if (port->state != SRC_READY && port->state != SNK_READY) {
		mutex_unlock(&port->lock);
		return -EAGAIN;
	}

	port->swap_status = 0;
	reinit_completion(&port->swap_complete);
	tcpm_set_state(port, DR_SWAP_SEND, 0);
	mutex_unlock(&port->lock);

	wait_for_completion(&port->swap_complete);

	return port->swap_status;
}

static int tcpm_pr_swap(struct typec_port *typec)
{
	struct tcpm_port *port = typec_to_tcpm(typec);

	mutex_lock(&port->lock);
	if (port->typec_caps.type != TYPEC_PORT_DRP) {
		mutex_unlock(&port->lock);
		return -EINVAL;
	}
	if (port->state != SRC_READY && port->state != SNK_READY) {
		mutex_unlock(&port->lock);
		return -EAGAIN;
	}

	port->swap_status = 0;
	reinit_completion(&port->swap_complete);
	tcpm_set_state(port, PR_SWAP_SEND, 0);
	mutex_unlock(&port->lock);

	wait_for_completion(&port->swap_complete);

	return port->swap_status;
}

static __maybe_unused int tcpm_vconn_swap(struct typec_port *typec)
{
	struct tcpm_port *port = typec_to_tcpm(typec);

	mutex_lock(&port->lock);
	if (port->state != SRC_READY && port->state != SNK_READY) {
		mutex_unlock(&port->lock);
		return -EAGAIN;
	}

	port->swap_status = 0;
	reinit_completion(&port->swap_complete);
	tcpm_set_state(port, VCONN_SWAP_SEND, 0);
	mutex_unlock(&port->lock);

	wait_for_completion(&port->swap_complete);

	return port->swap_status;
}

static void tcpm_init(struct tcpm_port *port)
{
	port->tcpc->init(port->tcpc);
	port->tcpc->set_pd_rx(port->tcpc, false);
	port->vbus_present = port->tcpc->get_vbus(port->tcpc);

	if (port->tcpc->config->default_role == TYPEC_PWR_SOURCE)
		tcpm_set_state(port, SRC_UNATTACHED, 0);
	else
		tcpm_set_state(port, SNK_UNATTACHED, 0);
}

void tcpm_tcpc_reset(struct tcpm_port *port)
{
	mutex_lock(&port->lock);
	/* XXX: Maintain PD connection if possible? */
	tcpm_init(port);
	mutex_unlock(&port->lock);
}
EXPORT_SYMBOL_GPL(tcpm_tcpc_reset);

struct tcpm_port *tcpm_register_port(struct device *dev, struct tcpc_dev *tcpc)
{
	struct tcpm_port *port;
	int err;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	port->dev = dev;
	port->tcpc = tcpc;

	mutex_init(&port->lock);

	port->wq = alloc_workqueue("pd", WQ_UNBOUND, 0);
	if (!port->wq)
		return ERR_PTR(-ENOMEM);
	INIT_DELAYED_WORK(&port->state_machine, tcpm_state_machine_work);

	init_completion(&port->tx_complete);
	init_completion(&port->swap_complete);

	port->typec_caps.type = port->tcpc->config->port_type;
	port->typec_caps.usb_pd = 1;
	port->typec_caps.dr_swap = tcpm_dr_swap;
	port->typec_caps.pr_swap = tcpm_pr_swap;

	/*
	 * TODO:
	 *  - alt_modes, set_alt_mode
	 *  - {debug,audio}_accessory
	 */

	port->typec_port = typec_register_port(port->dev, &port->typec_caps);
	if (IS_ERR(port->typec_port)) {
		err = PTR_ERR(port->typec_port);
		goto out_destroy_wq;
	}

	tcpm_init(port);

	return port;

out_destroy_wq:
	destroy_workqueue(port->wq);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(tcpm_register_port);

void tcpm_unregister_port(struct tcpm_port *port)
{
	typec_unregister_port(port->typec_port);
	destroy_workqueue(port->wq);
}
EXPORT_SYMBOL_GPL(tcpm_unregister_port);
