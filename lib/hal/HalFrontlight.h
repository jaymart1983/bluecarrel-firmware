#pragma once

#include <FrontlightManager.h>

// Thin firmware HAL over the SDK frontlight manager. It is inert on boards
// without a frontlight, so callers do not need board-specific conditionals.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  void begin(uint8_t brightness, uint8_t warmth, bool on);

  bool present() const { return manager.present(); }
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }

  // Brightness integrated over time since begin(), for the power log's line on
  // what the light actually cost. Integer accumulation closed on each change --
  // no timer, no per-frame work. `percentMs` is the sum of brightness% x ms lit,
  // so dividing it by the awake time gives the mean brightness over a session.
  // uint64_t because 100% for twelve hours overflows a uint32_t of percent-ms.
  struct Energy {
    uint64_t percentMs;
    uint32_t litMs;
  };
  Energy energySinceBoot() const;

  // Deep sleep: turn the light off, stop the PWM and hold the LED pads at their
  // off level. The pads would otherwise float while the rail feeding the LED
  // driver stays up (X4 Pro: power.latch0, held HIGH through sleep). The holds
  // outlive the wake reset; begin() releases them.
  void parkForDeepSleep();

 private:
  HalFrontlight() = default;

  // Closes the running brightness segment at the current level. Main-loop task
  // only, like every frontlight setter.
  void closeEnergySegment();

  FrontlightManager manager;
  // The SDK represents off as brightness 0. Keep the selected brightness so
  // toggling back on restores it.
  uint8_t lastBrightness = 60;
  bool lit = false;
  uint64_t energyPercentMs = 0;
  uint32_t energyLitMs = 0;
  uint32_t energySinceMs = 0;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
