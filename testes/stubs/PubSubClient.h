#pragma once
#include "Arduino.h"
#include <vector>
#include <string>
#include <functional>
extern bool fake_broker_ok;
struct Pub { std::string topic, msg; bool retained; };
extern std::vector<Pub> fake_pubs;
class PubSubClient {
 public:
  typedef void (*Cb)(char*, byte*, unsigned int);
  Cb cb = nullptr; bool con = false;
  PubSubClient(WiFiClient&) {}
  void setServer(const char*, int) {}
  void setCallback(Cb c) { cb = c; }
  bool setBufferSize(int) { return true; }
  bool connect(const char*, const char*, int, bool, const char*) { con = fake_broker_ok; return con; }
  bool connected() { if (!fake_broker_ok) con = false; return con; }
  bool publish(const char* t, const char* m, bool r = false) {
    if (!connected()) return false;
    fake_pubs.push_back({t, m, r}); return true;
  }
  bool subscribe(const char*) { return true; }
  bool loop() { return connected(); }
  int state() { return -2; }
};
