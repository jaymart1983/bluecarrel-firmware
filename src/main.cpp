#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#if FREEINK_CAP_NETWORK
#include <WiFi.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif
#endif

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceSleep.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/FirmwareReadyActivity.h"
#include "activities/settings/BlePairPromptActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#if FREEINK_CAP_NETWORK
#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#endif
#include "images/LoadingIcon.h"
#include "network/FirmwareStaging.h"
#include "network/FirmwareWatcher.h"
#include "platform/UsbSerialJtagHandoff.h"
#if FREEINK_CAP_BLE_TRANSFER
#include "network/BleLink.h"
#endif
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
// Set by the control centre's Sleep tile; consumed by loop(). Deferred rather
// than slept on the spot because enterDeepSleep() replaces the whole activity
// stack, which would destroy the panel while its own event handler is running.
static bool deviceSleepRequested = false;

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SLEEP = 2;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;
// setup() came up from an update installed on the way to sleep and must go
// straight back to sleep (SILENT_REBOOT_TARGET_SLEEP).
bool sleepAfterUpdateBoot = false;
// Set when enterDeepSleep() hands over to an install instead of sleeping. Every
// sleep path runs again on the next loop, before the activity manager has even
// entered the update screen, so sleep is held off while it gets going. Bounded,
// so an image that fails validation cannot keep the reader awake for good.
unsigned long installAtSleepStartedMs = 0;
constexpr unsigned long INSTALL_AT_SLEEP_GRACE_MS = 3UL * 60UL * 1000UL;

#if FREEINK_CAP_NETWORK && FREEINK_CAP_TOUCH
static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;

  // A software reset does not cycle externally powered touch/frontlight rails.
  // Shut down the network stack in place so those peripherals retain state.
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
  return true;
}
#endif

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_NETWORK && FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_NETWORK && FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToSleepAfterUpdate() {
  silentRebootTarget = SILENT_REBOOT_TARGET_SLEEP;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Restart after update (target=sleep)");
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage handoff reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

#if FREEINK_CAP_TOUCH
// Flip the frontlight and persist the new on/off preference.
static void toggleFrontlightAndPersist() {
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s by power double tap", lightOn ? "on" : "off");
}

// Power-button gesture decoder for boards with no physical Back or Confirm key
// (see CrossPointSettings::usesPowerGestures). One button, three gestures, the
// same everywhere including the reader page:
//
//   tap          the configured short-click action: toggles the Control Centre by default
//   double tap   frontlight on/off
//   hold ~600 ms sleep, fired WHILE THE BUTTON IS DOWN
//
// THE PARKED TAP. The double tap is back, so a tap can no longer be dispatched
// on its own release: it might be the first half of one. It is parked at the
// release and only published once POWER_DOUBLE_TAP_MS has passed with no second
// tap, which costs up to ~300 ms of latency.
//
// That latency was rejected twice in this file's history -- once when the tap
// was Select, once when it was a page turn -- and it is accepted here, so the
// difference is worth stating plainly. Select and page turn are rhythm actions:
// performed hundreds of times an hour, one after another, with the user's
// attention on the result, and a fixed 300 ms of dead air on each one is
// experienced as the device being slow. Opening the control centre is neither
// frequent nor rhythmic, and the panel it opens costs a ~487 ms full render, so
// the window is spent inside a transition the user is already waiting through
// rather than in front of one they are not. The other two bindings a tap can
// carry (force refresh, footnotes) are occasional in the same way. PAGE_TURN
// remains available as a binding and does pay the window -- that is the user
// explicitly choosing it, and there is no way to have both the double tap and a
// zero-latency tap on one button.
//
// THE HOLD FIRES UNDER THE FINGER. The close used to be decided on the RELEASE:
// the user held for a second, lifted, and only then did the ~1.3 s full panel
// refresh begin, so the whole gesture cost over two seconds and felt like the
// device had missed the press. POWER_MENU_HOLD_MS is now a deadline instead --
// the moment it elapses with the button still down the close is published, the
// same shape the side keys already use for Back and Select (SIDE_LONG_PRESS_MS,
// MappedInputManager::updateSideKeyGestures). The refresh starts while the
// finger is still on the button and is largely done by the time it lifts.
//
// The release that follows a fired hold is then swallowed WHOLE: it must not
// park a tap, because publishing one a double-tap window later would reopen the
// panel the hold just closed. holdFired is what remembers that, and it is
// cleared on the release rather than on the next press so a press that is still
// down across many frames only ever fires once.
//
// A release in the inert band between the tap ceiling and the hold floor is
// discarded: a slow tap and a short hold cannot be told apart there, and doing
// nothing costs less than a wrong open-or-close. The band's upper edge is in
// practice unreachable -- past POWER_MENU_HOLD_MS the close has already fired --
// so the band only ever swallows genuinely ambiguous 350-600 ms presses.
//
// Returns true when the frame is fully consumed.
void requestDeviceSleep();  // defined below; the hold asks for sleep

static bool handlePowerGestures() {
  if (!CrossPointSettings::usesPowerGestures()) return false;

  // Release of the parked tap: 0 means nothing parked.
  static unsigned long parkedTapAt = 0;
  // The current press already published its close; its release is now scrap.
  static bool holdFired = false;

  if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
    const unsigned long held = gpio.getPowerButtonHeldTime();
    if (holdFired) {
      // The close went out under the finger. This release is the tail of that
      // same gesture and must publish nothing at all.
      holdFired = false;
      parkedTapAt = 0;
      return true;
    }
    if (held <= CrossPointSettings::POWER_TAP_MAX_HOLD_MS) {
      if (parkedTapAt != 0 && millis() - parkedTapAt <= CrossPointSettings::POWER_DOUBLE_TAP_MS) {
        parkedTapAt = 0;
        toggleFrontlightAndPersist();
        return true;  // the double tap is the whole gesture; nothing else sees it
      }
      parkedTapAt = millis();
      if (parkedTapAt == 0) parkedTapAt = 1;  // millis() rollover: 0 is the "nothing parked" sentinel
      return true;                            // parked, not yet published
    }
    // A press long enough to be ambiguous supersedes a tap still inside its
    // window: opening the menu and then closing it again would be a confusing
    // net-nothing.
    parkedTapAt = 0;
    return true;  // the inert band between the tap ceiling and the hold floor
  }

  // The hold, decided while the button is still down. Not during the wake hold:
  // that press started before the device was awake, its release is already
  // spoken for below, and firing on it would close the control centre the user
  // has not opened yet -- the old release-edge decoder could not reach this case
  // because the wake release returns before the decoder ever runs.
  if (!holdFired && !wakePowerReleasePending && gpio.isPressed(HalGPIO::BTN_POWER) &&
      !gpio.isPressed(HalGPIO::BTN_DOWN) &&  // power + Down is the screenshot combination
      gpio.getPowerButtonHeldTime() >= CrossPointSettings::POWER_MENU_HOLD_MS) {
    holdFired = true;
    // A hold supersedes a tap still parked from the press before it.
    parkedTapAt = 0;
    // The hold is Sleep. It used to close the control centre; a tap toggles the
    // centre now (ActivityManager), which frees the hold for what a power button
    // is expected to do when held. Requested rather than slept on the spot so the
    // sleep runs from its normal place at the top of the next loop.
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    requestDeviceSleep();
    return true;
  }

  if (parkedTapAt != 0 && millis() - parkedTapAt > CrossPointSettings::POWER_DOUBLE_TAP_MS) {
    // The window closed with no second tap, so it really was a single tap.
    parkedTapAt = 0;
    mappedInputManager.setPowerClickFrame(true);
  }
  return false;  // fall through so the short-click action runs from its normal place
}
#endif

// Requested by the control centre's Sleep tile (see DeviceSleep.h).
void requestDeviceSleep() { deviceSleepRequested = true; }

// The screen the reader wears for as long as the host holds the card.
//
// Full screen, not a popup: this is the device's whole state, not a message
// about it, and there is nothing underneath worth showing through. Drawn while
// the filesystem is STILL ATTACHED -- see the call site -- because every glyph
// here comes off the SD card.
static void drawUsbDriveNotice() {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int bodyLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int sideInset = 40;
  const int maxWidth = screenWidth - sideInset * 2;

  // Measured before anything is drawn so the whole block can be centred as one.
  const auto bodyLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_USB_DRIVE_MODE_BODY), maxWidth, 8);
  const auto exitLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_USB_DRIVE_MODE_EXIT), maxWidth, 4);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int blockHeight = titleLine + 24 + static_cast<int>(bodyLines.size()) * bodyLine;
  int y = (screenHeight - blockHeight) / 2 - 40;
  if (y < 60) y = 60;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_USB_DRIVE_MODE_TITLE), true, EpdFontFamily::BOLD);
  y += titleLine + 10;
  renderer.drawLine(screenWidth / 2 - 60, y, screenWidth / 2 + 60, y, true);
  y += 14;
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += bodyLine;
  }

  // The way out, pinned to the bottom where a hint band would be. This is the
  // only instruction that still matters once the card is gone.
  int exitY = screenHeight - 40 - static_cast<int>(exitLines.size()) * smallLine;
  for (const auto& line : exitLines) {
    renderer.drawCenteredText(SMALL_FONT_ID, exitY, line.c_str());
    exitY += smallLine;
  }

  // Push it. Drawing only fills the framebuffer -- without this the notice was
  // composed and then never sent to the panel, and the repaint that followed
  // put the previous screen back over it.
  //
  // FULL_REFRESH, the one place that still asks for the clean waveform: this
  // frame is going to sit on the glass for minutes with the card detached and
  // nothing able to redraw it, so it is worth one flash to put it up without
  // any ghost of the screen underneath.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

// Requested by the control centre's USB Drive tile (see DeviceSleep.h).
static bool usbDriveRequested = false;
void requestUsbDriveMode() { usbDriveRequested = true; }

bool usbDriveAvailable() {
#if FREEINK_CAP_USB_MSC
  return gpio.isUsbConnected();
#else
  return false;
#endif
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(const bool fromTimeout) {
  if (installAtSleepStartedMs != 0 && millis() - installAtSleepStartedMs < INSTALL_AT_SLEEP_GRACE_MS) return;
  // "Later" on the update prompt, or auto-install: the reader is being put down,
  // the one moment an install interrupts nobody. Install, reboot, and the reboot
  // comes straight back here (SILENT_REBOOT_TARGET_SLEEP).
  if (!sleepAfterUpdateBoot && FIRMWARE_WATCHER.installAtSleep()) {
    FIRMWARE_WATCHER.clearDeferral();
    APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
    APP_STATE.saveToFile();
    installAtSleepStartedMs = std::max(1UL, millis());
    LOG_INF("SLP", "Installing the deferred firmware update before sleeping");
    activityManager.goToFirmwareUpdate(firmware_staging::IMAGE_PATH, /*stagedDrop=*/true, /*autoConfirm=*/true,
                                       /*sleepAfter=*/true);
    return;
  }
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  // Kept from before the install on the post-update boot, or waking would land
  // on Home instead of the book the reader was put down in.
  if (!sleepAfterUpdateBoot) APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

#if FREEINK_CAP_NETWORK
  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
#endif

#if FREEINK_CAP_BLE_TRANSFER
  // Say goodnight before the radio goes down. The reader is a peripheral, so a
  // phone that is not told cannot distinguish "asleep" from "out of range" or
  // "crashed" -- it only ever sees the link drop. One notification turns that
  // ambiguity into a fact and makes the app's last-known state trustworthy
  // rather than merely stale.
  BLE_LINK.notifySleeping();

  // Before the SD card goes: end() writes nothing, but it does close files and it
  // must not be racing a mount teardown. Deep sleep would otherwise hold the
  // modem power domain alive for a device nobody is using.
  BLE_LINK.end();
#endif

  halTiltSensor.deepSleep();
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  // Every screen that is not a reader page lays out in the device's UI frame:
  // portrait on boards that have a portrait mode, the panel's native 800x480
  // landscape on the X4 Pro, which has none (CrossPointSettings::
  // ORIENTATION_CHOICES). Set here rather than only on the way out of a book,
  // so the very first paint is already in it.
  ReaderUtils::applyUiOrientation(renderer);
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_SLEEP) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    mappedInputManager.isPressed(recoveryButton);

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
#if FREEINK_CAP_NETWORK
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
#endif
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots preserve the live state so they do not unexpectedly go dark.
  const bool restoreLightOn = SETTINGS.frontlightOn != 0 && (SETTINGS.frontlightRestoreOnWake != 0 || isSilentReboot);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool needsWakeRefresh = false;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      } else {
        // The first Home/Reader paint is followed by an explicit clean refresh
        // because the panel still physically shows the sleep image.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SLEEP) {
    // Came up from an update installed on the way to sleep. Land somewhere
    // harmless and go back to sleep at the end of setup(); the wake after that
    // resumes whatever the reader was doing before the install.
    activityManager.goHome();
    sleepAfterUpdateBoot = true;
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;

#if FREEINK_CAP_BLE_TRANSFER
  // The radio belongs to the device being awake, not to any screen. Started here,
  // once, after the first paint so it is not competing with boot for the SD bus
  // and the display; stopped in enterDeepSleep(). Wake from deep sleep is a chip
  // reset, so this is also where waking turns it back on.
  //
  // Except in firmware recovery, where the whole device exists to write one
  // image to flash and nothing should be arriving over the air while it does.
  if (!recoveryFirmwareMode) BLE_LINK.begin();
#endif

  if (sleepAfterUpdateBoot) {
    LOG_INF("MAIN", "Firmware installed at sleep; going back to sleep");
    enterDeepSleep();
  }
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  // The side-key scheme is context-sensitive, so the input layer has to know
  // which context this frame is in BEFORE it classifies any edge. Inside a book
  // the side keys are page keys and nothing else; outside it they also carry
  // Back and Select on a hold (see MappedInputManager::setInBookContext).
  MappedInputManager::setInBookContext(activityManager.isReaderPageActive());
#if FREEINK_CAP_BLE_TRANSFER
  {
    // Tell the phone the moment a book opens or closes, rather than at the next
    // 60-second heartbeat: the app marks the open book in its library.
    static bool lastReaderOpen = false;
    const bool readerOpen = activityManager.isReaderActivity();
    if (readerOpen != lastReaderOpen) {
      lastReaderOpen = readerOpen;
      BLE_LINK.notePositionChanged();
    }
  }
#endif
  mappedInputManager.update();

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

#if FREEINK_CAP_BLE_TRANSFER
  // The link is pumped here rather than from an activity, because it outlives
  // every activity. Below the exclusive-storage return above on purpose: while
  // USB Drive owns the raw card there is no filesystem for a transfer to land on.
  BLE_LINK.tick();
#endif

  // Look for a firmware image in the drop folder. Idle cost is two existence
  // checks every thirty seconds; the hashing it may start is spread a few KB per
  // tick so nothing here blocks a page turn.
  FIRMWARE_WATCHER.tick();

  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);
  // Change-accumulation ghost cleanup. Cheap to re-assert every loop (a no-op
  // unless the user just changed it) and this is the one place every screen,
  // not just the reader, passes through.
  renderer.setChangeBudgetPercent(SETTINGS.getGhostCleanupPercent());

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  // Touch only counts as activity while the glass is switched on: with the
  // touchscreen off (a palm resting on it while reading is the reason to switch
  // it off) a contact must not keep the inactivity timer alive either.
  const bool touchActivity = MappedInputManager::isTouchInputEnabled() && gpio.wasTouchActivity();
  const bool linkBusy =
#if FREEINK_CAP_BLE_TRANSFER
      BLE_LINK.isBusy();
#else
      false;
#endif
  // A book arriving over Bluetooth while the reader sits on the home screen is
  // work, even though nobody is touching the device. Sleeping through it would
  // drop the transfer at whatever byte it had reached.
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || touchActivity || halTiltSensor.hadActivity() || linkBusy ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

#if FREEINK_CAP_TOUCH
  // The power-gesture events last exactly one frame: clear them, then let the
  // decoder republish whichever one this frame resolves.
  mappedInputManager.setPowerClickFrame(false);
  mappedInputManager.setPowerCloseFrame(false);
  if (handlePowerGestures()) return;
#endif

  // The control centre's Sleep tile, honoured here rather than in the panel so
  // the panel has already been popped by the time the stack is torn down.
  if (deviceSleepRequested) {
    deviceSleepRequested = false;
    enterDeepSleep();
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  // Boards on the gesture scheme spend their power hold on closing the control
  // centre, so holding the button must never sleep the device out from under it
  // -- Sleep is a control centre tile there. An explicit Sleep binding still
  // wins: that is the user
  // asking for the old behaviour back, and it takes the gesture scheme with it.
  const bool powerHoldSleeps = !CrossPointSettings::usesPowerGestures() ||
                               SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP;
  if (powerHoldSleeps && powerReleasedSinceWake && millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }

#if FREEINK_CAP_USB_MSC
  // USB Drive, with no screen of its own.
  //
  // It used to open UsbDriveActivity, and that is what was locking the device up.
  // beginUsbDrive() DETACHES the FAT filesystem to hand the host raw blocks, and
  // fonts are read from that filesystem (SdCardFont -> Storage.openFileForRead).
  // The activity painted "Waiting for host" *after* the detach, so any glyph not
  // already in the font cache became an SD read against a filesystem that no
  // longer existed -- an intermittent hard hang, depending on what happened to be
  // cached. The old exit path had the same defect: it drew a popup before
  // handing the pads back.
  //
  // So nothing is rendered once the card is gone. The header repaint below runs
  // while the filesystem is still mounted -- that is what puts "USB" in the top
  // bar -- and after that the device draws nothing at all until the host lets go.
  //
  // The latch is unchanged: gpio's edge detector starts from "disconnected", so
  // a device that booted with a cable in reports a plug edge that never happened,
  // and that device is almost always one being flashed.
  static bool usbDriveActive = false;
  static bool usbDriveHostSeen = false;
  static unsigned long usbDriveStartedAt = 0;
  // Matches the old UsbDriveActivity::AUTO_HOST_WAIT_TIMEOUT_MS. Long enough for
  // a real host to enumerate and mount, short enough that a charger cannot
  // strand the device.
  constexpr unsigned long USB_DRIVE_HOST_WAIT_MS = 20UL * 1000UL;

  if (usbDriveActive) {
    // No activity loop, no repaint, no storage call except the state poll: the
    // card belongs to the host until it says otherwise.
    const UsbDriveState driveState = Storage.usbDriveState();
    if (driveState == UsbDriveState::Connected) usbDriveHostSeen = true;
    // A wall charger and a charge-only cable both raise a plug edge, and neither
    // will ever enumerate. Without this the device sits with the card detached,
    // drawing nothing and answering nothing, until the reset pin -- which is
    // indistinguishable from a freeze, and was reported as one. The screen this
    // replaced had exactly this timeout on its automatic path; dropping it was a
    // regression, not a simplification.
    const bool hostNeverCame = !usbDriveHostSeen && millis() - usbDriveStartedAt >= USB_DRIVE_HOST_WAIT_MS;
    const bool hostFinished = driveState == UsbDriveState::Ejected || driveState == UsbDriveState::Disconnected ||
                              driveState == UsbDriveState::IoError;
    if (hostFinished || hostNeverCame || !gpio.isUsbConnected()) {
      if (hostNeverCame) LOG_INF("USB", "no host in %lu ms; taking the card back", USB_DRIVE_HOST_WAIT_MS);
      LOG_INF("USB", "host released the card; rebooting to the serial personality");
      Storage.endUsbDrive();
      Storage.setUsbDriveHandoffPending(false);
      // Deliberately not restartToHomeAfterStorageHandoff(): that draws a popup,
      // and the filesystem the fonts live on has only just come back. A reboot
      // shows the boot screen anyway.
      silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
      silentRebootMagic = SILENT_REBOOT_MAGIC;
      handoffUsbOtgToSerialJtag();
      ESP.restart();
    }
    // Any button takes the card back. The host ejecting is the normal way out,
    // but a reader that can only be freed by the computer it is plugged into
    // looks broken whenever that computer is not cooperating -- and with nothing
    // drawn and no input answered, "looks broken" is indistinguishable from is.
    if (mappedInputManager.wasPressed(MappedInputManager::Button::Back) ||
        mappedInputManager.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInputManager.wasPressed(MappedInputManager::Button::Up) ||
        mappedInputManager.wasPressed(MappedInputManager::Button::Down) ||
        mappedInputManager.wasPressed(MappedInputManager::Button::Power)) {
      LOG_INF("USB", "user ended USB Drive from the device");
      Storage.endUsbDrive();
      Storage.setUsbDriveHandoffPending(false);
      silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
      silentRebootMagic = SILENT_REBOOT_MAGIC;
      handoffUsbOtgToSerialJtag();
      ESP.restart();
    }
    powerManager.setPowerSaving(true);
    delay(50);
    return;
  }

  // A cable is not an instruction, and it is no longer a question either.
  //
  // Mounting on sight meant plugging in for power took the reader out of
  // service: the card goes to the host, the loop stops drawing and stops
  // answering input, and the Library cannot refresh while a book arrives over
  // BLE. Every "it locked up" and "it only refreshes when I unplug" traces back
  // to that. Prompting on sight was not much better -- an eight-second question
  // over whatever you were reading, usually gone before it was read.
  //
  // So plugging in now does nothing at all, and USB Drive is a tile in the
  // control centre that only appears while a cable is attached. The user asks
  // for it, at the moment they mean it, from a screen they opened themselves.
  if (usbDriveRequested) {
    usbDriveRequested = false;
    if (!gpio.isUsbConnected()) {
      LOG_INF("USB", "drive mode requested but the cable is gone; ignoring");
      activityManager.requestUpdate();
    } else {
      LOG_INF("USB", "user asked for USB Drive; mounting the card");

      // PAINT FIRST, DETACH SECOND. The fonts are read from the SD card, so
      // once beginUsbDrive() hands it to the host there is nothing left to draw
      // text with -- an earlier version painted after the handover and hung,
      // which is the original "USB locks it up". E-ink holds an image with no
      // power and no card, so a frame put up now simply stays up for as long as
      // the host has the volume.
      // Settle the render task BEFORE painting, not after. requestUpdateAndWait
      // repaints whatever activity is current -- so running it after the notice
      // was drawn simply painted over it. Its job here is to make sure no
      // render is in flight when the card goes away; once it returns, nothing
      // further is queued and the framebuffer is ours.
      Storage.setUsbDriveHandoffPending(true);
      activityManager.requestUpdateAndWait();
      drawUsbDriveNotice();
#if FREEINK_CAP_BLE_TRANSFER
      BLE_LINK.end();
#endif
      if (Storage.beginUsbDrive()) {
        usbDriveActive = true;
        usbDriveHostSeen = false;
        usbDriveStartedAt = millis();
      } else {
        LOG_ERR("USB", "could not hand the card to the host; carrying on normally");
#if FREEINK_CAP_BLE_TRANSFER
        BLE_LINK.begin();
#endif
        Storage.setUsbDriveHandoffPending(false);
        activityManager.requestUpdate();
      }
      return;
    }
  }

#endif

#if FREEINK_CAP_BLE_TRANSFER
  // A phone sent `pair` with the pairing window closed. Asked over whatever is on
  // screen, a book included, the way the firmware offer below is; BleLink refuses
  // the request itself if nobody answers within its deadline.
  if (!activityManager.preventAutoSleep() && BLE_LINK.takePairPromptRequest()) {
    activityManager.pushActivity(std::make_unique<BlePairPromptActivity>(renderer, mappedInputManager));
    return;
  }
#endif

  // A verified image is waiting. Offered at once, over whatever is on screen --
  // a book included, because "Later" makes the offer cost the reader nothing: it
  // installs when the reader next sleeps. With auto-install on there is no offer
  // at all, only the install at sleep.
  if (FIRMWARE_WATCHER.updateReady() && !activityManager.preventAutoSleep()) {
    FIRMWARE_WATCHER.standDown();
    if (SETTINGS.autoInstallFirmware) {
      LOG_INF("FWDROP", "auto-install on: installing at the next sleep");
      FIRMWARE_WATCHER.deferToSleep();
    } else {
      activityManager.pushActivity(std::make_unique<FirmwareReadyActivity>(renderer, mappedInputManager));
      return;
    }
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
#if FREEINK_CAP_BLE_TRANSFER
      BLE_LINK.waitForWork(50);
#else
      delay(50);
#endif
    } else {
      // Short delay to prevent tight loop while still being responsive
#if FREEINK_CAP_BLE_TRANSFER
      // Same sleep, but a queued control write (get_ack, commit, start_get) ends
      // it, so a reply is not held back until the delay runs out.
      BLE_LINK.waitForWork(10);
#else
      delay(10);
#endif
    }
  }
}
