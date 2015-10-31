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
 * USB Power Delivery: board specific configuration.
 */

#include <linux/device.h>
#include <linux/kernel.h>

#include "usb_pd.h"

/* !!! TODO !!! REMOVE THIS TEMPORARY CRAP !!! */

#define PDO_FIXED_FLAGS (PDO_FIXED_DUAL_ROLE | PDO_FIXED_DATA_SWAP |\
			 PDO_FIXED_COMM_CAP)

/* Power Data Objects for the source and the sink */
static const u32 board_src_pdo[] = {
		PDO_FIXED(5000, 1500, PDO_FIXED_FLAGS),
};

static const u32 board_snk_pdo[] = {
		PDO_FIXED(5000, 500, PDO_FIXED_FLAGS),
		PDO_BATT(4500, 14000, 10000),
		PDO_VAR(4500, 14000, 3000),
};

/* TODO: temporary hardcoded machine specific config */
static const struct pd_supply_cfg board_cfg = {
	.turn_on_delay_ms	= 40 /* ms */,
	.turn_off_delay_ms	= 20 /* ms */,
	.vconn_swap_delay_ms	=  5 /* ms */,
	.operating_power_mw	= 10000 /* mW */,
	.max_power_mw		= 24000 /* mW */,
	.max_current_ma		=  3000 /* mA */,
	.max_voltage_mv		= 12000 /* mV */,

	.prefer_low_voltage = true,
	.use_debug_mode = true,
	.default_state = PD_STATE_SNK_DISCONNECTED,
	.debug_role = PD_ROLE_DFP,

	.src_pdo = board_src_pdo,
	.src_pdo_cnt = ARRAY_SIZE(board_src_pdo),
	.snk_pdo = board_src_pdo,
	.snk_pdo_cnt = ARRAY_SIZE(board_snk_pdo),
};

const struct pd_supply_cfg *pd_get_board_config(void)
{
	return &board_cfg;
}
