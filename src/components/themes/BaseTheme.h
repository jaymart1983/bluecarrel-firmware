#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  // FreeInkUI list shape, consumed by uiThemeTokens() for screens rendered
  // through FreeInkApp: the theme supplies geometry and selection style, the
  // uiScale fonts supply the sizes. Plain data by design — the eventual
  // SD-card theme files will provide exactly these values.
  int listRowGap;          // vertical gap between rows
  int listRowRadius;       // row corner radius (RoundedRaff cards, Lyra pill)
  int listInset;           // horizontal inset of the whole list band
  int listSidePadding;     // text inset within a row
  int listSelectionStyle;  // 0=invert fill, 1=light pill, 2=underline, 3=triangle (fui::SelectionStyle order)
  int listScrollWidth;     // scroll indicator thickness
  int listScrollSide;      // 0 = right edge, 1 = left edge
  bool listTitleBold;      // bold row titles (RoundedRaff)
  // FreeInkUI header shape, same contract as the list fields above.
  int headerSidePadding;    // title text inset
  int headerUnderlineSize;  // bottom rule thickness (Lyra), 0 = none
  int headerTitleAlign;     // 0 = left, 1 = center, 2 = right (fui::TextAlign order)
  int headerBatterySide;    // 0 = right edge, 1 = left edge
  // Battery in its own corner strip (batteryBarHeight tall) with the title on
  // the lower sub-band spanning the full width (Lyra), vs sharing the title
  // line with a width reserve (Classic, RoundedRaff).
  bool headerBatteryDetached;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;
  // Selected-tab pill fills its equal-width slot (legacy RoundedRaff tabs)
  // instead of shrinking to hug the label (legacy Lyra tabs).
  bool tabPillFullSlot = false;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionVPadding;
  int optionPopupDialogSideMargin;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;

  // FreeInkUI control shape (the control center panel), same contract as the
  // list fields above: quick-setting tiles and slider step buttons, the
  // sheet's free-edge corners, and the capsule slider's corners (255 = full
  // stadium, i.e. radius = half the control height).
  int controlRadius;
  int sheetRadius;
  int capsuleRadius;
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  Bookmark,
  Usb
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 0,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,  // invert fill
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 18,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 1,  // centered
                                 .headerBatterySide = 0,
                                 .headerBatteryDetached = false,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 0,
                                 .sheetRadius = 0,
                                 .capsuleRadius = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  static void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  // touchBack: on touch boards the four physical hints are replaced by a
  // single persistent on-screen Back chip. Pass false on screens where Back
  // has no meaning (the Home screen is the navigation root).
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4, bool touchBack = true) const;
  // --- Persistent on-screen Back chip (touch boards) --------------------------
  // The X4 Pro and the other touch boards have no physical Back key, so the
  // bottom hint band -- which those boards used to collapse to zero -- carries
  // a single Back chip instead. Drawing it publishes touchBackButtonVisible()
  // so MappedInputManager can route a tap in its rect to logical Back without
  // every activity growing its own hit test. ActivityManager clears the flag
  // before each render, so a screen that draws no hints (the reader page)
  // exposes no chip.
  static Rect touchBackButtonRect(const GfxRenderer& renderer);
  static Rect touchBackButtonHitRect(const GfxRenderer& renderer);
  static void drawTouchBackButton(const GfxRenderer& renderer);
  static bool touchBackButtonVisible();
  static void setTouchBackButtonVisible(bool visible);
  // Shared by every theme's drawButtonHints(): centres a hint label in its box,
  // wrapping to two lines rather than overflowing when it's too wide to fit.
  static void drawHintLabel(const GfxRenderer& renderer, int fontId, const char* label, int x, int boxWidth, int boxTop,
                            int boxHeight, int singleLineYOffset);
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  // Menu row height as DRAWN by drawButtonMenu. HomeActivity builds its touch
  // grid from this, so hit bands always match the visuals (RoundedRaff derives
  // its row height from the font, not the metrics table).
  virtual int getMenuRowHeight(const GfxRenderer& renderer) const;
  /// \p withBack draws "< Back |" at the left of the title and registers the
  /// touch target there, instead of the chip in the bottom band. The bottom chip
  /// is suppressed for the rest of the frame -- drawHeader always runs before
  /// drawButtonHints -- so a screen never shows two of them.
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr, bool withBack = false) const;
  /// True when this frame's header already drew Back. Reset by every drawHeader,
  /// so it describes this frame and not the last one.
  static bool backDrawnInHeader();
  /// The label used for Back in a title bar.
  static const char* backHeaderLabel();
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  /// \p rowCover, when supplied, returns a path to a 1-bit BMP to draw in place
  /// of the row's icon -- the book's cover, sent by the phone alongside the book.
  /// An empty path falls back to \p rowIcon, so a book with no cover still gets
  /// the generic mark rather than a hole in the list.
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon,
                              const std::function<std::string(int index)>& rowCover = nullptr,
                              int rowHeightOverride = 0,
                              const std::function<bool(int index)>& rowMarked = nullptr,
                              const std::function<float(int index)>& rowPercent = nullptr) const;
  /// Draws a cover BMP into the box, or an empty frame when it cannot be read.
  static void drawRowCover(const GfxRenderer& renderer, const std::string& path, int x, int y, int w, int h);
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  // --- Reader top status band -------------------------------------------------
  // The reader's chrome, moved from the foot of the page to the head of it:
  // time at the left, chapter and percent-of-book centred, battery at the
  // right, and a full-width rule separating the band from the text.
  //
  // Deliberately NOT drawStatusBar with a flag. That function is driven by
  // CrossPointSettings::statusBarSpec() -- clock side, progress bar mode,
  // page-count lane, bookmark pip -- all of which are bottom-bar concepts, and
  // threading a top mode through them would leave every combination to be
  // reasoned about on a screen that only ever wants this one. The page count
  // ("12/37") is gone with them: it measures the chapter, not the book, and it
  // moves when the font size does.
  // Gap between the status strip and the rule under it.
  static constexpr int kReaderTopBarRuleGap = 6;
  // Total height the reader must keep clear at the top. Metrics-derived rather
  // than a fixed number, so the band matches the header on any theme.
  static int readerTopBarHeight();
  static void drawReaderTopBar(const GfxRenderer& renderer, const std::string& chapter, float bookProgressPercent);
  static void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                            std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                            const bool fillMargin = true, const bool isPageBookmarked = false,
                            const bool pageCountEstimated = false);
  static void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label);
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);

  /// A header status word ("BLE", "USB"), struck through when \p slashed.
  ///
  /// Text rather than a drawn icon, in the same face as the battery percent it
  /// sits beside, so the two share a baseline by construction instead of being
  /// aligned by hand. \p centerY is the optical centre to sit on.
  static void drawStatusLabel(const GfxRenderer& renderer, int x, int centerY, const char* text, bool slashed);
};
