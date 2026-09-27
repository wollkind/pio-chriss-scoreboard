#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WIFI_STA 1
struct IPAddress { String toString() const { return String("192.168.1.42"); } };
struct WiFiClass {
  int status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress(); }
  void mode(int) {}
  void setHostname(const char *) {}
  void setAutoReconnect(bool) {}
  void setSleep(bool) {}
  void begin(const char *, const char *) {}
};
extern WiFiClass WiFi;
