#ifndef ETHC_DHCP_H
#define ETHC_DHCP_H

#include <stdint.h>
#include "eth.h"
#include "usbdev.h"

struct dhcp_lease {
    uint32_t your_ip;      /* network byte order */
    uint32_t server_ip;    /* network byte order */
    uint32_t gateway_ip;   /* network byte order, 0 if none offered */
    uint32_t netmask;      /* network byte order */
    uint32_t dns[4];       /* network byte order */
    int dns_count;
    uint32_t lease_seconds;
};

/* Runs a minimal DHCPDISCOVER -> OFFER -> REQUEST -> ACK exchange
 * directly over the RNDIS Ethernet link (no OS IP stack involved:
 * frames are hand-built and sent/received via rndis_send_frame /
 * rndis_recv_frame). This is how we learn the IP address, netmask,
 * gateway and DNS servers that Android's tether DHCP server (dnsmasq)
 * would normally hand out to a real Ethernet client.
 *
 * Returns 0 and fills *lease on success, negative on timeout/error. */
int dhcp_acquire_lease(rndis_dev_t *dev, const uint8_t our_mac[ETH_ALEN],
                        struct dhcp_lease *lease, unsigned timeout_ms);

#endif /* ETHC_DHCP_H */
