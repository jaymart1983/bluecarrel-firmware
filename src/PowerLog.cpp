#include "PowerLog.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace power_log {
namespace {

constexpr char LOG_PATH[] = "/power_log.csv";
constexpr char LOG_PATH_PREVIOUS[] = "/power_log.1.csv";
constexpr char HEADER[] = "time,pct,mv,uptime_s,event,detail\n";

constexpr uint32_t ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t FLUSH_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t AWAKE_SAMPLE_MS = 60UL * 60UL * 1000UL;
// A brightness slider calls the frontlight setter per step; one adjustment is
// one line, written once the value has held still this long.
constexpr uint32_t LIGHT_SETTLE_MS = 2000UL;
// The gauge sits on the shared touch/RTC bus. One read per burst of events is
// plenty: a percent does not move inside two seconds.
constexpr uint32_t BATTERY_TTL_MS = 2000UL;

// Sized so no line can ever be truncated: 23 (time) + 3 + 5 + 10 (uptime) + 9
// (event) + 5 commas + 2 quotes + DETAIL_BYTES + newline fits inside LINE_BYTES.
// Both buffers are locals, and together with the two number fields they keep
// writeEvent's frame under the 256-byte stack rule.
constexpr size_t LINE_BYTES = 136;
constexpr size_t DETAIL_BYTES = 72;
// Eight lines. Fixed static DRAM, never reallocated, so the log costs no heap
// and cannot grow with the session.
constexpr size_t BUFFER_BYTES = 1024;

constexpr uint16_t UNKNOWN16 = 0xFFFFU;

// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// sleep snapshot was taken before the reset (same rule as the panic capture in
// HalSystem.cpp).
constexpr uint32_t SNAPSHOT_MAGIC = 0x504C4731UL;  // "PLG1"

// What the previous sleep left for the wake after it to measure against. Wake
// from deep sleep is a fresh boot, so nothing in DRAM survives to carry this.
struct SleepSnapshot {
  uint32_t magic;
  uint32_t epoch;    // UTC at sleep; 0 when the device did not know the time
  uint32_t uptimeS;  // how long it had been awake before sleeping
  uint16_t pct;      // gauge state of charge, or UNKNOWN16
  uint16_t mv;       // gauge cell voltage, or UNKNOWN16
};
RTC_NOINIT_ATTR SleepSnapshot sleepSnapshot;

char pending[BUFFER_BYTES];
size_t pendingLen = 0;
uint32_t fileBytes = 0;  // bytes already on the card, so rotation needs no stat
uint32_t droppedLines = 0;

bool started = false;
// Latched by noteSleep(): the card is about to be stopped, so nothing may open
// it again. This is the ordering the last release's sleep crash was about.
bool sealed = false;

uint32_t lastFlushMs = 0;
uint32_t lastAwakeSampleMs = 0;

uint32_t bookOpenedAtMs = 0;
uint32_t bleConnectedAtMs = 0;
uint32_t bleConnectedMsTotal = 0;

bool chargerKnown = false;
bool chargerIn = false;

bool lightPending = false;
uint32_t lightChangedMs = 0;
bool lightOn = false;
uint8_t lightBrightness = 0;
uint8_t lightWarmth = 0;

struct BatteryReading {
  uint16_t pct = UNKNOWN16;
  uint16_t mv = UNKNOWN16;
};
BatteryReading battery;
uint32_t batteryAtMs = 0;
bool batteryRead = false;

// The CW2017 reports whole-percent state of charge (register 0x04; BatteryMonitor
// exposes no finer figure), which is coarse against 0.1%/h of standby drain --
// hence the millivolts beside it, which do move inside a percent. A failed read
// keeps the last good value rather than reporting a zero, as
// HalPowerManager::getBatteryPercentage() does.
const BatteryReading& readBattery() {
  const uint32_t now = millis();
  if (batteryRead && now - batteryAtMs < BATTERY_TTL_MS) return battery;
  static const BatteryMonitor monitor;
  const BatteryMonitor::Status status = monitor.readStatus();
  if (status.percentageKnown) battery.pct = status.percentage;
  if (status.millivoltsKnown) battery.mv = status.millivolts;
  batteryAtMs = now;
  batteryRead = true;
  return battery;
}

// "2026-10-01T21:03:12Z", or "-" when the RTC holds no plausible wall clock.
void formatTime(char* out, const size_t size) {
  uint32_t epoch = 0;
  if (!halClock.getEpoch(epoch)) {
    snprintf(out, size, "-");
    return;
  }
  const time_t seconds = static_cast<time_t>(epoch);
  struct tm utc = {};
  gmtime_r(&seconds, &utc);
  snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
           utc.tm_min, utc.tm_sec);
}

void formatDuration(char* out, const size_t size, const uint32_t seconds) {
  const unsigned long s = seconds;
  if (s >= 3600UL) {
    snprintf(out, size, "%luh%02lum", s / 3600UL, (s % 3600UL) / 60UL);
  } else if (s >= 60UL) {
    snprintf(out, size, "%lum", s / 60UL);
  } else {
    snprintf(out, size, "%lus", s);
  }
}

void formatBytes(char* out, const size_t size, const uint32_t bytes) {
  const unsigned long b = bytes;
  if (b >= 1024UL * 1024UL) {
    // Divide first: bytes * 10 would overflow a uint32_t on a large book.
    const unsigned long tenths = b / ((1024UL * 1024UL) / 10UL);
    snprintf(out, size, "%lu.%lu MB", tenths / 10UL, tenths % 10UL);
  } else if (b >= 1024UL) {
    snprintf(out, size, "%lu KB", b / 1024UL);
  } else {
    snprintf(out, size, "%lu B", b);
  }
}

// Pointer into `path`, never a copy.
const char* baseName(const char* path) {
  if (path == nullptr) return "-";
  const char* slash = strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

void rotate() {
  if (Storage.exists(LOG_PATH_PREVIOUS)) Storage.remove(LOG_PATH_PREVIOUS);
  if (Storage.exists(LOG_PATH)) Storage.rename(LOG_PATH, LOG_PATH_PREVIOUS);
  fileBytes = 0;
  LOG_INF("PWRLOG", "rotated the power log");
}

// Appends the buffer to the card. On failure the buffer is kept, so a transient
// SD error does not lose the sleep line; appendLine() is what eventually drops.
void flushNow() {
  if (pendingLen == 0) return;
  lastFlushMs = millis();
  if (fileBytes + pendingLen > ROTATE_BYTES) rotate();
  const bool needsHeader = fileBytes == 0;
  // O_APPEND seeks to the end on every write, so two writers can never
  // interleave a partial line.
  HalFile out = Storage.open(LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!out) {
    LOG_ERR("PWRLOG", "could not open %s", LOG_PATH);
    return;
  }
  size_t written = 0;
  if (needsHeader) written += out.write(HEADER, sizeof(HEADER) - 1);
  written += out.write(pending, pendingLen);
  out.flush();
  const size_t expected = pendingLen + (needsHeader ? sizeof(HEADER) - 1 : 0);
  if (written != expected) {
    LOG_ERR("PWRLOG", "short write: %u of %u", static_cast<unsigned>(written), static_cast<unsigned>(expected));
    // Whatever landed is already on the card; re-flushing would duplicate it.
    fileBytes += written;
    pendingLen = 0;
    return;
  }
  fileBytes += written;
  pendingLen = 0;
}

void appendLine(const char* line, const size_t len) {
  if (len == 0 || len > BUFFER_BYTES) return;
  if (pendingLen + len > BUFFER_BYTES) flushNow();
  if (pendingLen + len > BUFFER_BYTES) {
    // The card refused the flush. Drop this line rather than grow without bound.
    if (droppedLines++ == 0) LOG_ERR("PWRLOG", "buffer full, dropping lines");
    return;
  }
  memcpy(pending + pendingLen, line, len);
  pendingLen += len;
}

// One CSV line. `fmt` builds the detail field, which is quoted only when it
// holds a comma -- minimal CSV quoting, so a detail without one is plain text.
void writeEvent(const char* event, const char* fmt, ...) {
  char detail[DETAIL_BYTES];
  detail[0] = '\0';
  if (fmt != nullptr) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
  }
  // A quote inside the detail would need doubling; a book or host name is the
  // only way one could arrive, and swapping it costs no second buffer.
  for (char* c = detail; *c != '\0'; c++) {
    if (*c == '"') *c = '\'';
  }
  char when[24];
  formatTime(when, sizeof(when));
  const BatteryReading& now = readBattery();
  char pct[8];
  char mv[8];
  if (now.pct == UNKNOWN16) {
    snprintf(pct, sizeof(pct), "-");
  } else {
    snprintf(pct, sizeof(pct), "%u", static_cast<unsigned>(now.pct));
  }
  if (now.mv == UNKNOWN16) {
    snprintf(mv, sizeof(mv), "-");
  } else {
    snprintf(mv, sizeof(mv), "%u", static_cast<unsigned>(now.mv));
  }
  const char* quote = strchr(detail, ',') != nullptr ? "\"" : "";
  char line[LINE_BYTES];
  const int len = snprintf(line, sizeof(line), "%s,%s,%s,%lu,%s,%s%s%s\n", when, pct, mv, millis() / 1000UL, event,
                           quote, detail, quote);
  if (len <= 0) return;
  appendLine(line, static_cast<size_t>(len) < sizeof(line) ? static_cast<size_t>(len) : sizeof(line) - 1);
}

// Where the awake stretch went. Every counter is one PowerStats/HalPowerManager
// already keeps for `about`, read rather than counted a second time.
void buildSessionSummary(char* out, const size_t size) {
  const uint32_t awakeMs = millis();
  const HalPowerManager::Counters cpu = powerManager.counters();
  const uint32_t cpuTotalMs = cpu.normalMs + cpu.lowMs;
  // 64-bit: normalMs * 100 overflows a uint32_t after twelve hours awake.
  const uint32_t fullPct =
      cpuTotalMs > 0 ? static_cast<uint32_t>((static_cast<uint64_t>(cpu.normalMs) * 100ULL) / cpuTotalMs) : 0;
  const HalFrontlight::Energy light = Frontlight.energySinceBoot();
  // Mean brightness across the whole awake stretch: 0 with the light off the
  // entire time, 100 with it at full, so it reads directly as the light's share.
  const uint32_t lightAvgPct = awakeMs > 0 ? static_cast<uint32_t>(light.percentMs / awakeMs) : 0;
  uint32_t bleMs = bleConnectedMsTotal;
  if (bleConnectedAtMs != 0) bleMs += millis() - bleConnectedAtMs;
  char awake[12];
  formatDuration(awake, sizeof(awake), awakeMs / 1000UL);
  char ble[12];
  formatDuration(ble, sizeof(ble), bleMs / 1000UL);
  snprintf(out, size, "awake %s, cpu %lu%%, panel %lus, light %lu%%, ble %s", awake,
           static_cast<unsigned long>(fullPct), static_cast<unsigned long>(cpu.panelWaitMs / 1000UL),
           static_cast<unsigned long>(lightAvgPct), ble);
}

void writePendingLight() {
  if (!lightPending) return;
  lightPending = false;
  if (!lightOn) {
    writeEvent("light", "off");
  } else {
    writeEvent("light", "%u%% warm %u%%", static_cast<unsigned>(lightBrightness), static_cast<unsigned>(lightWarmth));
  }
}

void pollLight(const uint32_t now) {
  const bool on = Frontlight.isOn();
  const uint8_t brightness = Frontlight.brightness();
  const uint8_t warmth = Frontlight.warmth();
  if (on != lightOn || brightness != lightBrightness || warmth != lightWarmth) {
    lightOn = on;
    lightBrightness = brightness;
    lightWarmth = warmth;
    lightChangedMs = now;
    lightPending = true;
    return;
  }
  if (lightPending && now - lightChangedMs >= LIGHT_SETTLE_MS) writePendingLight();
}

// How long the device was in deep sleep, from the wall clock across the sleep --
// millis() restarts at the wake, so nothing on the chip can answer this. 0 when
// the RTC could not answer at one end or the other; HalClock::begin() seeds a
// stopped clock from BUILD_EPOCH, so that is close to impossible in practice.
uint32_t sleptSeconds(const uint32_t epochAtSleep) {
  uint32_t now = 0;
  if (epochAtSleep == 0 || !halClock.getEpoch(now) || now <= epochAtSleep) return 0;
  return now - epochAtSleep;
}

void writeWakeLine(const char* wakeReason) {
  if (sleepSnapshot.magic != SNAPSHOT_MAGIC) {
    // Cold boot, a flash, or a reboot that did not come through sleep: there is
    // no previous sleep to charge anything to.
    writeEvent("boot", "%s", wakeReason);
    return;
  }
  const uint32_t slept = sleptSeconds(sleepSnapshot.epoch);
  const uint16_t wasPct = sleepSnapshot.pct;
  const uint16_t wasMv = sleepSnapshot.mv;
  // Read and clear together: a later ESP.restart() must not charge its boot to
  // this sleep a second time.
  sleepSnapshot.magic = 0;
  char slept_[12];
  formatDuration(slept_, sizeof(slept_), slept);
  const BatteryReading& now = readBattery();
  if (slept == 0) {
    // No usable wall clock either side of the sleep, so the duration -- and with
    // it the drain rate -- is simply not knowable. Said, not guessed.
    writeEvent("wake", "%s; slept ?", wakeReason);
    return;
  }
  if (wasPct == UNKNOWN16 || now.pct == UNKNOWN16) {
    writeEvent("wake", "%s; slept %s", wakeReason, slept_);
    return;
  }
  const int32_t dPct = static_cast<int32_t>(now.pct) - static_cast<int32_t>(wasPct);
  const uint32_t absPct = static_cast<uint32_t>(dPct < 0 ? -dPct : dPct);
  // Hundredths of a percent per hour, integer throughout: 100 * 3600 = 360000.
  const uint32_t pctPerHour = static_cast<uint32_t>((static_cast<uint64_t>(absPct) * 360000ULL) / slept);
  if (wasMv == UNKNOWN16 || now.mv == UNKNOWN16) {
    writeEvent("wake", "%s; slept %s, %ld%% (%lu.%02lu%%/h)", wakeReason, slept_, static_cast<long>(dPct),
               static_cast<unsigned long>(pctPerHour / 100UL), static_cast<unsigned long>(pctPerHour % 100UL));
    return;
  }
  const int32_t dMv = static_cast<int32_t>(now.mv) - static_cast<int32_t>(wasMv);
  const uint32_t absMv = static_cast<uint32_t>(dMv < 0 ? -dMv : dMv);
  const uint32_t mvPerHour = static_cast<uint32_t>((static_cast<uint64_t>(absMv) * 36000ULL) / slept);
  writeEvent("wake", "%s; slept %s, %ld%% %ldmV (%lu.%02lu%%/h %lu.%lumV/h)", wakeReason, slept_,
             static_cast<long>(dPct), static_cast<long>(dMv), static_cast<unsigned long>(pctPerHour / 100UL),
             static_cast<unsigned long>(pctPerHour % 100UL), static_cast<unsigned long>(mvPerHour / 10UL),
             static_cast<unsigned long>(mvPerHour % 10UL));
}

}  // namespace

void begin(const char* wakeReason) {
  if (started) return;
  started = true;
  pendingLen = 0;
  lastFlushMs = millis();
  lastAwakeSampleMs = millis();
  // The size now, so no flush has to stat the file to decide on rotation.
  HalFile existing;
  if (Storage.openFileForRead("PWRLOG", LOG_PATH, existing)) {
    fileBytes = existing.fileSize();
    existing.close();
  } else {
    fileBytes = 0;
  }
  // Seed the frontlight latch from what Frontlight.begin() restored, so the
  // restore itself is not reported as the user changing the light.
  lightOn = Frontlight.isOn();
  lightBrightness = Frontlight.brightness();
  lightWarmth = Frontlight.warmth();
  writeWakeLine(wakeReason != nullptr ? wakeReason : "-");
  // The wake line is the other half of a sleep line written hours ago; it is
  // never worth holding in RAM for a device that might not come back.
  flushNow();
}

void tick() {
  if (!started || sealed) return;
  const uint32_t now = millis();
  pollLight(now);
  if (now - lastAwakeSampleMs >= AWAKE_SAMPLE_MS) {
    lastAwakeSampleMs = now;
    char summary[DETAIL_BYTES];
    buildSessionSummary(summary, sizeof(summary));
    writeEvent("awake", "%s", summary);
  }
  if (pendingLen != 0 && now - lastFlushMs >= FLUSH_INTERVAL_MS) flushNow();
}

void noteBookOpen(const char* path) {
  if (!started || sealed) return;
  bookOpenedAtMs = millis();
  writeEvent("book", "open %.48s", baseName(path));
}

void noteBookClose(const char* path) {
  if (!started || sealed) return;
  const uint32_t heldS = bookOpenedAtMs != 0 ? (millis() - bookOpenedAtMs) / 1000UL : 0;
  bookOpenedAtMs = 0;
  char held[12];
  formatDuration(held, sizeof(held), heldS);
  writeEvent("book", "close %.40s after %s", baseName(path), held);
}

void noteChargerState(const bool connected) {
  if (!started || sealed) return;
  if (chargerKnown && connected == chargerIn) return;
  const bool wasKnown = chargerKnown;
  chargerKnown = true;
  chargerIn = connected;
  if (!wasKnown) return;  // the state at boot is not an edge
  writeEvent("charger", connected ? "in" : "out");
}

void noteBleConnected() {
  if (!started || sealed) return;
  bleConnectedAtMs = millis();
  writeEvent("ble", "connected");
}

void noteBleDisconnected(const char* peerName) {
  if (!started || sealed) return;
  uint32_t heldS = 0;
  if (bleConnectedAtMs != 0) {
    const uint32_t heldMs = millis() - bleConnectedAtMs;
    bleConnectedMsTotal += heldMs;
    heldS = heldMs / 1000UL;
    bleConnectedAtMs = 0;
  }
  char held[12];
  formatDuration(held, sizeof(held), heldS);
  writeEvent("ble", "disconnected %.24s after %s", peerName != nullptr && *peerName != '\0' ? peerName : "-", held);
}

void noteTransfer(const char* kind, const bool up, const uint32_t bytes, const uint32_t ms) {
  if (!started || sealed) return;
  char size[16];
  formatBytes(size, sizeof(size), bytes);
  writeEvent("transfer", "%s %.16s %s in %lu.%lus", up ? "in" : "out", kind != nullptr ? kind : "-", size,
             static_cast<unsigned long>(ms / 1000UL), static_cast<unsigned long>((ms % 1000UL) / 100UL));
}

void noteFirmwareInstall(const char* imagePath) {
  if (!started || sealed) return;
  writeEvent("firmware", "installed %.40s", baseName(imagePath));
  flushNow();  // a reboot follows
}

void noteSleep(const char* reason) {
  if (!started || sealed) return;
  // A frontlight change still inside its settle window belongs before the sleep
  // line, not lost with the session.
  writePendingLight();
  char summary[DETAIL_BYTES];
  buildSessionSummary(summary, sizeof(summary));
  writeEvent("sleep", "%s; %s", reason != nullptr ? reason : "-", summary);
  const BatteryReading& now = readBattery();
  uint32_t epoch = 0;
  sleepSnapshot.epoch = halClock.getEpoch(epoch) ? epoch : 0;
  sleepSnapshot.uptimeS = millis() / 1000UL;
  sleepSnapshot.pct = now.pct;
  sleepSnapshot.mv = now.mv;
  sleepSnapshot.magic = SNAPSHOT_MAGIC;
  flushNow();
  // From here the SD card is being taken down. Nothing may reopen it.
  sealed = true;
}

void flush() {
  if (!started || sealed) return;
  writePendingLight();
  flushNow();
}

}  // namespace power_log
