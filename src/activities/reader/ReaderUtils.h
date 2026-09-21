#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <components/bars/tap-zones.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

enum ReaderTouchAction : freeink::ui::ActionId {
  READER_TOUCH_PREV = 1,
  READER_TOUCH_NEXT = 3,
};

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

// The frame every screen that is NOT a reader page lays out in: home, the file
// browser, settings, the control centre, the sleep screens. Portrait on every
// board that has a portrait mode; the panel's native landscape on the X4 Pro,
// which has none (CrossPointSettings::ORIENTATION_CHOICES). Reader activities
// call it on the way out so the screen underneath is put back in the UI frame.
inline void applyUiOrientation(GfxRenderer& renderer) {
  applyOrientation(renderer, CrossPointSettings::UI_ORIENTATION);
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const auto pageButtonTriggered = [&](const MappedInputManager::Button button) {
    if (usePress) return input.wasPressed(button);
    return input.wasLongPressed(button, SKIP_HOLD_MS) || input.wasReleased(button);
  };
  const bool prev =
      tiltPrev || (pageButtonTriggered(MappedInputManager::Button::PageBack) || pageButtonTriggered(prevButton));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = tiltNext || pageButtonTriggered(MappedInputManager::Button::PageForward) || powerTurn ||
                    pageButtonTriggered(nextButton);
  return {prev, next, tiltPrev || tiltNext};
}

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

inline TouchPageTurn detectTouchPageTurn(const GfxRenderer& renderer, const MappedInputManager& input) {
  TouchPageTurn result{false, false, 0};
  // Page-turn taps are switched off only by the global touchscreen switch
  // (Action Centre): wasScreenTapped() and wasSwipe() report nothing while it is
  // off. touchReaderControls does not gate them, because it is not reachable on
  // every device; it only selects Swipe mode and which side goes back (Inverted
  // Tap).
  if (!input.hasTouch()) {
    return result;
  }

  if (SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE) {
    // Horizontal swipes turn pages. A slow swipe never becomes a long-press
    // chapter skip. Swipe mode takes the page-turn taps below too.
    const auto dir = input.wasSwipe();
    if (dir == MappedInputManager::SwipeDir::Left) {
      result.next = true;
      return result;
    }
    if (dir == MappedInputManager::SwipeDir::Right) {
      result.prev = true;
      return result;
    }
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }

  // Page-turn taps fill the middle half of the page: the left third turns back and
  // the right two thirds turn forward, so the common tap has the larger target.
  // Inverted Tap mirrors the split, keeping forward on the larger side. The top
  // quarter holds the status bar (its centre opens the Control Centre, handled in
  // ActivityManager) and the bottom quarter opens the reader menu (isTouchMenuTap).
  // Logical coordinates, so the zones follow the reading orientation.
  const int16_t width = static_cast<int16_t>(renderer.getScreenWidth());
  const int16_t height = static_cast<int16_t>(renderer.getScreenHeight());
  const int16_t bandTop = height / 4;
  const int16_t bandHeight = static_cast<int16_t>(height - 2 * bandTop);
  const int16_t third = width / 3;
  const int16_t twoThirds = static_cast<int16_t>(width - third);
  const bool inverted = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
  const freeink::ui::TapZone zones[] = {
      {freeink::ui::Rect{0, bandTop, inverted ? twoThirds : third, bandHeight},
       inverted ? READER_TOUCH_NEXT : READER_TOUCH_PREV},
      {freeink::ui::Rect{inverted ? twoThirds : third, bandTop, inverted ? third : twoThirds, bandHeight},
       inverted ? READER_TOUCH_PREV : READER_TOUCH_NEXT},
  };

  for (const auto& zone : zones) {
    if (!zone.enabled || !zone.rect.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) continue;
    result.prev = zone.action == READER_TOUCH_PREV;
    result.next = zone.action == READER_TOUCH_NEXT;
    break;
  }
  result.heldMs = gpio.lastTouchHeldMs();
  return result;
}

// Tap in the bottom quarter of the screen: the tap path into the reader menu on
// every touch board. Page-turn taps take the whole middle half
// (detectTouchPageTurn), so the menu lives below them, across the full width.
// The Off/Swipe Up alternatives are only surfaced on home-key boards
// (SettingsList), where the menu stays reachable through the key's long-press
// function.
inline bool isTouchMenuTap(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  if (SETTINGS.showReaderMenu != CrossPointSettings::READER_MENU_TAP) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int height = renderer.getScreenHeight();
  // Same boundary as the bottom of the page-turn band in detectTouchPageTurn.
  return y >= height - height / 4;
}

// Reader menu opens on the menu edge-swipe or a bottom-quarter tap. On home-key
// boards a long press of the capacitive key runs the user-selected long-press
// function instead (SETTINGS.longPressMenuFunction), not the menu.
// Menu gestures honor showReaderMenu independently of touchReaderControls,
// which only selects the page-turn gesture style in detectTouchPageTurn().
inline bool isTouchMenuGesture(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  if (input.wasMenuGesture()) return true;
  // Bottom-edge up-swipe variant: only selectable on home-key boards, where
  // Home is the capacitive key and the bottom edge is otherwise unused.
  if (SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_SWIPE_UP && input.wasReaderMenuSwipeUp()) {
    return true;
  }
  return isTouchMenuTap(renderer, input);
}

// Display a reader page. Page turns always use the FAST waveform; there is no
// page-count schedule of clean refreshes. A clean refresh happens only on
// request -- the Action Centre Refresh tile (GfxRenderer::promoteNextRefresh) or
// a Force Refresh power-button press (ReaderActivity::handleForcedRefresh) --
// or, if ghost cleanup is enabled, when GfxRenderer's change budget decides the
// panel needs it.
//
// Blocking or deferred: the async form starts the refresh and returns so the
// caller can overlap CPU work with the panel's refresh time. Async callers must
// not touch the framebuffer until renderer.waitRefreshComplete() and must
// rebuild the differential baseline before the next page turn (the tiled
// grayscale cleanup does).
inline void displayReaderPage(const GfxRenderer& renderer, bool async = false) {
  if (async) {
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Display the B/W base of a page whose grayscale pass follows. Panels that
// combine the base (Paper Mono) defer the activation so base + gray planes go
// out as one waveform — displaying the base separately makes the gray pass
// re-drive the whole text body (a visible flash). Other panels display
// normally.
inline void displayReaderPageBase(const GfxRenderer& renderer) {
  if (!renderer.combinesGrayscaleBase()) {
    displayReaderPage(renderer);
    return;
  }
  renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    // A combined-base panel may still hold a deferred B/W activation; flush it
    // so the page reaches the panel even without its grays.
    if (renderer.combinesGrayscaleBase()) renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
// Long press (>= GO_BACK_OR_HOME_MS):
// - default: go to file browser
// - with backShortToFileBrowser: go home
// Short press (< GO_BACK_OR_HOME_MS):
// - default: go home
// - with backShortToFileBrowser: go to file browser.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath, BackNavCallback goHome) {
  // The reading surface deliberately has no left-edge swipe-to-exit path: in
  // swipe page-turn mode a right swipe must page back instead. Home remains
  // available through the board's dedicated Home gesture/key. Back swipes stay
  // available in menus and other activities; only this reader-surface handler
  // ignores them. Physical Back buttons are unaffected: isPressed() is
  // button-only, and this guard skips just the gesture's own release frame.
  if (mappedInput.wasBackGesture()) {
    return false;
  }

  const bool backTriggered = mappedInput.wasLongPressed(MappedInputManager::Button::Back, GO_BACK_OR_HOME_MS) ||
                             mappedInput.wasReleased(MappedInputManager::Button::Back);
  if (!backTriggered) return false;

  const bool longPress = mappedInput.getHeldTime() >= GO_BACK_OR_HOME_MS;
  if (longPress != SETTINGS.backShortToFileBrowser) {
    activityManager.goToFileBrowser(filePath);
  } else {
    goHome.fn(goHome.ctx);
  }
  return true;
}

}  // namespace ReaderUtils
