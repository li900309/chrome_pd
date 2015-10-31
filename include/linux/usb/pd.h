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
 */

#ifndef __LINUX_USB_PD_H
#define __LINUX_USB_PD_H

#include <linux/types.h>
#include <linux/usb/typec.h>

/* USB PD Messages */
enum pd_ctrl_msg_type {
	/* 0 Reserved */
	PD_CTRL_GOOD_CRC = 1,
	PD_CTRL_GOTO_MIN = 2,
	PD_CTRL_ACCEPT = 3,
	PD_CTRL_REJECT = 4,
	PD_CTRL_PING = 5,
	PD_CTRL_PS_RDY = 6,
	PD_CTRL_GET_SOURCE_CAP = 7,
	PD_CTRL_GET_SINK_CAP = 8,
	PD_CTRL_DR_SWAP = 9,
	PD_CTRL_PR_SWAP = 10,
	PD_CTRL_VCONN_SWAP = 11,
	PD_CTRL_WAIT = 12,
	PD_CTRL_SOFT_RESET = 13,
	/* 14-15 Reserved */
};

enum pd_data_msg_type {
	/* 0 Reserved */
	PD_DATA_SOURCE_CAP = 1,
	PD_DATA_REQUEST = 2,
	PD_DATA_BIST = 3,
	PD_DATA_SINK_CAP = 4,
	/* 5-14 Reserved */
	PD_DATA_VENDOR_DEF = 15,
};

#define PD_REV10	0x0
#define PD_REV20	0x1

#define PD_HEADER_CNT_SHIFT	12
#define PD_HEADER_CNT_MASK	0x7
#define PD_HEADER_ID_SHIFT	9
#define PD_HEADER_ID_MASK	0x7
#define PD_HEADER_PWR_ROLE	BIT(8)
#define PD_HEADER_REV_SHIFT	6
#define PD_HEADER_REV_MASK	0x3
#define PD_HEADER_DATA_ROLE	BIT(5)
#define PD_HEADER_TYPE_SHIFT	0
#define PD_HEADER_TYPE_MASK	0xf

#define PD_HEADER(type, pwr, data, id, cnt)				\
	((((type) & PD_HEADER_TYPE_MASK) << PD_HEADER_TYPE_SHIFT) |	\
	 ((pwr) == TYPEC_PWR_SOURCE ? PD_HEADER_PWR_ROLE : 0) |		\
	 ((data) == TYPEC_HOST ? PD_HEADER_DATA_ROLE : 0) |		\
	 (PD_REV20 << PD_HEADER_REV_SHIFT) |				\
	 (((id) & PD_HEADER_ID_MASK) << PD_HEADER_ID_SHIFT) |		\
	 (((cnt) & PD_HEADER_CNT_MASK) << PD_HEADER_CNT_SHIFT))

static inline unsigned int pd_header_cnt(u16 header)
{
	return (header >> PD_HEADER_CNT_SHIFT) & PD_HEADER_CNT_MASK;
}

static inline unsigned int pd_header_type(u16 header)
{
	return (header >> PD_HEADER_TYPE_SHIFT) & PD_HEADER_TYPE_MASK;
}

#define PD_MAX_PAYLOAD		7

struct pd_message {
	u16 header;
	u32 payload[PD_MAX_PAYLOAD];
};

/* PDO: Power Data Object */
#define PDO_MAX_OBJECTS		7

enum pd_pdo_type {
	PDO_TYPE_FIXED = 0,
	PDO_TYPE_BATT = 1,
	PDO_TYPE_VAR = 2,
};

#define PDO_TYPE_SHIFT		30
#define PDO_TYPE_MASK		0x3

#define PDO_FIXED_DUAL_ROLE	BIT(29)	/* Power role swap supported */
#define PDO_FIXED_SUSPEND	BIT(28) /* USB Suspend supported (Source) */
#define PDO_FIXED_HIGHER_CAP	BIT(28) /* Requires more than vSafe5V (Sink) */
#define PDO_FIXED_EXTPOWER	BIT(27) /* Externally powered */
#define PDO_FIXED_USB_COMM	BIT(26) /* USB communications capable */
#define PDO_FIXED_DATA_SWAP	BIT(25) /* Data role swap supported */
#define PDO_FIXED_VOLT_SHIFT	10	/* 50mV units */
#define PDO_FIXED_VOLT_MASK	0x3ff
#define PDO_FIXED_MAX_CURR_SHIFT	0	/* 10mA units */
#define PDO_FIXED_MAX_CURR_MASK		0x3ff

#define PDO_FIXED(mv, ma, flags)					\
	((PDO_TYPE_FIXED << PDO_TYPE_SHIFT) | (flags) |			\
	 ((((mv) / 50) & PDO_FIXED_VOLT_MASK) << PDO_FIXED_VOLT_SHIFT) | \
	 ((((ma) / 10) & PDO_FIXED_MAX_CURR_MASK) << PDO_FIXED_MAX_CURR_SHIFT))

#define PDO_BATT_MAX_VOLT_SHIFT	20	/* 50mV units */
#define PDO_BATT_MAX_VOLT_MASK	0x3ff
#define PDO_BATT_MIN_VOLT_SHIFT	10	/* 50mV units */
#define PDO_BATT_MIN_VOLT_MASK	0x3ff
#define PDO_BATT_MAX_PWR_SHIFT	0	/* 250mW units */
#define PDO_BATT_MAX_PWR_MASK	0x3ff

#define PDO_BATT(min_mv, max_mv, max_mw)				\
	((PDO_TYPE_BATT << PDO_TYPE_SHIFT) |				\
	 ((((min_mv) / 50) & PDO_BATT_MIN_VOLT_MASK) <<			\
	  PDO_BATT_MIN_VOLT_SHIFT) |					\
	 ((((max_mv) / 50) & PDO_BATT_MAX_VOLT_MASK) <<			\
	  PDO_BATT_MAX_VOLT_SHIFT) |					\
	 ((((max_mw) / 50) & PDO_BATT_MAX_PWR_MASK) <<			\
	  PDO_BATT_MAX_PWR_SHIFT))

#define PDO_VAR_MAX_VOLT_SHIFT	20	/* 50mV units */
#define PDO_VAR_MAX_VOLT_MASK	0x3ff
#define PDO_VAR_MIN_VOLT_SHIFT	10	/* 50mV units */
#define PDO_VAR_MIN_VOLT_MASK	0x3ff
#define PDO_VAR_MAX_CURR_SHIFT	0	/* 10mA units */
#define PDO_VAR_MAX_CURR_MASK	0x3ff

#define PDO_VAR(min_mv, max_mv, max_ma)				\
	((PDO_TYPE_VAR << PDO_TYPE_SHIFT) |				\
	 ((((min_mv) / 50) & PDO_VAR_MIN_VOLT_MASK) <<			\
	  PDO_VAR_MIN_VOLT_SHIFT) |					\
	 ((((max_mv) / 50) & PDO_VAR_MAX_VOLT_MASK) <<			\
	  PDO_VAR_MAX_VOLT_SHIFT) |					\
	 ((((max_ma) / 50) & PDO_VAR_MAX_CURR_MASK) <<			\
	  PDO_VAR_MAX_CURR_SHIFT))

static inline enum pd_pdo_type pdo_type(u32 pdo)
{
	return (pdo >> PDO_TYPE_SHIFT) & PDO_TYPE_MASK;
}

static inline unsigned int pdo_fixed_voltage(u32 pdo)
{
	return ((pdo >> PDO_FIXED_VOLT_SHIFT) & PDO_FIXED_VOLT_MASK) * 50;
}

static inline unsigned int pdo_min_voltage(u32 pdo)
{
	return ((pdo >> PDO_VAR_MIN_VOLT_SHIFT) & PDO_VAR_MIN_VOLT_MASK) * 50;
}

static inline unsigned int pdo_max_voltage(u32 pdo)
{
	return ((pdo >> PDO_VAR_MAX_VOLT_SHIFT) & PDO_VAR_MAX_VOLT_MASK) * 50;
}

static inline unsigned int pdo_max_current(u32 pdo)
{
	return ((pdo >> PDO_VAR_MAX_CURR_SHIFT) & PDO_VAR_MAX_CURR_MASK) * 10;
}

static inline unsigned int pdo_max_power(u32 pdo)
{
	return ((pdo >> PDO_BATT_MAX_PWR_SHIFT) & PDO_BATT_MAX_PWR_MASK) * 250;
}

/* RDO: Request Data Object */
#define RDO_OBJ_POS_SHIFT	28
#define RDO_OBJ_POS_MASK	0x7
#define RDO_GIVE_BACK		BIT(27)	/* Supports reduced operating current */
#define RDO_CAP_MISMATCH	BIT(26) /* Not satisfied by source capabilities */
#define ROD_USB_COMM		BIT(25) /* USB communications capable */
#define RDO_NO_SUSPEND		BIT(24) /* USB Suspend not supported */ 

#define RDO_FIXED_OP_CURR_SHIFT		10	/* 10mA units */
#define RDO_FIXED_OP_CURR_MASK		0x3ff
#define RDO_FIXED_MAX_CURR_SHIFT	10	/* 10mA units */
#define RDO_FIXED_MAX_CURR_MASK		0x3ff

#define RDO_FIXED(idx, op_ma, max_ma, flags)				\
	((((idx) & RDO_OBJ_POS_MASK) << RDO_OBJ_POS_SHIFT) | (flags) |	\
	 ((((op_ma) / 10) & RDO_FIXED_OP_CURR_MASK) <<			\
	  RDO_FIXED_OP_CURR_SHIFT) |					\
	 ((((max_ma) / 10) & RDO_FIXED_MAX_CURR_MASK) <<		\
	  RDO_FIXED_MAX_CURR_SHIFT))

#define RDO_BATT_OP_PWR_SHIFT		10	/* 250mW units */
#define RDO_BATT_OP_PWR_MASK		0x3ff
#define RDO_BATT_MAX_PWR_SHIFT		0	/* 250mW units */
#define RDO_BATT_MAX_PWR_MASK		0x3ff

#define RDO_BATT(idx, op_mw, max_mw, flags)				\
	((((idx) & RDO_OBJ_POS_MASK) << RDO_OBJ_POS_SHIFT) | (flags) |	\
	 ((((op_mw) / 250) & RDO_BATT_OP_PWR_MASK) <<			\
	  RDO_BATT_OP_PWR_SHIFT) |					\
	 ((((max_mw) / 250) & RDO_BATT_MAX_PWR_MASK) <<			\
	  RDO_BATT_MAX_PWR_SHIFT))

static inline unsigned int rdo_index(u32 rdo)
{
	return (rdo >> RDO_OBJ_POS_SHIFT) & RDO_OBJ_POS_MASK;
}

static inline unsigned int rdo_op_current(u32 rdo)
{
	return ((rdo >> RDO_FIXED_OP_CURR_SHIFT) & RDO_FIXED_OP_CURR_MASK) * 10;
}

static inline unsigned int rdo_max_current(u32 rdo)
{
	return ((rdo >> RDO_FIXED_MAX_CURR_SHIFT) &
		RDO_FIXED_MAX_CURR_MASK) * 10;
}

static inline unsigned int rdo_op_power(u32 rdo)
{
	return ((rdo >> RDO_BATT_OP_PWR_SHIFT) & RDO_BATT_OP_PWR_MASK) * 250;
}

static inline unsigned int rdo_max_power(u32 rdo)
{
	return ((rdo >> RDO_BATT_MAX_PWR_SHIFT) & RDO_BATT_MAX_PWR_MASK) * 250;
}

/* USB PD timers and counters */
#define PD_T_SEND_SOURCE_CAP	100
#define PD_T_SENDER_RESPONSE	30
#define PD_T_SOURCE_ACTIVITY	45
#define PD_T_SINK_ACTIVITY	135
#define PD_T_SINK_WAIT_CAP	240
#define PD_T_PS_TRANSITION	500
#define PD_T_SRC_TRANSITION	35
#define PD_T_PS_SOURCE_OFF	920
#define PD_T_PS_SOURCE_ON	480
#define PD_T_PS_HARD_RESET	15
#define PD_T_SRC_RECOVER	760
#define PD_T_SRC_RECOVER_MAX	1000
#define PD_T_SRC_TURN_ON	275
#define PD_T_SAFE_0V		650
#define PD_T_VCONN_SOURCE_ON	100
#define PD_N_CAPS_COUNT		50
#define PD_N_HARD_RESET_COUNT	2

#endif /* __LINUX_USB_PD_H */
