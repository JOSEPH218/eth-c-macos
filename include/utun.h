#ifndef ETHC_UTUN_H
#define ETHC_UTUN_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int fd;
    char name[16]; /* e.g. "utun7" (or "tun0" in the Linux dev stub) */
} utun_dev_t;

/* Creates a point-to-point IP tunnel interface (macOS: a kernel utun
 * via the PF_SYSTEM/SYSPROTO_CONTROL control socket API; no kernel
 * extension involved). Does not assign an address - call
 * utun_configure() once DHCP has told us what to assign. */
int utun_open(utun_dev_t *tun);

void utun_close(utun_dev_t *tun);

/* Brings the interface up with `local_ip` as our address and `peer_ip`
 * as the point-to-point far end, then replaces the current default
 * route with one through this interface and points DNS at `dns`.
 * Shells out to ifconfig/route (via exec, not a shell, so
 * network-supplied strings can't be interpreted as shell syntax)
 * since neither is exposed as an in-process macOS API. */
int utun_configure(utun_dev_t *tun, const char *local_ip, const char *peer_ip,
                    const char *dns[], int dns_count);

/* Restores whatever default route existed before utun_configure()
 * replaced it. Best-effort - call before exiting. */
void utun_restore_route(void);

/* Reads one raw IP packet (no link-layer/protocol-family header) into
 * buf. Returns packet length, negative on error. */
int utun_read(utun_dev_t *tun, uint8_t *buf, size_t buflen);

/* Writes one raw IP packet to the tunnel. */
int utun_write(utun_dev_t *tun, const uint8_t *buf, size_t len);

#endif /* ETHC_UTUN_H */
