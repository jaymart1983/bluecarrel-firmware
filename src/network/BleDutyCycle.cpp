#include "BleDutyCycle.h"

#include <Arduino.h>
#include <Logging.h>

#include "BleLink.h"
#include "PowerLog.h"

BleDutyCycle bleDutyCycle;

namespace {
// windowStartedMs_ == 0 means "no window", so a window that opens on tick zero
// has to claim the next millisecond instead.
uint32_t nonZero(const uint32_t ms) { return ms == 0 ? 1 : ms; }
}  // namespace

bool BleDutyCycle::parked() const { return BLE_LINK.radioParked(); }

uint32_t BleDutyCycle::msUntilWindow() const {
  if (!parking_ || windowStartedMs_ != 0) return 0;
  const uint32_t elapsed = millis() - lastSyncMs_;
  return elapsed >= INTERVAL_MS ? 0 : INTERVAL_MS - elapsed;
}

void BleDutyCycle::openWindow(const char* why) {
  windowStartedMs_ = nonZero(millis());
  idleSinceMs_ = 0;
  authSeenInWindow_ = false;
  windowWhy_ = why;
  BLE_LINK.unparkRadio();
  LOG_INF("BLEDUTY", "sync window open (%s)", why);
}

// Tell the phone before the radio goes dark, so a park is not mistaken for the
// reader going out of range. Only worth saying while someone is listening.
void BleDutyCycle::park() {
  if (BLE_LINK.radioParked()) return;
  if (BLE_LINK.isPeerConnected()) BLE_LINK.notifyParking(INTERVAL_MS);
  BLE_LINK.parkRadio();
}

void BleDutyCycle::closeWindow() {
  if (windowStartedMs_ == 0) return;
  const uint32_t heldMs = millis() - windowStartedMs_;
  LOG_INF("BLEDUTY", "sync window closed after %lums, phone %s", static_cast<unsigned long>(heldMs),
          authSeenInWindow_ ? "synced" : "absent");
  power_log::noteSyncWindow(windowWhy_, authSeenInWindow_, heldMs);
  windowStartedMs_ = 0;
  idleSinceMs_ = 0;
  authSeenInWindow_ = false;
}

void BleDutyCycle::update(const Context& ctx) {
  if (!BLE_LINK.isRunning()) {
    // USB Drive or a firmware install owns the radio. Come back to a full
    // interval rather than opening a window the moment the link returns.
    closeWindow();
    parking_ = false;
    started_ = false;
    return;
  }

  const uint32_t now = millis();
  if (!started_) {
    started_ = true;
    lastSyncMs_ = now;
  }

  // The link simply stays up: no book, or a reason to be reachable regardless.
  // The interval is held at full so closing a book and opening another does not
  // inherit a window that was already due.
  if (!ctx.bookOpen || ctx.charging || ctx.holdLinkUp) {
    closeWindow();
    parking_ = false;
    lastSyncMs_ = now;
    BLE_LINK.unparkRadio();
    return;
  }

  parking_ = true;

  // Never paired: there is no phone to open a window for, so stay dark.
  if (!BLE_LINK.hasTrustedHost()) {
    closeWindow();
    park();
    return;
  }

  // A transfer in flight outlasts its window. Parking mid-book would drop it at
  // whatever byte it had reached.
  if (BLE_LINK.isBusy()) {
    authSeenInWindow_ = true;
    idleSinceMs_ = 0;
    if (windowStartedMs_ == 0) windowStartedMs_ = nonZero(now);
    BLE_LINK.unparkRadio();
    return;
  }

  if (windowStartedMs_ != 0) {
    if (BLE_LINK.isAuthenticated()) {
      authSeenInWindow_ = true;
      idleSinceMs_ = 0;
    } else if (authSeenInWindow_ && idleSinceMs_ == 0) {
      idleSinceMs_ = now;
    }
    const bool settled = authSeenInWindow_ && idleSinceMs_ != 0 && now - idleSinceMs_ >= SETTLE_MS;
    if (settled || now - windowStartedMs_ >= WINDOW_MS) {
      // Either way the next window is a full interval away: an empty window is
      // not retried sooner, or a reader out of range would never park.
      lastSyncMs_ = now;
      closeWindow();
      park();
    }
    return;
  }

  if (now - lastSyncMs_ >= INTERVAL_MS) {
    openWindow("interval");
    return;
  }
  park();
}

bool BleDutyCycle::runWindowBlocking(const char* why) {
  if (!BLE_LINK.isRunning() || !BLE_LINK.hasTrustedHost()) return false;
  openWindow(why);
  const uint32_t startMs = millis();
  uint32_t idleSinceMs = 0;
  bool authSeen = false;
  while (millis() - startMs < WINDOW_MS) {
    // The link is pumped from the main loop normally; nothing else is running
    // here, so this loop has to pump it itself.
    BLE_LINK.tick();
    if (BLE_LINK.isAuthenticated() || BLE_LINK.isBusy()) {
      authSeen = true;
      idleSinceMs = 0;
    } else if (authSeen && idleSinceMs == 0) {
      idleSinceMs = millis();
    }
    if (authSeen && idleSinceMs != 0 && millis() - idleSinceMs >= SETTLE_MS) break;
    // delay() yields to the idle task, so the watchdog is fed for the whole wait.
    BLE_LINK.waitForWork(20);
  }
  lastSyncMs_ = millis();
  authSeenInWindow_ = authSeen;
  closeWindow();
  return authSeen;
}
