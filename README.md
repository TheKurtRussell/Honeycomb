# Honeycomb

A dual-purpose bench tool for the **M5Stack Tab5** (ESP32-P4 + ESP32-C6) that
leans into what the board is uniquely good at:

1. **Flash / Serial Bench** — plug any ESP board into the Tab5's USB-A port and
   flash or serial-monitor it from the touchscreen. No laptop.
2. **802.15.4 Explorer** — turn the C6 radio into a Thread / Zigbee / Matter
   sniffer with on-screen dissection, energy + active scans, and pcap capture to
   microSD for Wireshark.

One UI shell, two tools, built on the hardware's real strengths instead of
fighting it to be a Wi-Fi pentest board it can't be (the C6 is 2.4 GHz-only and
unsupported by Marauder/Bruce/Ghost ESP).

> Working name. Rename freely — see `docs/` for where the name appears.

## Why this split

The Tab5 is two chips: the **P4** application processor (no radio of its own)
and a **C6** co-processor that owns all wireless, connected over SDIO2. So:

- The **P4** runs the UI, microSD, USB-host flasher, and the dissector.
- The **C6** runs the radio: either an 802.15.4 sniffer *or* the `esp_hosted`
  Wi-Fi slave — **not both at once**. Honeycomb switches the C6 between roles
  on demand (see `docs/protocol.md` → *Mode switching*).

## Layout

```
honeycomb/
├── common/            shared headers compiled into BOTH images
│   ├── hc_link_protocol.h      inter-chip message protocol
│   └── hc_ieee802154_defs.h    802.15.4 MAC definitions
├── p4-app/            ESP32-P4 firmware (the brain + UI)
│   ├── main/                   app_main wiring
│   └── components/
│       ├── hc_board/           Tab5 BSP (pins, C6 reset, display hook)
│       ├── hc_link_host/       P4 side of the C6 link
│       ├── hc_mode_manager/    radio mode state machine + event fan-out
│       ├── hc_explorer/        802.15.4 dissector + pcap sink
│       ├── hc_flasher/         USB-host esptool + serial monitor
│       ├── hc_storage/         microSD + pcap writer
│       └── hc_ui/              LVGL 3-tab UI
└── c6-slave/          ESP32-C6 firmware (the radio service)
    ├── main/                   c6_main wiring
    └── components/
        ├── hc_link_slave/      C6 side of the link
        ├── hc_mode_agent/      applies mode commands, forwards captures
        ├── hc_radio_sniffer/   esp_ieee802154 promiscuous capture + scans
        └── hc_radio_wifi/      esp_hosted slave integration point
```

## Install (no toolchain — recommended)

Let GitHub build it and hand you a one-click web flasher — you install nothing.
Full steps in **`docs/flashing.md`**. In short:

1. Upload this folder to a GitHub repo; set **Settings → Pages → Source =
   GitHub Actions**.
2. The `build` workflow compiles both chips and publishes a flasher at
   `https://<you>.github.io/<repo>/`.
3. **P4 app:** flash it from that page over USB-C (Chrome/Edge).
4. **C6 radio:** copy `merged-c6.bin` to the SD card as
   `/honeycomb/c6_service.bin`; the P4 flashes the C6 itself on boot (no USB
   pass-through — uses `esp-serial-flasher`).

## Build (from source, if you prefer)

Requires **ESP-IDF v5.5+**. Two separate images (two chips).

```bash
# C6 radio service
cd c6-slave
idf.py set-target esp32c6
idf.py build flash monitor      # flash the C6 via its own USB / the Tab5 header

# P4 host app
cd ../p4-app
idf.py set-target esp32p4
idf.py reconfigure              # pulls managed components (lvgl, usb_host_vcp, esp_hosted)
idf.py build flash monitor
```

See `docs/hardware.md` for how to reach each chip for flashing on the Tab5.

## What works today vs. what's staged

This is a **clean, correctly-architected scaffold** with the hard interfaces
defined and the highest-value path implemented. It is not yet flashed-and-tested
firmware — the Tab5 is new silicon and several pieces need on-hardware iteration.

| Subsystem | Status |
|---|---|
| Inter-chip link protocol (UART backend) | **Implemented** both sides |
| 802.15.4 sniffer core (C6) | **Implemented** (capture + hop + energy/active scan) |
| 802.15.4 dissector (P4) | **Implemented** (MAC header, addressing, type labels) |
| pcap capture to microSD | **Implemented** |
| Mode manager + C6 reset/reboot switch | **Implemented** |
| esptool bootloader protocol (host) | **Implemented** (classic ROM loader) |
| C6 updater (P4 flashes C6 from SD) | **Implemented** against esp-serial-flasher; port init is the one TODO |
| GitHub Actions build + web flasher | **Implemented** — auto-builds both chips, publishes Pages installer |
| USB-host VCP transport binding | **Integration point** — 4 functions in `hc_flasher.c` |
| LVGL UI | **Skeleton** (3 tabs, headless fallback) |
| Display BSP bring-up | **Integration point** — `hc_board.c` |
| Wi-Fi mode (`esp_hosted` slave) | **Integration point** — `hc_radio_wifi.c` |

Recommended first milestone: get the **sniffer** streaming frames over the UART
link and into Wireshark. It needs no USB host and no display. See
`docs/roadmap.md`.

## Docs

- `ARCHITECTURE.md` — the whole design, data flow, and the mode-switch model.
- `docs/protocol.md` — the inter-chip wire protocol + mode-switch sequence.
- `docs/hardware.md` — Tab5 pinout, the C6 link wiring, USB-host notes.
- `docs/roadmap.md` — phased plan, including the path to Thread/Matter
  *participation* (option 3 you flagged for later).
