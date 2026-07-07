/*
 * eth-c-macos: USB tethering for macOS without a kernel extension.
 *
 * Talks RNDIS to an Android phone's tether gadget over libusb, runs a
 * hand-rolled DHCP client to learn an IP/gateway/DNS from it, then
 * bridges IP traffic between the phone and a macOS utun interface.
 * See README.md for the full picture and docs/DESIGN.md for the wire
 * protocol details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "usbdev.h"
#include "dhcp.h"
#include "utun.h"
#include "bridge.h"
#include "log.h"

static volatile sig_atomic_t g_keep_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static void generate_locally_administered_mac(uint8_t mac[ETH_ALEN]) {
    srand((unsigned)(time(NULL) ^ getpid()));
    for (int i = 0; i < ETH_ALEN; i++) mac[i] = (uint8_t)(rand() & 0xFF);
    mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02); /* unicast, locally administered */
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [--vendor 0xHHHH] [--product 0xHHHH] [--timeout SECONDS] [-q]\n"
        "  --vendor/--product  match a specific USB device instead of autodetecting\n"
        "                      any Samsung (0x%04x) RNDIS gadget\n"
        "  --timeout           DHCP timeout in seconds (default 15)\n"
        "  -q                  quiet (errors/warnings only)\n",
        argv0, SAMSUNG_USB_VENDOR_ID);
}

int main(int argc, char **argv) {
    int vendor = -1, product = -1;
    unsigned dhcp_timeout_s = 15;
    log_set_verbose(1);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--vendor") == 0 && i + 1 < argc) {
            vendor = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--product") == 0 && i + 1 < argc) {
            product = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            dhcp_timeout_s = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-q") == 0) {
            log_set_verbose(0);
        } else {
            usage(argv[0]);
            return (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) ? 0 : 1;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "This needs to run as root (raw USB access + creating a utun interface).\n"
                         "Try: sudo %s\n", argv[0]);
        return 1;
    }

    rndis_dev_t dev;
    int rc = (vendor >= 0)
                 ? rndis_usb_open(&dev, (uint16_t)vendor, (uint16_t)product)
                 : rndis_usb_autodetect(&dev, SAMSUNG_USB_VENDOR_ID);
    if (rc != 0) {
        log_error("no matching USB tethering device found (is USB tethering enabled on the phone,"
                   " and the cable plugged in?). libusb error %d", rc);
        return 1;
    }

    if (rndis_bring_up(&dev) != 0) {
        log_error("RNDIS handshake with the phone failed");
        rndis_usb_close(&dev);
        return 1;
    }

    uint8_t our_mac[ETH_ALEN];
    generate_locally_administered_mac(our_mac);

    struct dhcp_lease lease;
    if (dhcp_acquire_lease(&dev, our_mac, &lease, dhcp_timeout_s * 1000) != 0) {
        log_error("could not get an IP address from the phone's tethering DHCP server");
        rndis_usb_close(&dev);
        return 1;
    }

    if (lease.gateway_ip == 0) {
        /* Some gadgets omit the router option since, on this link,
         * the far end of the cable (the gadget itself) is the only
         * possible next hop anyway. */
        lease.gateway_ip = lease.server_ip;
        log_warn("DHCP reply had no router option, assuming gateway = DHCP server (%s)",
                  inet_ntoa(*(struct in_addr *)&lease.gateway_ip));
    }

    utun_dev_t tun;
    if (utun_open(&tun) != 0) {
        log_error("failed to create utun interface");
        rndis_usb_close(&dev);
        return 1;
    }

    char local_ip[INET_ADDRSTRLEN], gateway_ip[INET_ADDRSTRLEN];
    snprintf(local_ip, sizeof(local_ip), "%s", inet_ntoa(*(struct in_addr *)&lease.your_ip));
    snprintf(gateway_ip, sizeof(gateway_ip), "%s", inet_ntoa(*(struct in_addr *)&lease.gateway_ip));

    char dns_strs[4][INET_ADDRSTRLEN];
    const char *dns_ptrs[4];
    for (int i = 0; i < lease.dns_count; i++) {
        snprintf(dns_strs[i], sizeof(dns_strs[i]), "%s", inet_ntoa(*(struct in_addr *)&lease.dns[i]));
        dns_ptrs[i] = dns_strs[i];
    }

    if (utun_configure(&tun, local_ip, gateway_ip, dns_ptrs, lease.dns_count) != 0) {
        log_error("failed to configure %s", tun.name);
        utun_close(&tun);
        rndis_usb_close(&dev);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    log_info("tethering up: %s is %s, internet via phone (%02x:%02x:%02x:%02x:%02x:%02x)",
              tun.name, local_ip,
              dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);

    /* On this point-to-point link the phone's gadget is the only
     * possible peer, so its RNDIS-reported MAC doubles as the
     * "gateway MAC" - no ARP resolution needed for outbound traffic,
     * only for answering the phone's ARP requests for our address
     * (handled inside bridge_run). */
    bridge_run(&dev, &tun, our_mac, lease.your_ip, lease.gateway_ip, dev.mac, &g_keep_running);

    log_info("shutting down");
    utun_restore_route();
    utun_close(&tun);
    rndis_usb_close(&dev);
    return 0;
}
