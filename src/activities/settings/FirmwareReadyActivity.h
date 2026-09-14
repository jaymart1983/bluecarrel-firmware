#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

// Offered the moment a verified firmware update is waiting, over whatever is on
// screen -- a book included. Three answers:
//   Update now  install and reboot (SdFirmwareUpdateActivity, no second prompt)
//   Later       install the next time the reader goes to sleep, then sleep
//   Cancel      leave the image on the card; offered again after a restart
// Pushed rather than replaced, so Later and Cancel land back on the page the
// reader was on.
class FirmwareReadyActivity : public Activity {
 public:
  FirmwareReadyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FirmwareReady", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void choose(int index);
  void showPopup();

  OptionPopup popup;
  std::string heading;
  std::string body;
  std::string versionLine;
  bool decided = false;
  int startY = 0;
  int lineHeight = 0;
  static constexpr int margin = 20;
  static constexpr int spacing = 30;
  static constexpr int fontId = UI_10_FONT_ID;
};
