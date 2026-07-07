# Manual test checklist

This project's DHCP/ARP/RNDIS control-plane logic was written and compiled
on a Linux container with no physical Mac or Samsung phone available, so
none of this has been run end-to-end yet. Run through this checklist on
real hardware before trusting it for daily use.

## Setup

- [ ] `brew install libusb`, then `make` completes without errors on macOS.
- [ ] Phone: enable Settings → Connections → Mobile Hotspot and Tethering →
      USB Tethering, connected via a data-capable USB-C cable.
- [ ] Confirm the phone enumerates at all: `system_profiler SPUSBDataType`
      should show a Samsung device (vendor `04e8`).

## Bring-up

- [ ] `sudo ./eth-c-macos` finds the device (autodetect) without needing
      `--vendor`/`--product`.
- [ ] RNDIS handshake succeeds and logs a MAC address that looks like a
      normal locally-administered/vendor address (not all-zero, not
      all-`ff`).
- [ ] DHCP completes within the default 15s timeout and logs a plausible
      `192.168.42.x`-range address (or whatever range the phone uses) plus a
      gateway and lease time.
- [ ] `ifconfig utunN` (matching the logged interface name) shows the
      assigned address and `UP` flag.
- [ ] `route -n get default` shows the new `utunN` as the default route.

## Traffic

- [ ] `ping <gateway-ip>` (the address logged as "gateway") gets replies.
- [ ] `ping 8.8.8.8` gets replies (confirms the phone is NAT'ing us to the
      internet, not just answering ARP/local traffic).
- [ ] `curl -v https://example.com` succeeds (confirms DNS resolution via
      the phone's DNS servers, written to `/etc/resolv.conf`).
- [ ] Open a real browser tab and load a page — confirms nothing about the
      Mac's normal networking stack chokes on a `utun`-only default route.
- [ ] Run a throughput test (e.g. `iperf3` against a server reachable
      through the phone) and sanity-check it's actually faster/more stable
      than Wi-Fi tethering, which is the whole point of doing this over USB.

## Shutdown / edge cases

- [ ] Ctrl+C stops cleanly, logs "shutting down", and `route -n get
      default` afterwards shows your original gateway restored (Wi-Fi or
      whatever it was before).
- [ ] Unplugging the cable mid-session: confirm it fails loudly (logged
      error + exit) rather than hanging silently.
- [ ] Re-running immediately after a previous clean shutdown works without
      needing to toggle USB tethering on the phone.
- [ ] Locking/sleeping the Mac and waking it up: does the tether session
      survive, or does it need a restart? (Likely needs a restart — note
      whatever actually happens here.)

## Known-shaky areas to look at first if something breaks

- `find_rndis_interfaces()` in `src/usbdev.c` assumes the control interface
  is numbered directly before the data interface — if a given phone/Android
  version numbers them differently, autodetect will pick the wrong control
  interface number. Dump `system_profiler SPUSBDataType -detailLevel full`
  for the exact interface layout Samsung's gadget uses if this happens.
- DHCP option parsing in `src/dhcp.c` only reads options up to whatever
  MTU-sized frame the phone sends in one RNDIS packet — if a given gadget
  splits its DHCP reply across multiple RNDIS packet messages (unlikely,
  but not something we defend against), the reply will look truncated.
- `/etc/resolv.conf` handling: macOS's `configd` may fight us over this
  file. If DNS mysteriously stops working a few minutes in, that's the
  first thing to check.
