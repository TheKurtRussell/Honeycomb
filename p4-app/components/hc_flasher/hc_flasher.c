/*
 * Honeycomb — USB-host ESP flasher implementation
 *
 * Orchestration is real and complete: enter download mode -> sync -> detect ->
 * flash_begin/data/end, streaming image bytes from the SD card. The one piece
 * that must bind to hardware is the USB VCP transport (open the plugged-in
 * device and get read/write/line-control). That is isolated in the four
 * vcp_* functions below and is the single integration point to finish on-device.
 *
 * To wire the transport, add these managed components (idf_component.yml already
 * lists them) and open the device in vcp_open():
 *     espressif/usb_host_vcp, usb_host_cdc_acm, usb_host_cp210x_vcp,
 *     usb_host_ch34x_vcp, usb_host_ftdi_vcp
 * The usb_host_vcp "VCP::open_from_new_dev()" call returns a CdcAcmDevice-like
 * handle exposing tx_blocking()/register_rx + line_coding/control for DTR/RTS.
 */
#include "hc_flasher.h"
#include "hc_esp_loader.h"
#include "hc_board.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if __has_include("usb/usb_host.h")
#include "usb/usb_host.h"
#define HC_HAVE_USB 1
#else
#define HC_HAVE_USB 0
#endif

#define TAG "hc_flash"
#define FLASH_BLOCK 1024

static hc_flash_progress_cb_t s_cb;
static void                  *s_ctx;
static bool                   s_usb_jtag_target;  /* set during detect */

/* ---------------- USB CDC-ACM serial transport -------------------------- */
/* The hc_esp_loader protocol + the serial monitor both talk through these four
 * functions. They wrap the esp usb_host_cdc_acm driver: TX is a blocking write,
 * RX is pulled from a stream buffer that the driver's data callback fills, and
 * DTR/RTS + baud map straight onto the CDC line-control requests. */
#if HC_HAVE_USB && __has_include("usb/cdc_acm_host.h")
#include "usb/cdc_acm_host.h"
#include "freertos/stream_buffer.h"
#define HC_HAVE_CDC 1
static cdc_acm_dev_hdl_t    s_cdc;
static StreamBufferHandle_t s_rx;          /* device -> host byte stream */
static uint32_t             s_baud = 115200;
static volatile bool        s_cdc_ready;   /* true only once a device is fully open */
#else
#define HC_HAVE_CDC 0
#endif

static int vcp_write(const uint8_t *buf, size_t len, void *io) {
    (void)io;
#if HC_HAVE_CDC
    if (s_cdc && cdc_acm_host_data_tx_blocking(s_cdc, buf, len, 1000) == ESP_OK)
        return (int)len;
#endif
    (void)buf; (void)len; return -1;
}
static int vcp_read(uint8_t *buf, size_t len, uint32_t to_ms, void *io) {
    (void)io;
#if HC_HAVE_CDC
    if (s_rx) return (int)xStreamBufferReceive(s_rx, buf, len, pdMS_TO_TICKS(to_ms));
#endif
    (void)buf; (void)len; (void)to_ms; return 0;
}
static void vcp_set_dtr_rts(bool dtr, bool rts, void *io) {
    (void)io;
#if HC_HAVE_CDC
    if (s_cdc) { cdc_acm_host_set_control_line_state(s_cdc, dtr, rts); vTaskDelay(pdMS_TO_TICKS(2)); }
#else
    (void)dtr; (void)rts;
#endif
}
static void vcp_set_baud(uint32_t baud, void *io) {
    (void)io;
#if HC_HAVE_CDC
    s_baud = baud;
    if (s_cdc) {
        cdc_acm_line_coding_t lc = { .dwDTERate = baud, .bCharFormat = 0,
                                     .bParityType = 0, .bDataBits = 8 };
        cdc_acm_host_line_coding_set(s_cdc, &lc);
    }
#else
    (void)baud;
#endif
}

static const hc_loader_transport_t k_transport = {
    .write = vcp_write, .read = vcp_read,
    .set_dtr_rts = vcp_set_dtr_rts, .set_baud = vcp_set_baud, .io = NULL,
};

/* ---------------- chip detect ------------------------------------------- */
/* CHIP_DETECT_MAGIC register (0x40001000) returns a per-target magic. */
#define CHIP_MAGIC_REG 0x40001000u
static hc_target_chip_t magic_to_chip(uint32_t m) {
    switch (m) {
    case 0x00f01d83: return HC_TGT_ESP32;
    case 0x000007c6: return HC_TGT_ESP32S2;   /* example values; verify per esptool */
    case 0x09: /* fallthrough placeholders */  return HC_TGT_ESP32S3;
    default: return HC_TGT_UNKNOWN;
    }
}

const char *hc_target_chip_name(hc_target_chip_t c) {
    switch (c) {
    case HC_TGT_ESP32:   return "ESP32";
    case HC_TGT_ESP32S2: return "ESP32-S2";
    case HC_TGT_ESP32S3: return "ESP32-S3";
    case HC_TGT_ESP32C3: return "ESP32-C3";
    case HC_TGT_ESP32C6: return "ESP32-C6";
    case HC_TGT_ESP32C5: return "ESP32-C5";
    case HC_TGT_ESP32H2: return "ESP32-H2";
    case HC_TGT_ESP32P4: return "ESP32-P4";
    default:             return "unknown";
    }
}

static void report(hc_flash_state_t st, hc_target_chip_t chip,
                   uint32_t done, uint32_t total, const char *msg) {
    if (!s_cb) return;
    hc_flash_progress_t p = { .state = st, .chip = chip,
                              .bytes_total = total, .bytes_done = done };
    strncpy(p.message, msg ? msg : "", sizeof(p.message) - 1);
    s_cb(&p, s_ctx);
}

/* ---------------- public API -------------------------------------------- */
esp_err_t hc_flasher_init(hc_flash_progress_cb_t cb, void *ctx) {
    s_cb = cb; s_ctx = ctx;
    hc_loader_bind(&k_transport);
    return ESP_OK;
}

/* ---------------- USB host bring-up + device enumeration ----------------
 * Iteration 1: bring up the USB host stack on the Tab5's USB-A port and report
 * whatever board is plugged in (VID/PID + device class). This isolates the one
 * thing that can only be tested on hardware — does the port power and enumerate
 * a device — from the serial I/O + flashing, which bind on top next via the
 * usb_host_cdc_acm driver. Core IDF USB host API only (no managed component). */
#if HC_HAVE_USB
static bool                     s_usb_up;
static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_cur_dev;

static void usb_lib_task(void *arg) {
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        (void)flags;  /* NO_CLIENTS / ALL_FREE: nothing to clean up yet */
    }
}

#if HC_HAVE_CDC
static SemaphoreHandle_t s_cdc_open_sig;   /* given when a device wants opening */
static uint16_t          s_want_vid, s_want_pid;

static bool cdc_rx_cb(const uint8_t *data, size_t len, void *arg) {
    (void)arg;
    if (s_rx) xStreamBufferSend(s_rx, data, len, 0);
    return true;   /* data consumed */
}

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *ev, void *arg) {
    (void)arg;
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        s_cdc_ready = false;
        if (s_cdc) { cdc_acm_host_close(s_cdc); s_cdc = NULL; }
        report(HC_FL_IDLE, HC_TGT_UNKNOWN, 0, 0, "device unplugged");
    }
}

/* Opening a CDC device blocks, so it runs on its own task, woken when the raw
 * client enumerates a device and hands over its vid/pid. */
static void cdc_open_task(void *arg) {
    (void)arg;
    const cdc_acm_host_device_config_t dcfg = {
        .connection_timeout_ms = 2000,
        .out_buffer_size = 512,
        .event_cb = cdc_event_cb,
        .data_cb  = cdc_rx_cb,
        .user_arg = NULL,
    };
    while (1) {
        if (xSemaphoreTake(s_cdc_open_sig, portMAX_DELAY) != pdTRUE) continue;
        if (s_cdc) continue;   /* already have a serial session */
        ESP_LOGI(TAG, "cdc: opening %04X:%04X", s_want_vid, s_want_pid);
        esp_err_t e = cdc_acm_host_open(s_want_vid, s_want_pid, 0, &dcfg, &s_cdc);
        ESP_LOGI(TAG, "cdc: open rc=%s", esp_err_to_name(e));
        if (e == ESP_OK) {
            cdc_acm_line_coding_t lc = { .dwDTERate = s_baud, .bCharFormat = 0,
                                         .bParityType = 0, .bDataBits = 8 };
            ESP_LOGI(TAG, "cdc: set line coding");
            cdc_acm_host_line_coding_set(s_cdc, &lc);
            ESP_LOGI(TAG, "cdc: set control lines");
            cdc_acm_host_set_control_line_state(s_cdc, false, false);  /* let it run */
            s_cdc_ready = true;        /* fully open — safe to flash/monitor now */
            ESP_LOGI(TAG, "cdc: ready");
            char m[48];
            snprintf(m, sizeof(m), "serial open @ %lu baud", (unsigned long)s_baud);
            report(HC_FL_DEVICE_READY, HC_TGT_UNKNOWN, 0, 0, m);
        } else {
            ESP_LOGW(TAG, "cdc_acm_host_open: %s", esp_err_to_name(e));
            report(HC_FL_DEVICE_READY, HC_TGT_UNKNOWN, 0, 0, "not a serial (CDC) device");
        }
    }
}
#endif /* HC_HAVE_CDC */

static void describe_device(usb_device_handle_t dev) {
    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || !dd) {
        report(HC_FL_DEVICE_READY, HC_TGT_UNKNOWN, 0, 0, "device (no descriptor)");
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "found %04X:%04X class %02X",
             dd->idVendor, dd->idProduct, dd->bDeviceClass);
    ESP_LOGI(TAG, "USB device: %s", msg);
    report(HC_FL_DEVICE_READY, HC_TGT_UNKNOWN, 0, 0, msg);
    /* VID 0x303A == Espressif native USB-Serial-JTAG: selects the USB-JTAG
     * download-mode reset sequence (vs the classic RTS/DTR one for bridges). */
    s_usb_jtag_target = (dd->idVendor == 0x303A);
#if HC_HAVE_CDC
    s_want_vid = dd->idVendor; s_want_pid = dd->idProduct;
#endif
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (usb_host_device_open(s_client, msg->new_dev.address, &s_cur_dev) == ESP_OK) {
            describe_device(s_cur_dev);
            /* Hand off to the cdc_acm driver: close our descriptor-only handle,
             * then wake the open task (cdc_acm claims the interface itself). */
            usb_host_device_close(s_client, s_cur_dev);
            s_cur_dev = NULL;
#if HC_HAVE_CDC
            if (s_cdc_open_sig) xSemaphoreGive(s_cdc_open_sig);
#endif
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (s_cur_dev) { usb_host_device_close(s_client, s_cur_dev); s_cur_dev = NULL; }
#if !HC_HAVE_CDC
        report(HC_FL_IDLE, HC_TGT_UNKNOWN, 0, 0, "device unplugged");
#endif
        break;
    default: break;
    }
}

static void usb_client_task(void *arg) {
    (void)arg;
    while (1) usb_host_client_handle_events(s_client, portMAX_DELAY);
}
#endif /* HC_HAVE_USB */

esp_err_t hc_flasher_start(void) {
#if HC_HAVE_USB
    if (s_usb_up) {
        /* already running — re-report current state so a reopened Bench updates */
        if (s_cur_dev) describe_device(s_cur_dev);
        else report(HC_FL_IDLE, HC_TGT_UNKNOWN, 0, 0, "waiting for a board…");
        return ESP_OK;
    }
    /* Power the USB-A port AND install the host stack via the BSP. The port's
     * 5 V is off at boot (USB5V_EN on the 0x44 IO expander), so without this a
     * plugged-in board never powers up or enumerates. */
    esp_err_t e = hc_board_usb_host_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "hc_board_usb_host_start: %s", esp_err_to_name(e));
        report(HC_FL_ERROR, HC_TGT_UNKNOWN, 0, 0, "USB host start failed");
        return e;
    }
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);

#if HC_HAVE_CDC
    s_rx = xStreamBufferCreate(4096, 1);
    s_cdc_open_sig = xSemaphoreCreateBinary();
    esp_err_t ce = cdc_acm_host_install(NULL);  /* NULL = default driver config */
    ESP_LOGI(TAG, "cdc_acm_host_install rc=%s", esp_err_to_name(ce));
    if (ce != ESP_OK)
        ESP_LOGW(TAG, "cdc_acm_host_install failed — serial I/O disabled");
    else
        xTaskCreate(cdc_open_task, "cdc_open", 4096, NULL, 7, NULL);
#endif

    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    e = usb_host_client_register(&cc, &s_client);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register: %s", esp_err_to_name(e));
        report(HC_FL_ERROR, HC_TGT_UNKNOWN, 0, 0, "USB client register failed");
        return e;
    }
    xTaskCreate(usb_client_task, "usb_cli", 4096, NULL, 9, NULL);
    s_usb_up = true;
    ESP_LOGI(TAG, "USB host up on USB-A");
    report(HC_FL_IDLE, HC_TGT_UNKNOWN, 0, 0, "USB host up — plug in a board");
    return ESP_OK;
#else
    report(HC_FL_ERROR, HC_TGT_UNKNOWN, 0, 0, "no USB host support");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hc_flasher_detect(hc_target_chip_t *chip_out) {
    ESP_LOGI(TAG, "detect: entering download mode (usb_jtag=%d)", s_usb_jtag_target);
    hc_loader_enter_download_mode(s_usb_jtag_target);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "detect: syncing");
    if (hc_loader_sync() != ESP_OK) { report(HC_FL_ERROR, HC_TGT_UNKNOWN,0,0,"no sync"); return ESP_ERR_TIMEOUT; }

    ESP_LOGI(TAG, "detect: reading chip magic");
    uint32_t magic = 0;
    hc_loader_read_reg(CHIP_MAGIC_REG, &magic);
    hc_target_chip_t chip = magic_to_chip(magic);
    if (chip_out) *chip_out = chip;
    report(HC_FL_SYNCED, chip, 0, 0, hc_target_chip_name(chip));
    return ESP_OK;
}

esp_err_t hc_flasher_write(const hc_flash_region_t *regions, size_t n) {
    hc_target_chip_t chip;
    esp_err_t e = hc_flasher_detect(&chip);
    if (e != ESP_OK) return e;

    /* optional: SPI_ATTACH for targets that need it (safe no-op otherwise) */
    hc_loader_command(LDR_SPI_ATTACH, (const uint8_t[8]){0}, 8, 0, NULL, 1000);

    for (size_t i = 0; i < n; ++i) {
        FILE *f = fopen(regions[i].sd_path, "rb");
        if (!f) { report(HC_FL_ERROR, chip,0,0,"open img fail"); return ESP_FAIL; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);

        report(HC_FL_WRITING, chip, 0, sz, regions[i].sd_path);
        if (hc_loader_flash_begin((uint32_t)sz, regions[i].offset, FLASH_BLOCK) != ESP_OK) {
            fclose(f); report(HC_FL_ERROR, chip,0,sz,"flash_begin"); return ESP_FAIL;
        }

        uint8_t block[FLASH_BLOCK];
        uint32_t seq = 0, done = 0;
        size_t r;
        while ((r = fread(block, 1, FLASH_BLOCK, f)) > 0) {
            if (r < FLASH_BLOCK) memset(block + r, 0xFF, FLASH_BLOCK - r); /* pad */
            if (hc_loader_flash_block(block, FLASH_BLOCK, seq++) != ESP_OK) {
                fclose(f); report(HC_FL_ERROR, chip, done, sz, "flash_data"); return ESP_FAIL;
            }
            done += r;
            report(HC_FL_WRITING, chip, done, (uint32_t)sz, "writing");
        }
        fclose(f);
    }

    hc_loader_flash_end(true); /* reboot target */
    report(HC_FL_DONE, chip, 0, 0, "done");
    return ESP_OK;
}

/* ---------------- serial monitor ---------------------------------------- */
static hc_serial_line_cb_t s_mon_cb;
static void *s_mon_ctx;
static TaskHandle_t s_mon_task;
static volatile bool s_mon_run;

static void monitor_task(void *arg) {
    char line[256]; size_t n = 0; uint8_t b;
    while (s_mon_run) {
        if (vcp_read(&b, 1, 100, NULL) == 1) {
            if (b == '\n' || n == sizeof(line) - 1) {
                line[n] = 0; if (s_mon_cb) s_mon_cb(line, s_mon_ctx); n = 0;
            } else if (b != '\r') line[n++] = (char)b;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t hc_flasher_monitor_start(uint32_t baud, hc_serial_line_cb_t cb, void *ctx) {
    vcp_set_baud(baud, NULL);
    s_mon_cb = cb; s_mon_ctx = ctx;
    if (s_mon_run) return ESP_OK;   /* already monitoring — just updated cb/baud */
    s_mon_run = true;
    return xTaskCreate(monitor_task, "hc_mon", 4096, NULL, 8, &s_mon_task) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t hc_flasher_monitor_stop(void) { s_mon_run = false; return ESP_OK; }

bool hc_flasher_is_ready(void)
{
#if HC_HAVE_CDC
    return s_cdc_ready;
#else
    return false;
#endif
}
