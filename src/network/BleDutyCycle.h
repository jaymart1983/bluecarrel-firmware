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

  // While reading, the reader talks to the phone this often, and again as it is
  // put down. Five minutes rather than the fifteen this started at: fifteen is
  // how long the user waits before the phone can do ANYTHING they ask of it, and
  // a quarter of an hour of a dead app is not worth what it saves. An empty
  // window is about 25 s of advertising, so five minutes is an 8% duty on a
  // radio that now sleeps between events -- well under a milliamp against the
  // 45-75 mA of reading.
  static constexpr uint32_t INTERVAL_MS = 5UL * 60UL * 1000UL;
  // How long a window stays open waiting for the phone. A wake-to-connected
  // handshake measures 3-20 s in the field, so this is that with headroom; an
  // empty window costs about two seconds of radio per quarter hour.
  static constexpr uint32_t WINDOW_MS = 25UL * 1000UL;
  // The window after a wake is longer. Waking is a chip reset, so the phone has
  // to be told by the presence scan, connect, authenticate and then sync, and a
  // wake-to-connected handshake has been measured at up to 20 s. This is the one
  // window where falling short means the reader carries a stale position and no
  // clock for the next quarter hour.
  static constexpr uint32_t WAKE_WINDOW_MS = 45UL * 1000UL;
  // After the phone has authenticated, the window closes once the link has been
  // idle this long -- the phone has said what it came to say.
  static constexpr uint32_t SETTLE_MS = 2000UL;
  // How long a connected phone may say nothing before its session is treated as
  // finished. Generous on purpose: this is the window a user has to open the
  // app, pick a book or a firmware image and start it, and the cost of being
  // wrong is the reader going dark under their hands.
  static constexpr uint32_t SESSION_IDLE_MS = 30UL * 1000UL;

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
  void openWindow(const char* why, uint32_t windowMs = WINDOW_MS);
  void closeWindow();
  void park();

  bool started_ = false;
  uint32_t lastSyncMs_ = 0;
  uint32_t windowStartedMs_ = 0;
  uint32_t idleSinceMs_ = 0;
  bool authSeenInWindow_ = false;
  bool parking_ = false;
  // A wake is a chip reset, and the reader wakes into the book it was put down
  // in. Until this reader has talked to the phone once, a book open is not a
  // reason to park. See update().
  bool syncedSinceWake_ = false;
  // Points at a string literal from the caller, so it needs no storage.
  const char* windowWhy_ = "interval";
  uint32_t windowMs_ = WINDOW_MS;
};

extern BleDutyCycle bleDutyCycle;
