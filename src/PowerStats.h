#pragma once

#include <cstdint>

// Main-loop pass and touch-read rates for the `power` counters in `about`.
// Main loop task only: noteLoopPass() and lastMinute() both run there.
namespace power_stats {

// Once per main-loop pass. `touchReads` is the running touch controller read
// count (HalGPIO::touchReadCount()).
void noteLoopPass(uint32_t nowMs, uint32_t touchReads);

struct Rates {
  uint32_t loopAvg;   // passes per second, mean over the window
  uint32_t loopMax;   // busiest second in the window
  uint32_t touchAvg;  // touch controller reads per second, mean
  uint32_t touchMax;
  uint32_t seconds;  // complete seconds in the window (up to 60)
};
// Over the last minute of complete one-second buckets.
Rates lastMinute();

}  // namespace power_stats
