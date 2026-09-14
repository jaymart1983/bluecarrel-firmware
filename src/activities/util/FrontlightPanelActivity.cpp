#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "DeviceSleep.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a 2-column tile grid. The chrome
// itself is fui::sheet / fui::sliderRow / fui::tileGrid; these constants only
// size the bands, and computePanelBottom() mirrors them.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberHeight = 5;     // fui::SheetProps default, mirrored here
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kTileHeight = 84;
constexpr int16_t kTileGap = 16;
// The two band heights above are the ROOMY sizes, used whenever the frame has
// space for them; computeLayout() trims towards these floors when it does not.
// Both floors are still comfortably finger-sized (the SDK's touch target is
// 44px), so a trimmed sheet stays tappable rather than merely visible.
constexpr int16_t kMinSliderRowHeight = 44;
constexpr int16_t kMinTileHeight = 56;
constexpr int16_t kLayoutTrimStep = 4;
// Air left below the sheet, so it reads as a card hanging into the screen
// rather than a second full screen with a line across the bottom.
constexpr int16_t kMinBottomAir = 24;
// Above this logical width a two-column grid runs the sheet off the bottom of a
// 480-tall frame while leaving half the sheet empty; four columns fit the same
// tiles in half the rows. The X4 Pro's native landscape frame is 800 wide.
constexpr int kWideFrameWidth = 700;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  buildTileOrder();
  // A panel opened while the glass is already off is being driven from buttons,
  // so it must come up with a cursor on screen — an unfocused grid would leave
  // the user pressing keys at nothing.
  focusedTile = buttonNavActive() && tileCount > 0 ? 0 : -1;

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

// The tiles this board shows, in grid order: display first, then the touch
// switch, then the three ways off this screen. There is no Frontlight tile —
// the lamp button on the brightness row is the light's on/off switch, sitting
// right next to the level it belongs to, and a tile carrying the same toggle a
// few rows below it was simply a second control for one thing.
void FrontlightPanelActivity::buildTileOrder() {
  tileCount = 0;
  const auto add = [this](const TileId id) { tileIds[tileCount++] = id; };
  add(TILE_NIGHT);
  add(TILE_REFRESH);
#if !FREEINK_DEVICE_X4PRO
  add(TILE_ORIENTATION);
#endif
  add(TILE_TOUCH);
  add(TILE_SLEEP);
  add(TILE_SETTINGS);
  add(TILE_HOME);
  // LAST, and only while a cable is attached. A tile that comes and goes has to
  // come and go at the END of the grid: inserted anywhere else it shifts every
  // tile after it, so plugging in a cable would silently move Sleep and Home
  // out from under a finger that already knows where they are.
  if (usbDriveAvailable()) add(TILE_USB_DRIVE);
}

// The sheet lays out in whatever frame it opened over, so its bands are sized
// here rather than baked in: a 480x800 portrait frame has height to spare and
// room for only two tile columns, while the X4 Pro's native 800x480 landscape
// is the other way round — wide enough for four columns and 320px shorter, so
// the roomy band heights no longer fit. Anything the sheet does not fit is not
// merely cramped: Screen::sheet clamps its content area, so a row past the
// bottom is not drawn at all.
void FrontlightPanelActivity::computeLayout() {
  tileCols = renderer.getScreenWidth() >= kWideFrameWidth ? 4 : 2;
  tileHeight = kTileHeight;
  sliderRowHeight = kSliderRowHeight;

  // Trim the tiles first and the slider pills only once the tiles are at their
  // floor: the pills are dragged, the tiles are only tapped.
  const int maxBottom = renderer.getScreenHeight() - kMinBottomAir;
  while (computePanelBottom() > maxBottom) {
    if (tileHeight > kMinTileHeight) {
      tileHeight = static_cast<int16_t>(tileHeight - kLayoutTrimStep);
      continue;
    }
    if (sliderRowHeight > kMinSliderRowHeight) {
      sliderRowHeight = static_cast<int16_t>(sliderRowHeight - kLayoutTrimStep);
      continue;
    }
    // Both bands are at their floor. The sheet is as short as it can be while
    // staying usable; it simply reaches the bottom of this frame.
    break;
  }
}

void FrontlightPanelActivity::applyTileStyles(const uint8_t radius) {
  // The component's own 1-bit tile look (fui::tileGridStyles): an outlined card,
  // filled solid when the setting the tile carries is on. Never a dithered grey.
  fui::StyleSet& styles = gridProps.styles;
  styles.explicitlySet = true;
  styles.normal.background = fui::Paint::solid(fui::Color::White);
  styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  styles.normal.border = fui::Paint::solid(fui::Color::Black);
  styles.normal.borderWidth = 2;
  styles.normal.radius = radius;
  styles.selected = styles.normal;
  styles.selected.background = fui::Paint::solid(fui::Color::Black);
  styles.selected.foreground = fui::Paint::solid(fui::Color::White);
  styles.disabled = styles.normal;
  // On top of that, the button cursor: a heavy border over the plain card when
  // the tile is off and over the filled card when it is on, so focus and on/off
  // stay independently legible without inventing a second shade. StyleSet's
  // resolve() checks Active before Focused before Checked, which is why
  // "focused AND on" has to travel as StateActive.
  styles.focused = styles.normal;
  styles.focused.borderWidth = 6;
  styles.active = styles.selected;
  styles.active.borderWidth = 6;
}

bool FrontlightPanelActivity::buttonNavActive() const {
  return mappedInput.hasTouch() && !MappedInputManager::isTouchInputEnabled();
}

void FrontlightPanelActivity::moveFocus(const int delta) {
  if (tileCount <= 0) return;
  if (focusedTile < 0) {
    focusedTile = delta >= 0 ? 0 : tileCount - 1;
  } else {
    focusedTile = (focusedTile + delta + tileCount) % tileCount;
  }
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  // Held on Home: refresh the screen instead of going home. With no scheduled
  // ghost scrubs on this device, a clean full refresh is on demand -- this tile, or
  // the Refresh tile beside it. Any other tile treats a long press as a tap.
  if (event.longPress && event.value == TILE_HOME) {
    self->runTile(TILE_REFRESH);
    return;
  }
  self->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int id) {
  switch (id) {
    case TILE_NIGHT:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case TILE_REFRESH:  // Ghost-cleanup refresh of the whole frame
      // Refreshing with the panel still up would clean a frame the user is
      // about to dismiss anyway: drop the panel first and let the repaint of
      // the screen underneath carry the clean waveform instead.
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case TILE_ORIENTATION:
      // Steps through the orientations this build offers, which on a board with
      // no portrait mode is the two landscape ones — i.e. a 180-degree flip.
      SETTINGS.orientation = CrossPointSettings::cycleOrientation(SETTINGS.orientation, 1);
      SETTINGS.saveToFile();
      // Only the setting changes; the live frame is left alone. The reader
      // reflows to it on its next loop().
      requestUpdate();
      break;
    // The two navigation tiles replace the whole stack rather than popping:
    // the panel opens over any screen, so "Settings" and "Home" must be
    // absolute destinations, not a step back into whatever was underneath.
    // replaceActivity() runs this activity's onExit(), so the live
    // brightness/warmth still persist on the way out.
    case TILE_SETTINGS:
    case TILE_HOME:
      // Both are UI-frame screens and a power tap can open this panel over a
      // reader that has the renderer turned, so put the UI frame back before
      // leaving — otherwise Settings or Home draws in the reader's frame.
      ReaderUtils::applyUiOrientation(renderer);
      if (id == TILE_SETTINGS) {
        // On BLE builds this is the pairing and firmware page: other settings
        // are edited from the phone app, and pairing is the one thing the app
        // cannot do for you. Builds without BLE open the full Settings screen
        // (see ActivityManager::goToBlePairing).
        activityManager.goToBlePairing();
      } else {
        activityManager.goHome();
      }
      break;
    case TILE_USB_DRIVE:
      // Close first, then hand over from main.cpp's loop: the handover paints a
      // full-screen notice and detaches the card, and this panel must be gone
      // (and its own onExit run) before either happens.
      close();
      requestUsbDriveMode();
      break;
    case TILE_SLEEP:
      // Close first, then ask main.cpp to sleep at the top of its next loop:
      // enterDeepSleep() replaces the whole activity stack, so calling it from
      // this handler would delete the panel underneath its own event dispatch.
      // Popping here also lets onExit() persist the live brightness/warmth.
      close();
      requestDeviceSleep();
      break;
    case TILE_TOUCH:
      // The master touchscreen gate in MappedInputManager, which every tap,
      // swipe and long-press in the firmware is read through, reader page turns
      // and link taps included (SETTINGS.touchReaderControls only picks the
      // reader's gesture style). Runtime-only and never persisted, so a reboot or
      // a wake always brings the glass back; a power tap is the way back sooner.
      // Switching it off from here hands the panel straight to the button
      // cursor, so the screen that owns the switch stays usable.
      MappedInputManager::setTouchInputEnabled(!MappedInputManager::isTouchInputEnabled());
      LOG_INF("TOUCH", "Touchscreen %s from the control centre",
              MappedInputManager::isTouchInputEnabled() ? "enabled" : "disabled");
      if (buttonNavActive() && focusedTile < 0) {
        for (int i = 0; i < tileCount; ++i) {
          if (tileIds[i] == TILE_TOUCH) focusedTile = i;
        }
      }
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, /*withLongPress=*/true, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) draggingSlider = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }

  // With the glass off the panel is driven entirely from keys: the navigation
  // pair moves the tile cursor and Confirm activates it. Brightness stepping
  // gives way to that on purpose — the tile grid is the part that has to be
  // reachable, since it holds the switch that turns touch back on. The light's
  // on/off is not stranded by that: a power-button DOUBLE TAP toggles it from
  // anywhere, this panel included.
  if (buttonNavActive()) {
    // Confirm covers the X4 Pro's Right side-key hold, which MappedInputManager
    // folds into Button::Confirm outside a book. That is hardwired rather than
    // settings-driven, which matters here: this panel is the only way back to a
    // working touchscreen, so activating a tile must not depend on a binding the
    // user is free to change. The raw power click is deliberately NOT accepted
    // any more -- a power tap opens this panel, so treating it as "activate the
    // focused tile" would fire a tile on the way in.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (focusedTile >= 0 && focusedTile < tileCount) runTile(tileIds[focusedTile]);
      return;
    }
    buttonNavigator.onPressAndContinuous(ButtonNavigator::getPreviousButtons(), [this] { moveFocus(-1); });
    buttonNavigator.onPressAndContinuous(ButtonNavigator::getNextButtons(), [this] { moveFocus(1); });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  // NavPrevious/NavNext rather than the front Left/Right pair: on a board whose
  // only keys are the two side keys (X4 Pro) the front roles map to buttons that
  // do not exist, so the sliders were unreachable from hardware entirely.
  buttonNavigator.onPressAndContinuous(ButtonNavigator::getPreviousButtons(),
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous(ButtonNavigator::getNextButtons(),
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  // Slim battery band + the air around it (mirrors buildPanelScreen).
  const int y0 = std::max<int>(metrics.batteryHeight, lineHeight);
  int y = tokens.spaceMd + y0 + tokens.spaceMd;
  if (Frontlight.present()) {
    // Screen::sliderRow reserves caption + spaceMd + control band, then a
    // spaceMd gap; addSliderRow() adds one more spaceMd of air after each row.
    y += lineHeight + tokens.spaceMd + sliderRowHeight + 2 * tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceMd + sliderRowHeight + 2 * tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
  // Tiles are for touch boards: they are the finger-sized quick settings, and on
  // a buttons-only board the sheet is exactly the frontlight controls.
  const int tiles = mappedInput.hasTouch() ? tileCount : 0;
  y += fui::tileGridHeight(static_cast<uint16_t>(tiles), tileCols, tileHeight, kTileGap);
  // The sheet's grabber band: content margin + grabber + air to the edge.
  // buildPanelScreen() feeds the same theme spacings into SheetProps.
  y += tokens.spaceLg + kGrabberHeight + tokens.spaceLg + tokens.spaceMd;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  // Live percentage readout. The row draws before this call returns
  // (immediate mode), so borrowing a stack buffer is safe.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));

  // rowProps is a member (fui::SliderRowProps embeds a 324-byte StyleSet, well
  // past the 256-byte budget a local gets — AGENTS.md). Every field that
  // varies between the two rows is reassigned here; the rest keep their
  // constructed defaults, which already match the panel's card language.
  rowProps.label = label;
  rowProps.value = pct;
  rowProps.sliderValue = value;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }
  screen.sliderRow(rowProps, sliderRowHeight);
  // The wrapper's own trailing gap is one spaceMd; double it so the rows
  // breathe — a control band this tall reads cramped at the list cadence.
  screen.spacer(screen.theme().spaceMd);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  // Sheet chrome first: the card body, the 2px rule along its bottom edge, and
  // the grabber on the edge the sheet is dragged from. Screen::sheet() also
  // clamps the content area to the sheet, so every band below lays out inside
  // it. (No header: the panel is a floating card, and its own grabber says
  // what it is.)
  fui::SheetProps sheetProps;
  // A roomy band above the bottom rule: the grabber gets a full spaceLg of
  // air on both sides so the last row of content never crowds the sheet edge.
  sheetProps.grabberMargin = theme.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // Reuse the exact battery renderer and header rectangle used by Home. Call
  // the base implementation directly because RoundedRaff suppresses its
  // untitled Home header.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.spacer(theme.spaceMd);
    const int16_t bandH = std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight),
                                            screen.target().lineHeight(theme.smallText.font));
    screen.takeTop(bandH, theme.spaceMd);
    UITheme::getInstance().getTheme().BaseTheme::drawHeader(
        renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding - metrics.topPadding},
        nullptr);
  }

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/true);
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  // Quick-setting tiles. Two columns of finger-sized cards; a tile whose setting
  // is currently on draws filled (StateChecked -> the selected style), and the
  // button cursor draws as a heavy outline on top of that. Touch boards only —
  // the tiles are sized as touch targets.
  if (mappedInput.hasTouch()) {
    static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                              StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
    // The orientation tile is labelled with just the current mode ("Portrait"):
    // the mode names say what the tile is about on their own.
    const char* orientLabel = I18N.get(kOrientNames[SETTINGS.orientation % 4]);
    // "Touch On" / "Touch Off", from the existing state strings: the label names
    // the live state of the master touchscreen gate this tile drives.
    const bool touchOn = MappedInputManager::isTouchInputEnabled();
    char touchLabel[48];
    snprintf(touchLabel, sizeof(touchLabel), "%s %s", tr(STR_TOUCH_TOGGLE),
             I18N.get(touchOn ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));

    // The cursor is only shown while the panel is actually being driven from
    // keys; with touch alive a focus ring would just be a second selection
    // indicator competing with the finger.
    const bool showFocus = buttonNavActive();
    for (int slot = 0; slot < tileCount; ++slot) {
      const int16_t id = tileIds[slot];
      const char* label = nullptr;
      bool checked = false;
      switch (id) {
        case TILE_NIGHT:
          label = tr(STR_NIGHT_MODE);
          checked = SETTINGS.screenInverted != 0;
          break;
        case TILE_REFRESH:
          label = tr(STR_FORCE_REFRESH);
          break;
        case TILE_ORIENTATION:
          label = orientLabel;
          break;
        case TILE_TOUCH:
          label = touchLabel;
          // Filled when touch is OFF — the non-default, attention-worthy state.
          checked = !touchOn;
          break;
        case TILE_SLEEP:
          label = tr(STR_SLEEP);
          break;
        case TILE_USB_DRIVE:
          label = tr(STR_USB_DRIVE_TILE);
          break;
        case TILE_SETTINGS:
          label = tr(STR_SETTINGS_TITLE);
          break;
        case TILE_HOME:
        default:
          label = tr(STR_EOB_HOME);
          break;
      }
      const bool focused = showFocus && slot == focusedTile;
      // Active is the one style slot that can carry "focused AND on" — see
      // applyTileStyles().
      fui::State state = fui::StateNormal;
      if (checked && focused) {
        state = fui::StateActive;
      } else if (focused) {
        state = fui::StateFocused;
      } else if (checked) {
        state = fui::StateChecked;
      }
      gridItems[slot].label = label;
      gridItems[slot].value = id;
      gridItems[slot].state = state;
    }

    applyTileStyles(theme.controlRadius);
    gridProps.items = gridItems;
    gridProps.count = static_cast<uint16_t>(tileCount);
    gridProps.action = ACTION_TILE;
    // Long presses too: held on Home, the tile refreshes the screen (onTileEvent).
    gridProps.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
    gridProps.columns = tileCols;
    gridProps.tileHeight = tileHeight;
    gridProps.gap = kTileGap;
    screen.tileGrid(gridProps);
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  // The frame can change under the panel between renders (the orientation tile
  // does not turn it, but a reader underneath re-applies its own on the way
  // back), so the layout is re-measured every render rather than only at enter.
  computeLayout();
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); the battery band at the card's top is part of the screen build.
  renderUi();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
