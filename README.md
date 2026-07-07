# eth-c-macos

USB tethering for macOS (tested target: Samsung Galaxy phones), without a
kernel extension.

## Why this exists

[HoRNDIS](https://github.com/joshuawise/horndis) has been the standard way to
tether an Android phone to a Mac over USB for over a decade, but it's a kernel
extension (kext) — and Apple has been steadily tightening what third-party
kexts can do on every macOS release since Big Sur. On recent macOS versions
(including macOS Tahoe) it commonly fails to load at all, and even when it
loads, third-party kexts increasingly require lowering the Mac's security
settings (Reduced Security mode) on Apple Silicon, which most people
reasonably don't want to do just to tether a phone.

This project takes a different approach: instead of a kernel driver, it's a
small **userspace daemon**. It talks to the phone directly over USB using
[libusb](https://libusb.info/), speaks the RNDIS protocol that Android's
tethering gadget expects, and bridges the resulting IP traffic to a `utun`
virtual network interface that macOS already provides as a built-in,
no-kext-required API. No System Extension approval dialog, no Reduced
Security mode, no code signing fights — just a binary you run with `sudo`.

The tradeoff: this only carries **IP traffic for the Mac itself**, not a full
transparent Ethernet bridge. That's all normal internet tethering needs, so
in practice it's not a real limitation — but if you were hoping to bridge the
phone onto your LAN as a second Ethernet client, that's out of scope (see
[docs/DESIGN.md](docs/DESIGN.md) for why, and what it would take).

## How it works, in one paragraph

Android's "USB tethering" mode turns the phone into a USB RNDIS gadget: an
emulated Ethernet-over-USB device that presents a CDC control interface (for
protocol handshaking) and a CDC data interface (bulk endpoints carrying
Ethernet frames). This tool finds that device with libusb, performs the RNDIS
`INITIALIZE`/`SET`/`QUERY` control handshake, then runs a small hand-built
DHCP client over the raw Ethernet frames to get an IP address, gateway, and
DNS servers from the phone — exactly what a real Ethernet driver would do,
just without a kernel involved. Once configured, it creates a macOS `utun`
interface, sets it as the default route, and forwards IP packets in both
directions, adding/removing the Ethernet header and answering ARP requests as
needed at the boundary. Full protocol details are in
[docs/DESIGN.md](docs/DESIGN.md).

```
 macOS IP stack                  this daemon                      phone
┌────────────────┐   IP pkts   ┌──────────────────┐  RNDIS/USB  ┌─────────┐
│ apps, browser…  │◄──────────►│ utun <-> bridge   │◄───────────►│ tether  │
│ default route   │  (utun0)   │ <-> libusb RNDIS  │  (libusb)   │ gadget  │
└────────────────┘             └──────────────────┘             └─────────┘
```

## Requirements

- macOS (Intel or Apple Silicon). This has **not** been tested on real
  hardware yet (see [Status](#status-and-known-limitations) below) — it was
  developed and compile-checked in a Linux container with no Mac or Samsung
  device available, so treat it as a from-scratch draft that needs to be
  brought up on real hardware, not a finished driver.
- [Homebrew](https://brew.sh) `libusb`: `brew install libusb`
- USB tethering enabled on the phone (Settings → Connections → Mobile
  Hotspot and Tethering → USB Tethering) and the phone connected via a USB
  data cable (not a charge-only cable).
- `sudo`, since raw USB access and creating a `utun` interface both need root.

## Build

```sh
brew install libusb
make
```

## Run

```sh
sudo ./eth-c-macos
```

On success you'll see something like:

```
[12:00:01] INFO  bound to USB device 04e8:6864 (control if=0, data if=1, bulk in=0x81 out=0x02)
[12:00:01] INFO  device MAC aa:bb:cc:dd:ee:ff, max transfer size 1600
[12:00:02] INFO  DHCP: bound 192.168.42.7, gateway 192.168.42.129, lease 3600 s
[12:00:02] INFO  opened utun7
[12:00:02] INFO  tethering up: utun7 is 192.168.42.7, internet via phone (aa:bb:cc:dd:ee:ff)
[12:00:02] INFO  bridge running (Ctrl+C to stop)
```

Press Ctrl+C to stop; it restores whatever default route you had before.

Options:

```
--vendor 0xHHHH / --product 0xHHHH   match a specific USB device instead of
                                      autodetecting any Samsung (0x04e8) gadget
--timeout SECONDS                    DHCP timeout (default 15)
-q                                   quiet (errors/warnings only)
```

## Project layout

```
include/    public headers, one per module
src/        implementation
  rndis.h       wire structs/constants for the RNDIS-over-USB protocol
  usbdev.c      libusb device discovery + RNDIS control/data plane
  arp.c         ARP request/reply construction and parsing
  dhcp.c        minimal hand-rolled DHCP client (no OS network stack involved)
  utun.c        macOS utun interface creation/config (Linux TAP dev-stub for CI)
  bridge.c      the two forwarding loops that glue RNDIS <-> utun together
  main.c        CLI entry point
docs/
  DESIGN.md     protocol write-up / architecture rationale (useful for a report)
  TESTING.md    manual test checklist for real hardware
```

## Status and known limitations

This was built as a university project and a from-scratch HoRNDIS
replacement, developed in an environment without access to a physical Mac or
Samsung phone. Concretely, that means:

- The RNDIS/DHCP/ARP protocol logic is portable C and has been compiled
  clean (`-Wall -Wextra -Werror`) and exercised for basic sanity, but **the
  full pipeline has not been run against real hardware**. The macOS-specific
  `utun` and route/DNS configuration code is also unexercised for the same
  reason (this repo's dev container is Linux). Treat this as a solid,
  reviewed first draft, not a validated release — see
  [docs/TESTING.md](docs/TESTING.md) for the checklist to run through on
  your own Mac + phone before relying on it.
- Only one phone at a time; only IPv4 (DHCPv6/SLAAC for the tether link isn't
  implemented).
- Rewrites `/etc/resolv.conf` directly to set DNS, since this `utun`
  interface has no corresponding "service" in Network preferences for
  `networksetup` to target. macOS's `configd` may overwrite that file on the
  next network change; if DNS stops resolving after switching networks,
  re-run this tool.
- No retry/reconnect handling if the phone is unplugged mid-session — it
  will log an error and exit; just re-run it.

## License

MIT — see [LICENSE](LICENSE).
