#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "utun.h"
#include "log.h"

#if defined(__APPLE__)
static char g_saved_gateway[64];
static int g_have_saved_gateway = 0;
#endif

/* Runs a command via fork+execv (never a shell), so IP/DNS strings
 * that ultimately came from the phone's DHCP server can't be
 * interpreted as shell syntax. */
static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -errno;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_warn("command '%s' exited with status %d", argv[0], WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

#if defined(__APPLE__)
static void save_current_default_gateway(void) {
    g_have_saved_gateway = 0;
    /* Fixed literal command, no untrusted input - popen is fine here. */
    FILE *p = popen("route -n get default 2>/dev/null", "r");
    if (!p) return;

    char line[256];
    while (fgets(line, sizeof(line), p)) {
        char gw[64];
        if (sscanf(line, " gateway: %63s", gw) == 1) {
            strncpy(g_saved_gateway, gw, sizeof(g_saved_gateway) - 1);
            g_saved_gateway[sizeof(g_saved_gateway) - 1] = '\0';
            g_have_saved_gateway = 1;
            break;
        }
    }
    pclose(p);
}

void utun_restore_route(void) {
    char *del_argv[] = {"/sbin/route", "delete", "default", NULL};
    run_cmd(del_argv);

    if (g_have_saved_gateway) {
        char *add_argv[] = {"/sbin/route", "add", "default", g_saved_gateway, NULL};
        run_cmd(add_argv);
        log_info("restored previous default route via %s", g_saved_gateway);
    }
}
#else
void utun_restore_route(void) { /* dev-stub platforms: nothing to restore */ }
#endif

#if defined(__APPLE__)

#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/kern_control.h>
#include <sys/ioctl.h>
#include <net/if_utun.h>

int utun_open(utun_dev_t *tun) {
    memset(tun, 0, sizeof(*tun));

    tun->fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (tun->fd < 0) return -errno;

    struct ctl_info info;
    memset(&info, 0, sizeof(info));
    strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name) - 1);
    if (ioctl(tun->fd, CTLIOCGINFO, &info) < 0) {
        int e = errno;
        close(tun->fd);
        return -e;
    }

    struct sockaddr_ctl addr;
    memset(&addr, 0, sizeof(addr));
    addr.sc_id = info.ctl_id;
    addr.sc_len = sizeof(addr);
    addr.sc_family = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;
    addr.sc_unit = 0; /* 0 => kernel assigns the next free utunN */

    if (connect(tun->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int e = errno;
        close(tun->fd);
        return -e;
    }

    socklen_t name_len = sizeof(tun->name);
    if (getsockopt(tun->fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, tun->name, &name_len) < 0) {
        int e = errno;
        close(tun->fd);
        return -e;
    }

    log_info("opened %s", tun->name);
    return 0;
}

int utun_read(utun_dev_t *tun, uint8_t *buf, size_t buflen) {
    /* macOS prefixes every packet on a utun fd with a 4-byte
     * big-endian address-family header (AF_INET/AF_INET6). */
    uint8_t raw[4 + 65535];
    ssize_t n = read(tun->fd, raw, sizeof(raw));
    if (n < 0) return -errno;
    if ((size_t)n <= 4) return 0;
    size_t plen = (size_t)n - 4;
    if (plen > buflen) return -1;
    memcpy(buf, raw + 4, plen);
    return (int)plen;
}

int utun_write(utun_dev_t *tun, const uint8_t *buf, size_t len) {
    uint8_t raw[4 + 65535];
    if (len > sizeof(raw) - 4) return -1;

    uint8_t version = (buf[0] >> 4) & 0x0F;
    uint32_t family = (version == 6) ? AF_INET6 : AF_INET;
    raw[0] = (family >> 24) & 0xFF;
    raw[1] = (family >> 16) & 0xFF;
    raw[2] = (family >> 8) & 0xFF;
    raw[3] = family & 0xFF;
    memcpy(raw + 4, buf, len);

    ssize_t n = write(tun->fd, raw, len + 4);
    if (n < 0) return -errno;
    return (int)(n - 4);
}

#elif defined(__linux__)
/* Not part of the shipped product (this project targets macOS): kept
 * only so the platform-independent DHCP/ARP/USB logic in this repo
 * can be smoke-compiled and unit-tested on a Linux dev machine or CI
 * runner without access to a Mac. Uses a Linux TAP device instead of
 * utun since Linux has no utun equivalent. */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

int utun_open(utun_dev_t *tun) {
    memset(tun, 0, sizeof(*tun));
    tun->fd = open("/dev/net/tun", O_RDWR);
    if (tun->fd < 0) return -errno;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "ethc%%d");

    if (ioctl(tun->fd, TUNSETIFF, &ifr) < 0) {
        int e = errno;
        close(tun->fd);
        return -e;
    }
    snprintf(tun->name, sizeof(tun->name), "%s", ifr.ifr_name);
    log_info("opened %s (Linux dev-build TAP stub)", tun->name);
    return 0;
}

int utun_read(utun_dev_t *tun, uint8_t *buf, size_t buflen) {
    ssize_t n = read(tun->fd, buf, buflen);
    if (n < 0) return -errno;
    return (int)n;
}

int utun_write(utun_dev_t *tun, const uint8_t *buf, size_t len) {
    ssize_t n = write(tun->fd, buf, len);
    if (n < 0) return -errno;
    return (int)n;
}

#else
#error "utun.c has no implementation for this platform"
#endif

void utun_close(utun_dev_t *tun) {
    if (tun->fd >= 0) close(tun->fd);
    tun->fd = -1;
}

int utun_configure(utun_dev_t *tun, const char *local_ip, const char *peer_ip,
                    const char *dns[], int dns_count) {
#if defined(__APPLE__)
    char *ifconfig_argv[] = {"/sbin/ifconfig", tun->name, "inet",
                              (char *)local_ip, (char *)peer_ip, "up", NULL};
    if (run_cmd(ifconfig_argv) != 0) return -1;

    save_current_default_gateway();

    char *del_argv[] = {"/sbin/route", "delete", "default", NULL};
    run_cmd(del_argv); /* fine if there wasn't one */

    char *add_argv[] = {"/sbin/route", "add", "-inet", "default", "-interface", tun->name, NULL};
    if (run_cmd(add_argv) != 0) {
        log_error("failed to install default route through %s", tun->name);
        return -1;
    }

    if (dns_count > 0) {
        /* Written directly since this tunnel has no Network
         * preferences "service" for networksetup to target. Best
         * effort: configd may overwrite this on the next network
         * change. */
        FILE *f = fopen("/etc/resolv.conf", "w");
        if (f) {
            for (int i = 0; i < dns_count; i++) fprintf(f, "nameserver %s\n", dns[i]);
            fclose(f);
        } else {
            log_warn("could not write /etc/resolv.conf (%s) - DNS may still point at the old network", strerror(errno));
        }
    }

    log_info("configured %s: %s -> %s, default route installed", tun->name, local_ip, peer_ip);
    return 0;
#else
    char local_cidr[64];
    snprintf(local_cidr, sizeof(local_cidr), "%s/32", local_ip);
    char *addr_argv[] = {"/sbin/ip", "addr", "add", local_cidr, "peer", (char *)peer_ip, "dev", tun->name, NULL};
    if (run_cmd(addr_argv) != 0) return -1;
    char *link_argv[] = {"/sbin/ip", "link", "set", tun->name, "up", NULL};
    if (run_cmd(link_argv) != 0) return -1;
    log_info("(dev stub) configured %s: %s -> %s", tun->name, local_ip, peer_ip);
    (void)dns; (void)dns_count;
    return 0;
#endif
}
