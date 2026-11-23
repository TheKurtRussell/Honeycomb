/* Honeycomb — surveillance-infrastructure signatures (see hc_sigs.h)
 *
 * Flock Safety OUI list + SSID patterns sourced from open-source community
 * research (flock-finder / DeFlock, @NitekryDPaul et al.). OUI matches are
 * "suspected" and low-confidence by nature (shared chip vendors); SSID matches
 * are strong. Kept in one table so it is easy to extend as signatures evolve.
 */
#include "hc_sigs.h"
#include <string.h>
#include <ctype.h>

/* 3-byte OUI prefixes packed as 0xAABBCC. */
static const uint32_t FLOCK_OUI[] = {
    0x70C94E, 0x3C9180, 0xD8F3BC, 0x803049, 0xB83532, 0x145AFC, 0x744CA1,
    0x083A88, 0x9C2F9D, 0xC03532, 0x940853, 0xE4AAEA, 0xF46ADD, 0xF8A2D6,
    0x24B2B9, 0x00F48D, 0xD03957, 0xE8D0FC, 0xE04F43, 0xB81EA4, 0x700894,
    0x588E81, 0xEC1BBD, 0x3C71BF, 0x5800E3, 0x9035EA, 0x5C93A2, 0x646E69,
    0x4827EA, 0xA4CF12, 0x826BF2, 0xB41E52, 0x040D84, 0xF082C0, 0x1C34F1,
    0x385B44, 0x943469, 0xB4E3F9, 0xE00AF6,
};
#define FLOCK_OUI_N (sizeof(FLOCK_OUI)/sizeof(FLOCK_OUI[0]))

/* Case-insensitive "does s start with prefix?" */
static int starts_with_ci(const char *s, const char *prefix)
{
    if (!s) return 0;
    for (; *prefix; s++, prefix++) {
        if (!*s) return 0;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    }
    return 1;
}

int hc_sig_wifi(const char *ssid, const uint8_t mac[6])
{
    /* SSID is the strong signal (the camera's own management/battery network). */
    if (ssid && ssid[0]) {
        if (starts_with_ci(ssid, "flock") ||          /* Flock, Flock-XXXXXX, FLOCK% */
            starts_with_ci(ssid, "fs ext battery"))   /* battery-pack accessory      */
            return HC_SIG_FLOCK_SSID;
    }
    /* OUI is the weak signal (chip-vendor prefix, shared by other hardware). */
    if (mac) {
        uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
        for (unsigned i = 0; i < FLOCK_OUI_N; i++)
            if (FLOCK_OUI[i] == oui) return HC_SIG_FLOCK_OUI;
    }
    return HC_SIG_NONE;
}

const char *hc_sig_label(int sig)
{
    switch (sig) {
        case HC_SIG_FLOCK_SSID: return "Flock ALPR (SSID)";
        case HC_SIG_FLOCK_OUI:  return "possible Flock (OUI)";
        default:                return "";
    }
}

int hc_sig_confidence(int sig)
{
    switch (sig) {
        case HC_SIG_FLOCK_SSID: return 2;
        case HC_SIG_FLOCK_OUI:  return 1;
        default:                return 0;
    }
}
