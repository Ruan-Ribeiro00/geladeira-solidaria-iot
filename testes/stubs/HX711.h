#pragma once
#include "Arduino.h"
extern long fake_raw;
class HX711 {
 public:
  float scale = 1; long offset = 0;
  void begin(int, int) {}
  void set_scale(float s) { scale = s; }
  void tare() { offset = fake_raw; }
  bool wait_ready_timeout(unsigned long) { return true; }
  float get_units(int) { return (fake_raw - offset) / scale; }
};
