#pragma once
#include "Arduino.h"
extern float fake_temp, fake_hum; extern bool fake_dht_ok;
struct TempAndHumidity { float temperature; float humidity; };
class DHTesp {
 public:
  enum DHT_MODEL_t { DHT22 = 2 };
  void setup(int, DHT_MODEL_t) {}
  TempAndHumidity getTempAndHumidity() { return fake_dht_ok ? TempAndHumidity{fake_temp, fake_hum} : TempAndHumidity{NAN, NAN}; }
  int getStatus() { return fake_dht_ok ? 0 : 1; }
};
