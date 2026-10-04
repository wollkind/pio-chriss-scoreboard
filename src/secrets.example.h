#pragma once

// Copy this file to src/secrets.h (git-ignored) and fill it in.
// While secrets.h is missing, this file is used and the panel shows "SET WIFI".

// Every network the panel may join, as { "SSID", "password" }. At startup and whenever the
// connection drops, it scans and joins the strongest one in range. Add as many as needed.
#define WIFI_NETWORKS {      \
  { "", "" },                \
}
// An older secrets.h with WIFI_SSID / WIFI_PASSWORD (one network) still works.

// Password for over-the-air updates. Empty means anyone on your network can flash the panel.
// Use the same value in the SCOREBOARD_OTA_PASSWORD environment variable when uploading.
#define OTA_PASSWORD ""
