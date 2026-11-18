#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WIFI_STA 1
extern bool fake_wifi_ok;
struct FakeWiFi {
  int status() { return fake_wifi_ok ? WL_CONNECTED : 0; }
  void begin(const char*, const char*, int) {}
  void disconnect() {}
  void mode(int) {}
  int RSSI() { return -55; }
};
extern FakeWiFi WiFi;
struct WiFiClient {};
