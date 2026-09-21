#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;
  // The low clock was taken by a panel BUSY wait (beginPanelWait), not by idle.
  bool panelWaitLow = false;
  // Serialises clock changes: the main loop (setPowerSaving) and the render task
  // (Lock, panel waits) both switch the clock.
  SemaphoreHandle_t freqMutex = nullptr;

  // Power counters since boot (every wake is a reset). Written under freqMutex.
  uint32_t freqSinceMs = 0;  // millis() of the last clock change
  uint32_t normalMs = 0;     // closed segments at normalFreq
  uint32_t lowMs = 0;        // closed segments at LOW_POWER_FREQ
  uint32_t panelWaitStartMs = 0;
  uint32_t panelWaitMsTotal = 0;
  uint32_t panelWaitCount = 0;
  bool panelWaitOpen = false;
  // Caller holds freqMutex. Closes the running segment and switches.
  bool switchFrequency(bool low);

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
  // After the last input or activity, drop to LOW_POWER_FREQ this soon. Every
  // input restores the full clock before its frame is handled, and renders hold
  // a Lock, so this only trims idle time at full speed. 80 MHz keeps the APB
  // (SPI, I2C, UART) clock unchanged, so switching is cheap.
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 1000;  // ms
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
#endif
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Panel BUSY wait hooks (FreeInkDisplay::setBusyWaitHooks). The SDK calls them
  // once a BUSY wait passes 20 ms, i.e. while the panel runs its waveform and
  // the CPU only polls a pin. The clock drops to LOW_POWER_FREQ for the wait
  // even under a Lock and returns to normalFreq when it ends; the render work
  // before and after the wait keeps the full clock. A setPowerSaving(false)
  // during the wait (input, a spinning loop) restores it early.
  static void panelWaitBeginHook();
  static void panelWaitEndHook();

  // Cumulative counters since boot/wake for `about`.
  struct Counters {
    uint32_t normalMs;  // at normalFreq
    uint32_t lowMs;     // at LOW_POWER_FREQ
    int normalMhz;
    int lowMhz;
    uint32_t panelWaitMs;  // in panel BUSY waits longer than 20 ms, from the 20 ms mark
    uint32_t panelWaits;
  };
  Counters counters() const;

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
