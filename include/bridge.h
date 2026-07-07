#ifndef ETHC_BRIDGE_H
#define ETHC_BRIDGE_H

#include <stdint.h>
#include <signal.h>

#include "usbdev.h"
#include "utun.h"
#include "eth.h"

/* Runs the two forwarding loops (RNDIS->utun and utun->RNDIS) until
 * *keep_running is cleared (e.g. from a signal handler). Blocks the
 * calling thread; spawns one helper pthread internally for the second
 * direction. Handles ARP requests from the phone for `our_ip` inline
 * so the Mac doesn't need a real ARP table for this link. */
int bridge_run(rndis_dev_t *dev, utun_dev_t *tun, const uint8_t our_mac[ETH_ALEN],
                uint32_t our_ip, uint32_t gateway_ip, const uint8_t gateway_mac[ETH_ALEN],
                volatile sig_atomic_t *keep_running);

#endif /* ETHC_BRIDGE_H */
