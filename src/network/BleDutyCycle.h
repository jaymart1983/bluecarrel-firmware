#pragma once

#include <cstdint>

// Keeps the Bluetooth radio parked while a book is open, opening a short sync
// window every INTERVAL_MS and once more when the reader is put down.
//
// Why a book is the condition: reading is the one thing the device does for
// hours at a stretch with nobody touching it, and an idle advertiser costs
// current for the whole of it. Outside a book -- the home screen, the library,
// settings -- the link simply stays up, so browsing the library from the phone
// or pushing a book never waits for a window.
//
// Nothing here starts or stops the NimBLE stack; it parks and unparks the radio
// (BleLink::parkRadio), which is reversible and cheap. Costs no heap and no
// task: one update() call per main loop pass.
class BleDutyCycle {
 public:
  struct Context {
    bool bookOpen = false;
    // Charging: the power argument for parking is gone, so stay reachable.
    bool charging = false;
    // Pairing or a pair prompt on screen -- the phone has to be able to reach
    // the reader right now, whatever the window says.
    bool holdLinkUp = false;
  };

  // The user-facing promise: while reading, the reader talks to the phone every
  // quarter of an hour, and again as it goes to sleep.
  static constexpr uint32_t INTERVAL_MS = 15UL * 60UL * 1000UL;
  // How long a window stays open waiting for the phone. A wake-to-connected
  // handshake measures 3-20 s in the field, so this is that with headroom; an
  // empty window costs about two seconds of radio per quarter hour.
  static constexpr uint32_t WINDOW_MS = 25UL * 1000UL;
  // After the phone has authenticated, the window closes once the link has been
  // idle this long -- the phone has said what it came to say.
  static constexpr uint32_t SETTLE_MS = 2000UL;

  // Called once per main loop pass. Parks and unparks the radio as the policy
  // requires; does nothing at all when the link is not running.
  void update(const Context& ctx);

  // Run one window to completion on the calling task, for the sleep path: the
  // sleep screen is already up and no activity is running, so blocking here is
  // what "appears asleep, syncs, then sleeps" means. Returns true if the phone
  // authenticated within the window.
  bool runWindowBlocking(const char* why);

  bool parked() const;
  bool windowOpen() const { return windowStartedMs_ != 0; }
  // Milliseconds until the next window is due, or 0 when one is open or the
  // policy is not parking at all. Light sleep uses this as its timer wake.
  uint32_t msUntilWindow() const;

 private:
  void openWindow(const char* why);
  void closeWindow();

  bool started_ = false;
  uint32_t lastSyncMs_ = 0;
  uint32_t windowStartedMs_ = 0;
  uint32_t idleSinceMs_ = 0;
  bool authSeenInWindow_ = false;
  bool parking_ = false;
  // Points at a string literal from the caller, so it needs no storage.
  const char* windowWhy_ = "interval";
};

extern BleDutyCycle bleDutyCycle;
