#include "BlePairingActivity.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "FirmwareReadyActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
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

// The link's auth_error, in words the owner can act on. Null when there is
// nothing to say: "confirm on reader" is the pair prompt itself, on screen.
const char* authErrorText(const std::string& error) {
  if (error == "unknown trusted host" || error == "invalid trusted host auth") return tr(STR_BLE_PAIRING_OUT_OF_DATE);
  if (error == "pairing window closed") return tr(STR_BLE_PAIRING_TAP_PAIR_NEW);
  if (error == "pairing denied") return tr(STR_BLE_PAIRING_DENIED);
  if (error == "confirm on reader") return nullptr;
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
  nameRejected_ = false;
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
  rows[count++] = Row::NAME;
  if (BLE_LINK.hasTrustedHost()) {
    if (!BLE_LINK.pairingWindowOpen() && BLE_LINK.pairingLockSecondsLeft() == 0) rows[count++] = Row::PAIR_NEW;
    rows[count++] = Row::FORGET;
  }
  if (firmwareReady_) rows[count++] = Row::FIRMWARE;
  return count;
}

const Rect& BlePairingActivity::rectFor(const Row row) const {
  switch (row) {
    case Row::NAME:
      return nameRect_;
    case Row::PAIR_NEW:
      return pairNewRect_;
    case Row::FORGET:
      return forgetRect_;
    case Row::FIRMWARE:
      break;
  }
  return firmwareRect_;
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
    case Row::NAME:
      editName();
      return;
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
      if (hitRect(rectFor(rows[i]), x, y)) {
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

void BlePairingActivity::editName() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DEVICE_NAME), SETTINGS.deviceName,
                                              CrossPointSettings::DEVICE_NAME_MAX_BYTES, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        char clean[CrossPointSettings::DEVICE_NAME_MAX_BYTES + 1];
        if (!CrossPointSettings::normalizeDeviceName(std::get<KeyboardResult>(result.data).text.c_str(), clean,
                                                     sizeof(clean))) {
          LOG_ERR("BLE", "device name not saved: over %u bytes or not printable ASCII",
                  static_cast<unsigned>(CrossPointSettings::DEVICE_NAME_MAX_BYTES));
          nameRejected_ = true;
          requestUpdate();
          return;
        }
        nameRejected_ = false;
        if (strcmp(clean, SETTINGS.deviceName) != 0) {
          {
            // render() reads the name.
            RenderLock lock(*this);
            snprintf(SETTINGS.deviceName, sizeof(SETTINGS.deviceName), "%s", clean);
          }
          if (!SETTINGS.saveToFile()) LOG_ERR("BLE", "could not save the device name");
          BLE_LINK.applyDeviceName();
        }
        requestUpdate();
      });
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
  const int pageHeight = renderer.getScreenHeight();
  const int margin = metrics.contentSidePadding;
  const int contentWidth = pageWidth - margin * 2;
  const int bodyLine = renderer.getLineHeight(UI_10_FONT_ID);
  // Landscape: Bluetooth in the left column, the name and Firmware in the right.
  // Stacked on a 480 px tall page, a paired reader's Forget row already ends
  // below the bottom edge with the Lyra metrics, and Firmware is off screen.
  const bool twoColumns = pageWidth > pageHeight;
  const int columnWidth = twoColumns ? (contentWidth - margin) / 2 : contentWidth;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

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

  // The column being drawn and the next free line in it.
  int left = margin;
  int y = top;

  // Everything left-aligned in the column, one line per call.
  const auto line = [&](const int font, const std::string& value, const EpdFontFamily::Style style) {
    const std::string fitted = renderer.truncatedText(font, value.c_str(), columnWidth, style);
    renderer.drawText(font, left, y, fitted.c_str(), true, style);
    y += renderer.getLineHeight(font);
  };
  // A sentence that may need a second line in a column.
  const auto sentence = [&](const int font, const char* value) {
    for (const auto& text : renderer.wrappedText(font, value, columnWidth, 2, EpdFontFamily::REGULAR)) {
      renderer.drawText(font, left, y, text.c_str(), true, EpdFontFamily::REGULAR);
      y += renderer.getLineHeight(font);
    }
  };
  const auto sectionTitle = [&](const char* title) {
    line(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
    y += 2;
    renderer.drawLine(left, y, left + columnWidth, y, true);
    y += metrics.verticalSpacing;
  };
  // A row that does something: outlined, label left-aligned, heavier when it is
  // the one Confirm would activate.
  const auto actionRow = [&](const std::string& label, const bool highlighted) -> Rect {
    const int rowTop = y + metrics.verticalSpacing;
    const int height = metrics.menuRowHeight;
    renderer.drawRoundedRect(left, rowTop, columnWidth, height, highlighted ? 2 : 1, 6, true);
    const std::string fitted =
        renderer.truncatedText(UI_10_FONT_ID, label.c_str(), columnWidth - 24, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, left + 12, rowTop + (height - bodyLine) / 2, fitted.c_str(), true,
                      EpdFontFamily::REGULAR);
    y = rowTop + height + metrics.verticalSpacing;
    return Rect{left, rowTop, columnWidth, height};
  };

  // --- Device name -----------------------------------------------------------
  // Label left, name right. With no name of its own the reader shows the default
  // in the small regular font, as a placeholder: the UI font has no grey or italic.
  const auto drawName = [&] {
    const int rowTop = y;
    const int height = metrics.menuRowHeight;
    renderer.drawRoundedRect(left, rowTop, columnWidth, height, selection == Row::NAME ? 2 : 1, 6, true);
    const char* label = tr(STR_DEVICE_NAME);
    renderer.drawText(UI_10_FONT_ID, left + 12, rowTop + (height - bodyLine) / 2, label, true, EpdFontFamily::REGULAR);
    const bool named = SETTINGS.deviceName[0] != '\0';
    const int valueFont = named ? UI_10_FONT_ID : SMALL_FONT_ID;
    const EpdFontFamily::Style valueStyle = named ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
    const int valueMaxWidth = std::max(0, columnWidth - 48 - labelWidth);
    const std::string value =
        renderer.truncatedText(valueFont, SETTINGS.effectiveDeviceName(), valueMaxWidth, valueStyle);
    const int valueWidth = renderer.getTextWidth(valueFont, value.c_str(), valueStyle);
    renderer.drawText(valueFont, left + columnWidth - 12 - valueWidth,
                      rowTop + (height - renderer.getLineHeight(valueFont)) / 2, value.c_str(), true, valueStyle);
    nameRect_ = Rect{left, rowTop, columnWidth, height};
    y = rowTop + height;
    if (nameRejected_) {
      y += 4;
      sentence(SMALL_FONT_ID, tr(STR_DEVICE_NAME_INVALID));
    }
    y += metrics.verticalSpacing * 2;
  };

  // --- Bluetooth -------------------------------------------------------------
  const auto drawBluetooth = [&] {
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
      sentence(SMALL_FONT_ID, tr(STR_BLE_PAIR_HINT));
    }
    if (!authError.empty()) {
      const char* errorText = authErrorText(authError);
      if (errorText) sentence(SMALL_FONT_ID, errorText);
    }
    if (paired && !windowOpen && lockSeconds == 0) {
      pairNewRect_ = actionRow(tr(STR_BLE_PAIR_NEW_PHONE), selection == Row::PAIR_NEW);
    }
    if (paired) forgetRect_ = actionRow(tr(STR_FORGET_BUTTON), selection == Row::FORGET);
    // Two spacings, not three: the name row above took the room.
    y += metrics.verticalSpacing * 2;
  };

  // --- Firmware --------------------------------------------------------------
  const auto drawFirmware = [&] {
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
  };

  if (twoColumns) {
    drawBluetooth();
    left = margin + columnWidth + margin;
    y = top;
    drawName();
    drawFirmware();
  } else {
    drawName();
    drawBluetooth();
    drawFirmware();
  }

  GUI.drawButtonHints(renderer, "", "", "", "", /*touchBack=*/false);
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_BLE_TRANSFER
