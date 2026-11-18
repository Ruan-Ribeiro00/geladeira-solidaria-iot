// Stub minimo do Arduino para compilar e simular o firmware no PC (teste host).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <string>
using std::isnan;
typedef uint8_t byte;
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
extern unsigned long fake_millis;
extern int fake_pins[64];
extern int fake_tone_freq;
inline unsigned long millis() { return fake_millis; }
inline void delay(unsigned long ms) { fake_millis += ms; }
inline void pinMode(int, int) {}
inline int digitalRead(int p) { return fake_pins[p]; }
inline void digitalWrite(int p, int v) { fake_pins[p] = v; }
inline void tone(int, unsigned int f) { fake_tone_freq = f; }
inline void noTone(int) { fake_tone_freq = 0; }
inline long random(long m) { return rand() % m; }
inline void randomSeed(unsigned long s) { srand(s); }
inline uint32_t esp_random() { return 1234; }
struct FakeSerial {
  bool quiet = false;
  void begin(int) {}
  void println(const char* s = "") { if (!quiet) std::printf("%s\n", s); }
  void print(const char* s) { if (!quiet) std::printf("%s", s); }
  void printf(const char* fmt, ...) { if (quiet) return; va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a); }
};
extern FakeSerial Serial;
