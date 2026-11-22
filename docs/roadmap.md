# Roadmap

Ordered so each phase produces something you can actually use, and the riskiest
hardware bring-up is de-risked early but not blocking.

## Phase 0 — scaffold (done)
- Project tree, build system, shared protocol, all component interfaces.
- Real implementations: sniffer core, dissector, pcap writer, link layer (UART),
  mode manager, esptool bootloader protocol.
- Integration points clearly isolated: USB-host VCP transport, display BSP,
  esp_hosted Wi-Fi slave.

## Phase 1 — sniffer to Wireshark (highest value, least risk)
Goal: capture live Zigbee/Thread traffic and open it on your PC.
1. Flash the C6 image; confirm `HC_EV_HELLO` over the UART link (Status tab /
   console).
2. Bring a known 802.15.4 network nearby (a Zigbee bulb pairing, a Thread border
   router). Fixed channel first (15 or 25 are common), then hopping.
3. Verify frames stream (`HC_EV_FRAME`) and the dissector labels types/addresses.
4. Insert microSD, start a capture, pull `capNNN.pcap`, open in Wireshark
   (link type IEEE 802.15.4 w/ FCS). ← **first real milestone**
No USB host, no display needed — runs headless over the console.

## Phase 2a — display + touch UI
- Bind the Tab5 display BSP in `hc_board.c`; pass `lv_display` to `hc_ui_init`.
- The Explorer tab's table, channel roller, and buttons are already wired.

## Phase 2b — USB-host flasher transport
- Add the `usb_host_vcp` component family; implement the four `vcp_*` functions
  in `hc_flasher.c` (open/read/write/line-control).
- Test order: enumerate a target → `hc_loader_sync()` → chip detect → flash a
  small app → serial-monitor it. Classic ROM loader first; add the stub loader +
  compressed writes for speed afterward.
- Fill in the chip-magic table in `hc_flasher.c` from esptool's values for each
  target you care about (your fleet: CYD/ESP32, C5 Piglet, C6, S3 Cardputer).

## Phase 2c — Wi-Fi mode (esp_hosted)
- Decide option A (separate hosted-slave image + reboot into `ota_1`) or option B
  (combined image). A is less code.
- Wire `hc_radio_wifi_run_hosted_slave()`; on the P4 bring up `esp_wifi_remote`
  after the C6 reboots into WIFI. Then the Tab5 gets normal Wi-Fi for, e.g., a
  web UI / OTA / WiGLE upload.
- Move the inter-chip link from UART onto an SDIO custom-RPC channel so Wi-Fi and
  the control link share one bus.

## Phase 3 — active scans → participation (your "prepare for later")
The sniffer's `hc_sniffer_active_scan` currently sweeps and collects beacons
passively. To grow toward **participation** (option 3):
1. Transmit real MAC **Beacon Requests** (`esp_ieee802154_transmit`) and parse
   beacon payloads into `hc_scan_result_t` with proper Zigbee/Thread labelling.
2. Add MAC-level association (send AssocReq, handle AssocResp) behind a new
   opcode — still in `hc_radio_sniffer`/`hc_mode_agent`.
3. For **Thread/Matter** proper, pivot the C6 to OpenThread (it can run an
   OpenThread RCP or full stack); the P4 drives it via `openthread`'s host API.
   This is a separate C6 personality — slot it into the mode manager as a new
   `HC_MODE_THREAD` alongside SNIFFER/WIFI. The protocol/opcode design already
   leaves room (modes are an enum; events are append-only).

The architecture was chosen so this last step is an *addition*, not a rewrite:
new mode, new opcodes, same link and UI shell.

## Nice-to-haves
- Energy-scan heatmap view on the Explorer tab (data already arrives via
  `HC_EV_ENERGY`).
- WiGLE-style CSV export alongside pcap (needs GPS — out of scope without a
  module, but the capture records carry everything except location).
- Save/replay a capture back out the radio for testing.

## USB serial bench — status & next step (bridges)
Done: USB-A power (bsp_usb_host_start / USB5V_EN), host bring-up, device
enumeration (VID/PID/class), CDC-ACM serial monitor with a scrolling log and a
baud selector (9600/115200/230400/921600), and flash-from-SD (pick a .bin,
stop monitor, enter download mode, hc_flasher_write at 0x0 with a progress bar,
resume monitor). Works for **native-USB** ESP targets (VID 0x303A: S3/C3/C6/
C5/P4/H2) — confirmed on a Cardputer ADV (303A:1001) and XIAO ESP32-C5.

Next: **USB-serial bridge chips** (CP210x, CH34x, FTDI) for classic DevKits.
The esp bridge drivers (usb_host_cp210x_vcp / ch34x_vcp / ftdi_vcp) are built on
the C++ esp_usb::VCP umbrella — there is no pure-C open API. Plan:
1. Add a small C++ shim (hc_vcp.cpp, extern "C") exposing: hc_vcp_open_any(rx_cb)
   using esp_usb::VCP::open_from_new_dev(), plus tx / line_coding_set /
   set_control_line_state wrappers.
2. Route hc_flasher's transport through hc_vcp instead of calling cdc_acm_host
   directly (VCP already handles native CDC too, so it can replace the cdc_acm
   path rather than run beside it — avoids two drivers claiming one device).
3. Deps to add: espressif/usb_host_vcp + the three bridge vcp components.
   Enable C++ exceptions? No — keep -fno-exceptions; VCP works without.
Deferred (not built blind): the VCP C++ API couldn't be verified offline, and
shipping it untested risked the working native-USB bench.

## C6 radio (Wi-Fi/BLE/802.15.4) via ESP-Hosted — BLOCKED on SDMMC coexistence

Phase 1 (bring Wi-Fi up on the factory C6 slave via esp_wifi_remote + esp_hosted
over SDIO) got **the mechanism and pins exactly right** but boot-loops the board.
Reverted in the app for now; the known-good config is kept commented in
sdkconfig.defaults / main/idf_component.yml for the next attempt.

What the 5b19639 boot log proved:
- The esp_hosted Tab5 board profile (ESP32P4_TAB5_C6_BOARD) works: transport
  came up `SDIO 4-bit 40000 kHz CLK=12 CMD=13 D0=11 D1=10 D2=9 D3=8 RESET=15`,
  C6 reset fired on GPIO15. No pin guessing needed.

Two problems to solve before re-enabling:
1. **SDMMC controller is shared.** The C6 SDIO and the microSD both sit on the
   P4's one SDMMC host. esp_hosted inits that host very early (before app_main),
   so when hc_storage later mounts the microSD it hits
   `SDMMC host already initialized, skipping init flow` and the SD init times out
   (`sdmmc_card_init failed 0x107`). The SD-mount failure then trips
   `assert(xQueueSemaphoreTake ... pxQueue)` -> panic -> **boot loop**. M5's own
   stack runs C6 Wi-Fi and the SD together, so this is an init-ordering/shared-
   host problem, not a hardware limit. Fix direction: drive one SDMMC host and
   add each slot (slot 0 = SD, slot 1 = C6) explicitly, or mount the SD before
   esp_hosted claims the host; and make hc_storage mount failure non-fatal
   (never assert) as belt-and-braces.
2. **C6 didn't answer SDIO** (`send_op_cond 0x107` on the C6 slot too). Likely
   the factory slave (ESP32C6-WiFi-SDIO-Interface-V1.4.1) not matching this newer
   esp_hosted host, and/or WLAN_PWR_EN on the 0x44 IO expander not asserted
   before probe. This is the anticipated Phase-2 trigger: reflash the C6 over
   SDIO (shadowlab tab5-p4-c6-sdio-ota, no downloader kit) with a matching —
   eventually sniff-capable — slave.
