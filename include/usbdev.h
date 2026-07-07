#ifndef ETHC_USBDEV_H
#define ETHC_USBDEV_H

#include <stdint.h>
#include <stddef.h>
#include <libusb-1.0/libusb.h>
#include "eth.h"

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *handle;

    int control_ifnum;
    int data_ifnum;

    uint8_t ep_int_in;   /* notification endpoint, 0 if none present */
    uint8_t ep_bulk_in;
    uint8_t ep_bulk_out;

    uint32_t next_request_id;
    uint32_t max_transfer_size;

    uint8_t mac[ETH_ALEN];
} rndis_dev_t;

/* Vendor ID Samsung ships on virtually all Galaxy phones/tablets. Used
 * as the default autodetect filter; callers can override with an
 * explicit vid/pid via rndis_usb_open(). */
#define SAMSUNG_USB_VENDOR_ID 0x04E8

/* Scans all attached USB devices for one exposing the CDC control +
 * CDC data interface pair that Android's RNDIS gadget presents, whose
 * vendor ID is `vendor_id` (pass -1 to match any vendor). Returns 0 and
 * fills *dev on success, negative libusb error / -ENODEV otherwise. */
int rndis_usb_autodetect(rndis_dev_t *dev, int vendor_id);

/* Opens a specific vendor:product device instead of autodetecting. */
int rndis_usb_open(rndis_dev_t *dev, uint16_t vendor_id, uint16_t product_id);

void rndis_usb_close(rndis_dev_t *dev);

/* Performs the RNDIS INITIALIZE handshake, sets the packet filter so
 * the gadget starts passing traffic, and queries the device's MAC
 * address into dev->mac. Must be called once after open/autodetect. */
int rndis_bring_up(rndis_dev_t *dev);

/* Sends a raw Ethernet frame (dst+src+ethertype+payload) to the device,
 * wrapping it in an RNDIS PACKET_MSG. */
int rndis_send_frame(rndis_dev_t *dev, const uint8_t *frame, size_t len);

/* Blocks (up to timeout_ms, 0 = no timeout) for the next Ethernet frame
 * from the device, unwraps it from its RNDIS PACKET_MSG, and copies it
 * into buf. Returns the frame length, 0 on timeout, negative on error. */
int rndis_recv_frame(rndis_dev_t *dev, uint8_t *buf, size_t buflen, unsigned timeout_ms);

#endif /* ETHC_USBDEV_H */
