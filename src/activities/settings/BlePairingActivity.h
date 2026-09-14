#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_BLE_TRANSFER

#include <cstddef>
#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect
#include "network/BleLink.h"

// The device's Settings screen (the Action Centre's Settings tile): Bluetooth and
// Firmware, as two sections on one page.
//
// Everything else a reader can be configured with is read and written from the
// app over the link. What stays here is what the app cannot do for you: pair in
// the first place, and see and install the firmware update the phone sent.
//
// Pairing happens only while this screen is up. It opens BleLink's pairing
// window on entry when no phone is paired, or when the user taps Pair new phone,
// shows the passkey for the attempt in progress, and closes the window on exit.
class BlePairingActivity final : public Activity, public BleLink::Observer {
 public:
  BlePairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BlePairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // BleLink::Observer
  void onBleLinkChanged() override { requestUpdate(); }

 private:
  enum class Row { PAIR_NEW, FORGET, FIRMWARE };
  static constexpr size_t MAX_ROWS = 3;

  // Geometry of the action rows, written by render() and read by the touch
  // hit-test. Zero height means "not drawn".
  Rect pairNewRect_{0, 0, 0, 0};
  Rect forgetRect_{0, 0, 0, 0};
  Rect firmwareRect_{0, 0, 0, 0};
  // A verified update is waiting (FirmwareWatcher::StageState::READY). Refreshed
  // once a second rather than per loop: each look is two SD existence checks.
  bool firmwareReady_ = false;
  int lastStage_ = -1;
  unsigned long lastStageCheckMs_ = 0;
  // The row Confirm activates; moved with Up/Down.
  Row selected_ = Row::PAIR_NEW;
  // Lockout countdown as last painted, in 5 s steps.
  uint32_t lastLockStep_ = 0;

  size_t visibleRows(Row rows[MAX_ROWS]) const;
  Row effectiveSelection() const;
  void moveSelection(int delta);
  void activate(Row row);
  void refreshFirmwareStage();
  void promptForget();
  void openFirmware();
};

#endif  // FREEINK_CAP_BLE_TRANSFER
