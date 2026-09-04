// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"

// Start the ESP-NOW peer provider: initialise dc_peer with this DragonBreath's
// stable id and periodically broadcast its heater capability (dc_peer_heater_t) for
// consumers such as DragonVent. Advisory only — carries state, never control.
// Call once, AFTER dc_wifi_start().
esp_err_t pb_peer_start(void);
