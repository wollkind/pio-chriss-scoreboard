#pragma once

// Copy this file to src/secrets.h (git-ignored) and fill it in.
// While secrets.h is missing, this file is used and the panel shows "SET WIFI".

#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// Password for over-the-air updates. Empty means anyone on your network can flash the panel.
// Use the same value in the SCOREBOARD_OTA_PASSWORD environment variable when uploading.
#define OTA_PASSWORD ""
