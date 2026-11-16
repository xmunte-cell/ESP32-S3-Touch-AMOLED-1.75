/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi helpers for the factory Wi-Fi code:
 *  - WPA2/WPA3-Enterprise (user + password, PEAP/TTLS with MSCHAPv2);
 *  - a list of saved networks (up to 5, normal or company); when the current
 *    one does not answer the board tries the next saved one.
 * Everything is stored in NVS (namespace "jarvis").
 */
#pragma once

#include <stddef.h>

#include <string>
#include <vector>

namespace jarvis::wifi {

struct Network {
    std::string ssid;
    std::string user;      // empty = normal network (password only)
    std::string password;  // empty = open network
};

constexpr size_t MAX_NETWORKS = 5;

// true for the enterprise (802.1X) wifi_auth_mode_t values.
bool isEnterprise(int authmode);

// Call right before esp_wifi_connect(): after two failed tries moves on to
// the next saved network, and turns the enterprise login on or off for it.
void prepareConnect();

std::vector<Network> savedNetworks();
// Adds or updates (same SSID) a network; the newest goes first.
bool rememberNetwork(const Network &network);
bool forgetNetwork(size_t index);

} // namespace jarvis::wifi
