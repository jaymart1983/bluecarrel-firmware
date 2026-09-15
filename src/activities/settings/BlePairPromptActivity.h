#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_BLE_TRANSFER

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

// "Pair with <name>?" -- asked when a phone sends `pair` while the pairing window
// is closed. Any app on a bonded phone can send `pair`, so only a person holding
// the reader can let it replace the trusted host.
//
// Pushed by main.cpp over whatever is on screen, like FirmwareReadyActivity, so a
// book underneath stays open. BleLink holds the request and its 60 s deadline;
// this screen only answers it. It does not dismiss on an outside tap or Back, and
// leaving it any other way (Home, sleep) counts as Deny.
class BlePairPromptActivity final : public Activity {
 public:
  BlePairPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BlePairPrompt", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void choose(int index);
  void showPopup();

  OptionPopup popup;
  std::string heading;
  std::string body;
  bool decided = false;
  static constexpr int margin = 20;
  static constexpr int spacing = 30;
  static constexpr int fontId = UI_10_FONT_ID;
};

#endif  // FREEINK_CAP_BLE_TRANSFER
