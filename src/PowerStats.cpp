#include "PowerStats.h"

namespace power_stats {
namespace {

constexpr uint32_t WINDOW_S = 60;
constexpr uint32_t BUCKET_MS = 1000;

// Two rings of sixty 16-bit per-second counts: 240 bytes of static DRAM, fixed
// size and never reallocated, so the counters cost no heap. 16 bits saturate at
// 65535 a second, far above any loop rate this firmware reaches.
uint16_t loopBuckets[WINDOW_S] = {};
uint16_t touchBuckets[WINDOW_S] = {};
uint32_t filled = 0;  // complete buckets held
uint32_t head = 0;    // next bucket to write
uint32_t bucketStartMs = 0;
uint32_t loopsInBucket = 0;
uint32_t touchAtBucketStart = 0;
bool started = false;

uint16_t saturate(const uint32_t value) { return value > 0xFFFFU ? 0xFFFFU : static_cast<uint16_t>(value); }

void closeBucket(const uint32_t loops, const uint32_t touches) {
  loopBuckets[head] = saturate(loops);
  touchBuckets[head] = saturate(touches);
  head = (head + 1) % WINDOW_S;
  if (filled < WINDOW_S) filled++;
}

}  // namespace

void noteLoopPass(const uint32_t nowMs, const uint32_t touchReads) {
  if (!started) {
    started = true;
    bucketStartMs = nowMs;
    touchAtBucketStart = touchReads;
  }
  // A pass that arrives after a stall closes the seconds it missed as well;
  // they held no passes. At most a window's worth, then the clock resyncs.
  uint32_t closed = 0;
  while (nowMs - bucketStartMs >= BUCKET_MS && closed < WINDOW_S) {
    closeBucket(loopsInBucket, touchReads - touchAtBucketStart);
    loopsInBucket = 0;
    touchAtBucketStart = touchReads;
    bucketStartMs += BUCKET_MS;
    closed++;
  }
  if (nowMs - bucketStartMs >= BUCKET_MS) bucketStartMs = nowMs;
  loopsInBucket++;
}

Rates lastMinute() {
  Rates rates{};
  rates.seconds = filled;
  if (filled == 0) return rates;
  uint32_t loopSum = 0;
  uint32_t touchSum = 0;
  for (uint32_t i = 0; i < filled; i++) {
    loopSum += loopBuckets[i];
    touchSum += touchBuckets[i];
    if (loopBuckets[i] > rates.loopMax) rates.loopMax = loopBuckets[i];
    if (touchBuckets[i] > rates.touchMax) rates.touchMax = touchBuckets[i];
  }
  rates.loopAvg = loopSum / filled;
  rates.touchAvg = touchSum / filled;
  return rates;
}

}  // namespace power_stats
