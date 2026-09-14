#include "FirmwareReadyActivity.h"

#include <I18n.h>
#include <Logging.h>

#include "HalDisplay.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "network/FirmwareStaging.h"
#include "network/FirmwareWatcher.h"

namespace {
constexpr int CHOICE_UPDATE_NOW = 0;
constexpr int CHOICE_LATER = 1;
}  // namespace

void FirmwareReadyActivity::onEnter() {
  Activity::onEnter();
  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);
  heading = renderer.truncatedText(fontId, I18N.get(StrId::STR_FIRMWARE_DROP_FOUND), maxWidth, EpdFontFamily::BOLD);
  body = renderer.truncatedText(fontId, I18N.get(StrId::STR_FIRMWARE_READY_HINT), maxWidth, EpdFontFamily::REGULAR);
  startY = renderer.getScreenHeight() / 6;
  std::string version;
  if (firmware_staging::readVersion(version)) {
    versionLine = std::string(I18N.get(StrId::STR_FIRMWARE_VERSION)) + " " + version;
  }

  showPopup();
}

void FirmwareReadyActivity::showPopup() {
  const char* options[] = {I18N.get(StrId::STR_FIRMWARE_UPDATE_NOW), I18N.get(StrId::STR_FIRMWARE_LATER),
                           I18N.get(StrId::STR_CANCEL)};
  // Cancel is an option; the X4 Pro has no Back button to mirror.
  popup.setTouchBack(false);
  popup.show(heading.c_str(), options, 3, CHOICE_UPDATE_NOW, [this](int idx) { choose(idx); });
  requestUpdate(true);
}

void FirmwareReadyActivity::choose(const int index) {
  if (decided) return;
  decided = true;
  if (index == CHOICE_UPDATE_NOW) {
    LOG_INF("FWDROP", "update now");
    // Replace, not push: the install reboots, and replacing exits the book
    // underneath first, so its position is saved before the flash starts.
    activityManager.goToFirmwareUpdate(firmware_staging::IMAGE_PATH, /*stagedDrop=*/true, /*autoConfirm=*/true);
    return;
  }
  if (index == CHOICE_LATER) {
    LOG_INF("FWDROP", "update deferred to the next sleep");
    FIRMWARE_WATCHER.deferToSleep();
  } else {
    LOG_INF("FWDROP", "update cancelled; offered again after a restart");
  }
  finish();
}

void FirmwareReadyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  int y = startY;
  renderer.drawCenteredText(fontId, y, heading.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight + spacing;
  if (!versionLine.empty()) {
    renderer.drawCenteredText(fontId, y, versionLine.c_str(), true, EpdFontFamily::REGULAR);
    y += lineHeight + spacing / 2;
  }
  renderer.drawCenteredText(fontId, y, body.c_str(), true, EpdFontFamily::REGULAR);
  if (popup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void FirmwareReadyActivity::loop() {
  if (decided) return;
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  // The popup closed without one of its three answers -- a tap outside the
  // dialog, or Back. This question does not dismiss: it stays until Update Now,
  // Later or Cancel is chosen. Going Home through the Action Centre replaces this
  // screen instead, which is the other way out.
  showPopup();
}
