#include "CpuDoze.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>

namespace cpu_doze {
namespace {

// After this many refusals from the platform, stop asking. A build or a board
// that cannot light sleep should cost one log line, not a failed syscall per
// loop pass for the rest of the session.
constexpr uint8_t MAX_FAILURES = 3;

// A doze that returns almost immediately is reported as no doze at all, so the
// caller falls back to its ordinary wait. Without this, a wake source that is
// already asserted for a reason we did not predict turns the idle branch into a
// hot loop at full speed -- worse than the delay it replaced. A real key or
// touch wake also lands here, and paying the ordinary 50 ms wait for it is the
// latency the firmware already had.
constexpr int64_t MIN_USEFUL_US = 2000;

bool armed = false;
bool givenUp = false;
uint8_t failures = 0;
uint64_t dozedUs = 0;
uint32_t dozeCount = 0;

// The touch interrupt and its resting level. Light sleep only offers a level
// wake, and this board's GT911 interrupt polarity is not documented, so the
// resting level is sampled once at boot and the wake is whatever it is not.
gpio_num_t touchIrq = GPIO_NUM_NC;
int touchIdleLevel = 1;

// A pressed button holds its pin at the active level, which is the only thing
// light sleep can wake on.
void armButton(const int8_t pin, const bool activeHigh) {
  if (pin < 0) return;
  const auto gpio = static_cast<gpio_num_t>(pin);
  const gpio_int_type_t level = activeHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
  if (gpio_wakeup_enable(gpio, level) != ESP_OK) {
    LOG_ERR("DOZE", "GPIO%d refused as a wake source", pin);
  }
}

}  // namespace

void begin() {
  const auto& buttons = BoardConfig::ACTIVE.input;
  armButton(buttons.power, buttons.powerActiveHigh);
  // The side keys are plain active-low GPIO on every board that has them.
  armButton(buttons.up, false);
  armButton(buttons.down, false);

  const auto& touch = BoardConfig::ACTIVE.touch;
  if (touch.irq >= 0) {
    touchIrq = static_cast<gpio_num_t>(touch.irq);
    touchIdleLevel = gpio_get_level(touchIrq);
    const gpio_int_type_t level = touchIdleLevel == 0 ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
    if (gpio_wakeup_enable(touchIrq, level) != ESP_OK) {
      LOG_ERR("DOZE", "touch IRQ GPIO%d refused as a wake source", touch.irq);
      touchIrq = GPIO_NUM_NC;
    }
  }

  if (esp_sleep_enable_gpio_wakeup() != ESP_OK) {
    LOG_ERR("DOZE", "GPIO wake unavailable; the core will not doze");
    return;
  }
  armed = true;
  LOG_INF("DOZE", "armed: power/up/down + touch IRQ GPIO%d idle=%d", static_cast<int>(touchIrq), touchIdleLevel);
}

bool doze(const uint32_t ms, const Preconditions& pre) {
  if (!armed || givenUp || ms == 0) return false;
  if (!pre.radioParked || pre.usbPresent || !pre.inputIdle || !pre.renderIdle) return false;
  // Already asserted: there is a touch report waiting, so the doze would return
  // immediately and the poll should just run.
  if (touchIrq != GPIO_NUM_NC && gpio_get_level(touchIrq) != touchIdleLevel) return false;

  if (esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(ms) * 1000ULL) != ESP_OK) return false;
  const int64_t startUs = esp_timer_get_time();
  if (esp_light_sleep_start() != ESP_OK) {
    if (++failures >= MAX_FAILURES) {
      givenUp = true;
      LOG_ERR("DOZE", "light sleep refused %u times; giving up for this session", failures);
    }
    return false;
  }
  failures = 0;
  const int64_t sleptUs = esp_timer_get_time() - startUs;
  if (sleptUs < MIN_USEFUL_US) return false;
  dozedUs += static_cast<uint64_t>(sleptUs);
  dozeCount++;
  return true;
}

Counters counters() { return Counters{static_cast<uint32_t>(dozedUs / 1000ULL), dozeCount}; }

}  // namespace cpu_doze
