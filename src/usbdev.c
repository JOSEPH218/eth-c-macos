/*
 * USB transport + RNDIS control-plane handshake.
 *
 * Android's tethering gadget (Linux u_ether "rndis" function) exposes
 * two USB interfaces: a CDC "control" interface with a single
 * interrupt-IN endpoint used to signal "a response is ready", and a
 * CDC "data" interface with a bulk-IN/bulk-OUT pair that carries
 * RNDIS-framed Ethernet frames. RNDIS control messages themselves ride
 * on endpoint 0 via the two CDC "encapsulated command" requests.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include "usbdev.h"
#include "rndis.h"
#include "log.h"

#define USB_CLASS_CDC_CONTROL 0x02
#define USB_CLASS_CDC_DATA    0x0A

#define REQTYPE_SEND (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT)
#define REQTYPE_RECV (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_IN)

#define CTRL_TIMEOUT_MS 2000
#define BULK_TIMEOUT_MS 2000

struct iface_candidate {
    int ifnum;
    int altsetting;
    int bulk_in_count;
    int bulk_out_count;
    int int_in_count;
    uint8_t ep_bulk_in;
    uint8_t ep_bulk_out;
    uint8_t ep_int_in;
};

static void classify_interface(const struct libusb_interface_descriptor *alt,
                                struct iface_candidate *out) {
    memset(out, 0, sizeof(*out));
    out->ifnum = alt->bInterfaceNumber;
    out->altsetting = alt->bAlternateSetting;

    for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
        uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
        int is_in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;

        if (type == LIBUSB_TRANSFER_TYPE_BULK) {
            if (is_in) { out->bulk_in_count++; out->ep_bulk_in = ep->bEndpointAddress; }
            else       { out->bulk_out_count++; out->ep_bulk_out = ep->bEndpointAddress; }
        } else if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && is_in) {
            out->int_in_count++;
            out->ep_int_in = ep->bEndpointAddress;
        }
    }
}

/* Looks for the { CDC-data with 2 bulk endpoints, paired CDC-control
 * with an interrupt-in endpoint on the adjacent lower interface number }
 * pattern. Returns 0 and fills the endpoint/interface fields of *dev on
 * a match. */
static int find_rndis_interfaces(struct libusb_device *usbdev, rndis_dev_t *dev) {
    struct libusb_config_descriptor *cfg;
    int rc = libusb_get_active_config_descriptor(usbdev, &cfg);
    if (rc != 0) {
        rc = libusb_get_config_descriptor(usbdev, 0, &cfg);
        if (rc != 0) return rc;
    }

    struct iface_candidate data_if = {0}, ctrl_if = {0};
    int have_data = 0, have_ctrl = 0;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            struct iface_candidate cand;
            classify_interface(alt, &cand);

            if (cand.bulk_in_count == 1 && cand.bulk_out_count == 1 &&
                cand.int_in_count == 0 && !have_data) {
                data_if = cand;
                have_data = 1;
            } else if (cand.bulk_in_count == 0 && cand.bulk_out_count == 0 &&
                       cand.int_in_count == 1 && !have_ctrl) {
                ctrl_if = cand;
                have_ctrl = 1;
            }
        }
    }

    libusb_free_config_descriptor(cfg);

    if (!have_data) return LIBUSB_ERROR_NOT_FOUND;

    dev->data_ifnum = data_if.ifnum;
    dev->ep_bulk_in = data_if.ep_bulk_in;
    dev->ep_bulk_out = data_if.ep_bulk_out;

    /* Control interface without an interrupt endpoint is workable too
     * (we fall back to polling GET_ENCAPSULATED_RESPONSE); only wire
     * up ep_int_in when we actually found one. */
    dev->control_ifnum = have_ctrl ? ctrl_if.ifnum : (data_if.ifnum > 0 ? data_if.ifnum - 1 : data_if.ifnum);
    dev->ep_int_in = have_ctrl ? ctrl_if.ep_int_in : 0;

    return 0;
}

static int open_matching_device(rndis_dev_t *dev, int vendor_filter, int product_filter) {
    libusb_device **list;
    ssize_t count = libusb_get_device_list(dev->ctx, &list);
    if (count < 0) return (int)count;

    int result = LIBUSB_ERROR_NOT_FOUND;

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;

        if (vendor_filter >= 0 && desc.idVendor != vendor_filter) continue;
        if (product_filter >= 0 && desc.idProduct != product_filter) continue;

        rndis_dev_t candidate = {0};
        if (find_rndis_interfaces(list[i], &candidate) != 0) continue;

        libusb_device_handle *handle;
        int rc = libusb_open(list[i], &handle);
        if (rc != 0) {
            log_warn("found candidate device %04x:%04x but couldn't open it (%s)",
                      desc.idVendor, desc.idProduct, libusb_error_name(rc));
            continue;
        }

        libusb_set_auto_detach_kernel_driver(handle, 1);

        rc = libusb_claim_interface(handle, candidate.control_ifnum);
        if (rc != 0 && candidate.control_ifnum != candidate.data_ifnum) {
            log_warn("claim of control interface %d failed: %s",
                      candidate.control_ifnum, libusb_error_name(rc));
        }
        rc = libusb_claim_interface(handle, candidate.data_ifnum);
        if (rc != 0) {
            log_warn("claim of data interface %d failed: %s",
                      candidate.data_ifnum, libusb_error_name(rc));
            libusb_close(handle);
            continue;
        }

        dev->handle = handle;
        dev->control_ifnum = candidate.control_ifnum;
        dev->data_ifnum = candidate.data_ifnum;
        dev->ep_bulk_in = candidate.ep_bulk_in;
        dev->ep_bulk_out = candidate.ep_bulk_out;
        dev->ep_int_in = candidate.ep_int_in;
        dev->next_request_id = 1;
        dev->max_transfer_size = RNDIS_MAX_FRAME_SIZE;

        log_info("bound to USB device %04x:%04x (control if=%d, data if=%d, bulk in=0x%02x out=0x%02x)",
                  desc.idVendor, desc.idProduct, dev->control_ifnum, dev->data_ifnum,
                  dev->ep_bulk_in, dev->ep_bulk_out);

        result = 0;
        break;
    }

    libusb_free_device_list(list, 1);
    return result;
}

int rndis_usb_autodetect(rndis_dev_t *dev, int vendor_id) {
    memset(dev, 0, sizeof(*dev));
    int rc = libusb_init(&dev->ctx);
    if (rc != 0) return rc;

    rc = open_matching_device(dev, vendor_id, -1);
    if (rc != 0) {
        libusb_exit(dev->ctx);
        memset(dev, 0, sizeof(*dev));
    }
    return rc;
}

int rndis_usb_open(rndis_dev_t *dev, uint16_t vendor_id, uint16_t product_id) {
    memset(dev, 0, sizeof(*dev));
    int rc = libusb_init(&dev->ctx);
    if (rc != 0) return rc;

    rc = open_matching_device(dev, vendor_id, product_id);
    if (rc != 0) {
        libusb_exit(dev->ctx);
        memset(dev, 0, sizeof(*dev));
    }
    return rc;
}

void rndis_usb_close(rndis_dev_t *dev) {
    if (!dev->handle) return;
    libusb_release_interface(dev->handle, dev->data_ifnum);
    if (dev->control_ifnum != dev->data_ifnum)
        libusb_release_interface(dev->handle, dev->control_ifnum);
    libusb_close(dev->handle);
    libusb_exit(dev->ctx);
    memset(dev, 0, sizeof(*dev));
}

static int send_encapsulated(rndis_dev_t *dev, const void *msg, uint16_t len) {
    int rc = libusb_control_transfer(dev->handle, REQTYPE_SEND,
                                      RNDIS_REQ_SEND_ENCAPSULATED_COMMAND,
                                      0, (uint16_t)dev->control_ifnum,
                                      (unsigned char *)msg, len, CTRL_TIMEOUT_MS);
    return rc < 0 ? rc : 0;
}

/* Waits for RESPONSE_AVAILABLE on the interrupt endpoint if we have
 * one; otherwise just gives the device a moment before polling, which
 * every RNDIS gadget we've tested tolerates fine. */
static void wait_for_response_ready(rndis_dev_t *dev) {
    if (dev->ep_int_in) {
        uint8_t notif[8];
        int actual = 0;
        libusb_interrupt_transfer(dev->handle, dev->ep_int_in, notif, sizeof(notif),
                                   &actual, CTRL_TIMEOUT_MS);
    } else {
        usleep(2000);
    }
}

static int recv_encapsulated(rndis_dev_t *dev, void *buf, uint16_t buflen) {
    wait_for_response_ready(dev);
    int rc = libusb_control_transfer(dev->handle, REQTYPE_RECV,
                                      RNDIS_REQ_GET_ENCAPSULATED_RESPONSE,
                                      0, (uint16_t)dev->control_ifnum,
                                      (unsigned char *)buf, buflen, CTRL_TIMEOUT_MS);
    return rc; /* number of bytes received, or negative libusb error */
}

int rndis_bring_up(rndis_dev_t *dev) {
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];

    /* --- INITIALIZE --- */
    struct rndis_initialize_msg init = {
        .message_type = RNDIS_MSG_INITIALIZE,
        .message_length = sizeof(init),
        .request_id = dev->next_request_id++,
        .major_version = RNDIS_MAJOR_VERSION,
        .minor_version = RNDIS_MINOR_VERSION,
        .max_transfer_size = RNDIS_MAX_FRAME_SIZE,
    };
    int rc = send_encapsulated(dev, &init, sizeof(init));
    if (rc != 0) { log_error("RNDIS initialize send failed: %s", libusb_error_name(rc)); return rc; }

    rc = recv_encapsulated(dev, buf, sizeof(buf));
    if (rc < (int)sizeof(struct rndis_initialize_cmplt)) {
        log_error("RNDIS initialize response too short (%d bytes)", rc);
        return LIBUSB_ERROR_IO;
    }
    struct rndis_initialize_cmplt *cmplt = (struct rndis_initialize_cmplt *)buf;
    if (cmplt->status != RNDIS_STATUS_SUCCESS) {
        log_error("RNDIS initialize rejected, status=0x%08x", cmplt->status);
        return LIBUSB_ERROR_IO;
    }
    if (cmplt->max_transfer_size > 0 && cmplt->max_transfer_size < dev->max_transfer_size)
        dev->max_transfer_size = cmplt->max_transfer_size;

    /* --- SET packet filter so the gadget actually forwards traffic --- */
    uint32_t filter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_MULTICAST;
    uint8_t setbuf[sizeof(struct rndis_set_msg) + sizeof(filter)];
    struct rndis_set_msg *set = (struct rndis_set_msg *)setbuf;
    set->message_type = RNDIS_MSG_SET;
    set->message_length = sizeof(setbuf);
    set->request_id = dev->next_request_id++;
    set->oid = OID_GEN_CURRENT_PACKET_FILTER;
    set->info_buffer_length = sizeof(filter);
    set->info_buffer_offset = RNDIS_INFO_BUFFER_OFFSET(struct rndis_set_msg);
    memcpy(setbuf + sizeof(*set), &filter, sizeof(filter));

    rc = send_encapsulated(dev, setbuf, sizeof(setbuf));
    if (rc != 0) { log_error("RNDIS set packet filter failed: %s", libusb_error_name(rc)); return rc; }
    rc = recv_encapsulated(dev, buf, sizeof(buf));
    if (rc < (int)sizeof(struct rndis_set_cmplt)) {
        log_error("RNDIS set-filter response too short (%d bytes)", rc);
        return LIBUSB_ERROR_IO;
    }

    /* --- QUERY MAC address --- */
    struct rndis_query_msg query = {
        .message_type = RNDIS_MSG_QUERY,
        .message_length = sizeof(query),
        .request_id = dev->next_request_id++,
        .oid = OID_802_3_CURRENT_ADDRESS,
        .info_buffer_length = 0,
        .info_buffer_offset = 0,
        .reserved = 0,
    };
    rc = send_encapsulated(dev, &query, sizeof(query));
    if (rc != 0) { log_error("RNDIS query MAC failed: %s", libusb_error_name(rc)); return rc; }
    rc = recv_encapsulated(dev, buf, sizeof(buf));
    if (rc < (int)sizeof(struct rndis_query_cmplt)) {
        log_error("RNDIS query-MAC response too short (%d bytes)", rc);
        return LIBUSB_ERROR_IO;
    }
    struct rndis_query_cmplt *qc = (struct rndis_query_cmplt *)buf;
    if (qc->status != RNDIS_STATUS_SUCCESS || qc->info_buffer_length < ETH_ALEN) {
        log_error("RNDIS query-MAC rejected or too short, status=0x%08x len=%u",
                   qc->status, qc->info_buffer_length);
        return LIBUSB_ERROR_IO;
    }
    const uint8_t *mac_ptr = buf + offsetof(struct rndis_query_cmplt, request_id) + qc->info_buffer_offset;
    memcpy(dev->mac, mac_ptr, ETH_ALEN);

    log_info("device MAC %02x:%02x:%02x:%02x:%02x:%02x, max transfer size %u",
              dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5],
              dev->max_transfer_size);
    return 0;
}

int rndis_send_frame(rndis_dev_t *dev, const uint8_t *frame, size_t len) {
    if (len > RNDIS_MAX_FRAME_SIZE) return -EMSGSIZE;

    uint8_t out[sizeof(struct rndis_packet_msg) + RNDIS_MAX_FRAME_SIZE];
    struct rndis_packet_msg *pkt = (struct rndis_packet_msg *)out;
    memset(pkt, 0, sizeof(*pkt));
    pkt->message_type = RNDIS_MSG_PACKET;
    pkt->message_length = (uint32_t)(sizeof(*pkt) + len);
    pkt->data_offset = RNDIS_PACKET_MSG_DATA_OFFSET;
    pkt->data_length = (uint32_t)len;
    memcpy(out + sizeof(*pkt), frame, len);

    int actual = 0;
    int rc = libusb_bulk_transfer(dev->handle, dev->ep_bulk_out, out,
                                   (int)(sizeof(*pkt) + len), &actual, BULK_TIMEOUT_MS);
    if (rc != 0) return rc;
    return actual == (int)(sizeof(*pkt) + len) ? 0 : LIBUSB_ERROR_IO;
}

int rndis_recv_frame(rndis_dev_t *dev, uint8_t *buf, size_t buflen, unsigned timeout_ms) {
    uint8_t raw[sizeof(struct rndis_packet_msg) + RNDIS_MAX_FRAME_SIZE];
    int actual = 0;
    int rc = libusb_bulk_transfer(dev->handle, dev->ep_bulk_in, raw, sizeof(raw), &actual, timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT) return 0;
    if (rc != 0) return rc;
    if ((size_t)actual < sizeof(struct rndis_packet_msg)) return -EIO;

    struct rndis_packet_msg *pkt = (struct rndis_packet_msg *)raw;
    if (pkt->message_type != RNDIS_MSG_PACKET) return 0; /* ignore stray control chatter */

    size_t data_off = offsetof(struct rndis_packet_msg, data_offset) + pkt->data_offset;
    if (data_off + pkt->data_length > (size_t)actual) return -EIO;
    if (pkt->data_length > buflen) return -EMSGSIZE;

    memcpy(buf, raw + data_off, pkt->data_length);
    return (int)pkt->data_length;
}
