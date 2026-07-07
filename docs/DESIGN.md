# Design notes

This document covers the protocol details and design decisions behind
`eth-c-macos`, mainly so it's useful as reference material for a project
write-up.

## 1. Why not a kernel extension (like HoRNDIS)?

HoRNDIS implements an `IOKit` kernel extension: it registers as a real
`IONetworkInterface`, so the tethered phone shows up in System
Settings → Network like any other Ethernet adapter, and the OS's own DHCP
client and routing configure it automatically. That's a nicer end result,
but it costs a lot of stability: kexts require the kernel and userspace
pieces to agree on ABI, Apple has been shrinking what unsigned/non-notarized
kexts can do release over release, and on Apple Silicon, loading a
third-party kext at all requires the user to boot into Reduced Security
mode — a machine-wide security posture change most people won't want to
make just to tether a phone. That combination is almost certainly why
HoRNDIS breaks on new macOS releases faster than its maintainers can keep up.

Apple's supported replacement for this class of driver is
[DriverKit](https://developer.apple.com/documentation/driverkit) — you'd
write a `NetworkingDriverKit`/`IOUSBHostDriverKit`-based System Extension
instead of a kext. That's the "correct" long-term answer and a legitimate,
larger project, but it needs Xcode, entitlements, and either a paid
Developer ID or local-only signing, and the user still has to approve the
extension in System Settings.

This project takes a third path: don't touch kernel/driver APIs at all.
Everything happens in a normal, unprivileged-except-for-USB-and-utun
userspace process:

| | HoRNDIS (kext) | DriverKit dext | This project |
|---|---|---|---|
| Runs as | kernel extension | System Extension | plain process (needs root) |
| Survives macOS updates | fragile | robust | robust (uses stable public APIs) |
| Shows as real "Ethernet" in Network prefs | yes | yes | no (see §4) |
| Setup friction | kext approval / Reduced Security | Xcode, signing, extension approval | `brew install libusb && make` |
| Implementation size | large (C++, IOKit) | large (Swift/C++, DriverKit) | small (~1.5k lines of C) |

## 2. USB/RNDIS transport

Android's tethering gadget (`u_ether`/`g_ether`/`f_rndis` in the Linux
kernel, which is what runs the USB side on the phone) presents two USB
interfaces:

- **Control interface** (CDC, class `0x02`): one interrupt-IN endpoint used
  purely to notify the host "a response is ready to be read." RNDIS control
  messages themselves don't travel over this endpoint — they ride on
  endpoint 0 (the standard USB control endpoint) via two class-specific
  requests defined by the USB CDC spec:
  - `SEND_ENCAPSULATED_COMMAND` (`bRequest = 0x00`, host → device): host
    sends an RNDIS message.
  - `GET_ENCAPSULATED_RESPONSE` (`bRequest = 0x01`, device → host): host
    reads the device's response, once notified.
- **Data interface** (CDC-Data, class `0x0A`): one bulk-IN, one bulk-OUT
  endpoint. Every Ethernet frame in either direction is wrapped in an RNDIS
  `REMOTE_NDIS_PACKET_MSG` envelope and sent over these.

`src/usbdev.c` finds this pair by walking the active USB configuration
descriptor for an interface with exactly one bulk-in + one bulk-out endpoint
(the data interface), and pairs it with the interrupt-only interface next to
it (the control interface) — falling back to polling
`GET_ENCAPSULATED_RESPONSE` after a short delay if no interrupt endpoint is
found, which every gadget we know of tolerates fine.

### RNDIS handshake

1. **`REMOTE_NDIS_INITIALIZE_MSG`** — negotiate protocol version and maximum
   transfer size. The gadget replies with `INITIALIZE_CMPLT`, including the
   max transfer size it supports (we clamp ourselves to whichever is
   smaller).
2. **`REMOTE_NDIS_SET_MSG`** on `OID_GEN_CURRENT_PACKET_FILTER` — tells the
   gadget which frames to forward to us (directed + broadcast + multicast).
   Gadgets sit silent until this is set — this is the step most naive RNDIS
   implementations forget and then wonder why no data ever arrives.
3. **`REMOTE_NDIS_QUERY_MSG`** on `OID_802_3_CURRENT_ADDRESS` — reads the
   gadget's own MAC address. On this point-to-point link, that address
   doubles as "the gateway's MAC," since the gadget is the only possible
   peer (see §4).

All three exchanges are request/response pairs correlated by a
monotonically increasing `RequestID` field. Offsets inside `QUERY`/`SET`
messages (`InformationBufferOffset`) are measured from the start of the
`RequestID` field per the RNDIS spec — an easy off-by-N to get wrong, so
it's called out explicitly in `include/rndis.h` next to
`RNDIS_INFO_BUFFER_OFFSET`.

## 3. DHCP without an OS network stack

Once the RNDIS link is up, we need an IP address, subnet, gateway, and DNS
servers — normally the OS's DHCP client would handle this the moment a new
Ethernet interface appears. But at this point there *is* no OS-visible
network interface yet (the `utun` doesn't exist until we know what address
to give it), so `src/dhcp.c` runs its own minimal DHCP client directly over
`rndis_send_frame`/`rndis_recv_frame`: it hand-builds
Ethernet+IP+UDP+BOOTP/DHCP frames for `DISCOVER` and `REQUEST`, parses
`OFFER`/`ACK` replies, and extracts the options it needs (subnet mask,
router, DNS servers, lease time). This is the same
DISCOVER→OFFER→REQUEST→ACK exchange any DHCP client performs — it's just
implemented over raw frames instead of a socket, since there's no IP stack
bound to this link yet to open a socket on.

## 4. Why `utun` (L3) instead of a bridge, and how ARP fits in

macOS's `utun` pseudo-interfaces (created via a `PF_SYSTEM` /
`SYSPROTO_CONTROL` control socket, no kext required — this is the same
public mechanism VPN clients and tools like WireGuard use) are **IP-only**:
every read/write carries a raw IP packet prefixed with a 4-byte
address-family header, with no Ethernet framing. Apple does not expose a
public, kext-free "TAP" (raw Ethernet) equivalent, which is what a full
transparent bridge to the phone would require.

Since normal tethering only ever needs the Mac's own IP traffic to reach the
internet — not to bridge the phone onto the LAN as another Ethernet client
— this isn't a real limitation, just a framing mismatch we bridge in
software:

- **`utun` → phone**: read a raw IP packet off `utun`, prepend an Ethernet
  header (`src` = our locally-administered MAC, `dst` = the gadget's MAC we
  learned via RNDIS `QUERY`), send as an RNDIS packet message.
- **Phone → `utun`**: unwrap the RNDIS packet message, strip the Ethernet
  header, hand the IP payload to `utun`.
- **ARP**: since this link has exactly one possible peer, we don't need a
  real ARP table or ARP resolution for outbound traffic — the gadget's MAC
  from the RNDIS handshake is already the only address we'd ever resolve.
  We do still need to *answer* ARP requests, though: the phone's own IP
  stack will occasionally ARP for our address (e.g. before sending a
  unicast DNS reply), and if we don't answer, that traffic silently drops.
  `src/bridge.c` watches for `ARP_OP_REQUEST` frames targeting our IP and
  replies inline.

## 5. Route and DNS configuration

`utun_configure()` (`src/utun.c`) brings the interface up as a
point-to-point link (`ifconfig utunN <local> <gateway> up`), tears down the
existing default route and replaces it with one through the new interface
(`route add -inet default -interface utunN`), and — since this `utun`
doesn't correspond to any "service" in Network preferences for
`networksetup` to target — writes DNS servers straight into
`/etc/resolv.conf`. All of this shells out via `fork`/`execv` (never a
shell), specifically so that attacker-influenced strings (this project takes
addresses fresh from a network handshake with the phone, which is normally
trusted but is still external input) can never be interpreted as shell
syntax — the classic path for a command-injection bug in exactly this kind
of "glue script that shells out with data from the network" code.

The previous default route is captured before being replaced and restored
on exit (`utun_restore_route()`), so unplugging the phone or hitting Ctrl+C
doesn't strand the Mac without a route to anywhere.
