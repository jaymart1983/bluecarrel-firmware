#include "BlePairingActivity.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "FirmwareReadyActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BuildStamp.h"
#include "network/FirmwareWatcher.h"

namespace {

constexpr unsigned long STAGE_POLL_MS = 1000;

bool hitRect(const Rect& r, const int x, const int y) {
  return r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace

void BlePairingActivity::onEnter() {
  Activity::onEnter();
  // The radio is already up. If it is not, something failed at boot, so ask for a
  // start here too -- begin() is idempotent.
  BLE_LINK.begin();
  BLE_LINK.setObserver(this);
  refreshFirmwareStage();
  requestUpdate();
}

void BlePairingActivity::onExit() {
  BLE_LINK.clearObserver(this);
  Activity::onExit();
}

void BlePairingActivity::refreshFirmwareStage() {
  lastStageCheckMs_ = millis();
  const auto stage = FIRMWARE_WATCHER.stageState();
  firmwareReady_ = stage == FirmwareWatcher::StageState::READY;
  const int key = static_cast<int>(stage) * 2 + (FIRMWARE_WATCHER.installAtSleep() ? 1 : 0);
  if (key != lastStage_) {
    lastStage_ = key;
    requestUpdate();
  }
}

void BlePairingActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // The staged image changes on its own -- a hash finishing, an upload landing --
  // and nothing tells this screen, so look once a second and repaint on a change.
  if (millis() - lastStageCheckMs_ >= STAGE_POLL_MS) refreshFirmwareStage();

  const bool canForget = BLE_LINK.hasTrustedHost();

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (firmwareReady_ && hitRect(firmwareRect_, x, y)) {
      openFirmware();
      return;
    }
    if (canForget && hitRect(forgetRect_, x, y)) {
      promptForget();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // The highlighted row: the waiting update when there is one, else Forget.
    if (firmwareReady_) {
      openFirmware();
    } else if (canForget) {
      promptForget();
    }
  }
}

void BlePairingActivity::promptForget() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BLE_FORGET_HOST),
                                             BLE_LINK.trustedHostLabel()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        {
          RenderLock lock(*this);
          if (!BLE_LINK.forgetTrustedHost()) LOG_ERR("BLE", "could not forget the trusted host");
        }
        requestUpdate();
      });
}

void BlePairingActivity::openFirmware() {
  startActivityForResult(std::make_unique<FirmwareReadyActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) {
                           lastStage_ = -1;
                           refreshFirmwareStage();
                           requestUpdate();
                         });
}

void BlePairingActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int left = metrics.contentSidePadding;
  const int contentWidth = pageWidth - left * 2;
  const int bodyLine = renderer.getLineHeight(UI_10_FONT_ID);

  const bool paired = BLE_LINK.hasTrustedHost();
  const std::string hostLabel = BLE_LINK.trustedHostLabel();
  const std::string& authError = BLE_LINK.authError();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Everything left-aligned at the content margin, one line per call.
  const auto line = [&](const int font, const std::string& value, const EpdFontFamily::Style style) {
    const std::string fitted = renderer.truncatedText(font, value.c_str(), contentWidth, style);
    renderer.drawText(font, left, y, fitted.c_str(), true, style);
    y += renderer.getLineHeight(font);
  };
  const auto sectionTitle = [&](const char* title) {
    line(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
    y += 2;
    renderer.drawLine(left, y, pageWidth - left, y, true);
    y += metrics.verticalSpacing;
  };
  // A row that does something: outlined, label left-aligned, heavier when it is
  // the one Confirm would activate.
  const auto actionRow = [&](const std::string& label, const bool highlighted) -> Rect {
    const int top = y + metrics.verticalSpacing;
    const int height = metrics.menuRowHeight;
    renderer.drawRoundedRect(left, top, contentWidth, height, highlighted ? 2 : 1, 6, true);
    const std::string fitted =
        renderer.truncatedText(UI_10_FONT_ID, label.c_str(), contentWidth - 24, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, left + 12, top + (height - bodyLine) / 2, fitted.c_str(), true,
                      EpdFontFamily::REGULAR);
    y = top + height + metrics.verticalSpacing;
    return Rect{left, top, contentWidth, height};
  };

  // --- Bluetooth -------------------------------------------------------------
  sectionTitle(tr(STR_BLUETOOTH));
  forgetRect_ = Rect{0, 0, 0, 0};
  if (paired) {
    line(UI_10_FONT_ID, std::string(tr(STR_BLE_PAIRED_WITH)) + " " + hostLabel, EpdFontFamily::BOLD);
    line(SMALL_FONT_ID, BLE_LINK.isAuthenticated() ? tr(STR_CONNECTED) : tr(STR_BLE_NOT_CONNECTED),
         EpdFontFamily::REGULAR);
  } else {
    line(UI_10_FONT_ID, tr(STR_BLE_PAIR_PHONE), EpdFontFamily::BOLD);
    line(SMALL_FONT_ID, tr(STR_BLE_PAIR_HINT), EpdFontFamily::REGULAR);
    y += metrics.verticalSpacing;
    // The code only while no phone is paired: a paired phone reconnects over its
    // saved credential and never sends it. Forget brings it back.
    line(UI_12_FONT_ID, std::string(tr(STR_BLE_TRANSFER_CODE)) + BLE_LINK.sessionCode(), EpdFontFamily::BOLD);
  }
  if (!authError.empty()) {
    line(SMALL_FONT_ID, std::string(tr(STR_ERROR_MSG)) + ": " + authError, EpdFontFamily::REGULAR);
  }
  if (paired) forgetRect_ = actionRow(tr(STR_FORGET_BUTTON), !firmwareReady_);
  y += metrics.verticalSpacing * 3;

  // --- Firmware --------------------------------------------------------------
  sectionTitle(tr(STR_FIRMWARE_SECTION));
  line(UI_10_FONT_ID, std::string(tr(STR_FIRMWARE_VERSION)) + " " + X4_BUILD_STAMP, EpdFontFamily::REGULAR);
  firmwareRect_ = Rect{0, 0, 0, 0};
  const std::string status = firmwareStageStatusText();
  if (firmwareReady_) {
    // A row, because it does something: Update Now / Later / Cancel.
    firmwareRect_ = actionRow(status, true);
  } else {
    line(SMALL_FONT_ID, status, EpdFontFamily::REGULAR);
  }

  GUI.drawButtonHints(renderer, "", "", "", "", /*touchBack=*/false);
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_BLE_TRANSFER
