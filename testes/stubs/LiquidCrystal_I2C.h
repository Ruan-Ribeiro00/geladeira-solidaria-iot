#pragma once
#include "Arduino.h"
#include <string>
extern char fake_lcd[2][17];
class LiquidCrystal_I2C {
  int r = 0, c = 0;
 public:
  LiquidCrystal_I2C(int, int, int) { clear(); }
  void clear() { memset(fake_lcd, ' ', sizeof(fake_lcd)); fake_lcd[0][16] = fake_lcd[1][16] = 0; }
  void init() {}
  void backlight() {}
  void setCursor(int col, int row) { c = col; r = row; }
  void print(const char* s) { while (*s) { if (c < 16) fake_lcd[r][c] = *s; c++; s++; } }
  void print(char ch) { char b[2] = {ch, 0}; print(b); }
  void print(int v) { char b[12]; snprintf(b, sizeof b, "%d", v); print(b); }
};
