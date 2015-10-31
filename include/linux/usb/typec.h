
#ifndef __LINUX_USB_TYPEC_H
#define __LINUX_USB_TYPEC_H

#include <linux/types.h>

enum typec_port_type {
	TYPEC_PORT_DFP,
	TYPEC_PORT_UFP,
	TYPEC_PORT_DRP,
};

enum typec_data_role {
	TYPEC_DEVICE,
	TYPEC_HOST,
};

enum typec_pwr_role {
	TYPEC_PWR_SINK,
	TYPEC_PWR_SOURCE,
};

enum typec_pwr_opmode {
	TYPEC_PWR_MODE_USB,
	TYPEC_PWR_MODE_BC1_2,
	TYPEC_PWR_MODE_1_5A,
	TYPEC_PWR_MODE_3_0A,
	TYPEC_PWR_MODE_PD,
};

enum typec_partner_type {
	TYPEC_PARTNER_NONE,
	TYPEC_PARTNER_USB,
	TYPEC_PARTNER_CHARGER,
	TYPEC_PARTNER_ALTMODE,
	TYPEC_PARTNER_AUDIO,
	TYPEC_PARTNER_DEBUG,
};

enum typec_cc_status {
	TYPEC_CC_OPEN,
	TYPEC_CC_RA,
	TYPEC_CC_RD,
	TYPEC_CC_RP_DEF,
	TYPEC_CC_RP_1_5,
	TYPEC_CC_RP_3_0,
};

enum typec_cc_polarity {
	TYPEC_POLARITY_CC1,
	TYPEC_POLARITY_CC2,
};

#define TYPEC_T_CC_DEBOUNCE	100	/* Between 100ms and 200ms */
#define TYPEC_T_PD_DEBOUNCE	10	/* Between 10ms and 20ms */

struct typec_alt_mode {
	u16 svid;
	u32 mid;
};

struct typec_port;

/*
 * struct typec_capability - USB Type-C Port Capabilities
 * @type: DFP (Host-only), UFP (Device-only) or DRP (Dual Role)
 * @usb_pd: USB Power Delivery support
 * @alt_modes: Alternate Modes the connector supports (null terminated)
 * @audio_accessory: Audio Accessory Adapter Mode support
 * @debug_accessory: Debug Accessory Mode support
 * @dr_swap: Data Role Swap support
 * @pr_swap: Power Role Swap support
 * @set_alt_mode: Enter given Alternate Mode
 *
 * Static capabilities of a single USB Type-C port.
 */
struct typec_capability {
	enum typec_port_type	type;
	unsigned int		usb_pd:1;
	struct typec_alt_mode	*alt_modes;
	unsigned int		audio_accessory:1;
	unsigned int		debug_accessory:1;

	int			(*dr_swap)(struct typec_port *);
	int			(*pr_swap)(struct typec_port *);
	int			(*set_alt_mode)(struct typec_port *,
						struct typec_alt_mode *);
};

/*
 * struct typec_port - USB Type-C Port
 * @id: port index
 * @dev: struct device instance
 * @lock: Lock to protect concurrent access
 * @data_role: Current USB role - Host or Device
 * @pwr_role: Current Power role - Source or Sink
 * @pwr_opmode: The power level in use at the moment
 * @cur_alt_mode: The Alternate Mode currently in use
 * @connected: Connection status
 * @partner_type: Port type of the partner
 * @partner_alt_modes: Alternate Modes the partner supports (null terminated)
 * @cap: Port Capabilities
 *
 * Current status of a USB Type-C port and relevant partner details when
 * connected.
 */
struct typec_port {
	unsigned int		id;
	struct device		dev;
	struct mutex		lock;

	enum typec_data_role	data_role;
	enum typec_pwr_role	pwr_role;
	enum typec_pwr_opmode	pwr_opmode;
	struct typec_alt_mode	*cur_alt_mode;

	unsigned char		connected;
	enum typec_partner_type	partner_type;
	struct typec_alt_mode	*partner_alt_modes;

	const struct typec_capability *cap;
};

struct typec_port *typec_register_port(struct device *dev,
				       struct typec_capability *cap);
void typec_unregister_port(struct typec_port *port);

int typec_connect(struct typec_port *port);
void typec_disconnect(struct typec_port *port);

#endif /* __LINUX_USB_TYPEC_H */
