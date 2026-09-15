#include "BlePairPromptActivity.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "HalDisplay.h"
#include "components/UITheme.h"
#include "network/BleLink.h"

namespace {
constexpr int CHOICE_ALLOW = 0;
constexpr int CHOICE_DENY = 1;
}  // namespace

void BlePairPromptActivity::onEnter() {
  Activity::onEnter();
  const int maxWidth = renderer.getScreenWidth() - margin * 2;
  // host_name is at most 48 bytes of printable ASCII (sanitizeHostName in BleLink.cpp).
  char title[96];
  snprintf(title, sizeof(title), tr(STR_BLE_PAIR_PROMPT_FORMAT), BLE_LINK.pairPromptHostName().c_str());
  heading = renderer.truncatedText(fontId, title, maxWidth, EpdFontFamily::BOLD);
  body = renderer.truncatedText(fontId, tr(STR_BLE_PAIR_PROMPT_HINT), maxWidth, EpdFontFamily::REGULAR);
  showPopup();
}

void BlePairPromptActivity::onExit() {
  // Replaced without an answer (Home, sleep, a firmware install): refuse.
  if (!decided) BLE_LINK.resolvePairPrompt(false);
  Activity::onExit();
}

void BlePairPromptActivity::showPopup() {
  const char* options[] = {tr(STR_BLE_ALLOW), tr(STR_BLE_DENY)};
  // Deny is an option; no separate Back chip.
  popup.setTouchBack(false);
  // Confirm without moving the highlight denies.
  popup.show(heading.c_str(), options, 2, CHOICE_DENY, [this](const int idx) { choose(idx); });
  requestUpdate(true);
}

void BlePairPromptActivity::choose(const int index) {
  if (decided) return;
  decided = true;
  BLE_LINK.resolvePairPrompt(index == CHOICE_ALLOW);
  finish();
}

void BlePairPromptActivity::render(RenderLock&&) {
  renderer.clearScreen();
  int y = renderer.getScreenHeight() / 6;
  renderer.drawCenteredText(fontId, y, heading.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(fontId) + spacing;
  renderer.drawCenteredText(fontId, y, body.c_str(), true, EpdFontFamily::REGULAR);
  if (popup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void BlePairPromptActivity::loop() {
  if (decided) return;
  if (!BLE_LINK.pairPromptPending()) {
    // Timed out, or the link stopped: BleLink has already refused the request.
    LOG_INF("BLE", "pair prompt closed without an answer");
    decided = true;
    finish();
    return;
  }
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  // Closed without Allow or Deny (outside tap, Back): this question does not dismiss.
  showPopup();
}

#endif  // FREEINK_CAP_BLE_TRANSFER
