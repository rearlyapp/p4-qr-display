#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_MAX_PROFILES 5
typedef struct { char ssid[33], password[64]; } network_wifi_credentials_t;
typedef struct {
    uint8_t version, count;
    network_wifi_credentials_t entries[NETWORK_MAX_PROFILES];
} network_profiles_t;

/* SSID limits are bytes, not characters. Do not trim spaces from either field. */
static inline bool network_credentials_valid(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t sn = strlen(ssid), pn = strlen(password);
    if (!sn || sn > 32 || (pn != 0 && (pn < 8 || pn > 63))) return false;
    for (size_t i = 0; i < sn; ++i)
        if ((unsigned char)ssid[i] < 32 || (unsigned char)ssid[i] == 127) return false;
    for (size_t i = 0; i < pn; ++i)
        if ((unsigned char)password[i] < 32 || (unsigned char)password[i] > 126) return false;
    return true;
}

static inline int network_profile_find(const network_profiles_t *profiles, const char *ssid)
{
    for (unsigned i = 0; i < profiles->count; ++i)
        if (!strcmp(profiles->entries[i].ssid, ssid)) return (int)i;
    return -1;
}

static inline bool network_profiles_valid(const network_profiles_t *profiles)
{
    if (profiles->version != 2 || !profiles->count || profiles->count > NETWORK_MAX_PROFILES) return false;
    for (unsigned i = 0; i < profiles->count; ++i) {
        const network_wifi_credentials_t *entry = &profiles->entries[i];
        if (entry->ssid[32] || entry->password[63] ||
            !network_credentials_valid(entry->ssid, entry->password)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(entry->ssid, profiles->entries[j].ssid)) return false;
    }
    return true;
}

/* Update by exact SSID; never silently evict a network when full. */
static inline bool network_profile_put(network_profiles_t *profiles,
                                       const network_wifi_credentials_t *entry, bool preferred)
{
    if (profiles->count > NETWORK_MAX_PROFILES || entry->ssid[32] || entry->password[63] ||
        !network_credentials_valid(entry->ssid, entry->password)) return false;
    network_wifi_credentials_t copy = *entry;
    int index = network_profile_find(profiles, entry->ssid);
    if (index < 0) {
        if (profiles->count == NETWORK_MAX_PROFILES) return false;
        index = profiles->count++;
    }
    if (preferred) {
        memmove(&profiles->entries[1], &profiles->entries[0], (size_t)index * sizeof(*entry));
        index = 0;
    }
    profiles->entries[index] = copy;
    profiles->version = 2;
    return true;
}
