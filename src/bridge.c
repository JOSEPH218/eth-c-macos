/*
 * Glues the RNDIS Ethernet link to the local utun IP interface.
 *
 * The phone's tether interface is Ethernet, utun is IP-only, so we
 * strip/add a 14-byte Ethernet header at this boundary. Since this is
 * effectively a point-to-point link with exactly one peer (the
 * phone's gadget), we don't need a real ARP table - just enough ARP
 * to answer "who has our_ip" so the phone's side can address unicast
 * replies (DNS answers, etc.) back to us.
 */
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "bridge.h"
#include "rndis.h"
#include "arp.h"
#include "log.h"

struct bridge_ctx {
    rndis_dev_t *dev;
    utun_dev_t *tun;
    uint8_t our_mac[ETH_ALEN];
    uint32_t our_ip;
    uint32_t gateway_ip;
    uint8_t gateway_mac[ETH_ALEN];
    volatile sig_atomic_t *keep_running;
};

static void handle_arp(struct bridge_ctx *ctx, const uint8_t *buf, size_t len) {
    struct arp_frame arp;
    if (!arp_parse(buf, len, &arp)) return;
    if (arp.oper != ARP_OP_REQUEST) return;
    if (arp.target_ip != ctx->our_ip) return;

    uint8_t reply[sizeof(struct arp_frame)];
    size_t rlen = arp_build_reply(ctx->our_mac, ctx->our_ip, arp.sender_mac, arp.sender_ip, reply);
    int rc = rndis_send_frame(ctx->dev, reply, rlen);
    if (rc != 0) log_warn("failed to send ARP reply: %d", rc);
    else log_info("answered ARP request for our IP");
}

/* RNDIS -> utun: unwrap Ethernet frames from the phone, hand IP
 * payloads to the kernel's utun interface, answer ARP inline. */
static void *rndis_to_utun_loop(void *arg) {
    struct bridge_ctx *ctx = arg;
    uint8_t frame[RNDIS_MAX_FRAME_SIZE];

    while (*ctx->keep_running) {
        int n = rndis_recv_frame(ctx->dev, frame, sizeof(frame), 200);
        if (n < 0) {
            log_error("RNDIS read error (%d), stopping", n);
            break;
        }
        if (n == 0) continue;
        if ((size_t)n < sizeof(struct eth_header)) continue;

        struct eth_header *eth = (struct eth_header *)frame;
        uint16_t ethertype = ntohs(eth->ethertype);

        if (ethertype == ETHERTYPE_ARP) {
            handle_arp(ctx, frame, (size_t)n);
            continue;
        }
        if (ethertype != ETHERTYPE_IP) continue;

        const uint8_t *ip_payload = frame + sizeof(struct eth_header);
        size_t ip_len = (size_t)n - sizeof(struct eth_header);
        int wrc = utun_write(ctx->tun, ip_payload, ip_len);
        if (wrc < 0) log_warn("utun write failed (%d)", wrc);
    }
    return NULL;
}

/* utun -> RNDIS: read IP packets the Mac wants to send out and wrap
 * them in an Ethernet frame addressed to the phone's gadget MAC. */
static void utun_to_rndis_loop(struct bridge_ctx *ctx) {
    uint8_t frame[sizeof(struct eth_header) + RNDIS_MAX_FRAME_SIZE];
    struct pollfd pfd = {.fd = ctx->tun->fd, .events = POLLIN};

    while (*ctx->keep_running) {
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            log_error("poll on utun failed");
            break;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) continue;

        struct eth_header *eth = (struct eth_header *)frame;
        int n = utun_read(ctx->tun, frame + sizeof(*eth), sizeof(frame) - sizeof(*eth));
        if (n < 0) {
            log_error("utun read error (%d), stopping", n);
            break;
        }
        if (n == 0) continue;

        memcpy(eth->dst, ctx->gateway_mac, ETH_ALEN);
        memcpy(eth->src, ctx->our_mac, ETH_ALEN);
        eth->ethertype = htons(ETHERTYPE_IP);

        int rc = rndis_send_frame(ctx->dev, frame, sizeof(*eth) + (size_t)n);
        if (rc != 0) log_warn("RNDIS send failed (%d)", rc);
    }
}

int bridge_run(rndis_dev_t *dev, utun_dev_t *tun, const uint8_t our_mac[ETH_ALEN],
                uint32_t our_ip, uint32_t gateway_ip, const uint8_t gateway_mac[ETH_ALEN],
                volatile sig_atomic_t *keep_running) {
    struct bridge_ctx ctx;
    ctx.dev = dev;
    ctx.tun = tun;
    memcpy(ctx.our_mac, our_mac, ETH_ALEN);
    ctx.our_ip = our_ip;
    ctx.gateway_ip = gateway_ip;
    memcpy(ctx.gateway_mac, gateway_mac, ETH_ALEN);
    ctx.keep_running = keep_running;

    pthread_t rx_thread;
    int rc = pthread_create(&rx_thread, NULL, rndis_to_utun_loop, &ctx);
    if (rc != 0) {
        log_error("failed to spawn RNDIS->utun thread: %d", rc);
        return -1;
    }

    log_info("bridge running (Ctrl+C to stop)");
    utun_to_rndis_loop(&ctx);

    *keep_running = 0;
    pthread_join(rx_thread, NULL);
    return 0;
}
