#include "BlePairingActivity.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
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
// The lockout countdown repaints in steps of this many seconds, not every second.
constexpr uint32_t LOCK_STEP_SECONDS = 5;

bool hitRect(const Rect& r, const int x, const int y) {
  return r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

uint32_t lockStep(const uint32_t secondsLeft) { return (secondsLeft + LOCK_STEP_SECONDS - 1) / LOCK_STEP_SECONDS; }

// The link's auth_error, in words the owner can act on.
const char* authErrorText(const std::string& error) {
  if (error == "unknown trusted host" || error == "invalid trusted host auth") return tr(STR_BLE_PAIRING_OUT_OF_DATE);
  if (error == "pairing window closed") return tr(STR_BLE_PAIRING_TAP_PAIR_NEW);
  return tr(STR_BLE_PAIRING_FAILED);
}

}  // namespace

void BlePairingActivity::onEnter() {
  Activity::onEnter();
  // The radio is already up. If it is not, something failed at boot, so ask for a
  // start here too -- begin() is idempotent.
  BLE_LINK.begin();
  BLE_LINK.setObserver(this);
  // A refusal from before this screen opened is not news.
  BLE_LINK.clearAuthError();
  if (!BLE_LINK.hasTrustedHost()) BLE_LINK.openPairingWindow();
  refreshFirmwareStage();
  selected_ = firmwareReady_ ? Row::FIRMWARE : Row::PAIR_NEW;
  requestUpdate();
}

void BlePairingActivity::onExit() {
  BLE_LINK.closePairingWindow();
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

size_t BlePairingActivity::visibleRows(Row rows[MAX_ROWS]) const {
  size_t count = 0;
  if (BLE_LINK.hasTrustedHost()) {
    if (!BLE_LINK.pairingWindowOpen() && BLE_LINK.pairingLockSecondsLeft() == 0) rows[count++] = Row::PAIR_NEW;
    rows[count++] = Row::FORGET;
  }
  if (firmwareReady_) rows[count++] = Row::FIRMWARE;
  return count;
}

BlePairingActivity::Row BlePairingActivity::effectiveSelection() const {
  Row rows[MAX_ROWS];
  const size_t count = visibleRows(rows);
  if (count == 0) return selected_;
  for (size_t i = 0; i < count; i++) {
    if (rows[i] == selected_) return selected_;
  }
  return rows[0];
}

void BlePairingActivity::moveSelection(const int delta) {
  Row rows[MAX_ROWS];
  const size_t count = visibleRows(rows);
  if (count == 0) return;
  const Row current = effectiveSelection();
  size_t index = 0;
  for (size_t i = 0; i < count; i++) {
    if (rows[i] == current) index = i;
  }
  const int total = static_cast<int>(count);
  selected_ = rows[static_cast<size_t>((static_cast<int>(index) + delta % total + total) % total)];
  requestUpdate();
}

void BlePairingActivity::activate(const Row row) {
  switch (row) {
    case Row::PAIR_NEW:
      selected_ = Row::FORGET;
      BLE_LINK.openPairingWindow();
      requestUpdate();
      return;
    case Row::FORGET:
      promptForget();
      return;
    case Row::FIRMWARE:
      openFirmware();
      return;
  }
}

void BlePairingActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // The staged image changes on its own -- a hash finishing, an upload landing --
  // and nothing tells this screen, so look once a second and repaint on a change.
  // The lockout countdown rides the same poll.
  if (millis() - lastStageCheckMs_ >= STAGE_POLL_MS) {
    refreshFirmwareStage();
    const uint32_t step = lockStep(BLE_LINK.pairingLockSecondsLeft());
    if (step != lastLockStep_) {
      lastLockStep_ = step;
      requestUpdate();
    }
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    Row rows[MAX_ROWS];
    const size_t count = visibleRows(rows);
    for (size_t i = 0; i < count; i++) {
      const Rect& rect = rows[i] == Row::PAIR_NEW ? pairNewRect_ : rows[i] == Row::FORGET ? forgetRect_ : firmwareRect_;
      if (hitRect(rect, x, y)) {
        activate(rows[i]);
        return;
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) moveSelection(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) moveSelection(1);

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    Row rows[MAX_ROWS];
    if (visibleRows(rows) > 0) activate(effectiveSelection());
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
        // Nobody is paired and this screen is up, so pairing is open again.
        if (!BLE_LINK.hasTrustedHost()) BLE_LINK.openPairingWindow();
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
  const bool windowOpen = BLE_LINK.pairingWindowOpen();
  const uint32_t lockSeconds = BLE_LINK.pairingLockSecondsLeft();
  uint32_t passkey = 0;
  const bool pairingInProgress = BLE_LINK.pairingPasskey(passkey);
  const Row selection = effectiveSelection();

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
  pairNewRect_ = Rect{0, 0, 0, 0};
  forgetRect_ = Rect{0, 0, 0, 0};
  if (paired) {
    line(UI_10_FONT_ID, std::string(tr(STR_BLE_PAIRED_WITH)) + " " + hostLabel, EpdFontFamily::BOLD);
    line(SMALL_FONT_ID, BLE_LINK.isAuthenticated() ? tr(STR_CONNECTED) : tr(STR_BLE_NOT_CONNECTED),
         EpdFontFamily::REGULAR);
  } else {
    line(UI_10_FONT_ID, tr(STR_BLE_PAIR_PHONE), EpdFontFamily::BOLD);
  }

  if (lockSeconds > 0) {
    char locked[64];
    snprintf(locked, sizeof(locked), tr(STR_BLE_PAIRING_LOCKED_FORMAT),
             static_cast<unsigned>(lockStep(lockSeconds) * LOCK_STEP_SECONDS));
    y += metrics.verticalSpacing;
    line(UI_10_FONT_ID, locked, EpdFontFamily::BOLD);
  } else if (pairingInProgress) {
    y += metrics.verticalSpacing;
    line(SMALL_FONT_ID, tr(STR_BLE_PASSKEY_HINT), EpdFontFamily::REGULAR);
    char digits[8];
    snprintf(digits, sizeof(digits), "%03u %03u", static_cast<unsigned>(passkey / 1000),
             static_cast<unsigned>(passkey % 1000));
#ifndef OMIT_FONTS
    line(NOTOSANS_18_FONT_ID, digits, EpdFontFamily::BOLD);
#else
    line(UI_12_FONT_ID, digits, EpdFontFamily::BOLD);
#endif
  } else if (windowOpen) {
    line(SMALL_FONT_ID, tr(STR_BLE_PAIR_HINT), EpdFontFamily::REGULAR);
  }
  if (!authError.empty()) {
    line(SMALL_FONT_ID, authErrorText(authError), EpdFontFamily::REGULAR);
  }
  if (paired && !windowOpen && lockSeconds == 0) {
    pairNewRect_ = actionRow(tr(STR_BLE_PAIR_NEW_PHONE), selection == Row::PAIR_NEW);
  }
  if (paired) forgetRect_ = actionRow(tr(STR_FORGET_BUTTON), selection == Row::FORGET);
  y += metrics.verticalSpacing * 3;

  // --- Firmware --------------------------------------------------------------
  sectionTitle(tr(STR_FIRMWARE_SECTION));
  line(UI_10_FONT_ID, std::string(tr(STR_FIRMWARE_VERSION)) + " " + X4_BUILD_STAMP, EpdFontFamily::REGULAR);
  firmwareRect_ = Rect{0, 0, 0, 0};
  const std::string status = firmwareStageStatusText();
  if (firmwareReady_) {
    // A row, because it does something: Update Now / Later / Cancel.
    firmwareRect_ = actionRow(status, selection == Row::FIRMWARE);
  } else {
    line(SMALL_FONT_ID, status, EpdFontFamily::REGULAR);
  }

  GUI.drawButtonHints(renderer, "", "", "", "", /*touchBack=*/false);
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_BLE_TRANSFER
