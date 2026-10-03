#include <assert.h>
#include <stdio.h>
#include "../main/network_credentials.h"

int main(void)
{
    assert(network_credentials_valid("Factory Wi-Fi", "safe-password"));
    assert(network_credentials_valid("Open network", ""));
    assert(network_credentials_valid("  spaces kept  ", "        "));
    assert(network_credentials_valid("12345678901234567890123456789012", "12345678"));
    assert(!network_credentials_valid("123456789012345678901234567890123", "12345678"));
    assert(!network_credentials_valid("", "12345678"));
    assert(!network_credentials_valid(NULL, "12345678"));
    assert(!network_credentials_valid("wifi", NULL));
    assert(!network_credentials_valid("wifi", "short"));
    assert(!network_credentials_valid("wifi\n", "12345678"));
    assert(!network_credentials_valid("wifi", "pass\nword"));
    assert(!network_credentials_valid("wifi", "1234567\177"));
    char password[65];
    memset(password, 'a', sizeof(password));
    password[63] = 0;
    assert(network_credentials_valid("wifi", password));
    password[63] = 'a'; password[64] = 0;
    assert(!network_credentials_valid("wifi", password));
    network_profiles_t profiles = {0};
    network_wifi_credentials_t a = {.ssid = "Factory", .password = "password-A"};
    network_wifi_credentials_t b = {.ssid = "Second", .password = "password-B"};
    assert(network_profile_put(&profiles, &a, false));
    assert(network_profile_put(&profiles, &b, false));
    assert(network_profiles_valid(&profiles));
    assert(profiles.count == 2 && network_profile_find(&profiles, "Factory") == 0);
    strcpy(b.password, "updated-pass");
    assert(network_profile_put(&profiles, &b, true));
    assert(profiles.count == 2 && network_profile_find(&profiles, "Second") == 0);
    assert(!strcmp(profiles.entries[0].password, "updated-pass"));
    assert(!strcmp(profiles.entries[1].ssid, "Factory"));
    for (unsigned i = 2; i < NETWORK_MAX_PROFILES; ++i) {
        network_wifi_credentials_t next = {.password = "password-C"};
        snprintf(next.ssid, sizeof(next.ssid), "Network-%u", i);
        assert(network_profile_put(&profiles, &next, false));
    }
    network_wifi_credentials_t extra = {.ssid = "Overflow", .password = "password-D"};
    assert(!network_profile_put(&profiles, &extra, false));
    assert(network_profiles_valid(&profiles));
    assert(network_profile_put(&profiles, &a, true));
    assert(profiles.count == 5 && network_profile_find(&profiles, "Factory") == 0);
    network_profiles_t invalid = profiles;
    invalid.count = NETWORK_MAX_PROFILES + 1;
    assert(!network_profiles_valid(&invalid));
    invalid = profiles; invalid.entries[1] = invalid.entries[0];
    assert(!network_profiles_valid(&invalid));
    invalid = profiles; memset(invalid.entries[0].ssid, 'x', sizeof(invalid.entries[0].ssid));
    assert(!network_profiles_valid(&invalid));
    puts("PASS: credential validation; multiple profiles, update/deduplication, priority, capacity and corrupt-record rejection");
}
