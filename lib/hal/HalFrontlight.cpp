#include "HalFrontlight.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <driver/gpio.h>

HalFrontlight HalFrontlight::instance;

namespace {
// The LEDC-driven frontlight pads, or -1. Boards whose light is behind a PMIC
// or on I2C have no ESP pad to park.
void ledPads(int8_t& cool, int8_t& warm) {
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  cool = fl.viaPm1Pwm ? BoardConfig::PIN_UNASSIGNED : fl.gpio;
  warm = fl.viaPm1Pwm ? BoardConfig::PIN_UNASSIGNED : fl.gpioWarm;
}
}  // namespace

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  if (!manager.present()) return;

  // parkForDeepSleep() left the pads held, and a held pad ignores the LEDC
  // drive. Released unconditionally: nothing in RAM survives the wake reset to
  // say whether they were parked, and releasing an unheld pad is a no-op.
  int8_t cool = BoardConfig::PIN_UNASSIGNED;
  int8_t warm = BoardConfig::PIN_UNASSIGNED;
  ledPads(cool, warm);
  for (const int8_t pin : {cool, warm}) {
    if (pin >= 0) gpio_hold_dis(static_cast<gpio_num_t>(pin));
  }

  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  if (lit) manager.setBrightness(lastBrightness);
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
}

void HalFrontlight::parkForDeepSleep() {
  if (!manager.present()) return;
  manager.setBrightness(0);
  const bool activeHigh = BoardConfig::ACTIVE.frontlight.activeHigh;
  int8_t cool = BoardConfig::PIN_UNASSIGNED;
  int8_t warm = BoardConfig::PIN_UNASSIGNED;
  ledPads(cool, warm);
  for (const int8_t pin : {cool, warm}) {
    if (pin < 0) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // pinMode() detaches the LEDC channel from the pad (Arduino-ESP32 3.x
    // peripheral manager), so the level below is the GPIO's, not the PWM's.
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, activeHigh ? LOW : HIGH);
    // Survives deep sleep through gpio_deep_sleep_hold_en() in
    // freeink::PowerManager::deepSleep().
    gpio_hold_en(g);
  }
  LOG_INF("LIGHT", "Frontlight parked for deep sleep (pads %d/%d held off)", cool, warm);
}
