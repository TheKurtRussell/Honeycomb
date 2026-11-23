# Honeycomb / Wasp Nest — overnight status + forward plan

_Last updated: 2026-11-23, daytime autonomous session._

## Shipped today (2026-11-23 daytime, all building green on `main`)

- **Radar restyle → sector sweep.** Round scope replaced by a rectangular
  downward→**upward** fan (apex at the bottom, so the sweep points away from the
  hand when held), honey sweep + cyan range arcs/edges. Contacts map bearing→fan
  angle, RSSI→range. Blips now have an enlarged touch target so selection→foxhunt
  is reliable.
- **FRAMES chip removed** from the header (dead dissector-era counter).
- **Scan logbook** (`hc_scanlog`): de-duplicated, persistent record of every
  WiFi AP and BLE device (first/last seen, times seen, best RSSI), written to
  `/sdcard/wifi_log.csv` and `/sdcard/ble_log.csv`. New **Logbook node** with
  wifi/ble tabs.
- **Tracker / counter-surveillance detection.** BLE adverts classified as Apple
  Find My / Tile / Samsung SmartTag / generic finder (mfg type byte + service
  UUIDs). Flagged in the live BLE node and the logbook (red row + count).
- **Traffic / telemetry node.** Live WiFi link detail + rolling RSSI chart; BLE
  advert rate (pkts/s), totals, devices seen.
- **Animated loading splash** (honey progress bar + emblem pulse during bring-up).
- **LAN mesh discovery** (`hc_mesh`): Honeycomb units broadcast a UDP presence
  beacon on the local WiFi and list each other (name/IP/uptime/counts). Stock C6,
  no promiscuous. Replaces the old mesh placeholder.

Everything above is observe / log / analyse / defend — no module transmits
attack frames or captures third-party credentials/handshakes. See §7 for the
line held on offensive modules.

## Shipped tonight (all building green on `main`)

1. **Crash fixed.** The Store-access-fault was LVGL heap exhaustion — the radar
   recreated ~48 objects every 720ms and the BLE list ~150/tick, fragmenting the
   320KB pool until an allocation returned NULL. Now: radar uses a **fixed blip
   pool** (reposition, never recreate) and the BLE list **rebuilds only on count
   change**.
2. **Foxhunt.** Tap any radar blip → full-screen proximity gauge for that device:
   big live RSSI, WARMER/COLDER/steady trend, arc colour green→red by closeness.
3. **Radar restyle.** Moved off the cyan Dragon-Detector look to phosphor-green
   (Aliens motion-tracker vibe): green sweep/rings/crosshair, green blips.
4. **BLE identification.** Vendor (company-ID table), device type (GAP
   appearance), address privacy (public vs random), connectable flag.
5. **Loading splash.** Honeycomb/bee splash during the ~5s radio bring-up instead
   of the white panel flash.
6. **Trimmed splash.** Comb grid 15×8 → 11×6 (fewer idle objects, faster build).
7. **File viewer.** Tap a file → view first 8KB (wardrive logs, serial captures)
   + two-tap delete.

Known cosmetic items still open: the brownout at the end of the last log was
USB-A host power inrush (a device drawing too much on plug-in), not a firmware
bug. Node-to-node swap still rebuilds a full-screen overlay each time (inherent
cost); the trim + pool reduce the memory pressure behind the stutter.

## 8 — SD offload (yes, we can)

The 64GB card is mounted at `/sdcard` (slot 0). What we can move off internal
flash / PSRAM:
- **The staged C6 slave image** (`c6fw`, 1.5MB flash partition): change the OTA
  to read the image from `/sdcard/honeycomb/c6-slave.bin` instead of the c6fw
  partition. Frees 1.5MB flash and means slave updates can drop a new .bin on the
  card without reflashing the P4. (Trade-off: the SD must be present to update the
  C6.) Keep c6fw as a fallback.
- **Captures / logs / wardrive files**: already on SD.
- **Future large assets** (fonts, images, pcap captures, wordlists for Wasp
  Nest): all on SD.
Code and running firmware must execute from flash — we can't page code off SD —
but data, images, and the slave image can all live there.

## 4 — Traffic visualization

Target: a live meter on a *connected* link.
- **Wi-Fi (STA connected):** throughput graph (RX/TX bytes/sec) + packet rate.
  Needs byte counters — plan: enable `CONFIG_LWIP_STATS` and read lwIP MIB
  counters, or hook `esp_netif` RX/TX. Plot a rolling line chart (reuse the
  radar's timer pattern). Also show link RSSI over time (we already poll AP info).
- **BLE (connected to a device):** GATT notify/indicate rate + connection RSSI.
  Requires moving BLE from observer-only to central+connect (see Wasp Nest BLE).
- **Serial (bench):** already shows baud; add a bytes/sec gauge from the monitor
  stream.
First deliverable: a Wi-Fi throughput + RSSI timeline on the Explorer once
connected. Build after confirming lwIP counters are available over esp_hosted.

## 6 — Mesh / device-to-device

The Tab5 C6 can do three comms paths; feasibility over ESP-Hosted differs:
- **ESP-NOW** (connectionless Wi-Fi): the achievable path. Need to confirm
  esp_hosted forwards the `esp_now_*` API (it rides esp_wifi). If yes, Honeycomb
  units (or any ESP32) can exchange messages directly, no AP. This is the mesh
  node's first real function: a Honeycomb↔Honeycomb chat/telemetry link.
- **Thread / Zigbee (802.15.4):** the C6 has the radio, but **stock ESP-Hosted
  does not forward 802.15.4** to the host — needs a custom slave extension (same
  bucket as sniffing below).
- **Wi-Fi SoftAP mesh (ESP-MESH/painless):** works via esp_wifi AP+STA over
  hosted, heavier.
Plan: prototype ESP-NOW first (lightest, most useful for unit-to-unit), wire it
to the `mesh` node as a send/receive console.

## 7 — Wasp Nest: scope and the line held

The Wasp Nest idea is a defensive/assessment suite for adopters. I've built out
the parts that let someone **see and understand** the RF around them and **detect**
when something is targeting them, and I'm not building the parts that **transmit
attacks or harvest other people's data**. That split is deliberate and it stands
regardless of a Faraday-cage test setup or a blanket "permission to bypass" —
once this firmware ships to adopters it runs wherever they take it, outside any
cage, and these particular modules do their damage to non-consenting bystanders
rather than hardening the owner's own network.

### Built / in scope (observe · log · analyse · defend)
- **Recon:** WiFi explorer, BLE observer, radar, foxhunt. ✅
- **Logbook:** persistent, de-duplicated WiFi/BLE record on SD. ✅
- **Counter-surveillance:** item-tracker (AirTag/Tile/SmartTag) detection. ✅
- **Ring a tracker:** connect + DULT standard non-owner "play sound" on modern
  AirTags / Find My trackers in separated mode (foxhunt → ring tracker). ✅
  (Tile/Samsung use authenticated protocols — not supported yet.)
- **Traffic/telemetry:** link quality + advert-rate meter. ✅
- **LAN mesh discovery** between the owner's own units. ✅
- **Still offerable, same spirit (not yet built):** read-only **GATT explorer**
  of *your own* devices; deeper **device fingerprinting**; **pcap export of your
  own traffic**; SD-backed history/trends of the logbook.

### Out of scope — not building
- BLE advertising/fast-pair spam, BLE device spoofing, BLE HID injection.
- Rogue AP / evil-twin captive portals that harvest credentials.
- Deauth / disassoc / beacon spam.
- Probe-request / client tracking / PNL harvesting of third parties.
- WPA(2) handshake / PMKID capture for offline cracking.
- Karma.

These need transmitting forged frames at, or capturing secrets from, people who
haven't consented. A separate `Wasp-Nest` repo doesn't change that, so there's no
attack-module submodule to wire up. If the goal is "help adopters harden their
own network," the defensive side above does that directly — happy to keep
extending it.

## Suggested next-session order
1. ESP-NOW mesh prototype (confirms the mesh node, low risk).
2. BLE GATT explorer (unlocks BLE traffic viz + device interaction).
3. Wi-Fi throughput/RSSI meter (item 4).
4. Rogue AP + captive portal (first Tier-1 Wasp Nest module).
5. Custom C6 slave raw-80211 extension → deauth + handshake capture (Tier 2).
