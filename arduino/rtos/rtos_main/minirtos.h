// minirtos.h - stand-ins for the few FreeRTOS calls rtos_main uses, for boards that run it as a plain
// loop instead (Feather M0 / SAMD21). Single-threaded: a "queue" is a ring buffer, the mutex does nothing,
// and waiting keeps the buttons/buzzer/LEDs running through idleWork().
// Only used when the board has no FreeRTOS (see the top of rtos_main.ino).
#pragma once
#include <Arduino.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void* TaskHandle_t;
typedef int SemaphoreHandle_t;
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFUL
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

void idleWork();                                   // in rtos_main.ino: runs the io step every 10 ms

inline TickType_t xTaskGetTickCount() { return millis(); }
inline void vTaskDelay(TickType_t ms) {
  uint32_t t0 = millis();
  do { idleWork(); } while (millis() - t0 < ms);
}

struct MiniQueue { uint8_t* buf; uint16_t itemSize, cap, head, count; };
typedef MiniQueue* QueueHandle_t;

inline QueueHandle_t xQueueCreate(int cap, int itemSize) {      // called once in setup()
  static MiniQueue pool[4];
  static int used = 0;
  if (used >= 4) return nullptr;
  MiniQueue* q = &pool[used++];
  q->buf = (uint8_t*)malloc((size_t)cap * itemSize);
  q->itemSize = itemSize; q->cap = cap; q->head = 0; q->count = 0;
  return q;
}
inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t) {     // never waits
  if (!q || !q->buf || q->count >= q->cap) return pdFALSE;
  memcpy(q->buf + ((q->head + q->count) % q->cap) * q->itemSize, item, q->itemSize);
  q->count++;
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t) {        // never waits
  if (!q || !q->count) return pdFALSE;
  memcpy(item, q->buf + q->head * q->itemSize, q->itemSize);
  q->head = (q->head + 1) % q->cap;
  q->count--;
  return pdTRUE;
}

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return 1; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
