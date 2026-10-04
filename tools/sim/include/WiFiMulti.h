#pragma once
struct WiFiMulti {
  bool addAP(const char *, const char *) { return true; }
  int run(unsigned long = 5000) { return 3; }
};
