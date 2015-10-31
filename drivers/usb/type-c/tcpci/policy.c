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
 * USB Power Delivery policy layer.
 */

#include <linux/bug.h>
#include <linux/device.h>
#include <linux/kernel.h>

#include "usb_pd.h"

int pd_check_requested_voltage(const struct pd_supply_cfg *cfg, u32 rdo)
{
	int max_ma = rdo & 0x3FF;
	int op_ma = (rdo >> 10) & 0x3FF;
	int idx = RDO_POS(rdo);
	u32 pdo;
	u32 pdo_ma;

	/* Check for invalid index */
	if (!idx || idx > cfg->src_pdo_cnt)
		return -EINVAL;

	/* check current ... */
	pdo = cfg->src_pdo[idx - 1];
	pdo_ma = (pdo & 0x3ff);
	if (op_ma > pdo_ma)
		return -EINVAL; /* too much op current */
	if (max_ma > pdo_ma && !(rdo & RDO_CAP_MISMATCH))
		return -EINVAL; /* too much max current */

	pr_info("Requested %d V %d mA (for %d/%d mA)\n",
		 ((pdo >> 10) & 0x3ff) * 50, (pdo & 0x3ff) * 10,
		 op_ma * 10, max_ma * 10);

	/* Accept the requested voltage */
	return 0;
}

/* Cap on the max voltage requested as a sink (in millivolts) */
static unsigned max_request_mv = UINT_MAX; /* no cap */

/**
 * Find PDO index that offers the most amount of power and stays within
 * max_mv voltage.
 *
 * @param cfg  board specific configuration
 * @param cnt  the number of Power Data Objects.
 * @param src_caps Power Data Objects representing the source capabilities.
 * @param max_mv maximum voltage (or -1 if no limit)
 * @return index of PDO within source cap packet
 */
static int pd_find_pdo_index(const struct pd_supply_cfg *cfg, int cnt,
			     u32 *src_caps, unsigned max_mv)
{
	int i;
	unsigned uw, max_uw = 0, mv, ma;
	int ret = -1;
	int cur_mv;

	/* max voltage is always limited by this boards max request */
	max_mv = min(max_mv, cfg->max_voltage_mv);

	/* Get max power that is under our max voltage input */
	for (i = 0; i < cnt; i++) {
		mv = ((src_caps[i] >> 10) & 0x3FF) * 50;
		/* Skip any voltage not supported by this board */
		//if (!pd_is_valid_input_voltage(mv))
		//	continue;

		if ((src_caps[i] & PDO_TYPE_MASK) == PDO_TYPE_BATTERY) {
			uw = 250000 * (src_caps[i] & 0x3FF);
		} else {
			ma = (src_caps[i] & 0x3FF) * 10;
			ma = min(ma, cfg->max_current_ma);
			uw = ma * mv;
		}
		if (cfg->prefer_low_voltage) {
			if (mv > max_mv)
				continue;
			uw = min(uw, cfg->max_power_mw * 1000);
			if ((uw > max_uw) || ((uw == max_uw) && mv < cur_mv)) {
				ret = i;
				max_uw = uw;
				cur_mv = mv;
			}
		} else {
			if ((uw > max_uw) && (mv <= max_mv)) {
				ret = i;
				max_uw = uw;
			}
		}
	}
	return ret;
}

/**
 * Extract power information out of a Power Data Object (PDO)
 *
 * @cfg board specific configuration
 * @pdo Power data object
 * @ma Current we can request from that PDO
 * @mv Voltage of the PDO
 */
static void pd_extract_pdo_power(const struct pd_supply_cfg *cfg, u32 pdo,
				 u32 *ma, u32 *mv)
{
	unsigned max_ma, uw;
	*mv = ((pdo >> 10) & 0x3FF) * 50;

	if ((pdo & PDO_TYPE_MASK) == PDO_TYPE_BATTERY) {
		uw = 250000 * (pdo & 0x3FF);
		max_ma = 1000 * min(1000 * uw, cfg->max_power_mw) / *mv;
	} else {
		max_ma = 10 * (pdo & 0x3FF);
		max_ma = min(max_ma, cfg->max_power_mw * 1000 / (unsigned)*mv);
	}

	*ma = min(max_ma, cfg->max_current_ma);
}

int pd_build_request(const struct pd_supply_cfg *cfg, int cnt, u32 *src_caps,
		     u32 *rdo, u32 *ma, u32 *mv, enum pd_request_type req_type)
{
	int pdo_index, flags = 0;
	int uw;

	if (req_type == PD_REQUEST_VSAFE5V)
		/* src cap 0 should be vSafe5V */
		pdo_index = 0;
	else
		/* find pdo index for max voltage we can request */
		pdo_index = pd_find_pdo_index(cfg, cnt, src_caps,
					      max_request_mv);

	/* If could not find desired pdo_index, then return error */
	if (pdo_index == -1)
		return -EINVAL;

	pd_extract_pdo_power(cfg, src_caps[pdo_index], ma, mv);
	uw = *ma * *mv;
	/* Mismatch bit set if less power offered than the operating power */
	if (uw < (1000 * cfg->operating_power_mw))
		flags |= RDO_CAP_MISMATCH;

	if ((src_caps[pdo_index] & PDO_TYPE_MASK) == PDO_TYPE_BATTERY) {
		int mw = uw / 1000;
		*rdo = RDO_BATT(pdo_index + 1, mw, mw, flags);
	} else {
		*rdo = RDO_FIXED(pdo_index + 1, *ma, *ma, flags);
	}
	return 0;
}

void pd_process_source_cap(struct pd_port *port,
			   const struct pd_supply_cfg *cfg,
			   int cnt, u32 *src_caps)
{
}

void pd_set_max_voltage(unsigned mv)
{
	max_request_mv = mv;
}

unsigned pd_get_max_voltage(void)
{
	return max_request_mv;
}

int pd_charge_from_device(u16 vid, u16 pid)
{
	/* TODO: rewrite into table if we get more of these */
	/*
	 * White-list Apple charge-through accessory since it doesn't set
	 * externally powered bit, but we still need to charge from it when
	 * we are a sink.
	 */
	return (vid == USB_VID_APPLE && (pid == 0x1012 || pid == 0x1013));
}

void pd_dfp_pe_init(struct pd_policy *policy, struct device *dev,
		    struct pd_port *port)
{
	memset(policy, 0, sizeof(struct pd_policy));
	policy->dev = dev;
	policy->port = port;
}

static void dfp_consume_identity(struct pd_policy *policy, int cnt,
				 u32 *payload)
{
	int ptype = PD_IDH_PTYPE(payload[VDO_I(IDH)]);
	size_t identity_size = min(sizeof(policy->identity),
				   (cnt - 1) * sizeof(u32));
	pd_dfp_pe_init(policy, policy->dev, policy->port);
	memcpy(&policy->identity, payload + 1, identity_size);
	switch (ptype) {
	case IDH_PTYPE_AMA:
		/* TODO(tbroch) do I disable VBUS here if power contract
		 * requested it
		 */
		if (!PD_VDO_AMA_VBUS_REQ(payload[VDO_I(AMA)]))
			pd_power_supply_reset(policy->port);
		break;
		/* TODO(crosbug.com/p/30645) provide vconn support here */
	default:
		break;
	}
}

static int dfp_discover_svids(struct pd_policy *policy, u32 *payload)
{
	payload[0] = VDO(USB_SID_PD, 1, CMD_DISCOVER_SVID);
	return 1;
}

static void dfp_consume_svids(struct pd_policy *policy, u32 *payload)
{
	int i;
	u32 *ptr = payload + 1;
	u16 svid0, svid1;

	for (i = policy->svid_cnt; i < policy->svid_cnt + 12; i += 2) {
		if (i == SVID_DISCOVERY_MAX) {
			dev_warn(policy->dev, "ERR:SVIDCNT\n");
			break;
		}

		svid0 = PD_VDO_SVID_SVID0(*ptr);
		if (!svid0)
			break;
		policy->svids[i].svid = svid0;
		policy->svid_cnt++;

		svid1 = PD_VDO_SVID_SVID1(*ptr);
		if (!svid1)
			break;
		policy->svids[i + 1].svid = svid1;
		policy->svid_cnt++;
		ptr++;
	}
	/* TODO(tbroch) need to re-issue discover svids if > 12 */
	if (i && ((i % 12) == 0))
		dev_warn(policy->dev, "ERR:SVID+12\n");
}

static int dfp_discover_modes(struct pd_policy *policy, u32 *payload)
{
	u16 svid = policy->svids[policy->svid_idx].svid;
	if (policy->svid_idx >= policy->svid_cnt)
		return 0;
	payload[0] = VDO(svid, 1, CMD_DISCOVER_MODES);
	return 1;
}

static void dfp_consume_modes(struct pd_policy *policy, int cnt, u32 *payload)
{
	int idx = policy->svid_idx;
	policy->svids[idx].mode_cnt = cnt - 1;
	if (policy->svids[idx].mode_cnt < 0) {
		dev_warn(policy->dev, "ERR:NOMODE\n");
	} else {
		memcpy(policy->svids[policy->svid_idx].mode_vdo, &payload[1],
		       sizeof(u32) * policy->svids[idx].mode_cnt);
	}

	policy->svid_idx++;
}

static int get_mode_idx(struct pd_policy *policy, u16 svid)
{
	int i;

	for (i = 0; i < PD_AMODE_COUNT; i++) {
		if (policy->amodes[i].fx->svid == svid)
			return i;
	}
	return -1;
}

static struct svdm_amode_data *get_modep(struct pd_policy *policy, u16 svid)
{
	int idx = get_mode_idx(policy, svid);

	return (idx == -1) ? NULL : &policy->amodes[idx];
}

int pd_alt_mode(struct pd_policy *policy, u16 svid)
{
	struct svdm_amode_data *modep = get_modep(policy, svid);

	return (modep) ? modep->opos : -1;
}

static int allocate_mode(struct pd_policy *policy, u16 svid)
{
	int i, j;
	struct svdm_amode_data *modep;
	int mode_idx = get_mode_idx(policy, svid);

	if (mode_idx != -1)
		return mode_idx;

	/* There's no space to enter another mode */
	if (policy->amode_idx == PD_AMODE_COUNT) {
		dev_warn(policy->dev, "ERR:NO AMODE SPACE\n");
		return -1;
	}

	/* Allocate ...  if SVID == 0 enter default supported policy */
	for (i = 0; i < supported_modes_cnt; i++) {
		if (!&supported_modes[i])
			continue;

		for (j = 0; j < policy->svid_cnt; j++) {
			struct svdm_svid_data *svidp = &policy->svids[j];
			if ((svidp->svid != supported_modes[i].svid) ||
			    (svid && (svidp->svid != svid)))
				continue;

			modep = &policy->amodes[policy->amode_idx];
			modep->fx = &supported_modes[i];
			modep->data = &policy->svids[j];
			policy->amode_idx++;
			return policy->amode_idx - 1;
		}
	}
	return -1;
}

/*
 * Enter default mode ( payload[0] == 0 ) or attempt to enter mode via svid &
 * opos
*/
u32 pd_dfp_enter_mode(struct pd_policy *policy, u16 svid, int opos)
{
	int mode_idx = allocate_mode(policy, svid);
	struct svdm_amode_data *modep;
	u32 mode_caps;

	if (mode_idx == -1)
		return 0;
	modep = &policy->amodes[mode_idx];

	if (!opos) {
		/* choose the lowest as default */
		modep->opos = 1;
	} else if (opos <= modep->data->mode_cnt) {
		modep->opos = opos;
	} else {
		dev_warn(policy->dev, "opos error\n");
		return 0;
	}

	mode_caps = modep->data->mode_vdo[modep->opos - 1];
	if (modep->fx->enter(policy, mode_caps) == -1)
		return 0;

	/* SVDM to send to UFP for mode entry */
	return VDO(modep->fx->svid, 1, CMD_ENTER_MODE | VDO_OPOS(modep->opos));
}

static int validate_mode_request(struct pd_policy *policy,
				 struct svdm_amode_data *modep,
				 u16 svid, int opos)
{
	if (!modep->fx)
		return 0;

	if (svid != modep->fx->svid) {
		dev_warn(policy->dev, "ERR:svid r:0x%04x != c:0x%04x\n",
			svid, modep->fx->svid);
		return 0;
	}

	if (opos != modep->opos) {
		dev_warn(policy->dev, "ERR:opos r:%d != c:%d\n",
			opos, modep->opos);
		return 0;
	}

	return 1;
}

static void dfp_consume_attention(struct pd_policy *policy, u32 *payload)
{
	u16 svid = PD_VDO_VID(payload[0]);
	int opos = PD_VDO_OPOS(payload[0]);
	struct svdm_amode_data *modep = get_modep(policy, svid);

	if (!modep || !validate_mode_request(policy, modep, svid, opos))
		return;

	if (modep->fx->attention)
		modep->fx->attention(policy, payload);
}

/*
 * This algorithm defaults to choosing higher pin config over lower ones.  Pin
 * configs are organized in pairs with the following breakdown.
 *
 *  NAME | SIGNALING | OUTPUT TYPE | MULTI-FUNCTION | PIN CONFIG
 * -------------------------------------------------------------
 *  A    |  USB G2   |  ?          | no             | 00_0001
 *  B    |  USB G2   |  ?          | yes            | 00_0010
 *  C    |  DP       |  CONVERTED  | no             | 00_0100
 *  D    |  PD       |  CONVERTED  | yes            | 00_1000
 *  E    |  DP       |  DP         | no             | 01_0000
 *  F    |  PD       |  DP         | yes            | 10_0000
 *
 * if UFP has NOT asserted multi-function preferred code masks away B/D/F
 * leaving only A/C/E.  For single-output dongles that should leave only one
 * possible pin config depending on whether its a converter DP->(VGA|HDMI) or DP
 * output.  If someone creates a multi-output dongle presumably they would need
 * to either offer different mode capabilities depending upon connection type or
 * the DFP would need additional system policy to expose those options.
 */
int pd_dfp_dp_get_pin_mode(struct pd_policy *policy, u32 status)
{
	struct svdm_amode_data *modep = get_modep(policy, USB_SID_DISPLAYPORT);
	u32 mode_caps;
	u32 pin_caps;
	int mode;
	if (!modep)
		return 0;

	mode_caps = modep->data->mode_vdo[modep->opos - 1];

	/* TODO(crosbug.com/p/39656) revisit with DFP that can be a sink */
	pin_caps = PD_DP_PIN_CAPS(mode_caps);

	/* if don't want multi-function then ignore those pin configs */
	if (!PD_VDO_DPSTS_MF_PREF(status))
		pin_caps &= ~MODE_DP_PIN_MF_MASK;

	/* TODO(crosbug.com/p/39656) revisit if DFP drives USB Gen 2 signals */
	pin_caps &= ~MODE_DP_PIN_BR2_MASK;

	if (!pin_caps)
		return 0;

	mode = 1 << __ffs(pin_caps);
	pin_caps &= ~mode;

	return mode;
}

int pd_dfp_exit_mode(struct pd_policy *policy, u16 svid, int opos)
{
	struct svdm_amode_data *modep;
	int idx;

	/*
	 * Empty svid signals we should reset DFP VDM state by exiting all
	 * entered modes then clearing state.  This occurs when we've
	 * disconnected or for hard reset.
	 */
	if (!svid) {
		for (idx = 0; idx < PD_AMODE_COUNT; idx++)
			if (policy->amodes[idx].fx)
				policy->amodes[idx].fx->exit(policy);

		pd_dfp_pe_init(policy, policy->dev, policy->port);
		return 0;
	}

	/*
	 * TODO(crosbug.com/p/33946) : below needs revisited to allow multiple
	 * mode exit.  Additionally it should honor OPOS == 7 as DFP's request
	 * to exit all modes.  We currently don't have any UFPs that support
	 * multiple modes on one SVID.
	 */
	modep = get_modep(policy, svid);
	if (!modep || !validate_mode_request(policy, modep, svid, opos))
		return 0;

	/* call DFPs exit function */
	modep->fx->exit(policy);
	/* exit the mode */
	modep->opos = 0;
	return 1;
}

u16 pd_get_identity_vid(struct pd_policy *policy)
{
	return PD_IDH_VID(policy->identity[0]);
}

u16 pd_get_identity_pid(struct pd_policy *policy)
{
	return PD_PRODUCT_PID(policy->identity[2]);
}

int pd_svdm(struct pd_policy *policy, int cnt, u32 *payload, u32 **rpayload)
{
	int cmd = PD_VDO_CMD(payload[0]);
	int cmd_type = PD_VDO_CMDT(payload[0]);
	int (*func)(struct pd_policy *policy, u32 *payload) = NULL;

	int rsize = 1; /* VDM header at a minimum */

	payload[0] &= ~VDO_CMDT_MASK;
	*rpayload = payload;

	if (cmd_type == CMDT_INIT) {
		switch (cmd) {
		case CMD_DISCOVER_IDENT:
			func = svdm_rsp.identity;
			break;
		case CMD_DISCOVER_SVID:
			func = svdm_rsp.svids;
			break;
		case CMD_DISCOVER_MODES:
			func = svdm_rsp.modes;
			break;
		case CMD_ENTER_MODE:
			func = svdm_rsp.enter_mode;
			break;
		case CMD_DP_STATUS:
			func = svdm_rsp.amode->status;
			break;
		case CMD_DP_CONFIG:
			func = svdm_rsp.amode->config;
			break;
		case CMD_EXIT_MODE:
			func = svdm_rsp.exit_mode;
			break;
		case CMD_ATTENTION:
			/*
			 * attention is only SVDM with no response
			 * (just goodCRC) return zero here.
			 */
			dfp_consume_attention(policy, payload);
			return 0;
		default:
			dev_warn(policy->dev, "ERR:CMD:%d\n", cmd);
			rsize = 0;
		}
		if (func)
			rsize = func(policy, payload);
		else /* not supported : NACK it */
			rsize = 0;
		if (rsize >= 1)
			payload[0] |= VDO_CMDT(CMDT_RSP_ACK);
		else if (!rsize) {
			payload[0] |= VDO_CMDT(CMDT_RSP_NAK);
			rsize = 1;
		} else {
			payload[0] |= VDO_CMDT(CMDT_RSP_BUSY);
			rsize = 1;
		}
	} else if (cmd_type == CMDT_RSP_ACK) {
		struct svdm_amode_data *modep;

		modep = get_modep(policy, PD_VDO_VID(payload[0]));
		switch (cmd) {
		case CMD_DISCOVER_IDENT:
			dfp_consume_identity(policy, cnt, payload);
			rsize = dfp_discover_svids(policy, payload);
			break;
		case CMD_DISCOVER_SVID:
			dfp_consume_svids(policy, payload);
			rsize = dfp_discover_modes(policy, payload);
			break;
		case CMD_DISCOVER_MODES:
			dfp_consume_modes(policy, cnt, payload);
			rsize = dfp_discover_modes(policy, payload);
			/* enter the default mode for DFP */
			if (!rsize) {
				payload[0] = pd_dfp_enter_mode(policy, 0, 0);
				if (payload[0])
					rsize = 1;
			}
			break;
		case CMD_ENTER_MODE:
			if (!modep) {
				rsize = 0;
			} else {
				if (!modep->opos)
					pd_dfp_enter_mode(policy, 0, 0);

				if (modep->opos) {
					rsize = modep->fx->status(policy,
								  payload);
					payload[0] |= PD_VDO_OPOS(modep->opos);
				}
			}
			break;
		case CMD_DP_STATUS:
			/* DP status response & UFP's DP attention have same
			   payload */
			dfp_consume_attention(policy, payload);
			if (modep && modep->opos)
				rsize = modep->fx->config(policy, payload);
			else
				rsize = 0;
			break;
		case CMD_DP_CONFIG:
			if (modep && modep->opos && modep->fx->post_config)
				modep->fx->post_config(policy);
			/* no response after DFPs ack */
			rsize = 0;
			break;
		case CMD_EXIT_MODE:
			/* no response after DFPs ack */
			rsize = 0;
			break;
		case CMD_ATTENTION:
			/* no response after DFPs ack */
			rsize = 0;
			break;
		default:
			dev_warn(policy->dev, "ERR:CMD:%d\n", cmd);
			rsize = 0;
		}

		payload[0] |= VDO_CMDT(CMDT_INIT);
	} else if (cmd_type == CMDT_RSP_BUSY) {
		switch (cmd) {
		case CMD_DISCOVER_IDENT:
		case CMD_DISCOVER_SVID:
		case CMD_DISCOVER_MODES:
			/* resend if its discovery */
			rsize = 1;
			break;
		case CMD_ENTER_MODE:
			/* Error */
			dev_warn(policy->dev, "ERR:ENTBUSY\n");
			rsize = 0;
			break;
		case CMD_EXIT_MODE:
			rsize = 0;
			break;
		default:
			rsize = 0;
		}
	} else if (cmd_type == CMDT_RSP_NAK) {
		/* nothing to do */
		rsize = 0;
	} else {
		dev_warn(policy->dev, "ERR:CMDT:%d\n", cmd);
		/* do not answer */
		rsize = 0;
	}
	return rsize;
}

int pd_vdm(struct pd_policy *policy, int cnt, u32 *payload, u32 **rpayload)
{
	return 0;
}

void pd_get_info(u32 *info_data)
{
	/* copy first 20 bytes of RW hash */
	memset(info_data, '\0', 5 * sizeof(u32));
	info_data[5] = 0;
}

void pd_set_input_current_limit(struct pd_port *port, u32 max_ma,
				u32 supply_voltage)
{
}

void pd_transition_voltage(int idx)
{
	/* No-operation: we are always 5V */
}

int pd_set_power_supply_ready(struct pd_port *port)
{
	return 0; /* we are ready */
}

void pd_power_supply_reset(struct pd_port *port)
{
}

int pd_board_checks(void)
{
	return 0;
}

int pd_check_power_swap(struct pd_port *port)
{
	/* TODO: use battery level to decide to accept/reject power swap */
	/*
	 * Allow power swap as long as we are acting as a dual role device,
	 * otherwise assume our role is fixed (not in S0 or console command
	 * to fix our role).
	 */
	return pd_get_dual_role() == PD_DRP_TOGGLE_ON ? 1 : 0;
}

int pd_check_data_swap(struct pd_port *port, int data_role)
{
	/* Always allow data swap: we can be DFP or UFP for USB */
	return 1;
}

int pd_check_vconn_swap(struct pd_port *port)
{
	/*
	 * VCONN is provided directly by the battery(PPVAR_SYS)
	 * but use the same rules as power swap
	 */
	return pd_get_dual_role() == PD_DRP_TOGGLE_ON ? 1 : 0;
}

void pd_execute_data_swap(struct pd_port *port, int data_role)
{
}

void pd_check_pr_role(struct pd_port *port, int pr_role, int flags)
{
	/*
	 * If partner is dual-role power and dualrole toggling is on, consider
	 * if a power swap is necessary.
	 */
	if ((flags & PD_FLAGS_PARTNER_DR_POWER) &&
	    pd_get_dual_role() == PD_DRP_TOGGLE_ON) {
		/*
		 * If we are source and partner is externally powered,
		 * swap to become a sink.
		 */
		if ((flags & PD_FLAGS_PARTNER_EXTPOWER) &&
		    pr_role == PD_ROLE_SOURCE)
			pd_request_power_swap(port);
	}
}

void pd_check_dr_role(struct pd_port *port, int dr_role, int flags)
{
	/* if the partner is a DRP (e.g. laptop), try to switch to UFP */
	if ((flags & PD_FLAGS_PARTNER_DR_DATA) && dr_role == PD_ROLE_DFP)
		pd_request_data_swap(port);
}

/* ----------------- Vendor Defined Messages ------------------ */
const struct svdm_response svdm_rsp = {
	.identity = NULL,
	.svids = NULL,
	.modes = NULL,
};

int pd_custom_vdm(struct pd_policy *policy, int cnt, u32 *payload,
		  u32 **rpayload)
{
	int cmd = PD_VDO_CMD(payload[0]);
	//u16 dev_id = 0;
	//int is_rw, is_latest;

	/* make sure we have some payload */
	if (cnt == 0)
		return 0;

	switch (cmd) {
	case VDO_CMD_VERSION:
		/* guarantee last byte of payload is null character */
		*(payload + cnt - 1) = 0;
		dev_info(policy->dev, "version: %s\n", (char *)(payload+1));
		break;
	case VDO_CMD_CURRENT:
		dev_info(policy->dev, "Current: %dmA\n", payload[1]);
		break;
	case VDO_CMD_FLIP:
		//board_flip_usb_mux(port);
		break;
	}

	return 0;
}

const struct svdm_amode_fx supported_modes[] = {
};
const int supported_modes_cnt = ARRAY_SIZE(supported_modes);
