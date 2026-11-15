/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WPA2/WPA3-Enterprise (user + password, PEAP/TTLS with MSCHAPv2) for the
 * factory Wi-Fi page. The user name is stored in NVS (Settings > Jarvis);
 * the password is the one typed on the WLAN page.
 */
#pragma once

namespace jarvis::wifi {

// true for the enterprise (802.1X) wifi_auth_mode_t values.
bool isEnterprise(int authmode);

// Call right before esp_wifi_connect(): turns enterprise login on with the
// stored user name when the saved network is an enterprise one, off otherwise.
void prepareConnect();

} // namespace jarvis::wifi
