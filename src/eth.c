#include "eth.h"

const uint8_t ETH_BROADCAST[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

uint16_t inet_checksum(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint32_t)((bytes[0] << 8) | bytes[1]);
        bytes += 2;
        len -= 2;
    }
    if (len == 1) sum += (uint32_t)(bytes[0] << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}
