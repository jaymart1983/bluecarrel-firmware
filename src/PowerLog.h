#pragma once

#include <cstdint>

// Append-only CSV of what the device was doing and what the gauge read while it
// did it, so "where did the charge go" is answerable from the file alone.
//
// `/power_log.csv`, header `time,pct,mv,uptime_s,event,detail`, rotated once to
// `/power_log.1.csv` at ~64 KB. The detail is the last field and is CSV-quoted
// only when it contains a comma, so a line without one reads as plain text.
//
// The sleep/wake pair is the point of the file: a sleep line snapshots the
// gauge into RTC_NOINIT memory, and the wake line after it -- a fresh boot --
// reports how long the device slept and what that cost. Standby is ~95% of the
// clock on a two-week charge, so it is the only measurement that settles where
// the charge goes.
//
// Lines are built into a fixed 1 KB static buffer and flushed on sleep, on a
// firmware install, on rotation, when the buffer fills and at most every 60 s:
// a sparse event log must not cost an SD write per event. No heap, no
// std::string, nothing per page turn.
//
// Main-loop task only. Every event hook runs there (BleLink pumps its event
// queue from BleLink::tick()), which is also what keeps the unguarded gauge
// reads consistent with HalPowerManager::getBatteryPercentage().
namespace power_log {

// Call after Storage.begin() and halClock.begin(), and after the boot paths
// that go straight back to sleep have had their chance -- a ghost wake must not
// consume the sleep snapshot. Writes and flushes the wake (or boot) line.
void begin(const char* wakeReason);

// Hourly "still awake" sample, the coalesced frontlight line and the 60-second
// flush. Once per main-loop pass; costs three inline getters when idle.
void tick();

void noteBookOpen(const char* path);
void noteBookClose(const char* path);

// Edge-detected inside, so the caller can hand over the current state every
// pass. The state at boot is not an edge.
void noteChargerState(bool connected);

void noteBleConnected();
// `peerName` may be null or empty when the link never authenticated.
void noteBleDisconnected(const char* peerName);

// A completed transfer. `kind` is the protocol's transfer-kind name; `up` is
// true for app -> reader.
void noteTransfer(const char* kind, bool up, uint32_t bytes, uint32_t ms);

// Flushed before it returns: a reboot follows.
void noteFirmwareInstall(const char* imagePath);

// The sleep line, the awake-session summary and the gauge snapshot the next
// wake measures against, flushed to the card before it returns.
//
// MUST be called before BleLink::prepareForDeepSleep() and display.deepSleep().
// After it returns the module refuses every further write, so nothing touches
// the SD card once Storage.prepareForDeepSleep() has stopped it.
void noteSleep(const char* reason);

// Flush whatever is buffered, so a reader of the file over BLE or USB Drive
// sees the lines this session has already produced.
void flush();

}  // namespace power_log
