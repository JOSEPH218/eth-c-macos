#ifndef ETHC_ARP_H
#define ETHC_ARP_H

#include <stdint.h>
#include <stddef.h>
#include "eth.h"

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#pragma pack(push, 1)
struct arp_frame {
    struct eth_header eth;
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sender_mac[ETH_ALEN];
    uint32_t sender_ip; /* network byte order */
    uint8_t target_mac[ETH_ALEN];
    uint32_t target_ip; /* network byte order */
};
#pragma pack(pop)

/* Builds a broadcast "who has target_ip" ARP request into `out`
 * (must hold at least sizeof(struct arp_frame) bytes). Returns the
 * frame length. */
size_t arp_build_request(const uint8_t src_mac[ETH_ALEN], uint32_t src_ip,
                          uint32_t target_ip, uint8_t *out);

/* Builds a unicast ARP reply "src_ip is at src_mac" addressed to the
 * original requester (dst_mac/dst_ip). */
size_t arp_build_reply(const uint8_t src_mac[ETH_ALEN], uint32_t src_ip,
                        const uint8_t dst_mac[ETH_ALEN], uint32_t dst_ip,
                        uint8_t *out);

/* Returns 1 and fills *frame if `data` is a well-formed Ethernet+ARP
 * frame for IPv4/Ethernet, 0 otherwise. */
int arp_parse(const uint8_t *data, size_t len, struct arp_frame *frame);

#endif /* ETHC_ARP_H */
