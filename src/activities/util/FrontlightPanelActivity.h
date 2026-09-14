#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored control center opened by a status-bar tap or by a power-button
// tap (iOS Control Center style): a grabber, the frontlight
// brightness/warmth sliders (on boards with a light), and a grid of
// quick-setting tiles — night mode, ghost-cleanup refresh, reading orientation,
// touchscreen on/off, sleep, settings and home. The frontlight controls are
// always there: they are what the panel is for, and the lamp button that sits
// after the brightness row's + is the on/off switch — there is deliberately no
// separate Frontlight TILE duplicating it. Pure 1-bit: no dithered fills, a
// tile whose setting is on reads as a filled tile. The grabber sits along the
// panel's bottom edge, the edge the sheet is dragged from.
//
// The sheet lays out in whatever frame it opens over and never turns the
// renderer itself (an earlier pass forced portrait for as long as it was up).
// That means it has to fit both the 480x800 portrait frame and the X4 Pro's
// 800x480 landscape one, which is what computeLayout() below is for.
//
// This panel is also the recovery screen for a device whose touchscreen has
// been switched off — the Touch tile is the only way back on — so it has to be
// fully workable from buttons alone: a power tap opens it and a ~1 s power hold
// closes it, the navigation keys move a tile cursor, and Confirm (a hold on the
// Right side key on the X4 Pro, which MappedInputManager folds into Confirm
// outside a book) activates the focused tile.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // legitimately diverges from the saved SETTINGS.frontlightOn preference —
  // e.g. after a wake with frontlightRestoreOnWake off, the light stays off
  // live while the saved "was on" preference is deliberately kept (see
  // main.cpp's restoreLightOn). brightness/warmth have no such divergence
  // (always restored unconditionally on boot), so only lightOn needs a
  // touched-by-the-user flag: onExit() must not persist a mirror that never
  // reflected user intent in the first place.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  int panelBottom = 0;

  // Quick-setting tiles. The id is what runTile() dispatches on and what the
  // grid carries as its per-item value, so it stays stable while the grid ORDER
  // (below) and the set of visible tiles are free to change — hiding the
  // frontlight tile on a board with no light renumbers nothing.
  enum TileId : int16_t {
    TILE_NIGHT = 0,
    TILE_REFRESH = 1,
    TILE_ORIENTATION = 2,
    TILE_TOUCH = 3,
    TILE_SETTINGS = 4,
    TILE_HOME = 5,
    TILE_SLEEP = 6,
    TILE_USB_DRIVE = 7,
  };
  static constexpr int kMaxTiles = 8;
  // Grid order, filled by buildTileOrder() in onEnter(): the visible subset of
  // the ids above, in the order they are laid out.
  int16_t tileIds[kMaxTiles] = {};
  int tileCount = 0;
  // Index into tileIds of the button cursor, or -1 while nothing is focused.
  // Only meaningful (and only drawn) while the touchscreen is switched off.
  int focusedTile = -1;

  // --- Live layout, sized against the frame the sheet opened in ----------------
  // Filled by computeLayout(); computePanelBottom() and buildPanelScreen() both
  // read these rather than the constants they start from.
  uint8_t tileCols = 2;
  int16_t tileHeight = 0;
  int16_t sliderRowHeight = 0;

  // fui::SliderRowProps and fui::TileGridProps embed a 324-byte fui::StyleSet,
  // so the props the render path fills in live here instead of on the stack
  // (AGENTS.md: locals stay under 256 bytes). The components take them by
  // const reference and draw immediately, so one instance per call site is
  // enough — every field either is reassigned on each use or keeps its
  // constructed default.
  freeink::ui::SliderRowProps rowProps;
  freeink::ui::TileGridProps gridProps;
  freeink::ui::TileGridItem gridItems[kMaxTiles];

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // One slider row: a caption line (name + live percentage) above
  // [-] [draggable 1-bit capsule] [+], plus a lamp on/off button after the +
  // when showToggle is set (the brightness row).
  void addSliderRow(UiScreen& screen, const char* label, uint8_t value, freeink::ui::ActionId sliderAction,
                    freeink::ui::ActionId stepAction, bool showToggle);
  int computePanelBottom() const;
  // Picks the tile column count and the two finger-sized band heights so the
  // whole sheet fits the current frame. A wide, short frame (the X4 Pro's
  // native 800x480) needs more columns and shorter bands than a portrait one,
  // and a sheet taller than the screen silently loses its bottom row of tiles.
  void computeLayout();
  // Fills tileIds/tileCount with the tiles this board actually shows.
  void buildTileOrder();
  // Fills gridProps.styles in place. Written field by field rather than through
  // fui::tileGridStyles() so no 324-byte StyleSet ever lands on the stack
  // (AGENTS.md: locals stay under 256 bytes) — gridProps is already a member for
  // exactly that reason.
  void applyTileStyles(uint8_t radius);
  // True while the panel has to be driven from buttons: the glass is off, so
  // the navigation keys move the tile cursor instead of stepping brightness.
  // The light is still reachable then — a power-button DOUBLE TAP toggles it
  // from anywhere, which is what replaced the Frontlight tile.
  bool buttonNavActive() const;
  void moveFocus(int delta);
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void runTile(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on every way out.
  void persistLightSettings();
  void close();

  // One-shot: a tile that rewrote the whole frame (night mode) re-drives it
  // with the ghost-cleanup waveform on the next render. The "refresh" tile does
  // not use this — it closes the panel and promotes the repaint underneath
  // instead (GfxRenderer::promoteNextRefresh).
  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
