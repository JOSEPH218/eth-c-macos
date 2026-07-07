#ifndef ETHC_IP_H
#define ETHC_IP_H

#include <stdint.h>

#define IPPROTO_UDP_ 17

#pragma pack(push, 1)
struct ip_header {
    uint8_t version_ihl;   /* version:4, IHL:4 (in 32-bit words) */
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
};

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
};
#pragma pack(pop)

#endif /* ETHC_IP_H */
