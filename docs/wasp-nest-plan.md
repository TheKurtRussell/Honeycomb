# Honeycomb / Wasp Nest — overnight status + forward plan

_Last updated: 2026-11-23, autonomous session._

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

## 7 — Wasp Nest: aggressive module roadmap

We now own the C6 slave build, so modules split by what the **stock** slave can
do vs. what needs a **custom C6 slave extension** (raw 802.11 / 802.15.4 — the
Evil-M5 / Marauder approach, which we can now add to `c6-hosted`).

### Tier 1 — buildable on the current stock slave (no new C6 firmware)
- **BLE advertising spam** — NimBLE can transmit custom advertisements
  (Apple/Google/Samsung fast-pair popups, "Flipper"-style). Highest-impact, fully
  in reach now.
- **BLE device spoofing** — advertise as a chosen target (clone name/UUIDs).
- **BLE GATT explorer / enumerate** — connect as central, walk services &
  characteristics, read/write (also unlocks BLE traffic viz in item 4).
- **BLE HID injection** — advertise as a BLE keyboard, inject keystrokes into a
  paired host.
- **Rogue AP / Evil Twin + captive portal** — esp_wifi SoftAP over hosted + an
  on-board HTTP server serving a captive page; log submitted creds to SD. Classic
  assessment tool. (We keep this clearly operator-gated.)
- **Beacon/SSID list AP cycling** — stand up APs from an SSID list (limited vs
  raw beacon spam, but real).

### Tier 2 — needs a custom C6 slave extension (raw 802.11 / 802.15.4)
Add raw TX/RX + promiscuous to `c6-hosted` (new RPC opcodes, Evil-M5 style),
then OTA it the same way we already do:
- **Deauth / disassoc** flood.
- **Beacon spam** (true raw beacons).
- **Probe-request sniffing** / client tracking / PNL harvesting.
- **WPA(2) handshake + PMKID capture** → SD (pcap), for offline cracking.
- **Karma** (respond to probe requests).
- **802.15.4 / Zigbee / Thread sniffing** → pcap.

### Repo linkage
The pentest suite will live under Wasp Nest. **I need the Wasp Nest repo URL** to
wire the link/submodule; until then I'll draft modules in-tree under a
`wasp-nest/` area and a `WASP_NEST.md` index, so we keep continuity and can split
them out later. Aggressive modules ship behind an explicit in-UI "armed" toggle.

## Suggested next-session order
1. ESP-NOW mesh prototype (confirms the mesh node, low risk).
2. BLE GATT explorer (unlocks BLE traffic viz + device interaction).
3. Wi-Fi throughput/RSSI meter (item 4).
4. Rogue AP + captive portal (first Tier-1 Wasp Nest module).
5. Custom C6 slave raw-80211 extension → deauth + handshake capture (Tier 2).
