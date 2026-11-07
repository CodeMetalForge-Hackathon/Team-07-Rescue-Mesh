// rtos_test.ino - small FreeRTOS test on one Team 7 box (no radio).
//
// Arduino IDE: Tools > Board > Adafruit Feather RP2040 RFM
//              Tools > Operating System > FreeRTOS SMP      <-- required
//
// What you should see:
//   - button 1 LED blinks slowly, button 2 LED blinks fast   (2 independent tasks)
//   - LED bar runs a chaser animation                       (another task)
//   - press a button -> short beep (button task -> queue -> buzzer task)
//   - every 5 s the Serial Monitor (115200) prints each task's CPU core and spare stack
//
// Rules this sketch follows (keep them in the real firmware too):
//   1. Each piece of hardware is used by ONE task only.
//   2. Tasks talk through queues, not shared variables.
//   3. Use vTaskDelay()/delay() to wait - they let other tasks run. Never spin in a loop.
//   4. Serial output goes through logf(), which holds a mutex so lines don't get mixed.

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include "pins.h"
#include "ledbar.h"

// ------------------------------------------------------------------ shared
SemaphoreHandle_t logMutex;
QueueHandle_t qPresses;          // button task -> buzzer task: which button (1 or 2)

TaskHandle_t hBlink1, hBlink2, hButtons, hBuzzer, hBar;

// Each task writes the CPU core it last ran on (RP2040 has 2 cores; FreeRTOS SMP uses both).
enum { T_BLINK1, T_BLINK2, T_BUTTONS, T_BUZZER, T_BAR, T_COUNT };
volatile int taskCore[T_COUNT] = {-1, -1, -1, -1, -1};

void logf(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.println(buf);
  xSemaphoreGive(logMutex);
}

// ------------------------------------------------------------------ tasks
// Two blink tasks at different speeds: proof that tasks run independently.
struct BlinkCfg { int pin; int ms; int slot; };
BlinkCfg blink1 = {PIN_BTN1_LED, 500, T_BLINK1};
BlinkCfg blink2 = {PIN_BTN2_LED, 150, T_BLINK2};

void blinkTask(void* arg) {
  BlinkCfg* c = (BlinkCfg*)arg;
  pinMode(c->pin, OUTPUT);
  for (;;) {
    taskCore[c->slot] = rp2040.cpuid();
    digitalWrite(c->pin, HIGH);
    vTaskDelay(pdMS_TO_TICKS(c->ms));
    digitalWrite(c->pin, LOW);
    vTaskDelay(pdMS_TO_TICKS(c->ms));
  }
}

// Polls both buttons every 10 ms, debounces, sends each new press into the queue.
void buttonTask(void*) {
  pinMode(PIN_BTN1, INPUT);       // Grove LED Button has its own pull-up
  pinMode(PIN_BTN2, INPUT);
  const int pins[2] = {PIN_BTN1, PIN_BTN2};
  int stable[2] = {HIGH, HIGH}, count[2] = {0, 0};
  for (;;) {
    taskCore[T_BUTTONS] = rp2040.cpuid();
    for (int i = 0; i < 2; i++) {
      int raw = digitalRead(pins[i]);
      if (raw != stable[i]) {
        if (++count[i] >= 3) {               // same new value 3 times (30 ms) = real change
          stable[i] = raw;
          count[i] = 0;
          if (raw == LOW) {
            int which = i + 1;
            xQueueSend(qPresses, &which, 0);  // don't block if the queue is full
            logf("button %d pressed", which);
          }
        }
      } else {
        count[i] = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Owns the buzzer. Waits (sleeping, no CPU) until a press arrives.
void buzzerTask(void*) {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  int which;
  for (;;) {
    if (xQueueReceive(qPresses, &which, portMAX_DELAY) == pdTRUE) {
      taskCore[T_BUZZER] = rp2040.cpuid();
      for (int k = 0; k < which; k++) {       // button 1 = 1 beep, button 2 = 2 beeps
        digitalWrite(PIN_BUZZER, HIGH);
        vTaskDelay(pdMS_TO_TICKS(70));
        digitalWrite(PIN_BUZZER, LOW);
        vTaskDelay(pdMS_TO_TICKS(70));
      }
    }
  }
}

// Owns the LED bar. A dot runs back and forth.
void barTask(void*) {
  barBegin();
  int pos = 0, dir = 1;
  uint8_t v[10];
  for (;;) {
    taskCore[T_BAR] = rp2040.cpuid();
    for (int i = 0; i < 10; i++) v[i] = 0;
    v[pos] = 80;
    if (pos - dir >= 0 && pos - dir < 10) v[pos - dir] = 12;   // faint tail
    barShow(v);
    pos += dir;
    if (pos == 9 || pos == 0) dir = -dir;
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);

  logMutex = xSemaphoreCreateMutex();
  qPresses = xQueueCreate(8, sizeof(int));

  // xTaskCreate(function, name, stack size in 32-bit words, argument, priority, handle)
  // Higher number = higher priority. setup()/loop() itself runs at priority 1 on core 0.
  xTaskCreate(blinkTask,  "blink1",  512, &blink1, 1, &hBlink1);
  xTaskCreate(blinkTask,  "blink2",  512, &blink2, 1, &hBlink2);
  xTaskCreate(buttonTask, "buttons", 768, nullptr, 3, &hButtons);   // highest: never miss a press
  xTaskCreate(buzzerTask, "buzzer",  512, nullptr, 2, &hBuzzer);
  xTaskCreate(barTask,    "bar",     512, nullptr, 1, &hBar);

  logf("=== Team 7 FreeRTOS test ===");
  logf("Button 1 LED slow, button 2 LED fast, bar animating. Press a button to beep.");
}

// loop() is also a task. Here it reports on the others every 5 s.
void loop() {
  struct { TaskHandle_t h; const char* name; int slot; } t[] = {
    {hBlink1, "blink1", T_BLINK1}, {hBlink2, "blink2", T_BLINK2}, {hButtons, "buttons", T_BUTTONS},
    {hBuzzer, "buzzer", T_BUZZER}, {hBar, "bar", T_BAR}};
  logf("--- tasks (loop() is on core %d) ---", (int)rp2040.cpuid());
  for (auto& x : t) {
    int c = taskCore[x.slot];
    if (c < 0) logf("  %-8s core -   spare stack: %4u words  (hasn't run yet)", x.name,
                    (unsigned)uxTaskGetStackHighWaterMark(x.h));
    else       logf("  %-8s core %d   spare stack: %4u words", x.name, c,
                    (unsigned)uxTaskGetStackHighWaterMark(x.h));
  }
  logf("  free heap: %d bytes", (int)rp2040.getFreeHeap());
  delay(5000);   // delay() also yields to other tasks under FreeRTOS
}
