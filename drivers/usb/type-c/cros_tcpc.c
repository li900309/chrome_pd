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
 * USB Type-C Port Controller Interface.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/cros_ec.h>
#include <linux/mfd/cros_ec_commands.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec.h>

#include "tcpci.h"
#include "tcpm.h"

#define PD_RETRY_COUNT 3

/* hard-coded for our TCPC port 0 */
#define TCPC_ADDR 0x4e

#define EC_MAX_IN_SIZE 128
#define EC_MAX_OUT_SIZE 128
static uint8_t ec_inbuf[EC_MAX_IN_SIZE];
static uint8_t ec_outbuf[EC_MAX_OUT_SIZE];

struct cros_tcpc {
	struct device *dev;
	struct cros_ec_device *ec_device;

	struct tcpm_port *port;

	bool controls_vbus;

	struct tcpc_dev tcpc;
};

static inline struct cros_tcpc *tcpc_to_cros(struct tcpc_dev *tcpc)
{
	return container_of(tcpc, struct cros_tcpc, tcpc);
}

static int cros_tcpc_read(struct cros_tcpc *cros_tcpc, unsigned int reg,
			  void *val, unsigned int len)
{
	struct cros_ec_command msg = {0};
	struct ec_params_i2c_passthru *p =
		(struct ec_params_i2c_passthru *)ec_outbuf;
	struct ec_response_i2c_passthru *r =
		(struct ec_response_i2c_passthru *)ec_inbuf;
	struct ec_params_i2c_passthru_msg *i2c_msg = p->msg;
	uint8_t *pdata;
	int size, err;

	p->num_msgs = 2;
	p->port = 2; /* i2c port */

	size = sizeof(*p) + p->num_msgs * sizeof(*i2c_msg);
	pdata = (uint8_t *)p + size;

	/* Write register to read */
	i2c_msg->addr_flags = TCPC_ADDR;
	i2c_msg->len = 1;
	pdata[0] = reg;

	/* Read data */
	i2c_msg++;
	i2c_msg->addr_flags = TCPC_ADDR | EC_I2C_FLAG_READ;
	i2c_msg->len = len;

	msg.command = EC_CMD_I2C_PASSTHRU;
	msg.outdata = (uint8_t *)p;
	msg.outsize = size + 1;
	msg.indata = (uint8_t *)r;
	msg.insize = sizeof(*r) + len;

	err = cros_ec_cmd_xfer_status(cros_tcpc->ec_device, &msg);
	if (err < 0) {
		dev_info(cros_tcpc->dev, "HC returned error %d", err);
		return -1;
	}

	if (r->i2c_status & (EC_I2C_STATUS_NAK | EC_I2C_STATUS_TIMEOUT)) {
		dev_info(cros_tcpc->dev, "i2c error %d", r->i2c_status);
		return -1;
	}

	memcpy(val, &(r->data[0]), len);

	return 0;
}

static int cros_tcpc_write_raw(struct cros_tcpc *cros_tcpc, unsigned int reg,
			       const void *val, unsigned int len)
{
	struct cros_ec_command msg = {0};
	struct ec_params_i2c_passthru *p =
		(struct ec_params_i2c_passthru *)ec_outbuf;
	struct ec_response_i2c_passthru *r =
		(struct ec_response_i2c_passthru *)ec_inbuf;
	struct ec_params_i2c_passthru_msg *i2c_msg = p->msg;
	uint8_t *pdata;
	int size, err;

	p->num_msgs = 1;
	p->port = 2; /* i2c port */

	size = sizeof(*p) + p->num_msgs * sizeof(*i2c_msg);
	pdata = (uint8_t *)p + size;

	/* Write register to read */
	i2c_msg->addr_flags = TCPC_ADDR;
	i2c_msg->len = len + 1;
	pdata[0] = reg;

	/* Write data */
	memcpy(&pdata[1], val, len);

	msg.command = EC_CMD_I2C_PASSTHRU;
	msg.outdata = (uint8_t *)p;
	msg.outsize = size + 1 + len;
	msg.indata = (uint8_t *)r;
	msg.insize = sizeof(*r);

	err = cros_ec_cmd_xfer_status(cros_tcpc->ec_device, &msg);
	if (err < 0) {
		dev_info(cros_tcpc->dev, "HC returned error %d", err);
		return -1;
	}

	if (r->i2c_status & (EC_I2C_STATUS_NAK | EC_I2C_STATUS_TIMEOUT)) {
		dev_info(cros_tcpc->dev, "i2c error %d", r->i2c_status);
		return -1;
	}

	return 0;
}

static int cros_tcpc_write(struct cros_tcpc *cros_tcpc, unsigned int reg,
			   unsigned int val, unsigned int len)
{
	return cros_tcpc_write_raw(cros_tcpc, reg, &val, len);
}

static int cros_tcpc_set_cc(struct tcpc_dev *tcpc, enum typec_cc_status cc)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	unsigned int reg;
	int ret;

	switch (cc) {
	case TYPEC_CC_RA:
		reg = (TCPC_ROLE_CTRL_CC_RA << TCPC_ROLE_CTRL_CC1_SHIFT) |
			(TCPC_ROLE_CTRL_CC_RA << TCPC_ROLE_CTRL_CC2_SHIFT);
		break;
	case TYPEC_CC_RD:
		reg = (TCPC_ROLE_CTRL_CC_RD << TCPC_ROLE_CTRL_CC1_SHIFT) |
			(TCPC_ROLE_CTRL_CC_RD << TCPC_ROLE_CTRL_CC2_SHIFT);
		break;
	case TYPEC_CC_RP_DEF:
		reg = (TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC1_SHIFT) |
			(TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC2_SHIFT) |
			(TCPC_ROLE_CTRL_RP_VAL_DEF <<
			 TCPC_ROLE_CTRL_RP_VAL_SHIFT);
		break;
	case TYPEC_CC_RP_1_5:
		reg = (TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC1_SHIFT) |
			(TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC2_SHIFT) |
			(TCPC_ROLE_CTRL_RP_VAL_1_5 <<
			 TCPC_ROLE_CTRL_RP_VAL_SHIFT);
		break;
	case TYPEC_CC_RP_3_0:
		reg = (TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC1_SHIFT) |
			(TCPC_ROLE_CTRL_CC_RP << TCPC_ROLE_CTRL_CC2_SHIFT) |
			(TCPC_ROLE_CTRL_RP_VAL_1_5 <<
			 TCPC_ROLE_CTRL_RP_VAL_SHIFT);
		break;
	case TYPEC_CC_OPEN:
	default:
		reg = (TCPC_ROLE_CTRL_CC_OPEN << TCPC_ROLE_CTRL_CC1_SHIFT) |
			(TCPC_ROLE_CTRL_CC_OPEN << TCPC_ROLE_CTRL_CC2_SHIFT);
		break;
	}

	ret = cros_tcpc_write(cros_tcpc, TCPC_ROLE_CTRL, reg, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int cros_tcpc_set_polarity(struct tcpc_dev *tcpc,
			      enum typec_cc_polarity polarity)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	int ret;

	ret = cros_tcpc_write(cros_tcpc, TCPC_TCPC_CTRL,
			      (polarity == TYPEC_POLARITY_CC2) ?
			      TCPC_TCPC_CTRL_ORIENTATION : 0, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int cros_tcpc_set_vconn(struct tcpc_dev *tcpc, bool enable)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	int ret;

	ret = cros_tcpc_write(cros_tcpc, TCPC_POWER_CTRL,
			      enable ? TCPC_POWER_CTRL_VCONN_ENABLE : 0, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int cros_tcpc_set_pd_header(struct tcpc_dev *tcpc, enum typec_pwr_role pwr,
			       enum typec_data_role data)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	unsigned int reg;
	int ret;

	reg = PD_REV20 << TCPC_MSG_HDR_INFO_REV_SHIFT;
	if (pwr == TYPEC_PWR_SOURCE)
		reg |= TCPC_MSG_HDR_INFO_PWR_ROLE;
	if (data == TYPEC_HOST)
		reg |= TCPC_MSG_HDR_INFO_DATA_ROLE;
	ret = cros_tcpc_write(cros_tcpc, TCPC_MSG_HDR_INFO, reg, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int cros_tcpc_set_pd_rx(struct tcpc_dev *tcpc, bool enable)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	unsigned int reg = 0;
	int ret;

	if (enable)
		reg = TCPC_RX_DETECT_SOP | TCPC_RX_DETECT_HARD_RESET;
	ret = cros_tcpc_write(cros_tcpc, TCPC_RX_DETECT, reg, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int cros_tcpc_get_vbus(struct tcpc_dev *tcpc)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	unsigned int reg;
	int ret;

	ret = cros_tcpc_read(cros_tcpc, TCPC_POWER_STATUS, &reg, 1);
	if (ret < 0)
		return ret;

	return !!(reg & TCPC_POWER_STATUS_VBUS_PRES);
}

static int cros_tcpc_pd_transmit(struct tcpc_dev *tcpc,
			     enum tcpm_transmit_type type,
			     const struct pd_message *msg)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	unsigned int reg, cnt, header;
	int ret;

	cnt = msg ? pd_header_cnt(msg->header) * 4 : 0;
	ret = cros_tcpc_write(cros_tcpc, TCPC_TX_BYTE_CNT, cnt, 1);
	if (ret < 0)
		return ret;

	header = msg ? msg->header : 0;
	ret = cros_tcpc_write(cros_tcpc, TCPC_TX_HDR, header, 2);
	if (ret < 0)
		return ret;

	if (cnt > 0) {
		ret = cros_tcpc_write_raw(cros_tcpc, TCPC_TX_DATA,
					  &msg->payload, cnt);
		if (ret < 0)
			return ret;
	}

	reg = (PD_RETRY_COUNT << TCPC_TRANSMIT_RETRY_SHIFT) |
		(type << TCPC_TRANSMIT_TYPE_SHIFT);
	ret = cros_tcpc_write(cros_tcpc, TCPC_TRANSMIT, reg, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int cros_tcpc_init(struct tcpc_dev *tcpc)
{
	struct cros_tcpc *cros_tcpc = tcpc_to_cros(tcpc);
	unsigned long timeout = jiffies + msecs_to_jiffies(2000); /* XXX */
	unsigned int reg;
	int ret;

	while (time_before_eq(jiffies, timeout)) {
		ret = cros_tcpc_read(cros_tcpc, TCPC_POWER_STATUS, &reg, 1);
		if (ret < 0)
			return ret;
		if (!(reg & TCPC_POWER_STATUS_UNINIT))
			break;
		usleep_range(10000, 20000);
	}
	if (time_after(jiffies, timeout))
		return -ETIMEDOUT;

	/* Clear all events */
	ret = cros_tcpc_write(cros_tcpc, TCPC_ALERT, 0xffff, 2);
	if (ret < 0)
		return ret;

	if (cros_tcpc->controls_vbus)
		reg = TCPC_POWER_STATUS_VBUS_PRES;
	else
		reg = 0;
	ret = cros_tcpc_write(cros_tcpc, TCPC_POWER_STATUS_MASK, reg, 1);
	if (ret < 0)
		return ret;

	reg = TCPC_ALERT_TX_SUCCESS | TCPC_ALERT_TX_FAILED |
		TCPC_ALERT_TX_DISCARDED | TCPC_ALERT_RX_STATUS |
		TCPC_ALERT_RX_HARD_RST | TCPC_ALERT_CC_STATUS;
	if (cros_tcpc->controls_vbus)
		reg |= TCPC_ALERT_POWER_STATUS;
	return cros_tcpc_write(cros_tcpc, TCPC_ALERT_MASK, reg, 2);
}

static enum typec_cc_status tcpci_to_typec_cc(unsigned int cc, bool sink)
{
	switch (cc) {
	case 0x1:
		return sink ? TYPEC_CC_RP_DEF : TYPEC_CC_RA;
	case 0x2:
		return sink ? TYPEC_CC_RP_1_5 : TYPEC_CC_RD;
	case 0x3:
		if (sink)
			return TYPEC_CC_RP_3_0;
	case 0x0:
	default:
		return TYPEC_CC_OPEN;
	}
}

static irqreturn_t cros_tcpc_alert(int irq, void *dev_id)
{
	struct cros_tcpc *cros_tcpc = dev_id;
	unsigned int status, reg;

	cros_tcpc_read(cros_tcpc, TCPC_ALERT, &status, 2);

	/*
	 * Clear alert status for everything except RX_STATUS, which shouldn't
	 * be cleared until we have successfully retrieved message.
	 */
	if (status & ~TCPC_ALERT_RX_STATUS)
		cros_tcpc_write(cros_tcpc, TCPC_ALERT,
				status & ~TCPC_ALERT_RX_STATUS, 2);

	if (status & TCPC_ALERT_CC_STATUS) {
		enum typec_cc_status cc1, cc2;

		cros_tcpc_read(cros_tcpc, TCPC_CC_STATUS, &reg, 1);

		cc1 = tcpci_to_typec_cc((reg >> TCPC_CC_STATUS_CC1_SHIFT) &
					TCPC_CC_STATUS_CC1_MASK,
					reg & TCPC_CC_STATUS_TERM);	
		cc2 = tcpci_to_typec_cc((reg >> TCPC_CC_STATUS_CC2_SHIFT) &
					TCPC_CC_STATUS_CC2_MASK,
					reg & TCPC_CC_STATUS_TERM);

		tcpm_cc_change(cros_tcpc->port, cc1, cc2);
	}

	if (status & TCPC_ALERT_POWER_STATUS) {
		cros_tcpc_read(cros_tcpc, TCPC_POWER_STATUS_MASK, &reg, 1);

		if (reg == 0xff) {
			/*
			 * If power status mask has been reset, then the TCPC
			 * has reset.
			 */
			tcpm_tcpc_reset(cros_tcpc->port);
		} else {
			cros_tcpc_read(cros_tcpc, TCPC_POWER_STATUS, &reg, 1);
			if (reg & TCPC_POWER_STATUS_VBUS_PRES)
				tcpm_vbus_on(cros_tcpc->port);
			else
				tcpm_vbus_off(cros_tcpc->port);
		}
	}

	if (status & TCPC_ALERT_RX_STATUS) {
		struct pd_message msg;
		unsigned int cnt = 0;

		memset(&msg, 0, sizeof(msg));

		cros_tcpc_read(cros_tcpc, TCPC_RX_BYTE_CNT, &cnt, 1);

		cros_tcpc_read(cros_tcpc, TCPC_RX_HDR, &reg, 2);
		msg.header = reg;

		if (WARN_ON(cnt > sizeof(msg.payload)))
			cnt = sizeof(msg.payload);

		if (cnt > 0)
			cros_tcpc_read(cros_tcpc, TCPC_RX_DATA, &msg.payload,
				       cnt);

		/* Read complete, clear RX status alert bit */
		cros_tcpc_write(cros_tcpc, TCPC_ALERT, TCPC_ALERT_RX_STATUS, 2);

		tcpm_pd_receive(cros_tcpc->port, &msg);
	}

	if (status & TCPC_ALERT_RX_HARD_RST)
		tcpm_pd_hard_reset(cros_tcpc->port);

	if (status & TCPC_ALERT_TX_SUCCESS)
		tcpm_pd_transmit_complete(cros_tcpc->port, TCPC_TX_SUCCESS);
	else if (status & TCPC_ALERT_TX_DISCARDED)
		tcpm_pd_transmit_complete(cros_tcpc->port, TCPC_TX_DISCARDED);
	else if (status & TCPC_ALERT_TX_FAILED)
		tcpm_pd_transmit_complete(cros_tcpc->port, TCPC_TX_FAILED);

	return IRQ_HANDLED;
}

void cros_ec_tcpci_notify(int event, void *dev_id)
{
	cros_tcpc_alert(0, dev_id);
}
EXPORT_SYMBOL_GPL(cros_ec_tcpci_notify);

extern void cros_ec_pd_update_register_tcpci(void *dev_id);

#define PDO_FIXED_FLAGS (PDO_FIXED_DUAL_ROLE | PDO_FIXED_DATA_SWAP |\
			 PDO_FIXED_USB_COMM)

static const u32 board_src_pdo[] = {
		PDO_FIXED(5000, 1500, PDO_FIXED_FLAGS),
};

static const u32 board_snk_pdo[] = {
		PDO_FIXED(5000, 500, PDO_FIXED_FLAGS),
		PDO_BATT(4500, 14000, 10000),
		PDO_VAR(4500, 14000, 3000),
};

static const struct tcpc_config board_config = {
	.src_pdo = board_src_pdo,
	.nr_src_pdo = ARRAY_SIZE(board_src_pdo),
	.snk_pdo = board_src_pdo,
	.nr_snk_pdo = ARRAY_SIZE(board_snk_pdo),

	.max_snk_mv = 12000,
	.max_snk_ma = 3000,
	.max_snk_mw = 24000,
	.operating_snk_mw = 10000,

	.port_type = TYPEC_PORT_DRP,
	.default_role = TYPEC_PWR_SINK,
};

static int cros_tcpc_parse_config(struct cros_tcpc *cros_tcpc)
{
	cros_tcpc->controls_vbus = true; /* XXX */

	/* TODO: Populate struct tcpc_config from ACPI/device-tree */
	cros_tcpc->tcpc.config = &board_config;

	return 0;
}

static int cros_tcpc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(pdev->dev.parent);
	struct cros_tcpc *cros_tcpc;
	int err;
	u32 host_events;

	cros_tcpc = devm_kzalloc(dev, sizeof(*cros_tcpc), GFP_KERNEL);
	if (!cros_tcpc)
		return -ENOMEM;

	dev_info(dev, "cros_tcpc: probe\n");

	platform_set_drvdata(pdev, cros_tcpc);
	cros_tcpc->ec_device = ec_dev->ec_dev;
	cros_tcpc->dev = dev;

	cros_tcpc->tcpc.init = cros_tcpc_init;
	cros_tcpc->tcpc.get_vbus = cros_tcpc_get_vbus;
	cros_tcpc->tcpc.set_cc = cros_tcpc_set_cc;
	cros_tcpc->tcpc.set_polarity = cros_tcpc_set_polarity;
	cros_tcpc->tcpc.set_vconn = cros_tcpc_set_vconn;
	cros_tcpc->tcpc.set_pd_rx = cros_tcpc_set_pd_rx;
	cros_tcpc->tcpc.set_pd_header = cros_tcpc_set_pd_header;
	cros_tcpc->tcpc.pd_transmit = cros_tcpc_pd_transmit;

	err = cros_tcpc_parse_config(cros_tcpc);
	if (err < 0)
		return err;

	cros_tcpc->port = tcpm_register_port(cros_tcpc->dev, &cros_tcpc->tcpc);
	if (IS_ERR(cros_tcpc->port))
		return PTR_ERR(cros_tcpc->port);

	/*
	 * TODO: wire up ACPI so we don't have to use existing PD update
	 * host event to get alert messages.
	 */
	cros_ec_pd_update_register_tcpci(cros_tcpc);
	cros_tcpc->ec_device->cmd_read_u32(cros_tcpc->ec_device, 0x34,
					   &host_events);

	return 0;
}

static int cros_tcpc_remove(struct platform_device *pd)
{
	struct cros_tcpc *cros_tcpc = platform_get_drvdata(pd);

	tcpm_unregister_port(cros_tcpc->port);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id cros_tcpc_of_match[] = {
	{ .compatible = "google,cros-ec-tcpc", },
	{},
};
MODULE_DEVICE_TABLE(of, cros_tcpc_of_match);
#endif

static struct platform_driver cros_tcpc_driver = {
	.driver = {
		.name = "cros-ec-tcpc",
		.of_match_table = of_match_ptr(cros_tcpc_of_match),
	},
	.probe = cros_tcpc_probe,
	.remove = cros_tcpc_remove,
};
module_platform_driver(cros_tcpc_driver);

MODULE_DESCRIPTION("Chrome EC USB Type-C Port Controller driver");
MODULE_LICENSE("GPL");
