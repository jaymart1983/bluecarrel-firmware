#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_BLE_TRANSFER

#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect
#include "network/BleLink.h"

// The device's Settings screen (the Action Centre's Settings tile): Bluetooth and
// Firmware, as two sections on one page.
//
// Everything else a reader can be configured with is read and written from the
// app over the link. What stays here is what the app cannot do for you: pair in
// the first place (the code is on screen whenever no phone is paired, and Forget
// is under the pairing), and see and install the firmware update the phone sent
// -- which is also the way back to an update prompt that was dismissed.
//
// The page owns no radio. BleLink has been advertising since the device woke, so
// this screen only reads it and repaints when it says something changed.
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
  // Geometry of the two tiles, written by render() and read by the touch
  // hit-test. Zero height means "not drawn".
  Rect forgetRect_{0, 0, 0, 0};
  Rect firmwareRect_{0, 0, 0, 0};
  // A verified update is waiting (FirmwareWatcher::StageState::READY). Refreshed
  // once a second rather than per loop: each look is two SD existence checks.
  bool firmwareReady_ = false;
  int lastStage_ = -1;
  unsigned long lastStageCheckMs_ = 0;

  void refreshFirmwareStage();
  void promptForget();
  void openFirmware();
};

#endif  // FREEINK_CAP_BLE_TRANSFER
