#pragma once
#include "Arduino.h"
struct WiFiClient : public Stream { bool connected() { return false; } };
struct WiFiClientSecure : public WiFiClient { void setInsecure() {} };
