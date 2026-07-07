/*
 * Minimal DHCP client that speaks directly over the RNDIS Ethernet
 * link. We deliberately don't hand this off to the OS's own DHCP
 * client: until we've configured the utun interface there is no real
 * network interface for the OS to run DHCP against, so we hand-build
 * BOOTP/DHCP frames the same way a kernel would and feed them through
 * rndis_send_frame/rndis_recv_frame ourselves.
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "dhcp.h"
#include "rndis.h"
#include "ip.h"
#include "log.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_COOKIE 0x63825363u

#define BOOTP_OP_REQUEST 1
#define BOOTP_OP_REPLY   2
#define BOOTP_HTYPE_ETHERNET 1

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define OPT_PAD 0
#define OPT_SUBNET_MASK 1
#define OPT_ROUTER 3
#define OPT_DNS 6
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME 51
#define OPT_MSG_TYPE 53
#define OPT_SERVER_ID 54
#define OPT_PARAM_REQUEST_LIST 55
#define OPT_END 255

#define BOOTP_FIXED_LEN 236 /* up to and including the `file` field */
#define DHCP_PAYLOAD_LEN 300 /* padded, like most DHCP clients send */

#pragma pack(push, 1)
struct bootp_fixed {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic_cookie;
};
#pragma pack(pop)

static uint32_t make_xid(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t seed = (uint32_t)(ts.tv_nsec ^ ts.tv_sec ^ getpid());
    srand(seed);
    return (uint32_t)rand();
}

static uint8_t *put_option_bytes(uint8_t *p, uint8_t tag, const void *data, uint8_t len) {
    *p++ = tag;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static size_t build_dhcp_payload(uint8_t *out, uint32_t xid, const uint8_t mac[ETH_ALEN],
                                  uint8_t msg_type, uint32_t requested_ip, uint32_t server_id) {
    memset(out, 0, DHCP_PAYLOAD_LEN);
    struct bootp_fixed *b = (struct bootp_fixed *)out;
    b->op = BOOTP_OP_REQUEST;
    b->htype = BOOTP_HTYPE_ETHERNET;
    b->hlen = ETH_ALEN;
    b->xid = xid;
    b->flags = htons(0x8000); /* ask for a broadcast reply, we have no IP yet */
    memcpy(b->chaddr, mac, ETH_ALEN);
    b->magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    uint8_t *p = out + sizeof(struct bootp_fixed);
    uint8_t type = msg_type;
    p = put_option_bytes(p, OPT_MSG_TYPE, &type, 1);

    if (msg_type == DHCP_MSG_REQUEST) {
        p = put_option_bytes(p, OPT_REQUESTED_IP, &requested_ip, 4);
        p = put_option_bytes(p, OPT_SERVER_ID, &server_id, 4);
    }

    uint8_t params[] = {OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE_TIME};
    p = put_option_bytes(p, OPT_PARAM_REQUEST_LIST, params, sizeof(params));

    *p++ = OPT_END;
    return DHCP_PAYLOAD_LEN; /* fixed-size, zero-padded frame */
}

static size_t wrap_in_ip_udp_eth(const uint8_t src_mac[ETH_ALEN], uint32_t src_ip,
                                  uint32_t dst_ip, const uint8_t *payload, size_t payload_len,
                                  uint8_t *out) {
    struct eth_header *eth = (struct eth_header *)out;
    memcpy(eth->dst, ETH_BROADCAST, ETH_ALEN);
    memcpy(eth->src, src_mac, ETH_ALEN);
    eth->ethertype = htons(ETHERTYPE_IP);

    struct ip_header *ip = (struct ip_header *)(out + sizeof(*eth));
    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->total_length = htons((uint16_t)(sizeof(*ip) + sizeof(struct udp_header) + payload_len));
    ip->flags_frag = htons(0x4000); /* don't fragment */
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP_;
    ip->src = src_ip;
    ip->dst = dst_ip;
    ip->checksum = htons(inet_checksum(ip, sizeof(*ip)));

    struct udp_header *udp = (struct udp_header *)((uint8_t *)ip + sizeof(*ip));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length = htons((uint16_t)(sizeof(*udp) + payload_len));
    udp->checksum = 0; /* optional over IPv4; skipped for simplicity */

    memcpy((uint8_t *)udp + sizeof(*udp), payload, payload_len);

    return sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + payload_len;
}

/* Pulls DHCP options out of a BOOTP frame we received. Returns 1 and
 * points *data at the option's payload (still inside `frame`) on a
 * match, 0 if the option isn't present. */
static int find_option(const uint8_t *options, size_t options_len, uint8_t tag,
                        const uint8_t **data, uint8_t *len) {
    size_t i = 0;
    while (i < options_len) {
        uint8_t t = options[i];
        if (t == OPT_END) break;
        if (t == OPT_PAD) { i++; continue; }
        if (i + 1 >= options_len) break;
        uint8_t l = options[i + 1];
        if (i + 2 + l > options_len) break;
        if (t == tag) {
            *data = &options[i + 2];
            *len = l;
            return 1;
        }
        i += 2 + l;
    }
    return 0;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits for the next DHCP reply matching `xid` addressed to our MAC.
 * Returns the parsed message type (DHCP_MSG_*) and fills `lease`
 * fields that were present in the reply's options; 0 on timeout,
 * negative on hard error. */
static int wait_for_reply(rndis_dev_t *dev, uint32_t xid, const uint8_t our_mac[ETH_ALEN],
                           int64_t deadline_ms, struct dhcp_lease *lease, uint32_t *offered_ip) {
    uint8_t frame[RNDIS_MAX_FRAME_SIZE];

    while (now_ms() < deadline_ms) {
        unsigned remaining = (unsigned)(deadline_ms - now_ms());
        int n = rndis_recv_frame(dev, frame, sizeof(frame), remaining > 500 ? 500 : remaining);
        if (n < 0) return n;
        if (n == 0) continue;
        if ((size_t)n < sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + sizeof(struct bootp_fixed))
            continue;

        struct eth_header *eth = (struct eth_header *)frame;
        if (ntohs(eth->ethertype) != ETHERTYPE_IP) continue;
        if (memcmp(eth->dst, our_mac, ETH_ALEN) != 0 && memcmp(eth->dst, ETH_BROADCAST, ETH_ALEN) != 0) continue;

        struct ip_header *ip = (struct ip_header *)(frame + sizeof(*eth));
        if (ip->protocol != IPPROTO_UDP_) continue;
        size_t ip_hdr_len = (size_t)(ip->version_ihl & 0x0F) * 4;

        struct udp_header *udp = (struct udp_header *)(frame + sizeof(*eth) + ip_hdr_len);
        if (ntohs(udp->src_port) != DHCP_SERVER_PORT || ntohs(udp->dst_port) != DHCP_CLIENT_PORT) continue;

        struct bootp_fixed *b = (struct bootp_fixed *)((uint8_t *)udp + sizeof(*udp));
        if (b->op != BOOTP_OP_REPLY) continue;
        if (b->xid != xid) continue;
        if (ntohl(b->magic_cookie) != DHCP_MAGIC_COOKIE) continue;

        size_t consumed = sizeof(*eth) + ip_hdr_len + sizeof(*udp) + sizeof(struct bootp_fixed);
        const uint8_t *options = frame + consumed;
        size_t options_len = (size_t)n - consumed;

        const uint8_t *data; uint8_t len;
        if (!find_option(options, options_len, OPT_MSG_TYPE, &data, &len) || len < 1) continue;
        int msg_type = data[0];

        *offered_ip = b->yiaddr;

        if (find_option(options, options_len, OPT_SUBNET_MASK, &data, &len) && len == 4)
            memcpy(&lease->netmask, data, 4);
        if (find_option(options, options_len, OPT_ROUTER, &data, &len) && len >= 4)
            memcpy(&lease->gateway_ip, data, 4);
        if (find_option(options, options_len, OPT_SERVER_ID, &data, &len) && len == 4)
            memcpy(&lease->server_ip, data, 4);
        if (find_option(options, options_len, OPT_LEASE_TIME, &data, &len) && len == 4) {
            uint32_t t; memcpy(&t, data, 4); lease->lease_seconds = ntohl(t);
        }
        if (find_option(options, options_len, OPT_DNS, &data, &len) && len >= 4) {
            lease->dns_count = 0;
            for (int off = 0; off + 4 <= len && lease->dns_count < 4; off += 4)
                memcpy(&lease->dns[lease->dns_count++], data + off, 4);
        }

        return msg_type;
    }
    return 0;
}

int dhcp_acquire_lease(rndis_dev_t *dev, const uint8_t our_mac[ETH_ALEN],
                        struct dhcp_lease *lease, unsigned timeout_ms) {
    memset(lease, 0, sizeof(*lease));

    uint8_t payload[DHCP_PAYLOAD_LEN];
    uint8_t frame[sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + DHCP_PAYLOAD_LEN];

    uint32_t xid = make_xid();
    int64_t deadline = now_ms() + timeout_ms;

    /* --- DISCOVER --- */
    build_dhcp_payload(payload, xid, our_mac, DHCP_MSG_DISCOVER, 0, 0);
    size_t flen = wrap_in_ip_udp_eth(our_mac, 0, 0xFFFFFFFFu, payload, sizeof(payload), frame);
    log_info("DHCP: sending DISCOVER (xid=0x%08x)", xid);
    int rc = rndis_send_frame(dev, frame, flen);
    if (rc != 0) return rc;

    uint32_t offered_ip = 0;
    int msg_type = wait_for_reply(dev, xid, our_mac, deadline, lease, &offered_ip);
    if (msg_type <= 0 || msg_type != DHCP_MSG_OFFER) {
        log_error("DHCP: no OFFER received before timeout");
        return -1;
    }
    log_info("DHCP: got OFFER of %s", inet_ntoa(*(struct in_addr *)&offered_ip));

    /* --- REQUEST --- */
    build_dhcp_payload(payload, xid, our_mac, DHCP_MSG_REQUEST, offered_ip, lease->server_ip);
    flen = wrap_in_ip_udp_eth(our_mac, 0, 0xFFFFFFFFu, payload, sizeof(payload), frame);
    log_info("DHCP: sending REQUEST for %s", inet_ntoa(*(struct in_addr *)&offered_ip));
    rc = rndis_send_frame(dev, frame, flen);
    if (rc != 0) return rc;

    uint32_t acked_ip = 0;
    msg_type = wait_for_reply(dev, xid, our_mac, deadline, lease, &acked_ip);
    if (msg_type != DHCP_MSG_ACK) {
        log_error("DHCP: request was %s", msg_type == DHCP_MSG_NAK ? "NAK'd" : "not acknowledged in time");
        return -1;
    }

    lease->your_ip = acked_ip;
    log_info("DHCP: bound %s, gateway %s, lease %u s",
              inet_ntoa(*(struct in_addr *)&lease->your_ip),
              inet_ntoa(*(struct in_addr *)&lease->gateway_ip),
              lease->lease_seconds);
    return 0;
}
