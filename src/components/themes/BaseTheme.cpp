#include "BaseTheme.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include "I18n.h"
#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookmark.h"
#if FREEINK_CAP_BLE_TRANSFER
#include "network/BleLink.h"
#endif
#include "fontIds.h"

// Internal constants
namespace {
// Whether this frame's header drew Back. drawTouchBackButton reads it to decide
// whether the bottom chip is still needed; every drawHeader writes it, so it can
// never describe a previous frame.
std::atomic<bool> gBackInHeader{false};
constexpr int kHeaderBackGap = 8;
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = 16;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconGap = 4;
constexpr int bookmarkStatusIconTopCrop = 2;

void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  }
}

void BaseTheme::drawStatusLabel(const GfxRenderer& renderer, const int x, const int centerY, const char* text,
                                const bool slashed) {
  const int width = renderer.getTextWidth(SMALL_FONT_ID, text);
  if (width <= 0) return;

  // Centre on the ink, not the font box. getTextHeight is the font-wide
  // ascender; centring short all-caps text on it sits it visibly low -- the
  // same bias that had to be taken out of the Back chip.
  int inkTop = 0;
  int inkBottom = 0;
  const int ascender = renderer.getFontAscenderSize(SMALL_FONT_ID);
  const bool measured = renderer.getTextInkBounds(SMALL_FONT_ID, text, inkTop, inkBottom);
  const int top = measured ? centerY + (inkTop + inkBottom) / 2 - ascender : centerY - ascender / 2;
  renderer.drawText(SMALL_FONT_ID, x, top, text);

  if (slashed && measured) {
    // Corner to corner across the ink box only, so it reads as a negation of
    // the word rather than as a rule under the whole row.
    const int inkTopY = top + ascender - inkTop;
    const int inkBottomY = top + ascender - inkBottom;
    renderer.drawLine(x - 1, inkBottomY + 1, x + width + 1, inkTopY - 1, true);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current, const size_t total) {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

// Centre a button-hint label inside its box. A label that fits is drawn on the
// single baseline it always was; one too wide used to overflow the button border
// and run into the neighbouring hint, and now wraps to at most two centred lines
// (wrappedText() ellipsises anything that still doesn't fit). Shared so every
// theme's drawButtonHints() gets the same behaviour.
void BaseTheme::drawHintLabel(const GfxRenderer& renderer, const int fontId, const char* label, const int x,
                              const int boxWidth, const int boxTop, const int boxHeight, const int singleLineYOffset) {
  constexpr int textPadding = 4;  // keeps a wrapped label off the button's border
  const int maxTextWidth = boxWidth - (textPadding * 2);

  const int textWidth = renderer.getTextWidth(fontId, label);
  if (textWidth <= maxTextWidth) {
    renderer.drawText(fontId, x + (boxWidth - 1 - textWidth) / 2, boxTop + singleLineYOffset, label);
    return;
  }

  // Spaced by the glyph height, not getLineHeight() — that returns the font's
  // full advanceY (leading included), which stacks two lines taller than the
  // button and clips the second one.
  constexpr int lineGap = 2;
  const int step = renderer.getTextHeight(fontId) + lineGap;
  const auto lines = renderer.wrappedText(fontId, label, maxTextWidth, 2);
  const int block = static_cast<int>(lines.size()) * step - lineGap;
  int lineY = boxTop + std::max(1, (boxHeight - block) / 2);
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(fontId, line.c_str());
    renderer.drawText(fontId, x + (boxWidth - 1 - lineWidth) / 2, lineY, line.c_str());
    lineY += step;
  }
}

namespace {
// Chip geometry, in the CURRENT orientation: a touch user reads "bottom left of
// what I am looking at", not "bottom left of the portrait panel".
constexpr int kBackChipWidth = 108;
constexpr int kBackChipMargin = 4;  // 40px band - 2*4 = 32px chip; 8 left the text cramped
// Extra slop so the chip clears the 44px touch-target guidance even though the
// hint band itself is 40px tall.
constexpr int kBackChipHitPadding = 6;
std::atomic<bool> backChipVisible{false};
}  // namespace

namespace {
// Trim a rectangle to the panel. The chip and its hit rect are derived from
// theme metrics, so a theme whose band does not fit the screen must degrade to
// a smaller chip rather than to coordinates the renderer has to clip.
Rect clampToScreen(const GfxRenderer& renderer, const Rect r) {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int x0 = std::max(0, r.x);
  const int y0 = std::max(0, r.y);
  const int x1 = std::min(screenWidth, r.x + r.width);
  const int y1 = std::min(screenHeight, r.y + r.height);
  return Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}
}  // namespace

bool BaseTheme::backDrawnInHeader() { return gBackInHeader.load(std::memory_order_relaxed); }

const char* BaseTheme::backHeaderLabel() { return I18N.get(StrId::STR_BACK); }

Rect BaseTheme::touchBackButtonRect(const GfxRenderer& renderer) {
  if (gBackInHeader.load(std::memory_order_relaxed)) {
    // The whole header band on the left, not just the text line: a touch target
    // the height of one line of small text is too small to hit reliably, and
    // nothing else is drawn in that column.
    const auto& metrics = UITheme::getInstance().getMetrics();
    return clampToScreen(
        renderer, Rect{0, static_cast<int>(metrics.topPadding),
                       metrics.contentSidePadding + renderer.getTextWidth(UI_10_FONT_ID, backHeaderLabel()) +
                           kHeaderBackGap,
                       static_cast<int>(metrics.headerHeight)});
  }
  // The band the active theme reserves at the bottom of every screen (see
  // UITheme::getMetrics), so the chip always lands inside reserved space.
  const int band = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int height = band - 2 * kBackChipMargin;
  return clampToScreen(renderer, Rect{kBackChipMargin, renderer.getScreenHeight() - band + kBackChipMargin,
                                      kBackChipWidth, height > 0 ? height : band});
}

Rect BaseTheme::touchBackButtonHitRect(const GfxRenderer& renderer) {
  const Rect r = touchBackButtonRect(renderer);
  // The padding is slop for fingers, not a licence to describe a target off the
  // panel: with a 4px chip margin it would otherwise start at x=-2 and end two
  // rows below the last one, and a hit rect that does not exist on screen is a
  // hit rect nobody can aim at.
  return clampToScreen(renderer, Rect{r.x - kBackChipHitPadding, r.y - kBackChipHitPadding,
                                      r.width + 2 * kBackChipHitPadding, r.height + 2 * kBackChipHitPadding});
}

bool BaseTheme::touchBackButtonVisible() { return backChipVisible.load(std::memory_order_relaxed); }

void BaseTheme::setTouchBackButtonVisible(const bool visible) {
  backChipVisible.store(visible, std::memory_order_relaxed);
}

void BaseTheme::drawTouchBackButton(const GfxRenderer& renderer) {
  // The X4 Pro has no Back control on screen: a side-key hold is Back outside a
  // book (MappedInputManager), so a chip would be a second way to do one thing.
  if (CrossPointSettings::usesPowerGestures()) return;
  // Already drawn in the title bar this frame: drawing it again would put two
  // Back controls on one screen, and the hit rect can only describe one of them.
  if (gBackInHeader.load(std::memory_order_relaxed)) return;
  const Rect rect = touchBackButtonRect(renderer);
  // White fill first: the chip floats over whatever the screen drew underneath
  // and must stay legible on a busy list.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, 6, true);
  const char* label = I18N.get(StrId::STR_BACK);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
  const int ascender = renderer.getFontAscenderSize(UI_10_FONT_ID);
  // drawText's Y is the TOP of the text box -- it adds the ascender itself
  // (GfxRenderer.cpp: yPos = y + getFontAscenderSize). So centring on
  // getTextHeight (== the font-wide ascender) centres the font's design box,
  // not the ink: the ascender covers the tallest glyph in the whole font plus
  // the designer's leading, while "<< Back" only reaches cap height. With UI_10
  // (ascender 20, cap height 15) that leaves visibly more slack above the ink
  // than below it.
  //
  // Measuring the glyphs actually being drawn removes it. Solving
  //   baseline - inkTop - rect.y == rect.y + rect.height - baseline
  // for the baseline gives (rect.height + inkTop) / 2, and drawText wants the
  // box top, which is the baseline minus the ascender.
  //
  // Do NOT add the text height to this Y: that pushes the ink below the chip and
  // off the bottom of the panel.
  int inkTop = 0;
  int inkBottom = 0;
  const int textTop = renderer.getTextInkBounds(UI_10_FONT_ID, label, inkTop, inkBottom)
                          ? rect.y + (rect.height + inkTop + inkBottom) / 2 - ascender
                          : rect.y + (rect.height - ascender) / 2;
  renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - textWidth) / 2, textTop, label);
  setTouchBackButtonVisible(true);
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, const bool touchBack) const {
  if (gpio.hasTouch()) {
    if (touchBack) drawTouchBackButton(renderer);
    return;
  }

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      drawHintLabel(renderer, UI_10_FONT_ID, labels[i], x, buttonWidth, pageHeight - buttonY, buttonHeight,
                    textYOffset);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (gpio.hasEdgeSideButtons()) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool withBack) const {
  // No header Back on the X4 Pro either -- see drawTouchBackButton().
  const bool showBack = withBack && !CrossPointSettings::usesPowerGestures();
  gBackInHeader.store(showBack, std::memory_order_relaxed);
  // Every activity header renders through the FreeInkUI header + battery
  // indicator components, styled by the active theme's tokens (padding,
  // centering, underline). Non-interactive frame: no hit rects registered.
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  // Refresh the app-wide shared tokens instead of copying ~1.5KB of
  // ThemeTokens onto this render-path stack frame; the values derived here
  // are identical to what every FreeInkApp screen derives. Goes through the
  // same publish-a-fresh-slot path applySharedUiTheme() uses (see
  // UiAppHelpers.h) rather than overwriting the previously-published
  // instance in place, since some other FreeInkApp could be mid-read of it.
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(ui.target);
  // Header status text (battery percent, right label) stays at the fixed
  // small font like the legacy headers; the uiScale small font is for list
  // subtitles.
  ui.target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect band{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y), static_cast<int16_t>(rect.width),
                       static_cast<int16_t>(rect.height)};

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(percentage));
  // The icon glyph extends 2px past glyphWidth (terminal nub); reserve it or
  // the percent label's rect comes up short and the text truncates.
  constexpr int16_t batteryNubWidth = 2;
  int16_t batteryReserve = static_cast<int16_t>(metrics.batteryWidth + batteryNubWidth);
  if (showBatteryPercentage) {
    batteryReserve = static_cast<int16_t>(
        batteryReserve + batteryPercentSpacing +
        ui.target.measureText(fui::GfxRendererTarget::FONT_SMALL, percentText, tokens.smallText).width);
  }

  // --- status cluster: clock + Bluetooth, to the left of the battery ---------
  // Both answer a question the user cannot otherwise ask the device: "is the
  // radio actually up, and is the phone actually on it?" -- and, for the clock,
  // whether the RTC has ever been told the time at all. Sized here so it can be
  // added to the same reserve the battery uses; the header component then keeps
  // the title clear of all of it.
  char clockText[16];
  const bool showClock =
      halClock.isAvailable() && halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockUtcOffsetQ,
                                                    SETTINGS.clockFormat == 1);
  // "BLE" plain when a phone is connected and past the hello gate, struck
  // through otherwise. Paired-but-absent is deliberately the slashed state:
  // what this answers is "can the Store reach my phone right now", and a saved
  // trusted host does not answer that.
  bool showBle = false;
#if FREEINK_CAP_BLE_TRANSFER
  // Shown only while a phone is connected AND through the hello gate, and absent
  // otherwise -- the same rule as USB. A struck-through word was a third state to
  // read at a glance for no gain: "no phone" and "no cable" are the ordinary
  // condition, and the header should be quiet in it.
  showBle = BLE_LINK.isAuthenticated();
#endif
  // "USB" means a cable is attached, not that a host has the card: USB Drive is
  // an explicit choice from the control centre, so a cable can sit there with
  // the card still ours. A wall charger reads HIGH on VBUS too, which is fine --
  // the label only claims something is plugged in, which is exactly what
  // decides whether the control centre offers the USB Drive tile.
  const bool showUsb = gpio.isUsbConnected();

  const int16_t clockWidth =
      showClock ? ui.target.measureText(fui::GfxRendererTarget::FONT_SMALL, clockText, tokens.smallText).width : 0;
  // The status words ride with the battery; the clock takes the opposite end of
  // the band. Two reserves rather than one cluster, so the title is kept clear
  // of each of them on the side it is actually on.
  const int bleWidth = showBle ? renderer.getTextWidth(SMALL_FONT_ID, "BLE") : 0;
  const int usbWidth = showUsb ? renderer.getTextWidth(SMALL_FONT_ID, "USB") : 0;
  int16_t labelReserve = 0;
  if (showBle) labelReserve = static_cast<int16_t>(labelReserve + bleWidth + tokens.spaceMd);
  if (showUsb) labelReserve = static_cast<int16_t>(labelReserve + usbWidth + tokens.spaceMd);
  const int16_t clockReserve = showClock ? static_cast<int16_t>(clockWidth + tokens.spaceMd) : 0;

  fui::HeaderProps props;
  props.title = title;
  props.rightLabel = subtitle;  // firmware headers right-align the secondary text
  const bool batteryLeft = metrics.headerBatterySide == 1;
  const bool batteryDetached = metrics.headerBatteryDetached;
  // Shared-line headers with the battery on the right: the header component
  // places rightLabel inside the battery reserve, so it sits mid-band next to
  // the icon and shifts with the percent label's width. Draw it manually below
  // instead, pinned at the fixed side inset in the band's lower half — the
  // same corner the detached (Lyra) layout puts it — so the label holds one
  // position across themes and battery states.
  const bool manualRightLabel = subtitle != nullptr && !batteryDetached && !batteryLeft;
  if (manualRightLabel) {
    props.rightLabel = nullptr;
  }
  props.borderEdges = fui::EdgeBottom;
  props.titleText = tokens.titleText;
  props.titleText.align = tokens.headerTitleAlign;
  // A centred title ignores leftReserve, so with Back in the band the two land
  // on top of each other and read as one control. Left-align the title when Back
  // is present, so it sits after the rule rather than over it.
  if (showBack) props.titleText.align = fui::TextAlign::Left;
  props.subtitleText = tokens.smallText;
  props.styles = tokens.popup;
  props.sidePadding = tokens.headerSidePadding;
  if (batteryDetached) {
    // Battery in its own corner strip; the title owns the full width of the
    // lower sub-band, so long book titles span the header (Lyra layout).
    // Anchor the title with explicit clearance above the band's bottom rule
    // instead of naive sub-band centering, which left the glyphs nearly
    // touching it.
    const int titleLineHeight = ui.target.lineHeight(fui::GfxRendererTarget::FONT_TITLE);
    const int titleTop = static_cast<int>(band.height) - tokens.headerUnderline - tokens.spaceMd - titleLineHeight;
    props.titleOffsetY = static_cast<int16_t>(titleTop - (static_cast<int>(band.height) - titleLineHeight) / 2);
  } else {
    // The Back label lives at the far left of the band, so the title has to be
    // kept clear of it exactly as it is kept clear of the battery.
    const int16_t backReserve =
        showBack ? static_cast<int16_t>(renderer.getTextWidth(UI_10_FONT_ID, backHeaderLabel()) + kHeaderBackGap * 3)
                 : 0;
    const int16_t reserve = static_cast<int16_t>(batteryReserve + labelReserve + tokens.spaceMd);
    (void)clockReserve;
    if (batteryLeft) {
      props.leftReserve = reserve;
      props.rightReserve = clockReserve;
    } else {
      props.rightReserve = static_cast<int16_t>(reserve + (showBack ? clockReserve : 0));
      props.leftReserve = static_cast<int16_t>(backReserve + (showBack ? 0 : clockReserve));
    }
    if (batteryLeft) {
      props.leftReserve = static_cast<int16_t>(props.leftReserve + clockReserve + backReserve);
    }
  }
  // Underline only under a titled header: an untitled band (Lyra home screen)
  // historically drew no rule, and the old themes keyed the line on the title.
  if (title != nullptr && props.styles.normal.border.kind == fui::PaintKind::None && tokens.headerUnderline > 0) {
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = tokens.headerUnderline;
  }
  fui::header(ui.frame, band, props);

  fui::BatteryIndicatorProps battery;
  battery.percent = static_cast<uint8_t>(percentage > 100 ? 100 : percentage);
  battery.charging = gpio.isUsbConnected();
  battery.label = showBatteryPercentage ? percentText : nullptr;
  battery.text = tokens.smallText;
  battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  battery.gap = batteryPercentSpacing;
  // Detached: hug the corner (12px, the legacy inset) within the battery
  // strip; shared line: sit on the content grid. Both anchor to the band's top
  // strip (batteryBarHeight) — the legacy shared-line headers drew the battery
  // at the top edge, and it keeps the lower-right corner free for the manual
  // right label below.
  const int16_t batteryEdgeInset = batteryDetached ? 12 : tokens.headerSidePadding;
  const int16_t batteryX = batteryLeft ? static_cast<int16_t>(band.x + batteryEdgeInset)
                                       : static_cast<int16_t>(band.right() - batteryEdgeInset - batteryReserve);
  const int16_t batteryH = static_cast<int16_t>(metrics.batteryBarHeight);
  fui::batteryIndicator(ui.frame, fui::Rect{batteryX, band.y, batteryReserve, batteryH}, battery);

  // Labels and clock all centre on the battery band's midline -- where the
  // percent text sits -- so the whole row shares one optical line.
  {
    const int centerY = static_cast<int>(band.y) + static_cast<int>(batteryH) / 2;

    // Laid out away from the battery, so the order holds whichever side the
    // theme puts the battery on.
    int cursorX = batteryLeft ? static_cast<int>(batteryX + batteryReserve + tokens.spaceMd)
                              : static_cast<int>(batteryX - tokens.spaceMd);
    const auto place = [&](const int width, const char* text, const bool slashed) {
      drawStatusLabel(renderer, batteryLeft ? cursorX : cursorX - width, centerY, text, slashed);
      cursorX += batteryLeft ? width + tokens.spaceMd : -(width + tokens.spaceMd);
    };
    if (showBle) place(bleWidth, "BLE", false);
    if (showUsb) place(usbWidth, "USB", false);

    if (showClock) {
      // Top left, which is where a clock belongs -- unless this header also
      // carries Back, which occupies that inset. Then it sits inboard of the
      // battery instead; the two stacked on each other when both wanted the
      // left edge.
      const int clockX = (!showBack && !batteryLeft)
                             ? static_cast<int>(band.x) + tokens.headerSidePadding
                             : (batteryLeft ? cursorX : cursorX - clockWidth);
      drawStatusLabel(renderer, clockX, centerY, clockText, false);
    }
  }

  if (showBack) {
    // "< Back |" beside the title, ON THE TITLE'S OWN LINE.
    //
    // Centring it in the whole band put it above the title in themes that give
    // the title a lower sub-band (Lyra), so the two stacked instead of sitting
    // side by side -- Back over the top of the word it belongs next to. The
    // title's line is computed here the same way the header component computes
    // it, so the two agree by construction rather than by luck.
    const int titleLineHeight = ui.target.lineHeight(fui::GfxRendererTarget::FONT_TITLE);
    const int titleTop = batteryDetached
                             ? static_cast<int>(band.y) + static_cast<int>(band.height) - tokens.headerUnderline -
                                   tokens.spaceMd - titleLineHeight
                             : static_cast<int>(band.y) + (static_cast<int>(band.height) - titleLineHeight) / 2;
    const int lineCenterY = titleTop + titleLineHeight / 2;

    const char* label = backHeaderLabel();
    int inkTop = 0;
    int inkBottom = 0;
    const int ascender = renderer.getFontAscenderSize(UI_10_FONT_ID);
    const int top = renderer.getTextInkBounds(UI_10_FONT_ID, label, inkTop, inkBottom)
                        ? lineCenterY + (inkTop + inkBottom) / 2 - ascender
                        : lineCenterY - ascender / 2;
    const int backX = static_cast<int>(band.x) + tokens.headerSidePadding;
    renderer.drawText(UI_10_FONT_ID, backX, top, label);
    // The rule sits between Back and the title, spanning only the title's line.
    const int ruleX = backX + renderer.getTextWidth(UI_10_FONT_ID, label) + kHeaderBackGap;
    renderer.drawLine(ruleX, titleTop + 2, ruleX, titleTop + titleLineHeight - 2, true);
    setTouchBackButtonVisible(true);
  }

  if (manualRightLabel) {
    const fui::Size labelSize = ui.target.measureText(fui::GfxRendererTarget::FONT_SMALL, subtitle, tokens.smallText);
    const int16_t labelH = ui.target.lineHeight(fui::GfxRendererTarget::FONT_SMALL);
    const fui::Rect labelRect{static_cast<int16_t>(band.right() - tokens.headerSidePadding - labelSize.width),
                              static_cast<int16_t>(band.bottom() - tokens.headerUnderline - tokens.spaceSm - labelH),
                              labelSize.width, labelH};
    ui.target.text(labelRect, subtitle, tokens.smallText);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int labelGap = 10;
  const int contentWidth = std::max(0, rect.width - BaseMetrics::values.contentSidePadding * 2);

  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - labelGap);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(UI_12_FONT_ID, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, rect.y, truncatedLabel.c_str(),
                      true, EpdFontFamily::REGULAR);
  }
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

int BaseTheme::getMenuRowHeight(const GfxRenderer&) const { return UITheme::getInstance().getMetrics().menuRowHeight; }

void BaseTheme::drawRowCover(const GfxRenderer& renderer, const std::string& path, const int x, const int y,
                             const int w, const int h) {
  // A missing or unreadable cover draws as an empty frame: it is one row of the
  // list, not the list. drawBitmap fits the art to the box, so a cover rendered
  // at the detail geometry scales down to a row without a second thumbnail.
  renderer.drawRect(x, y, w, h);
  if (path.empty()) return;
  HalFile file;
  if (!Storage.openFileForRead("ROW", path, file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    renderer.drawBitmap(bitmap, x, y, w, h);
  }
  file.close();
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon,
                              const std::function<std::string(int index)>& rowCover, const int rowHeightOverride,
                              const std::function<bool(int index)>& rowMarked,
                              const std::function<float(int index)>& rowPercent) const {
  (void)rowHeightOverride;  // the base menu draws at its own row height
  (void)rowMarked;          // the Last Read badge is drawn by the Library's theme only
  (void)rowPercent;         // ...as is the progress dial
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, metrics.popupTextInverted, popupFontFamily);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

int BaseTheme::readerTopBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  // topPadding + the status strip the header uses + a gap + the rule. Derived
  // from the same metrics the header is built from, so the band is the same
  // height here as it is everywhere else rather than a number that happens to
  // look close.
  return metrics.topPadding + metrics.batteryBarHeight + kReaderTopBarRuleGap + 2;
}

void BaseTheme::drawReaderTopBar(const GfxRenderer& renderer, const std::string& chapter,
                                 const float bookProgressPercent) {
  // Deliberately built from the SAME pieces, metrics and anchors as
  // drawHeader's status strip: the clock at headerSidePadding, the battery
  // inset from the right edge, and every label centred on the battery band's
  // midline. The first version of this laid the row out on its own terms --
  // its own insets, its own vertical centring -- and the result was a top bar
  // that was subtly out of step with the identical bar on every other screen.
  // The only thing that differs here is the chapter and percent between them.
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(ui.target);
  ui.target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  const fui::Rect band{0, static_cast<int16_t>(metrics.topPadding),
                       static_cast<int16_t>(renderer.getScreenWidth()),
                       static_cast<int16_t>(metrics.batteryBarHeight)};

  // --- battery: measured exactly as the header measures it -------------------
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(percentage));
  // The icon glyph extends 2px past glyphWidth (terminal nub); reserve it or
  // the percent label's rect comes up short and the text truncates.
  constexpr int16_t batteryNubWidth = 2;
  int16_t batteryReserve = static_cast<int16_t>(metrics.batteryWidth + batteryNubWidth);
  if (showBatteryPercentage) {
    batteryReserve = static_cast<int16_t>(
        batteryReserve + batteryPercentSpacing +
        ui.target.measureText(fui::GfxRendererTarget::FONT_SMALL, percentText, tokens.smallText).width);
  }

  char clockText[16];
  const bool showClock =
      halClock.isAvailable() && halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockUtcOffsetQ,
                                                    SETTINGS.clockFormat == 1);
  bool showBle = false;
#if FREEINK_CAP_BLE_TRANSFER
  showBle = BLE_LINK.isAuthenticated();
#endif
  // Same rule as the header's: a cable, not a handover. See drawHeader.
  const bool showUsb = gpio.isUsbConnected();
  const int bleWidth = showBle ? renderer.getTextWidth(SMALL_FONT_ID, "BLE") : 0;
  const int usbWidth = showUsb ? renderer.getTextWidth(SMALL_FONT_ID, "USB") : 0;
  const int clockWidth = showClock ? renderer.getTextWidth(SMALL_FONT_ID, clockText) : 0;

  fui::BatteryIndicatorProps battery;
  battery.percent = static_cast<uint8_t>(percentage > 100 ? 100 : percentage);
  battery.charging = gpio.isUsbConnected();
  battery.label = showBatteryPercentage ? percentText : nullptr;
  battery.text = tokens.smallText;
  battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  battery.gap = batteryPercentSpacing;
  const int16_t batteryEdgeInset = metrics.headerBatteryDetached ? 12 : tokens.headerSidePadding;
  const int16_t batteryX = static_cast<int16_t>(band.right() - batteryEdgeInset - batteryReserve);
  fui::batteryIndicator(ui.frame, fui::Rect{batteryX, band.y, batteryReserve, band.height}, battery);

  // --- labels and clock, on the battery band's midline -----------------------
  const int centerY = static_cast<int>(band.y) + static_cast<int>(band.height) / 2;
  int cursorX = static_cast<int>(batteryX) - tokens.spaceMd;
  const auto place = [&](const int width, const char* text) {
    drawStatusLabel(renderer, cursorX - width, centerY, text, false);
    cursorX -= width + tokens.spaceMd;
  };
  if (showBle) place(bleWidth, "BLE");
  if (showUsb) place(usbWidth, "USB");

  const int clockX = static_cast<int>(band.x) + tokens.headerSidePadding;
  if (showClock) drawStatusLabel(renderer, clockX, centerY, clockText, false);

  // --- centre: chapter, then percent of book ---------------------------------
  // Percent of the BOOK, not of the chapter: it is the number the library list
  // shows for the same book, and two different "progress" numbers on the same
  // device is worse than one.
  // One decimal, to match the app exactly.
  //
  // Whole percent is a coarse unit at the start of a book: "0%" for a book you
  // have demonstrably started reads as "nothing was recorded" rather than "not
  // far in". The precision was always there -- the sidecar stores basis points
  // (ProgressFile.h) and the wire value carries four decimals -- so rounding to
  // an int here was the only thing throwing it away, and it made the device and
  // the app disagree about the same book by up to half a percent.
  //
  // Zero stays "0%": a decimal on nothing is noise, and the app renders it the
  // same way.
  char progressText[12];
  if (bookProgressPercent <= 0.0f) {
    snprintf(progressText, sizeof(progressText), "0%%");
  } else {
    snprintf(progressText, sizeof(progressText), "%.1f%%", static_cast<double>(bookProgressPercent));
  }
  const int progressWidth = renderer.getTextWidth(SMALL_FONT_ID, progressText);
  constexpr int chapterPercentGap = 10;

  // What is left between the clock and whatever the right cluster has grown to.
  const int laneLeft = clockX + clockWidth + (showClock ? tokens.spaceMd : 0);
  const int laneRight = cursorX;
  const int laneWidth = laneRight - laneLeft;
  if (laneWidth > progressWidth + chapterPercentGap) {
    std::string title = chapter;
    const int maxChapterWidth = laneWidth - progressWidth - chapterPercentGap;
    if (renderer.getTextWidth(SMALL_FONT_ID, title.c_str()) > maxChapterWidth) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), maxChapterWidth);
    }
    const int chapterWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    const int blockWidth = chapterWidth + (chapterWidth > 0 ? chapterPercentGap : 0) + progressWidth;
    int x = laneLeft + (laneWidth - blockWidth) / 2;
    if (chapterWidth > 0) {
      drawStatusLabel(renderer, x, centerY, title.c_str(), false);
      x += chapterWidth + chapterPercentGap;
    }
    drawStatusLabel(renderer, x, centerY, progressText, false);
  }

  // --- the rule --------------------------------------------------------------
  // Full width, edge to edge, ignoring the side insets: it separates two regions
  // of the screen, so stopping it short would read as an underline belonging to
  // the text above it. screenWidth - 1 because the last valid column is
  // width - 1; drawing to width puts a pixel off-panel every frame.
  const int ruleY = band.y + band.height + kReaderTopBarRuleGap;
  renderer.drawLine(0, ruleY, renderer.getScreenWidth() - 1, ruleY, true);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin, const bool isPageBookmarked, const bool pageCountEstimated) {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto sb = SETTINGS.statusBarSpec();
  const bool showStatusBarTextLane = sb.textLaneVisible(halClock.isAvailable());

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  const int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int rightClusterX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (sb.showBookProgressPercent || sb.showChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Draw the estimate marker separately so it can use the next UI font size.
    const bool showEstimate = pageCountEstimated && sb.showChapterPageCount;

    if (sb.showBookProgressPercent && sb.showChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (sb.showBookProgressPercent) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }

    int progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    const int estimateWidth = showEstimate ? renderer.getTextWidth(UI_10_FONT_ID, "~") : 0;
    constexpr int estimateGap = 2;
    const int estimateSpacing = showEstimate ? estimateGap : 0;
    const int progressX = rightClusterX - estimateWidth - estimateSpacing - progressTextWidth;
    if (showEstimate) {
      const int estimateY = textY + (renderer.getLineHeight(SMALL_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, progressX, estimateY, "~");
    }
    renderer.drawText(SMALL_FONT_ID, progressX + estimateWidth + estimateSpacing, textY, progressStr);

    rightClusterWidth += estimateWidth + estimateSpacing + progressTextWidth;
  }

  // Draw Progress Bar
  if (sb.showsProgressBar()) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = renderer.getScreenWidth() - barMarginLeft - barMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom - sb.progressBarHeightPx -
                             paddingBottom + (fillMargin ? 1 : 0);
    size_t progress;
    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int barHeight = sb.progressBarHeightPx + (fillMargin ? orientedMarginBottom - 1 : 0);
    renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
  }

  // Draw Battery
  const bool showBatteryPercentage = sb.showBatteryPercent;

  if (sb.showBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{leftClusterX + leftClusterWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    int batteryWidth = metrics.batteryWidth;

    if (showBatteryPercentage) {
      const uint16_t percentage = powerManager.getBatteryPercentage();
      // width of icon + spacing + text for layout purposes
      batteryWidth +=
          batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, (std::to_string(percentage) + "%").c_str());
    }

    leftClusterWidth += batteryWidth;
  }

  // Draw Clock (X3 only — DS3231 RTC)
  if (sb.showsClock() && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), sb.clockUtcOffsetQ, sb.clock12h)) {
      int clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      int clockX = 0;
      // Position to the left or right of the progress text (with a small gap)
      if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {
        clockX = leftClusterX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);
        leftClusterWidth += clockTextWidth + 10;
      } else if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {
        clockX = rightClusterX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockTextWidth;
        rightClusterWidth += clockTextWidth + 10;
      }
      renderer.drawText(SMALL_FONT_ID, clockX, textY, timeBuf);
    }
  }

  // Draw Bookmark
  if (showStatusBarTextLane && isPageBookmarked) {
    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    const int bookmarkX = leftClusterX + leftClusterWidth + bookmarkGap;
    const int bookmarkY = textY + 5;
    drawBookmarkStatusIcon(renderer, bookmarkX, bookmarkY);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}
