#ifndef ETHC_ETH_H
#define ETHC_ETH_H

#include <stdint.h>
#include <stddef.h>

#define ETH_ALEN 6
#define ETH_MTU_PAYLOAD 1500

#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806

extern const uint8_t ETH_BROADCAST[ETH_ALEN];

#pragma pack(push, 1)
struct eth_header {
    uint8_t dst[ETH_ALEN];
    uint8_t src[ETH_ALEN];
    uint16_t ethertype; /* network byte order */
};
#pragma pack(pop)

/* One's-complement checksum as used by IP/UDP/TCP headers. `data` is
 * summed as network-byte-order 16-bit words; pass an odd length and
 * the trailing byte is treated as padded with a zero low byte. */
uint16_t inet_checksum(const void *data, size_t len);

#endif /* ETHC_ETH_H */
