#include <string.h>
#include <arpa/inet.h>

#include "arp.h"

#define ARP_HTYPE_ETHERNET 1
#define ARP_HLEN_ETHERNET  6
#define ARP_PLEN_IPV4      4

static void fill_common(struct arp_frame *f, uint16_t oper) {
    f->eth.ethertype = htons(ETHERTYPE_ARP);
    f->htype = htons(ARP_HTYPE_ETHERNET);
    f->ptype = htons(ETHERTYPE_IP);
    f->hlen = ARP_HLEN_ETHERNET;
    f->plen = ARP_PLEN_IPV4;
    f->oper = htons(oper);
}

size_t arp_build_request(const uint8_t src_mac[ETH_ALEN], uint32_t src_ip,
                          uint32_t target_ip, uint8_t *out) {
    struct arp_frame f;
    memset(&f, 0, sizeof(f));

    memcpy(f.eth.dst, ETH_BROADCAST, ETH_ALEN);
    memcpy(f.eth.src, src_mac, ETH_ALEN);
    fill_common(&f, ARP_OP_REQUEST);

    memcpy(f.sender_mac, src_mac, ETH_ALEN);
    f.sender_ip = src_ip;
    memset(f.target_mac, 0, ETH_ALEN);
    f.target_ip = target_ip;

    memcpy(out, &f, sizeof(f));
    return sizeof(f);
}

size_t arp_build_reply(const uint8_t src_mac[ETH_ALEN], uint32_t src_ip,
                        const uint8_t dst_mac[ETH_ALEN], uint32_t dst_ip,
                        uint8_t *out) {
    struct arp_frame f;
    memset(&f, 0, sizeof(f));

    memcpy(f.eth.dst, dst_mac, ETH_ALEN);
    memcpy(f.eth.src, src_mac, ETH_ALEN);
    fill_common(&f, ARP_OP_REPLY);

    memcpy(f.sender_mac, src_mac, ETH_ALEN);
    f.sender_ip = src_ip;
    memcpy(f.target_mac, dst_mac, ETH_ALEN);
    f.target_ip = dst_ip;

    memcpy(out, &f, sizeof(f));
    return sizeof(f);
}

int arp_parse(const uint8_t *data, size_t len, struct arp_frame *frame) {
    if (len < sizeof(struct arp_frame)) return 0;

    memcpy(frame, data, sizeof(struct arp_frame));
    if (ntohs(frame->eth.ethertype) != ETHERTYPE_ARP) return 0;
    if (ntohs(frame->htype) != ARP_HTYPE_ETHERNET) return 0;
    if (ntohs(frame->ptype) != ETHERTYPE_IP) return 0;
    if (frame->hlen != ARP_HLEN_ETHERNET || frame->plen != ARP_PLEN_IPV4) return 0;

    frame->oper = ntohs(frame->oper);
    return 1;
}
