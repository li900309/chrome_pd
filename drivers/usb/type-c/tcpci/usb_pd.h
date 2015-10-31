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

#ifndef __LINUX_USB_PD_H
#define __LINUX_USB_PD_H

struct pd_port;

enum pd_rx_errors {
	PD_RX_ERR_INVAL = -1,           /* Invalid packet */
	PD_RX_ERR_HARD_RESET = -2,      /* Got a Hard-Reset packet */
	PD_RX_ERR_CRC = -3,             /* CRC mismatch */
	PD_RX_ERR_ID = -4,              /* Invalid ID number */
	PD_RX_ERR_UNSUPPORTED_SOP = -5, /* Unsupported SOP */
	PD_RX_ERR_CABLE_RESET = -6      /* Got a Cable-Reset packet */
};

/* BDO : BIST Data Object */
#define BDO_MODE_RECV       (0 << 28)
#define BDO_MODE_TRANSMIT   (1 << 28)
#define BDO_MODE_COUNTERS   (2 << 28)
#define BDO_MODE_CARRIER0   (3 << 28)
#define BDO_MODE_CARRIER1   (4 << 28)
#define BDO_MODE_CARRIER2   (5 << 28)
#define BDO_MODE_CARRIER3   (6 << 28)
#define BDO_MODE_EYE        (7 << 28)

#define BDO(mode, cnt)      ((mode) | ((cnt) & 0xFFFF))

#define SVID_DISCOVERY_MAX 16

/* Timers */
//#define PD_T_SEND_SOURCE_CAP  (100*MSEC) /* between 100ms and 200ms */
//#define PD_T_SINK_WAIT_CAP    (240*MSEC) /* between 210ms and 250ms */
#define PD_T_SINK_TRANSITION   (35*MSEC) /* between 20ms and 35ms */
//#define PD_T_SOURCE_ACTIVITY   (45*MSEC) /* between 40ms and 50ms */
//#define PD_T_SENDER_RESPONSE   (30*MSEC) /* between 24ms and 30ms */
//#define PD_T_PS_TRANSITION    (500*MSEC) /* between 450ms and 550ms */
//#define PD_T_PS_SOURCE_ON     (480*MSEC) /* between 390ms and 480ms */
//#define PD_T_PS_SOURCE_OFF    (920*MSEC) /* between 750ms and 920ms */
//#define PD_T_PS_HARD_RESET     (15*MSEC) /* between 10ms and 20ms */
#define PD_T_ERROR_RECOVERY    (25*MSEC) /* 25ms */
#define PD_T_CC_DEBOUNCE       (100*MSEC) /* between 100ms and 200ms */
/* DRP_SNK + DRP_SRC must be between 50ms and 100ms with 30%-70% duty cycle */
#define PD_T_DRP_SNK           (40*MSEC) /* toggle time for sink DRP */
#define PD_T_DRP_SRC           (30*MSEC) /* toggle time for source DRP */
#define PD_T_DEBOUNCE          (15*MSEC) /* between 10ms and 20ms */
#define PD_T_SINK_ADJ          (55*MSEC) /* between PD_T_DEBOUNCE and 60ms */
//#define PD_T_SRC_RECOVER      (760*MSEC) /* between 660ms and 1000ms */
//#define PD_T_SRC_RECOVER_MAX (1000*MSEC) /* 1000ms */
//#define PD_T_SRC_TURN_ON      (275*MSEC) /* 275ms */
//#define PD_T_SAFE_0V          (650*MSEC) /* 650ms */
#define PD_T_NO_RESPONSE     (5500*MSEC) /* between 4.5s and 5.5s */
#define PD_T_BIST_TRANSMIT     (50*MSEC) /* 50ms (used for task_wait arg) */
#define PD_T_BIST_RECEIVE      (60*MSEC) /* 60ms (max time to process bist) */
//#define PD_T_VCONN_SOURCE_ON  (100*MSEC) /* 100ms */
#define PD_T_TRY_SRC          (125*MSEC) /* Max time for Try.SRC state */
#define PD_T_TRY_WAIT         (600*MSEC) /* Max time for TryWait.SNK state */

/* from USB Type-C Specification Table 5-1 */
#define PD_T_AME (1000*MSEC) /* timeout from UFP attach to Alt Mode Entry */

/* VDM Timers ( USB PD Spec Rev2.0 Table 6-30 )*/
#define PD_T_VDM_BUSY         (100*MSEC) /* at least 100ms */
#define PD_T_VDM_E_MODE        (25*MSEC) /* enter/exit the same max */
#define PD_T_VDM_RCVR_RSP      (15*MSEC) /* max of 15ms */
#define PD_T_VDM_SNDR_RSP      (30*MSEC) /* max of 30ms */
#define PD_T_VDM_WAIT_MODE_E  (100*MSEC) /* enter/exit the same max */

struct pd_policy;
struct pd_supply_cfg;

/* function table for entered mode */
struct amode_fx {
	int (*status)(struct pd_policy *policy, u32 *payload);
	int (*config)(struct pd_policy *policy, u32 *payload);
};

/* function table for alternate mode capable responders */
struct svdm_response {
	int (*identity)(struct pd_policy *policy, u32 *payload);
	int (*svids)(struct pd_policy *policy, u32 *payload);
	int (*modes)(struct pd_policy *policy, u32 *payload);
	int (*enter_mode)(struct pd_policy *policy, u32 *payload);
	int (*exit_mode)(struct pd_policy *policy, u32 *payload);
	struct amode_fx *amode;
};

struct svdm_svid_data {
	u16 svid;
	int mode_cnt;
	u32 mode_vdo[PDO_MODES];
};

struct svdm_amode_fx {
	u16 svid;
	int (*enter)(struct pd_policy *policy, u32 mode_caps);
	int (*status)(struct pd_policy *policy, u32 *payload);
	int (*config)(struct pd_policy *policy, u32 *payload);
	void (*post_config)(struct pd_policy *policy);
	int (*attention)(struct pd_policy *policy, u32 *payload);
	void (*exit)(struct pd_policy *policy);
};

/* defined in <board>/usb_pd_policy.c */
/* All UFP_U should have */
extern const struct svdm_response svdm_rsp;
/* All DFP_U should have */
extern const struct svdm_amode_fx supported_modes[];
extern const int supported_modes_cnt;

/* DFP data needed to support alternate mode entry and exit */
struct svdm_amode_data {
	const struct svdm_amode_fx *fx;
	/* VDM object position */
	int opos;
	/* mode capabilities specific to SVID amode. */
	struct svdm_svid_data *data;
};

enum hpd_event {
	hpd_none,
	hpd_low,
	hpd_high,
	hpd_irq,
};

/* DisplayPort flags */
#define DP_FLAGS_DP_ON              (1 << 0) /* Display port mode is on */
#define DP_FLAGS_HPD_HI_PENDING     (1 << 1) /* Pending HPD_HI */

/* supported alternate modes */
enum pd_alternate_modes {
	PD_AMODE_GOOGLE,
	PD_AMODE_DISPLAYPORT,
	/* not a real mode */
	PD_AMODE_COUNT,
};

/* Policy structure for driving alternate mode */
struct pd_policy {
	struct device *dev;
	/* Type-C port owning this policy */
	struct pd_port *port;
	/* index of svid currently being operated on */
	int svid_idx;
	/* count of svids discovered */
	int svid_cnt;
	/* SVDM identity info (Id, Cert Stat, 0-4 Typec specific) */
	u32 identity[PDO_MAX_OBJECTS - 1];
	/* supported svids & corresponding vdo mode data */
	struct svdm_svid_data svids[SVID_DISCOVERY_MAX];
	/*  active modes */
	struct svdm_amode_data amodes[PD_AMODE_COUNT];
	/* Next index to insert DFP alternate mode into amodes */
	int amode_idx;
};

/*
 * VDO : Vendor Defined Message Object
 * VDM object is minimum of VDM header + 6 additional data objects.
 */

/*
 * VDM header
 * ----------
 * <31:16>  :: SVID
 * <15>     :: VDM type ( 1b == structured, 0b == unstructured )
 * <14:13>  :: Structured VDM version (can only be 00 == 1.0 currently)
 * <12:11>  :: reserved
 * <10:8>   :: object position (1-7 valid ... used for enter/exit mode only)
 * <7:6>    :: command type (SVDM only?)
 * <5>      :: reserved (SVDM), command type (UVDM)
 * <4:0>    :: command
 */
#define VDO_MAX_SIZE 7
#define VDO(vid, type, custom)				\
	(((vid) << 16) |				\
	 ((type) << 15) |				\
	 ((custom) & 0x7FFF))

#define VDO_SVDM_TYPE     (1 << 15)
#define VDO_SVDM_VERS(x)  (x << 13)
#define VDO_OPOS(x)       (x << 8)
#define VDO_CMDT(x)       (x << 6)
#define VDO_OPOS_MASK     VDO_OPOS(0x7)
#define VDO_CMDT_MASK     VDO_CMDT(0x3)

#define CMDT_INIT     0
#define CMDT_RSP_ACK  1
#define CMDT_RSP_NAK  2
#define CMDT_RSP_BUSY 3


/* reserved for SVDM ... for Google UVDM */
#define VDO_SRC_INITIATOR (0 << 5)
#define VDO_SRC_RESPONDER (1 << 5)

#define CMD_DISCOVER_IDENT  1
#define CMD_DISCOVER_SVID   2
#define CMD_DISCOVER_MODES  3
#define CMD_ENTER_MODE      4
#define CMD_EXIT_MODE       5
#define CMD_ATTENTION       6
#define CMD_DP_STATUS      16
#define CMD_DP_CONFIG      17

#define VDO_CMD_VENDOR(x)    (((10 + (x)) & 0x1f))

/* ChromeOS specific commands */
#define VDO_CMD_VERSION      VDO_CMD_VENDOR(0)
#define VDO_CMD_SEND_INFO    VDO_CMD_VENDOR(1)
#define VDO_CMD_READ_INFO    VDO_CMD_VENDOR(2)
#define VDO_CMD_REBOOT       VDO_CMD_VENDOR(5)
#define VDO_CMD_FLASH_ERASE  VDO_CMD_VENDOR(6)
#define VDO_CMD_FLASH_WRITE  VDO_CMD_VENDOR(7)
#define VDO_CMD_ERASE_SIG    VDO_CMD_VENDOR(8)
#define VDO_CMD_PING_ENABLE  VDO_CMD_VENDOR(10)
#define VDO_CMD_CURRENT      VDO_CMD_VENDOR(11)
#define VDO_CMD_FLIP         VDO_CMD_VENDOR(12)
#define VDO_CMD_GET_LOG      VDO_CMD_VENDOR(13)
#define VDO_CMD_CCD_EN       VDO_CMD_VENDOR(14)

#define PD_VDO_VID(vdo)  ((vdo) >> 16)
#define PD_VDO_SVDM(vdo) (((vdo) >> 15) & 1)
#define PD_VDO_OPOS(vdo) (((vdo) >> 8) & 0x7)
#define PD_VDO_CMD(vdo)  ((vdo) & 0x1f)
#define PD_VDO_CMDT(vdo) (((vdo) >> 6) & 0x3)

/*
 * SVDM Identity request -> response
 *
 * Request is simply properly formatted SVDM header
 *
 * Response is 4 data objects:
 * [0] :: SVDM header
 * [1] :: Identitiy header
 * [2] :: Cert Stat VDO
 * [3] :: (Product | Cable) VDO
 * [4] :: AMA VDO
 *
 */
#define VDO_INDEX_HDR     0
#define VDO_INDEX_IDH     1
#define VDO_INDEX_CSTAT   2
#define VDO_INDEX_CABLE   3
#define VDO_INDEX_PRODUCT 3
#define VDO_INDEX_AMA     4
#define VDO_I(name) VDO_INDEX_##name

/*
 * SVDM Identity Header
 * --------------------
 * <31>     :: data capable as a USB host
 * <30>     :: data capable as a USB device
 * <29:27>  :: product type
 * <26>     :: modal operation supported (1b == yes)
 * <25:16>  :: SBZ
 * <15:0>   :: USB-IF assigned VID for this cable vendor
 */
#define IDH_PTYPE_UNDEF  0
#define IDH_PTYPE_HUB    1
#define IDH_PTYPE_PERIPH 2
#define IDH_PTYPE_PCABLE 3
#define IDH_PTYPE_ACABLE 4
#define IDH_PTYPE_AMA    5

#define VDO_IDH(usbh, usbd, ptype, is_modal, vid)		\
	((usbh) << 31 | (usbd) << 30 | ((ptype) & 0x7) << 27	\
	 | (is_modal) << 26 | ((vid) & 0xffff))

#define PD_IDH_PTYPE(vdo) (((vdo) >> 27) & 0x7)
#define PD_IDH_VID(vdo)   ((vdo) & 0xffff)

/*
 * Cert Stat VDO
 * -------------
 * <31:20> : SBZ
 * <19:0>  : USB-IF assigned TID for this cable
 */
#define VDO_CSTAT(tid)    ((tid) & 0xfffff)
#define PD_CSTAT_TID(vdo) ((vdo) & 0xfffff)

/*
 * Product VDO
 * -----------
 * <31:16> : USB Product ID
 * <15:0>  : USB bcdDevice
 */
#define VDO_PRODUCT(pid, bcd) (((pid) & 0xffff) << 16 | ((bcd) & 0xffff))
#define PD_PRODUCT_PID(vdo) (((vdo) >> 16) & 0xffff)

/*
 * Cable VDO
 * ---------
 * <31:28> :: Cable HW version
 * <27:24> :: Cable FW version
 * <23:20> :: SBZ
 * <19:18> :: type-C to Type-A/B/C (00b == A, 01 == B, 10 == C)
 * <17>    :: Type-C to Plug/Receptacle (0b == plug, 1b == receptacle)
 * <16:13> :: cable latency (0001 == <10ns(~1m length))
 * <12:11> :: cable termination type (11b == both ends active VCONN req)
 * <10>    :: SSTX1 Directionality support (0b == fixed, 1b == cfgable)
 * <9>     :: SSTX2 Directionality support
 * <8>     :: SSRX1 Directionality support
 * <7>     :: SSRX2 Directionality support
 * <6:5>   :: Vbus current handling capability
 * <4>     :: Vbus through cable (0b == no, 1b == yes)
 * <3>     :: SOP" controller present? (0b == no, 1b == yes)
 * <2:0>   :: USB SS Signaling support
 */
#define CABLE_ATYPE 0
#define CABLE_BTYPE 1
#define CABLE_CTYPE 2
#define CABLE_PLUG       0
#define CABLE_RECEPTACLE 1
#define CABLE_CURR_1A5   0
#define CABLE_CURR_3A    1
#define CABLE_CURR_5A    2
#define CABLE_USBSS_U2_ONLY  0
#define CABLE_USBSS_U31_GEN1 1
#define CABLE_USBSS_U31_GEN2 2
#define VDO_CABLE(hw, fw, cbl, gdr, lat, term, tx1d, tx2d, rx1d, rx2d, cur,\
		  vps, sopp, usbss) \
	(((hw) & 0x7) << 28 | ((fw) & 0x7) << 24 | ((cbl) & 0x3) << 18	\
	 | (gdr) << 17 | ((lat) & 0x7) << 13 | ((term) & 0x3) << 11	\
	 | (tx1d) << 10 | (tx2d) << 9 | (rx1d) << 8 | (rx2d) << 7	\
	 | ((cur) & 0x3) << 5 | (vps) << 4 | (sopp) << 3		\
	 | ((usbss) & 0x7))

/*
 * AMA VDO
 * ---------
 * <31:28> :: Cable HW version
 * <27:24> :: Cable FW version
 * <23:12> :: SBZ
 * <11>    :: SSTX1 Directionality support (0b == fixed, 1b == cfgable)
 * <10>    :: SSTX2 Directionality support
 * <9>     :: SSRX1 Directionality support
 * <8>     :: SSRX2 Directionality support
 * <7:5>   :: Vconn power
 * <4>     :: Vconn power required
 * <3>     :: Vbus power required
 * <2:0>   :: USB SS Signaling support
 */
#define VDO_AMA(hw, fw, tx1d, tx2d, rx1d, rx2d, vcpwr, vcr, vbr, usbss) \
	(((hw) & 0x7) << 28 | ((fw) & 0x7) << 24			\
	 | (tx1d) << 11 | (tx2d) << 10 | (rx1d) << 9 | (rx2d) << 8	\
	 | ((vcpwr) & 0x3) << 5 | (vcr) << 4 | (vbr) << 3		\
	 | ((usbss) & 0x7))

#define PD_VDO_AMA_VCONN_REQ(vdo) (((vdo) >> 4) & 1)
#define PD_VDO_AMA_VBUS_REQ(vdo)  (((vdo) >> 3) & 1)

#define AMA_VCONN_PWR_1W   0
#define AMA_VCONN_PWR_1W5  1
#define AMA_VCONN_PWR_2W   2
#define AMA_VCONN_PWR_3W   3
#define AMA_VCONN_PWR_4W   4
#define AMA_VCONN_PWR_5W   5
#define AMA_VCONN_PWR_6W   6
#define AMA_USBSS_U2_ONLY  0
#define AMA_USBSS_U31_GEN1 1
#define AMA_USBSS_U31_GEN2 2
#define AMA_USBSS_BBONLY   3

/*
 * SVDM Discover SVIDs request -> response
 *
 * Request is properly formatted VDM Header with discover SVIDs command.
 * Response is a set of SVIDs of all all supported SVIDs with all zero's to
 * mark the end of SVIDs.  If more than 12 SVIDs are supported command SHOULD be
 * repeated.
 */
#define VDO_SVID(svid0, svid1) (((svid0) & 0xffff) << 16 | ((svid1) & 0xffff))
#define PD_VDO_SVID_SVID0(vdo) ((vdo) >> 16)
#define PD_VDO_SVID_SVID1(vdo) ((vdo) & 0xffff)

/*
 * Google modes capabilities
 * <31:8> : reserved
 * <7:0>  : mode
 */
#define VDO_MODE_GOOGLE(mode) (mode & 0xff)

#define MODE_GOOGLE_FU 1 /* Firmware Update mode */

/*
 * Mode Capabilities
 *
 * Number of VDOs supplied is SID dependent (but <= 6 VDOS?)
 */
#define VDO_MODE_CNT_DISPLAYPORT 1

/*
 * DisplayPort modes capabilities
 * -------------------------------
 * <31:24> : SBZ
 * <23:16> : UFP_D pin assignment supported
 * <15:8>  : DFP_D pin assignment supported
 * <7>     : USB 2.0 signaling (0b=yes, 1b=no)
 * <6>     : Plug | Receptacle (0b == plug, 1b == receptacle)
 * <5:2>   : xxx1: Supports DPv1.3, xx1x Supports USB Gen 2 signaling
 *           Other bits are reserved.
 * <1:0>   : signal direction ( 00b=rsv, 01b=sink, 10b=src 11b=both )
 */
#define VDO_MODE_DP(snkp, srcp, usb, gdr, sign, sdir)			\
	(((snkp) & 0xff) << 16 | ((srcp) & 0xff) << 8			\
	 | ((usb) & 1) << 7 | ((gdr) & 1) << 6 | ((sign) & 0xF) << 2	\
	 | ((sdir) & 0x3))
#define PD_DP_PIN_CAPS(x) ((((x) >> 6) & 0x1) ? (((x) >> 16) & 0x3f)	\
			   : (((x) >> 8) & 0x3f))

#define MODE_DP_PIN_A 0x01
#define MODE_DP_PIN_B 0x02
#define MODE_DP_PIN_C 0x04
#define MODE_DP_PIN_D 0x08
#define MODE_DP_PIN_E 0x10
#define MODE_DP_PIN_F 0x20

/* Pin configs B/D/F support multi-function */
#define MODE_DP_PIN_MF_MASK 0x2a
/* Pin configs A/B support BR2 signaling levels */
#define MODE_DP_PIN_BR2_MASK 0x3
/* Pin configs C/D/E/F support DP signaling levels */
#define MODE_DP_PIN_DP_MASK 0x3c

#define MODE_DP_V13  0x1
#define MODE_DP_GEN2 0x2

#define MODE_DP_SNK  0x1
#define MODE_DP_SRC  0x2
#define MODE_DP_BOTH 0x3

/*
 * DisplayPort Status VDO
 * ----------------------
 * <31:9> : SBZ
 * <8>    : IRQ_HPD : 1 == irq arrived since last message otherwise 0.
 * <7>    : HPD state : 0 = HPD_LOW, 1 == HPD_HIGH
 * <6>    : Exit DP Alt mode: 0 == maintain, 1 == exit
 * <5>    : USB config : 0 == maintain current, 1 == switch to USB from DP
 * <4>    : Multi-function preference : 0 == no pref, 1 == MF preferred.
 * <3>    : enabled : is DPout on/off.
 * <2>    : power low : 0 == normal or LPM disabled, 1 == DP disabled for LPM
 * <1:0>  : connect status : 00b ==  no (DFP|UFP)_D is connected or disabled.
 *          01b == DFP_D connected, 10b == UFP_D connected, 11b == both.
 */
#define VDO_DP_STATUS(irq, lvl, amode, usbc, mf, en, lp, conn)		\
	(((irq) & 1) << 8 | ((lvl) & 1) << 7 | ((amode) & 1) << 6	\
	 | ((usbc) & 1) << 5 | ((mf) & 1) << 4 | ((en) & 1) << 3	\
	 | ((lp) & 1) << 2 | ((conn & 0x3) << 0))

#define PD_VDO_DPSTS_HPD_IRQ(x) (((x) >> 8) & 1)
#define PD_VDO_DPSTS_HPD_LVL(x) (((x) >> 7) & 1)
#define PD_VDO_DPSTS_MF_PREF(x) (((x) >> 4) & 1)

/* Per DisplayPort Spec v1.3 Section 3.3 */
#define HPD_USTREAM_DEBOUNCE_LVL (2*MSEC)
#define HPD_USTREAM_DEBOUNCE_IRQ (250)
#define HPD_DSTREAM_DEBOUNCE_IRQ (750)  /* between 500-1000us */

/*
 * DisplayPort Configure VDO
 * -------------------------
 * <31:24> : SBZ
 * <23:16> : SBZ
 * <15:8>  : Pin assignment requested.  Choose one from mode caps.
 * <7:6>   : SBZ
 * <5:2>   : signalling : 1h == DP v1.3, 2h == Gen 2
 *           Oh is only for USB, remaining values are reserved
 * <1:0>   : cfg : 00 == USB, 01 == DFP_D, 10 == UFP_D, 11 == reserved
 */
#define VDO_DP_CFG(pin, sig, cfg) \
	(((pin) & 0xff) << 8 | ((sig) & 0xf) << 2 | ((cfg) & 0x3))

#define PD_DP_CFG_DPON(x) (((x & 0x3) == 1) || ((x & 0x3) == 2))
/*
 * Get the pin assignment mask
 * for backward compatibility, if it is null,
 * get the former sink pin assignment we used to be in <23:16>.
 */
#define PD_DP_CFG_PIN(x) ((((x) >> 8) & 0xff) ? (((x) >> 8) & 0xff) \
					      : (((x) >> 16) & 0xff))
/*
 * ChromeOS specific PD device Hardware IDs. Used to identify unique
 * products and used in VDO_INFO. Note this field is 10 bits.
 */
#define USB_PD_HW_DEV_ID_RESERVED    0
#define USB_PD_HW_DEV_ID_ZINGER      1
#define USB_PD_HW_DEV_ID_MINIMUFFIN  2
#define USB_PD_HW_DEV_ID_DINGDONG    3
#define USB_PD_HW_DEV_ID_HOHO        4
#define USB_PD_HW_DEV_ID_HONEYBUNS   5

/*
 * ChromeOS specific VDO_CMD_READ_INFO responds with device info including:
 * RW Hash: First 20 bytes of SHA-256 of RW (20 bytes)
 * HW Device ID: unique descriptor for each ChromeOS model (2 bytes)
 *               top 6 bits are minor revision, bottom 10 bits are major
 * SW Debug Version: Software version useful for debugging (15 bits)
 * IS RW: True if currently in RW, False otherwise (1 bit)
 */
#define VDO_INFO(id, id_minor, ver, is_rw) ((id_minor) << 26 \
				  | ((id) & 0x3ff) << 16 \
				  | ((ver) & 0x7fff) << 1 \
				  | ((is_rw) & 1))
#define VDO_INFO_HW_DEV_ID(x)    ((x) >> 16)
#define VDO_INFO_SW_DBG_VER(x)   (((x) >> 1) & 0x7fff)
#define VDO_INFO_IS_RW(x)        ((x) & 1)

#define HW_DEV_ID_MAJ(x) (x & 0x3ff)
#define HW_DEV_ID_MIN(x) ((x) >> 10)

/* USB-IF SIDs */
#define USB_SID_PD          0xff00 /* power delivery */
#define USB_SID_DISPLAYPORT 0xff01

#define USB_GOOGLE_TYPEC_URL "http://www.google.com/chrome/devices/typec"
/* USB Vendor ID assigned to Google Inc. */
#define USB_VID_GOOGLE 0x18d1

/* Other Vendor IDs */
#define USB_VID_APPLE  0x05ac

/* Timeout for message receive in microseconds */
#define USB_PD_RX_TMOUT_US 1800

/* --- Protocol layer functions --- */

enum pd_states {
	PD_STATE_DISABLED,
	PD_STATE_SUSPENDED,
	PD_STATE_SNK_DISCONNECTED,
	PD_STATE_SNK_DISCONNECTED_DEBOUNCE,
	PD_STATE_SNK_HARD_RESET_RECOVER,
	PD_STATE_SNK_DISCOVERY,
	PD_STATE_SNK_REQUESTED,
	PD_STATE_SNK_TRANSITION,
	PD_STATE_SNK_READY,

	PD_STATE_SNK_SWAP_INIT,
	PD_STATE_SNK_SWAP_SNK_DISABLE,
	PD_STATE_SNK_SWAP_SRC_DISABLE,
	PD_STATE_SNK_SWAP_STANDBY,
	PD_STATE_SNK_SWAP_COMPLETE,

	PD_STATE_SRC_DISCONNECTED,
	PD_STATE_SRC_DISCONNECTED_DEBOUNCE,
	PD_STATE_SRC_ACCESSORY,
	PD_STATE_SRC_HARD_RESET_RECOVER,
	PD_STATE_SRC_STARTUP,
	PD_STATE_SRC_DISCOVERY,
	PD_STATE_SRC_NEGOCIATE,
	PD_STATE_SRC_ACCEPTED,
	PD_STATE_SRC_POWERED,
	PD_STATE_SRC_TRANSITION,
	PD_STATE_SRC_READY,
	PD_STATE_SRC_GET_SINK_CAP,
	PD_STATE_DR_SWAP,

	PD_STATE_SRC_SWAP_INIT,
	PD_STATE_SRC_SWAP_SNK_DISABLE,
	PD_STATE_SRC_SWAP_SRC_DISABLE,
	PD_STATE_SRC_SWAP_STANDBY,

	PD_STATE_VCONN_SWAP_SEND,
	PD_STATE_VCONN_SWAP_INIT,
	PD_STATE_VCONN_SWAP_READY,

	PD_STATE_SOFT_RESET,
	PD_STATE_HARD_RESET_SEND,
	PD_STATE_HARD_RESET_EXECUTE,
	PD_STATE_BIST_RX,
	PD_STATE_BIST_TX,

	/* Number of states. Not an actual state. */
	PD_STATE_COUNT,
};

#define PD_FLAGS_PING_ENABLED      (1 << 0) /* SRC_READY pings enabled */
#define PD_FLAGS_PARTNER_DR_POWER  (1 << 1) /* port partner is dualrole power */
#define PD_FLAGS_PARTNER_DR_DATA   (1 << 2) /* port partner is dualrole data */
#define PD_FLAGS_DATA_SWAPPED      (1 << 3) /* data swap complete */
#define PD_FLAGS_SNK_CAP_RECVD     (1 << 4) /* sink capabilities received */
#define PD_FLAGS_EXPLICIT_CONTRACT (1 << 6) /* explicit pwr contract in place */
#define PD_FLAGS_VBUS_NEVER_LOW    (1 << 7) /* VBUS input has never been low */
#define PD_FLAGS_PREVIOUS_PD_CONN  (1 << 8) /* previously PD connected */
#define PD_FLAGS_CHECK_PR_ROLE     (1 << 9) /* check power role in READY */
#define PD_FLAGS_CHECK_DR_ROLE     (1 << 10)/* check data role in READY */
#define PD_FLAGS_PARTNER_EXTPOWER  (1 << 11)/* port partner has external pwr */
#define PD_FLAGS_VCONN_ON          (1 << 12)/* vconn is being sourced */
#define PD_FLAGS_TRY_SRC           (1 << 13)/* Try.SRC states are active */
#define PD_FLAGS_PARTNER_USB_COMM  (1 << 14)/* port partner is USB comms */
/* Flags to clear on a disconnect */
#define PD_FLAGS_RESET_ON_DISCONNECT_MASK (PD_FLAGS_PARTNER_DR_POWER | \
					   PD_FLAGS_PARTNER_DR_DATA | \
					   PD_FLAGS_DATA_SWAPPED | \
					   PD_FLAGS_SNK_CAP_RECVD | \
					   PD_FLAGS_EXPLICIT_CONTRACT | \
					   PD_FLAGS_PREVIOUS_PD_CONN | \
					   PD_FLAGS_CHECK_PR_ROLE | \
					   PD_FLAGS_CHECK_DR_ROLE | \
					   PD_FLAGS_PARTNER_EXTPOWER | \
					   PD_FLAGS_VCONN_ON | \
					   PD_FLAGS_TRY_SRC | \
					   PD_FLAGS_PARTNER_USB_COMM)


enum pd_cc_states {
	PD_CC_NONE,

	/* From DFP perspective */
	PD_CC_NO_UFP,
	PD_CC_AUDIO_ACC,
	PD_CC_DEBUG_ACC,
	PD_CC_UFP_ATTACHED,

	/* From UFP perspective */
	PD_CC_DFP_ATTACHED
};

enum pd_dual_role_states {
	PD_DRP_TOGGLE_ON,
	PD_DRP_TOGGLE_OFF,
	PD_DRP_FORCE_SINK,
	PD_DRP_FORCE_SOURCE,
};
/**
 * Get dual role state
 *
 * @return Current dual-role state, from enum pd_dual_role_states
 */
enum pd_dual_role_states pd_get_dual_role(void);
/**
 * Set dual role state, from among enum pd_dual_role_states
 *
 * @param state New state of dual-role port, selected from
 *              enum pd_dual_role_states
 */
void pd_set_dual_role(enum pd_dual_role_states state);

/**
 * Get role, from among PD_ROLE_SINK and PD_ROLE_SOURCE
 *
 * @param port Port number from which to get role
 */
int pd_get_role(struct pd_port *port);

/* Protocol revision */
#define PD_REV10 0
#define PD_REV20 1

/* Power role */
#define PD_ROLE_SINK   0
#define PD_ROLE_SOURCE 1
/* Data role */
#define PD_ROLE_UFP    0
#define PD_ROLE_DFP    1
/* Vconn role */
#define PD_ROLE_VCONN_OFF 0
#define PD_ROLE_VCONN_ON  1

/* Port role at startup */
#define PD_ROLE_DEFAULT PD_ROLE_SINK

/* K-codes for special symbols */
#define PD_SYNC1 0x18
#define PD_SYNC2 0x11
#define PD_SYNC3 0x06
#define PD_RST1  0x07
#define PD_RST2  0x19
#define PD_EOP   0x0D

/* Minimum PD supply current  (mA) */
#define PD_MIN_MA	500

/* Minimum PD voltage (mV) */
#define PD_MIN_MV	5000

/* No connect voltage threshold for sources based on Rp */
#define PD_SRC_DEF_VNC_MV        1600
#define PD_SRC_1_5_VNC_MV        1600
#define PD_SRC_3_0_VNC_MV        2600

/* Rd voltage threshold for sources based on Rp */
#define PD_SRC_DEF_RD_THRESH_MV  200
#define PD_SRC_1_5_RD_THRESH_MV  400
#define PD_SRC_3_0_RD_THRESH_MV  800

/* Voltage threshold to detect connection when presenting Rd */
#define PD_SNK_VA_MV             250

/* --- Policy layer functions --- */

/* Request types for pd_build_request() */
enum pd_request_type {
	PD_REQUEST_VSAFE5V,
	PD_REQUEST_MAX,
};

/**
 * Decide which PDO to choose from the source capabilities.
 *
 * @param cfg board specific configuration
 * @param cnt  the number of Power Data Objects.
 * @param src_caps Power Data Objects representing the source capabilities.
 * @param rdo  requested Request Data Object.
 * @param ma  selected current limit (stored on success)
 * @param mv  selected supply voltage (stored on success)
 * @param req_type request type
 * @return <0 if invalid, else EC_SUCCESS
 */
int pd_build_request(const struct pd_supply_cfg *cfg, int cnt, u32 *src_caps,
		     u32 *rdo, u32 *ma, u32 *mv, enum pd_request_type req_type);

/**
 * Check if max voltage request is allowed (only used if
 * CONFIG_USB_PD_CHECK_MAX_REQUEST_ALLOWED is defined).
 *
 * @return True if max voltage request allowed, False otherwise
 */
int pd_is_max_request_allowed(void);

/**
 * Process source capabilities packet
 *
 * @param port USB-C port number
 * @param cfg board specific configuration
 * @param cnt  the number of Power Data Objects.
 * @param src_caps Power Data Objects representing the source capabilities.
 */
void pd_process_source_cap(struct pd_port *port,
			   const struct pd_supply_cfg *cfg,
			   int cnt, u32 *src_caps);

/**
 * Put a cap on the max voltage requested as a sink.
 * @param mv maximum voltage in millivolts.
 */
void pd_set_max_voltage(unsigned mv);

/**
 * Get the max voltage that can be requested as set by pd_set_max_voltage().
 * @return max voltage
 */
unsigned pd_get_max_voltage(void);

/**
 * Check if this board supports the given input voltage.
 *
 * @mv input voltage
 * @return 1 if voltage supported, 0 if not
 */
int pd_is_valid_input_voltage(int mv);

/**
 * Request a new operating voltage.
 *
 * @param cfg  board specific configuration
 * @param rdo  Request Data Object with the selected operating point.
 * @return EC_SUCCESS if we can get the requested voltage/OP, <0 else.
 */
int pd_check_requested_voltage(const struct pd_supply_cfg *cfg, u32 rdo);

/**
 * Run board specific checks on request message
 *
 * @return EC_SUCCESS if request is ok , <0 else.
 */
int pd_board_check_request(u32 rdo);

/**
 * Select a new output voltage.
 *
 * param idx index of the new voltage in the source PDO table.
 */
void pd_transition_voltage(int idx);

/**
 * Go back to the default/safe state of the power supply
 *
 * @param port USB-C port number
 */
void pd_power_supply_reset(struct pd_port *port);

/**
 * Enable the power supply output after the ready delay.
 *
 * @param port USB-C port number
 * @return EC_SUCCESS if the power supply is ready, <0 else.
 */
int pd_set_power_supply_ready(struct pd_port *port);

/**
 * Ask the specified voltage from the PD source.
 *
 * It triggers a new negotiation sequence with the source.
 * @param port USB-C port number
 * @param mv request voltage in millivolts.
 */
void pd_request_source_voltage(struct pd_port *port, int mv);

/**
 * Set a voltage limit from the PD source.
 *
 * If the source is currently active, it triggers a new negotiation.
 * @param port USB-C port number
 * @param mv limit voltage in millivolts.
 */
void pd_set_external_voltage_limit(struct pd_port *port, int mv);

/**
 * Set the PD input current limit.
 *
 * @param port USB-C port number
 * @param max_ma Maximum current limit
 * @param supply_voltage Voltage at which current limit is applied
 */
void pd_set_input_current_limit(struct pd_port *port, u32 max_ma,
				u32 supply_voltage);

/**
 * Set the type-C input current limit.
 *
 * @param port USB-C port number
 * @param max_ma Maximum current limit
 * @param supply_voltage Voltage at which current limit is applied
 */
void typec_set_input_current_limit(struct pd_port *port, u32 max_ma,
				   u32 supply_voltage);

/**
 * Verify board specific health status : current, voltages...
 *
 * @return EC_SUCCESS if the board is good, <0 else.
 */
int pd_board_checks(void);

/**
 * Return if VBUS is detected on type-C port
 *
 * @param port USB-C port number
 * @return VBUS is detected
 */
int pd_snk_is_vbus_provided(struct pd_port *port);

/**
 * Notify PD protocol that VBUS has gone low
 *
 * @param port USB-C port number
 */
void pd_vbus_low(struct pd_port *port);

/**
 * Check if power swap is allowed.
 *
 * @param port USB-C port number
 * @return True if power swap is allowed, False otherwise
 */
int pd_check_power_swap(struct pd_port *port);

/**
 * Check if data swap is allowed.
 *
 * @param port USB-C port number
 * @param data_role current data role
 * @return True if data swap is allowed, False otherwise
 */
int pd_check_data_swap(struct pd_port *port, int data_role);

/**
 * Check if vconn swap is allowed.
 *
 * @param port USB-C port number
 * @return True if vconn swap is allowed, False otherwise
 */

int pd_check_vconn_swap(struct pd_port *port);

/**
 * Check current power role for potential power swap
 *
 * @param port USB-C port number
 * @param pr_role Our power role
 * @param flags PD flags
 */
void pd_check_pr_role(struct pd_port *port, int pr_role, int flags);

/**
 * Check current data role for potential data swap
 *
 * @param port USB-C port number
 * @param dr_role Our data role
 * @param flags PD flags
 */
void pd_check_dr_role(struct pd_port *port, int dr_role, int flags);

/**
 * Check if we should charge from this device. This is
 * basically a white-list for chargers that are dual-role,
 * don't set the externally powered bit, but we should charge
 * from by default.
 *
 * @param vid Port partner Vendor ID
 * @param pid Port partner Product ID
 */
int pd_charge_from_device(u16 vid, u16 pid);

/**
 * Execute data swap.
 *
 * @param port USB-C port number
 * @param data_role new data role
 */
void pd_execute_data_swap(struct pd_port *port, int data_role);

/**
 * Get PD device info used for VDO_CMD_SEND_INFO / VDO_CMD_READ_INFO
 *
 * @param info_data pointer to info data array
 */
void pd_get_info(u32 *info_data);

/**
 * Handle Vendor Defined Messages
 *
 * @param policy   USB-C port policy
 * @param cnt      number of data objects in the payload.
 * @param payload  payload data.
 * @param rpayload pointer to the data to send back.
 * @return if >0, number of VDOs to send back.
 */
int pd_custom_vdm(struct pd_policy *policy, int cnt, u32 *payload,
		  u32 **rpayload);

/**
 * Handle Structured Vendor Defined Messages
 *
 * @param policy   USB-C port policy
 * @param cnt      number of data objects in the payload.
 * @param payload  payload data.
 * @param rpayload pointer to the data to send back.
 * @return if >0, number of VDOs to send back.
 */
int pd_svdm(struct pd_policy *policy, int cnt, u32 *payload, u32 **rpayload);

/**
 * Enter alternate mode on DFP
 *
 * @param policy     USB-C port policy
 * @param svid USB standard or vendor id to exit or zero for DFP amode reset.
 * @param opos object position of mode to exit.
 * @return vdm for UFP to be sent to enter mode or zero if not.
 */
u32 pd_dfp_enter_mode(struct pd_policy *policy, u16 svid, int opos);

/**
 *  Get DisplayPort pin mode for DFP to request from UFP's capabilities.
 *
 * @param policy   USB-C port policy.
 * @param status   DisplayPort Status VDO.
 * @return one-hot PIN config to request.
 */
int pd_dfp_dp_get_pin_mode(struct pd_policy *policy, u32 status);

/**
 * Exit alternate mode on DFP
 *
 * @param policy USB-C port policy
 * @param svid USB standard or vendor id to exit or zero for DFP amode reset.
 * @param opos object position of mode to exit.
 * @return 1 if UFP should be sent exit mode VDM.
 */
int pd_dfp_exit_mode(struct pd_policy *policy, u16 svid, int opos);

/**
 * Initialize policy engine for DFP
 *
 * @param policy     USB-C port policy
 */
void pd_dfp_pe_init(struct pd_policy *policy, struct device *dev,
		    struct pd_port *port);

/**
 * Return the VID of the USB PD accessory connected to a specified port
 *
 * @param policy  USB-C port policy
 * @return      the USB Vendor Identifier or 0 if it doesn't exist
 */
u16 pd_get_identity_vid(struct pd_policy *policy);

/**
 * Return the PID of the USB PD accessory connected to a specified port
 *
 * @param policy  USB-C port policy
 * @return      the USB Product Identifier or 0 if it doesn't exist
 */
u16 pd_get_identity_pid(struct pd_policy *policy);

/**
 * Try to fetch one PD log entry from accessory
 *
 * @param port	USB-C accessory port number
 * @return	EC_RES_SUCCESS if the VDM was sent properly else error code
 */
int pd_fetch_acc_log_entry(struct pd_port *port);

/**
 * Analyze the log entry received as the VDO_CMD_GET_LOG payload.
 *
 * @param port		USB-C accessory port number
 * @param cnt		number of data objects in payload
 * @param payload	payload data
 */
void pd_log_recv_vdm(struct pd_port *port, int cnt, u32 *payload);

/**
 * Send Vendor Defined Message
 *
 * @param port     USB-C port number
 * @param vid      Vendor ID
 * @param cmd      VDO command number
 * @param data     Pointer to payload to send
 * @param count    number of data objects in payload
 */
void pd_send_vdm(struct pd_port *port, u32 vid, int cmd, const u32 *data,
		 int count);

/**
 * Get PD source power data objects.
 *
 * @param src_pdo pointer to the data to return.
 * @return number of PDOs returned.
 */
int pd_get_source_pdo(const u32 **src_pdo);

/**
 * Request that a host event be sent to notify the AP of a PD power event.
 *
 * @param mask host event mask.
 */
void pd_send_host_event(int mask);

/**
 * Determine if in alternate mode or not.
 *
 * @param policy port policy.
 * @param svid USB standard or vendor id
 * @return object position of mode chosen in alternate mode otherwise zero.
 */
int pd_alt_mode(struct pd_policy *policy, u16 svid);

/**
 * Enable USB Billboard Device.
 */
void pd_usb_billboard_deferred(void);

/* --- Protocol layer functions --- */

/**
 * Decode a raw packet in the RX buffer.
 *
 * @param port USB-C port number
 * @param payload buffer to store the packet payload (must be 7x 32-bit)
 * @return the packet header or <0 in case of error
 */
int pd_analyze_rx(struct pd_port *port, u32 *payload);

/**
 * Get connected state
 *
 * @param port USB-C port number
 * @return True if port is in connected state
 */
int pd_is_connected(struct pd_port *port);

/**
 * Execute a hard reset
 *
 * @param port USB-C port number
 */
void pd_execute_hard_reset(struct pd_port *port);

/**
 * Signal to protocol layer that PD transmit is complete
 *
 * @param port USB-C port number
 * @param status status of the transmission
 */
void pd_transmit_complete(struct pd_port *port, int status);

/**
 * Get port polarity.
 *
 * @param port USB-C port number
 */
int pd_get_polarity(struct pd_port *port);

/**
 * Get port partner data swap capable status
 *
 * @param port USB-C port number
 */
int pd_get_partner_data_swap_capable(struct pd_port *port);

/**
 * Request power swap command to be issued
 *
 * @param port USB-C port number
 */
void pd_request_power_swap(struct pd_port *port);

/**
 * Request data swap command to be issued
 *
 * @param port USB-C port number
 */
void pd_request_data_swap(struct pd_port *port);

/**
 * Set the PD communication enabled flag. When communication is disabled,
 * the port can still detect connection and source power but will not
 * send or respond to any PD communication.
 *
 * @param enable Enable flag to set
 */
void pd_comm_enable(int enable);

/**
 * Set the PD pings enabled flag. When source has negotiated power over
 * PD successfully, it can optionally send pings periodically based on
 * this enable flag.
 *
 * @param port USB-C port number
 * @param enable Enable flag to set
 */
void pd_ping_enable(struct pd_port *port, int enable);

/* Issue PD soft reset */
void pd_soft_reset(void);

/* Prepare PD communication for reset */
void pd_prepare_reset(void);

/**
 * Signal power request to indicate a charger update that affects the port.
 *
 * @param port USB-C port number
 */
void pd_set_new_power_request(struct pd_port *port);

/* ----- Logging ----- */
#ifdef CONFIG_USB_PD_LOGGING
/**
 * Record one event in the PD logging FIFO.
 *
 * @param type event type as defined by PD_EVENT_xx in ec_commands.h
 * @param size_port payload size and port num (defined by PD_LOG_PORT_SIZE)
 * @param data type-defined information
 * @param payload pointer to the optional payload (0..16 bytes)
 */
void pd_log_event(u8 type, u8 size_port,
		  u16 data, void *payload);

/**
 * Retrieve one logged event and prepare a VDM with it.
 *
 * Used to answer the VDO_CMD_GET_LOG unstructured VDM.
 *
 * @param payload pointer to the payload data buffer (must be 7 words)
 * @return number of 32-bit words in the VDM payload.
 */
int pd_vdm_get_log_entry(u32 *payload);
#else  /* CONFIG_USB_PD_LOGGING */
static inline void pd_log_event(u8 type, u8 size_port,
				u16 data, void *payload) {}
static inline int pd_vdm_get_log_entry(u32 *payload) { return 0; }
#endif /* CONFIG_USB_PD_LOGGING */

/* Board specific configuration for a USB PD power supply */
struct pd_supply_cfg {
	unsigned turn_on_delay_ms;
	unsigned turn_off_delay_ms;
	unsigned vconn_swap_delay_ms;

	unsigned operating_power_mw;
	unsigned max_power_mw;
	unsigned max_current_ma;
	unsigned max_voltage_mv;

	bool prefer_low_voltage;
	bool use_debug_mode;

	int default_state;
	int debug_role;

	const u32 *src_pdo;
	int src_pdo_cnt;
	const u32 *snk_pdo;
	int snk_pdo_cnt;
};

const struct pd_supply_cfg *pd_get_board_config(void);

#endif  /* __LINUX_USB_PD_H */
