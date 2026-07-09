#ifndef USB_H
#define USB_H

#include <stdint.h>

/* ── Setup packet (8 bytes, packed) ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

/* ── bmRequestType bits ─────────────────────────────────────────────────── */
#define USB_DIR_OUT      0x00u
#define USB_DIR_IN       0x80u
#define USB_TYPE_STD     0x00u
#define USB_TYPE_CLASS   0x20u
#define USB_TYPE_VENDOR  0x40u
#define USB_RECIP_DEV    0x00u
#define USB_RECIP_IFACE  0x01u
#define USB_RECIP_EP     0x02u

/* ── Standard request codes ─────────────────────────────────────────────── */
#define USB_REQ_GET_STATUS      0x00u
#define USB_REQ_CLEAR_FEATURE   0x01u
#define USB_REQ_SET_FEATURE     0x03u
#define USB_REQ_SET_ADDRESS     0x05u
#define USB_REQ_GET_DESCRIPTOR  0x06u
#define USB_REQ_SET_CONFIG      0x09u
#define USB_REQ_SET_IFACE       0x0Bu

/* ── Descriptor types ───────────────────────────────────────────────────── */
#define USB_DESC_DEVICE     0x01u
#define USB_DESC_CONFIG     0x02u
#define USB_DESC_STRING     0x03u
#define USB_DESC_INTERFACE  0x04u
#define USB_DESC_ENDPOINT   0x05u
#define USB_DESC_HID        0x21u
#define USB_DESC_HIDREP     0x22u

/* ── USB class codes ─────────────────────────────────────────────────────── */
#define USB_CLASS_AUDIO    0x01u
#define USB_CLASS_HID      0x03u
#define USB_CLASS_MSC      0x08u
#define USB_CLASS_HUB      0x09u
#define USB_CLASS_VIDEO    0x0Eu
#define USB_CLASS_WIRELESS 0xE0u
#define USB_CLASS_VENDOR   0xFFu

/* ── HID subclass / protocol ─────────────────────────────────────────────── */
#define USB_HID_SC_BOOT    0x01u
#define USB_HID_P_KEYBOARD 0x01u
#define USB_HID_P_MOUSE    0x02u

/* ── Port speed values (as reported by xHCI PORTSC[13:10]) ─────────────── */
#define USB_SPEED_FULL   1u
#define USB_SPEED_LOW    2u
#define USB_SPEED_HIGH   3u
#define USB_SPEED_SUPER  4u
#define USB_SPEED_SUPERP 5u

static inline uint16_t usb_ep0_maxpkt(uint8_t spd) {
    return (spd >= USB_SPEED_SUPER) ? 512u : (spd == USB_SPEED_LOW ? 8u : 64u);
}

static inline const char *usb_speed_str(uint8_t spd) {
    switch (spd) {
    case USB_SPEED_LOW:    return "LS";
    case USB_SPEED_FULL:   return "FS";
    case USB_SPEED_HIGH:   return "HS";
    case USB_SPEED_SUPER:  return "SS";
    case USB_SPEED_SUPERP: return "SS+";
    default:               return "??";
    }
}

/* ── Device descriptor (18 bytes) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_dev_desc_t;

/* ── Configuration descriptor ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_cfg_desc_t;

/* ── Interface descriptor ───────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_iface_desc_t;

/* ── Endpoint descriptor ────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_ep_desc_t;

/* ── Device info record stored by xHCI driver ──────────────────────────── */
#define USB_MAX_DEV 16

typedef struct {
    uint8_t  valid;
    uint8_t  slot;      /* xHCI slot id (1-based)     */
    uint8_t  port;      /* 0-based root hub port index */
    uint8_t  speed;
    uint16_t vid, pid;
    uint8_t  dev_class, subclass, proto;
    uint8_t  max_pkt0;
    uint16_t bcd_usb;
} usb_device_t;

/* ── Public API (implemented in xhci.c) ─────────────────────────────────── */
int           usb_device_count(void);
usb_device_t *usb_get_device(int idx);

#endif
