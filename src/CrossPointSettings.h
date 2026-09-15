#pragma once
#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <Epub/ReaderRenderSpec.h>
#include <PersistableStore.h>

#include <cstdint>

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    TRANSPARENT_CUSTOM = 7,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // --- Orientations this build offers -------------------------------------------
  // A FILTER over ORIENTATION, never a renumbering: the persisted indices above
  // are untouched, so a settings.json written by a build that offered portrait
  // still loads and simply folds through normalizeOrientation() on the way in.
  //
  // The X4 Pro is used in the panel's native 800x480 landscape and portrait is
  // not reachable on it at all -- not merely defaulted away from -- so its list
  // is the two landscape modes, i.e. a 180-degree flip, with the native scan
  // first. Every other board keeps all four. ORIENTATION_CHOICES[0] is also the
  // frame every non-reader screen lays out in (UI_ORIENTATION below), so on a
  // board with no portrait the whole UI follows the reader into landscape
  // rather than flipping the frame back and forth around it.
  //
  // X4 Pro is portrait, full stop. The GT911 is mounted portrait (reports
  // X:0..480, Y:0..800) and every non-reader screen is laid out portrait.
  // Landscape was tried on hardware and rejected. There is no 180 flip either:
  // the page buttons are physically placed for one way up, so inverting them
  // is not a usable mode. One choice means every orientation control is dead
  // weight, so they are compiled out rather than left showing a single option.
#if FREEINK_DEVICE_X4PRO
  static constexpr uint8_t ORIENTATION_CHOICES[] = {PORTRAIT};
#else
  static constexpr uint8_t ORIENTATION_CHOICES[] = {PORTRAIT, LANDSCAPE_CW, INVERTED, LANDSCAPE_CCW};
#endif
  static constexpr uint8_t ORIENTATION_CHOICE_COUNT = static_cast<uint8_t>(sizeof(ORIENTATION_CHOICES));
  // True while all four modes are offered — i.e. while portrait exists at all.
  // Guards the places that still present a four-way orientation picker.
  static constexpr bool hasPortraitOrientation() { return ORIENTATION_CHOICE_COUNT == ORIENTATION_COUNT; }
  // The frame home, the file browser, settings, the control centre and the
  // sleep screens lay out in.
  static constexpr uint8_t UI_ORIENTATION = ORIENTATION_CHOICES[0];

  static constexpr uint8_t orientationChoiceIndex(const uint8_t value) {
    for (uint8_t i = 0; i < ORIENTATION_CHOICE_COUNT; ++i) {
      if (ORIENTATION_CHOICES[i] == value) return i;
    }
    return 0;
  }
  // Fold any stored or incoming orientation onto one this build offers.
  static constexpr uint8_t normalizeOrientation(const uint8_t value) {
    return ORIENTATION_CHOICES[orientationChoiceIndex(value)];
  }
  static constexpr uint8_t cycleOrientation(const uint8_t value, const int delta) {
    const int count = ORIENTATION_CHOICE_COUNT;
    return ORIENTATION_CHOICES[((orientationChoiceIndex(value) + delta) % count + count) % count];
  }

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTONS_DISABLED = 2, SIDE_BUTTON_LAYOUT_COUNT };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { NOTOSERIF = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader font size is a point size, not an enum slot — see fontPointSize.
  // Legacy 1.4-and-earlier files stored a 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE
  // slot; fromJson() folds that range up (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, EXTRA_WIDE = 3, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes).
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_FREQUENCY_COUNT
  };

  // Ghost cleanup: how much of the screen may change through fast differential
  // updates before the next one is promoted to a clean waveform. Counts every
  // screen update (reader pages, menus, popups), not page turns. Persisted by
  // index; append only.
  enum GHOST_CLEANUP {
    GHOST_CLEANUP_OFF = 0,
    GHOST_CLEANUP_LIGHT = 1,
    GHOST_CLEANUP_NORMAL = 2,
    GHOST_CLEANUP_AGGRESSIVE = 3,
    GHOST_CLEANUP_COUNT
  };

  // Short power button press actions. Persisted by index, so a new action MUST
  // be appended at the END here and at the end of the matching enumValues array
  // in SettingsList.h -- otherwise stored indices shift under existing saves.
  // The LONG press is not part of this enum: it always sleeps the device.
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    FOOTNOTES = 4,
    PWR_CONFIRM = 5,
    // Opens the control centre. The X4 Pro's default: with Select moved onto a
    // side-key hold, the power tap is free to carry the one action that has to
    // reach every screen, the reader page included.
    PWR_CONTROL_CENTER = 6,
    SHORT_PWRBTN_COUNT
  };

  // --- Power-button gesture timing -------------------------------------------
  // Boards that answer usesPowerGestures() carry three power-button gestures,
  // told apart only by timing. The power button is the menu key, everywhere,
  // the reader page included:
  //
  //   tap    (release <= POWER_TAP_MAX_HOLD_MS)    the short-click action,
  //                                                Control Centre by default
  //   double tap (two taps <= POWER_DOUBLE_TAP_MS  frontlight on/off
  //               apart)
  //   hold   (POWER_MENU_HOLD_MS while STILL DOWN) close the control centre
  //
  // WHY THE TAP IS PARKED. The double tap is back, so a tap can no longer be
  // dispatched on its own release -- it might turn out to be the first half of
  // a double tap. It is parked and published only once the double-tap window
  // closes, which costs up to POWER_DOUBLE_TAP_MS of latency.
  //
  // That cost was rejected twice before, when the tap was Select (686f90d) and
  // when it was a page turn: those are the two actions a reader performs
  // hundreds of times an hour, on a rhythm, and 300 ms of dead air on every one
  // of them is felt as the device being slow. It is accepted HERE because the
  // action the tap now carries is different in kind -- opening the control
  // centre is an occasional, deliberate act, and the panel it opens costs a
  // ~487 ms full panel render anyway, so the parking window is spent inside a
  // transition the user is already waiting on rather than in front of one they
  // are not. The window is kept at the short end of the usual 250-350 ms range
  // for the same reason it was before: it is pure cost.
  //
  // WHY THE HOLD FIRES WHILE THE BUTTON IS STILL DOWN. It used to fire on the
  // RELEASE at 1000 ms, which is the worst possible place for it: the user held
  // for a second, let go, and only then did a ~1.3 s full panel refresh start,
  // so closing the control centre cost well over two seconds and read as a
  // device that had not noticed the press at all. The threshold is now a
  // deadline, not a measurement of a finished press -- main.cpp's decoder
  // publishes the close the moment POWER_MENU_HOLD_MS elapses with the button
  // still down, exactly as the side keys already do for Back and Select
  // (SIDE_LONG_PRESS_MS, MappedInputManager::updateSideKeyGestures). The refresh
  // therefore starts under the user's finger and is most of the way through by
  // the time they let go. The later release is then swallowed whole: it must
  // never also publish a tap, or the panel would close and immediately reopen.
  //
  // THE VALUES. The hold floor drops 1000 -> 600 ms and the tap ceiling 700 ->
  // 350 ms.
  //
  //   600 ms hold floor: the same number the side keys use for Back and Select
  //   (SIDE_LONG_PRESS_MS), so every "hold" on this device means the same
  //   duration, and one already validated as long enough that a sloppy tap does
  //   not reach it. Below ~500 ms a deliberate press starts tripping the hold.
  //
  //   350 ms tap ceiling: the ordinary tap/hold boundary, and above the ~200 ms
  //   a relaxed press actually takes on a stiff side-mounted button. It is also
  //   about POWER_DOUBLE_TAP_MS, the window each half of a double tap has to fit
  //   in anyway -- a tap much slower than the double-tap window could not have
  //   been half of one.
  //
  //   It is a SEPARATE constant from POWER_CLICK_MAX_HOLD_MS rather than a lower
  //   value for it. That one is still the ceiling every other touch board applies
  //   to a raw power release (MappedInputManager::wasPowerShortClick), and those
  //   boards have no double tap, no hold-to-close and no dead band -- their
  //   700 ms is not this 350 ms and tightening it there would be an unrelated
  //   behaviour change on hardware this work never touched.
  //
  //   The 350-600 ms dead band is the deliberate part, and it is why the ceiling
  //   was not simply raised to meet the floor. A release in there does nothing
  //   at all: it is where a slow tap and a short hold cannot be told apart, and
  //   the wrong guess is expensive in both directions (a stray open costs a
  //   ~487 ms panel render, a stray close throws away the panel the user just
  //   opened). 250 ms of dead band is wide enough that a sloppy tap has to be
  //   nearly twice its normal length before it can close the menu, and it can
  //   never close it by accident: past 600 ms the close has already fired under
  //   the finger, so the band's upper edge is unreachable by a tap that is still
  //   trying to be a tap.
  // Gesture boards only: the tap ceiling and the hold floor of the decoder in
  // main.cpp.
  static constexpr unsigned long POWER_TAP_MAX_HOLD_MS = 350;
  static constexpr unsigned long POWER_MENU_HOLD_MS = 600;
  static constexpr unsigned long POWER_DOUBLE_TAP_MS = 300;
  // Every other touch board: the longest raw power release that still counts as
  // a short click. Untouched.
  static constexpr unsigned long POWER_CLICK_MAX_HOLD_MS = 700;

  // --- Side-key long press -----------------------------------------------------
  // Out of a book only (see MappedInputManager::setInBookContext): hold Left for
  // Back, hold Right for Select. 600 ms rather than the usual 500: a sloppy tap
  // on a list row is common, and a stray Back or Select costs the user more than
  // having to hold a little longer.
  static constexpr unsigned long SIDE_LONG_PRESS_MS = 600;

  // True on boards whose whole physical input surface is the power button and
  // the two side keys: no Back key, no Confirm key, no d-pad. The X4 Pro. Those
  // boards run the power-gesture scheme above AND the out-of-book side-key long
  // presses, because between them that is the only place Back and Select can
  // live. Boards with real Back/Confirm keys keep the historical
  // short-press/long-press pair untouched.
  //
  // Sleep is not one of these gestures: it lives on the control centre's Sleep
  // tile, which is what leaves the whole hold range above 1 s free.
  static bool usesPowerGestures() { return BoardConfig::isX4Pro(); }

  // Long-press Confirm action while reading an EPUB. The setting cycles through these values.
  // Persisted in settings.json by index: any new function (e.g. dictionary, bookmark) MUST use a
  // value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the
  // stored indices shift and existing saves are silently misinterpreted.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LP_MENU_READER_MENU = 4,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // How Select opens the reader menu: the classic full-screen list, or a toolbar
  // overlay (top/bottom bars with Contents / Text / More bottom-sheet panels)
  // painted over the page.
  enum READER_MENU_STYLE { READER_MENU_LIST = 0, READER_MENU_TOOLBAR = 1, READER_MENU_STYLE_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  enum TOUCH_READER_CONTROLS {
    TOUCH_READER_OFF = 0,
    TOUCH_READER_ON = 1,
    TOUCH_READER_SWIPE = 2,
    TOUCH_READER_INVERTED_TAP = 3,
    TOUCH_READER_CONTROLS_COUNT
  };

  // How the reader menu opens on touch boards. Persisted under the legacy
  // "tapForReaderMenu" key: 0/1 keep their old Off/Tap meaning.
  enum SHOW_READER_MENU { READER_MENU_OFF = 0, READER_MENU_TAP = 1, READER_MENU_SWIPE_UP = 2, SHOW_READER_MENU_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Night mode: inverted output polarity, applied to every activity per
  // render by ActivityManager. The sleep screen opts out itself.
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // Short power button click behaviour. On the X4 Pro a single tap opens the
  // control centre: Select has moved to a Right side-key hold, so the power tap
  // is free for the one action that has to be reachable from every screen
  // including the reader page. Compile-time gated, not BoardConfig-gated: this
  // is a default member initialiser on a global, so it must not depend on
  // another global's initialisation order.
  uint8_t shortPwrBtn = FREEINK_DEVICE_X4PRO ? PWR_CONTROL_CENTER : IGNORE;
  // --- Settings-file revision ---------------------------------------------------
  // A DEFAULT ONLY CHANGES WHAT AN ABSENT KEY MEANS. Every field above is
  // written to settings.json on every save, so once a device has saved once, the
  // stored value wins over any later change to the initialiser -- which is
  // exactly how a device that ran an older build ended up with a power button
  // that did not open the control centre (see the migration in fromJson).
  // Changing a default that shipped therefore needs a migration keyed on this
  // revision, not just a new initialiser. Bump it when you add one; absent in
  // the file means 0, i.e. everything written before revisions existed.
  static constexpr uint8_t SETTINGS_REV = 2;
  // Not in SettingsList and not user-visible: read and written by hand in
  // fromJson()/toJson() alongside the other manually-persisted fields.
  uint8_t settingsRev = 0;
  // EPUB reading orientation settings
  // 0 = portrait, 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise.
  // The default is the first orientation this build offers (see
  // ORIENTATION_CHOICES): portrait everywhere except the X4 Pro, which has none.
  uint8_t orientation = ORIENTATION_CHOICES[0];
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = NOTOSERIF;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // Retained only for settings-file compatibility. Reader page turns have no
  // refresh cadence, and nothing reads or writes this field.
  uint8_t refreshFrequency = REFRESH_15;
  // Change-accumulation ghost cleanup (see GHOST_CLEANUP)
  // Off on the X4 Pro: its anti-ghosting holds up without scheduled scrubs, and the
  // scrub is a full-screen flash every few pages. Refresh is on demand there (Action
  // Centre Refresh tile, or a long press on its Home tile). See rev 2 in fromJson.
  uint8_t ghostCleanup = FREEINK_DEVICE_X4PRO ? GHOST_CLEANUP_OFF : GHOST_CLEANUP_NORMAL;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // What this reader is called: its Bluetooth advertising and GAP name, and the
  // label the phone app shows. Empty means DEFAULT_DEVICE_NAME. Edited on the X4
  // Pro's Settings page (BlePairingActivity) and from the app; category-less in
  // SettingsList, so SettingsActivity does not list it.
  static constexpr const char* DEFAULT_DEVICE_NAME = "Bluecarrel";
  static constexpr size_t DEVICE_NAME_MAX_BYTES = 16;
  char deviceName[DEVICE_NAME_MAX_BYTES + 1] = "";
  // The one rule for every way a name comes in: trims ASCII whitespace from both
  // ends, then accepts at most DEVICE_NAME_MAX_BYTES of printable ASCII
  // (0x20-0x7E), which an advertising packet carries unmangled. Writes the result
  // to `out` (outSize > DEVICE_NAME_MAX_BYTES) and returns true; false writes
  // nothing. An empty result is valid and means the default.
  static bool normalizeDeviceName(const char* in, char* out, size_t outSize);
  const char* effectiveDeviceName() const { return deviceName[0] != '\0' ? deviceName : DEFAULT_DEVICE_NAME; }
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Firmware updates: 1 = install a verified update at the next sleep without
  // asking; 0 = offer Update now / Later / Cancel the moment it is ready.
  uint8_t autoInstallFirmware = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // Toolbar, not the full-screen list. This board is touch-first, and the
  // toolbar is the chrome built for it: a bottom sheet painted OVER the page
  // that is already on screen, so the text stays visible behind it and opening
  // it costs one refresh instead of a whole screen change. usesToolbarMenu()
  // still forces the list on button-only boards, where a sheet with no cursor
  // would be unusable.
  uint8_t readerMenuStyle = READER_MENU_TOOLBAR;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_SWIPE;
  // Reader menu open gesture (SHOW_READER_MENU: off / center tap / bottom-edge
  // up-swipe). Only surfaced on home-key boards, where Home is the capacitive
  // key and the bottom edge is free; elsewhere it stays at the Tap default.
  uint8_t showReaderMenu = READER_MENU_TAP;
  // Frontlight quick-panel state. Category-less SettingsList entries persist
  // these without adding them to the regular Settings screen.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // Restore the saved on/off state after a normal boot or wake. Brightness and
  // warmth are always remembered even when this is disabled.
  uint8_t frontlightRestoreOnWake = 1;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Keyboard layouts the user can reach, using keyboard_layouts::ALL table bits.
  // 0 means "not configured", resolved to the UI language's layout plus English.
  // Any other value is an explicit choice and is used as-is: the language of the
  // books someone reads is not necessarily the language of their UI.
  // See keyboard_layouts:: for the bit assignment and the defaulting rules.
  uint16_t keyboardLayouts = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  // How long the power button must be held before the device sleeps.
  //
  // SLEEP mode means "a short click sleeps", so the threshold drops to
  // effectively nothing. Otherwise the historical 400 ms applies. Boards on the
  // gesture scheme do not reach this at all -- their power hold closes the
  // control centre, and main.cpp skips the hold-to-sleep path for them entirely
  // unless the user has explicitly bound the power button to Sleep.
  uint16_t getPowerButtonDuration() const {
    if (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) return 10;
    return 400;
  }
  int getReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    uint8_t clockUtcOffsetQ = 48;             // 48 = UTC+0
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  // Change budget for GfxRenderer::setChangeBudgetPercent(): percent of the
  // panel that may flip through fast updates before one is promoted to a clean
  // waveform. 0 = off.
  uint16_t getGhostCleanupPercent() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
