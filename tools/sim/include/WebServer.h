#pragma once
#include "Arduino.h"
#include <functional>
#include <map>
enum HTTPMethod { HTTP_GET, HTTP_POST };
struct WebServer {
  explicit WebServer(int) {}
  std::map<std::string, std::string> args;   // set by the simulation before calling a handler
  void on(const char *, HTTPMethod, std::function<void()>) {}
  void onNotFound(std::function<void()>) {}
  void begin() {}
  void handleClient() {}
  bool hasArg(const char *k) { return args.count(k) > 0; }
  String arg(const char *k) { return args.count(k) ? String(args[k]) : String(); }
  void send(int, const char *, const char *) {}
  void send(int, const char *, const String &) {}
  void send_P(int, const char *, const char *) {}
};
