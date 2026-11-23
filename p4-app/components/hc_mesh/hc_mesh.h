/*
 * Honeycomb — LAN mesh discovery
 *
 * A self-contained first step toward Honeycomb-to-Honeycomb mesh: each unit
 * broadcasts a small UDP presence beacon on the local WiFi network and listens
 * for others, so units on the same LAN discover each other by name, IP and
 * uptime. This needs only the stock C6 (ordinary station networking, no
 * promiscuous/ESP-NOW), works with two or more units on one network, and is a
 * no-op (safe) when WiFi isn't connected or only one unit is present.
 */
#ifndef HC_MESH_H
#define HC_MESH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name[24];   /* peer's advertised name, e.g. "hc-A1B2C3" */
    char     ip[16];     /* dotted IPv4 */
    uint32_t uptime_s;   /* peer uptime it reported */
    uint32_t wifi_cnt;   /* peer's logged WiFi count */
    uint32_t ble_cnt;    /* peer's logged BLE count */
    int32_t  age_s;      /* seconds since we last heard from it */
} hc_mesh_peer_t;

/* Start broadcasting/listening. Returns false if WiFi has no IP yet (caller
 * should tell the user to join a network first). Safe to call repeatedly. */
bool hc_mesh_start(void);

/* Stop the mesh task and close the socket. Safe if not running. */
void hc_mesh_stop(void);

/* True while the mesh task is active. */
bool hc_mesh_running(void);

/* This unit's advertised name and current IP (for display). */
const char *hc_mesh_self_name(void);
const char *hc_mesh_self_ip(void);

/* Copy up to `max` known peers into out, most-recently-heard first. */
int hc_mesh_get(hc_mesh_peer_t *out, int max);

#ifdef __cplusplus
}
#endif
#endif /* HC_MESH_H */
