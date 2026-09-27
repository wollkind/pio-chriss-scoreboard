#pragma once
#include "WiFiClientSecure.h"
#define HTTP_CODE_OK 200
struct HTTPClient {
  void useHTTP10(bool) {}
  void setConnectTimeout(int) {}
  void setTimeout(int) {}
  bool begin(WiFiClient &, const char *) { return false; }
  int GET() { return -1; }
  String getString() { return String(); }
  WiFiClient *getStreamPtr() { return nullptr; }
  void end() {}
};
