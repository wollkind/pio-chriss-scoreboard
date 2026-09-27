// Host stand-in for the Arduino core: just enough for src/main.cpp, Adafruit GFX and ArduinoJson to
// compile on a desktop so tools/sim can render the panel's frames. Nothing here talks to hardware.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define PROGMEM
#define F(s) (s)
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define pgm_read_pointer(a) (*(void *const *)(a))
typedef bool boolean;
typedef uint8_t byte;

using std::max;
using std::min;

uint32_t millis();   // the simulation clock (tools/sim/sim.cpp)
inline void delay(uint32_t) {}
inline uint32_t esp_random() { return 12345; }
inline void *ps_malloc(size_t n) { return malloc(n); }
#define radians(deg) ((deg) * 0.017453292519943295)
#define degrees(rad) ((rad) * 57.29577951308232)
class __FlashStringHelper;

class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String &operator=(const char *s) { s_ = s ? s : ""; return *this; }
  bool concat(const char *s) { s_ += s; return true; }
  bool concat(char c) { s_ += c; return true; }
  const char *c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  long toInt() const { return atol(s_.c_str()); }
  char operator[](size_t i) const { return s_[i]; }
 private:
  std::string s_;
};

class Print;
class Printable {
 public:
  virtual ~Printable() {}
  virtual size_t printTo(Print &p) const = 0;
};

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t *s, size_t n) { size_t k = 0; while (n--) k += write(*s++); return k; }
  size_t print(const char *s) { return write(reinterpret_cast<const uint8_t *>(s), strlen(s)); }
  size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  size_t print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return print(b); }
  size_t print(const String &s) { return print(s.c_str()); }
  size_t println(const char *s = "") { return print(s) + print('\n'); }
  size_t printf(const char *fmt, ...) { char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); return print(b); }
};

class Stream : public Print {
 public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  int read(uint8_t *, size_t) { return 0; }
  size_t readBytes(char *, size_t) { return 0; }
  size_t write(uint8_t) override { return 1; }
  void setTimeout(unsigned long) {}
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  void setTxTimeoutMs(uint32_t) {}
};
extern HardwareSerial Serial;

// FreeRTOS: single-threaded in the simulation.
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void *>(1); }
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
inline void vTaskDelay(TickType_t) {}
inline uint32_t ulTaskNotifyTake(int, TickType_t) { return 0; }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline void vTaskSuspend(TaskHandle_t) {}
inline void vTaskResume(TaskHandle_t) {}
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *, int) { return 1; }
