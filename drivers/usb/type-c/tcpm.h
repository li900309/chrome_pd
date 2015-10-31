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

#ifndef __LINUX_USB_TCPM_H
#define __LINUX_USB_TCPM_H

#include <linux/usb/typec.h>
#include <linux/usb/pd.h>

/* Time to wait for TCPC to complete transmit */
#define PD_T_TCPC_TX_TIMEOUT  100

enum tcpm_transmit_status {
	TCPC_TX_SUCCESS = 0,
	TCPC_TX_DISCARDED = 1,
	TCPC_TX_FAILED = 2,
};

enum tcpm_transmit_type {
	TCPC_TX_SOP = 0,
	TCPC_TX_SOP_PRIME = 1,
	TCPC_TX_SOP_PRIME_PRIME = 2,
	TCPC_TX_SOP_DEBUG_PRIME = 3,
	TCPC_TX_SOP_DEBUG_PRIME_PRIME = 4,
	TCPC_TX_HARD_RESET = 5,
	TCPC_TX_CABLE_RESET = 6,
	TCPC_TX_BIST_MODE_2 = 7
};

struct tcpc_config {
	const u32 *src_pdo;
	unsigned int nr_src_pdo;

	const u32 *snk_pdo;
	unsigned int nr_snk_pdo;

	unsigned int max_snk_mv;
	unsigned int max_snk_ma;
	unsigned int max_snk_mw;
	unsigned int operating_snk_mw;

	enum typec_port_type port_type;
	enum typec_pwr_role default_role;
};

struct tcpc_dev {
	const struct tcpc_config *config;

	int (*init)(struct tcpc_dev *dev);
	int (*get_vbus)(struct tcpc_dev *dev);
	int (*set_cc)(struct tcpc_dev *dev, enum typec_cc_status cc);
	int (*set_polarity)(struct tcpc_dev *dev,
			    enum typec_cc_polarity polarity);
	int (*set_vconn)(struct tcpc_dev *dev, bool on);
	int (*set_pd_rx)(struct tcpc_dev *dev, bool on);
	int (*set_pd_header)(struct tcpc_dev *dev, enum typec_pwr_role pwr,
			     enum typec_data_role data);
	int (*pd_transmit)(struct tcpc_dev *dev, enum tcpm_transmit_type type,
			   const struct pd_message *msg);
};

struct tcpm_port;

extern struct tcpm_port *tcpm_register_port(struct device *dev,
					    struct tcpc_dev *tcpc);
extern void tcpm_unregister_port(struct tcpm_port *port);

extern void tcpm_vbus_on(struct tcpm_port *port);
extern void tcpm_vbus_off(struct tcpm_port *port);
extern void tcpm_cc_change(struct tcpm_port *port, enum typec_cc_status cc1,
			   enum typec_cc_status cc2);
extern void tcpm_pd_receive(struct tcpm_port *port,
			    const struct pd_message *msg);
extern void tcpm_pd_transmit_complete(struct tcpm_port *port,
				      enum tcpm_transmit_status status);
extern void tcpm_pd_hard_reset(struct tcpm_port *port);
extern void tcpm_tcpc_reset(struct tcpm_port *port);

#endif /* __LINUX_USB_TCPM_H */
