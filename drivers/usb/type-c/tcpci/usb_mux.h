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
 * USB Type-C port data lanes mux driver.
 */

enum usb_switch {
	USB_SWITCH_CONNECT,
	USB_SWITCH_DISCONNECT,
	USB_SWITCH_RESTORE, /* TODO FIXME */
};

/* Mux state attributes */
#define MUX_USB_ENABLED        (1 << 0) /* USB is enabled */
#define MUX_DP_ENABLED         (1 << 1) /* DP is enabled */
#define MUX_POLARITY_INVERTED  (1 << 2) /* Polarity is inverted */

/* Mux modes, decoded to attributes */
enum typec_mux {
	TYPEC_MUX_NONE = 0,                /* Open switch */
	TYPEC_MUX_USB  = MUX_USB_ENABLED,  /* USB only */
	TYPEC_MUX_DP   = MUX_DP_ENABLED,   /* DP only */
	TYPEC_MUX_DOCK = MUX_USB_ENABLED | /* Both USB and DP */
			 MUX_DP_ENABLED,
};

struct usb_mux_device {
	int (*set)(struct usb_mux_device *dev, enum typec_mux mux_mode,
		   enum usb_switch usb_config, int polarity);

	bool dfp_only;

	void *priv_data;
};
