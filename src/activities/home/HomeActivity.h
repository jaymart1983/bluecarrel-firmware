#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// The home screen is a shelf, not a menu of verbs.
//
// Layout, top to bottom:
//
//   the cover tile      the first `coverCount` books, drawn by the theme
//                       (BaseTheme::drawRecentBookCover). How many fit is the
//                       theme's call: 1 for Lyra and RoundedRaff, 3 for Lyra
//                       3-Covers.
//   book rows           every remaining book the menu band has room for
//   Store               Task A's app-backed Calibre browser
//   More                Browse Files / Recent Books / File Transfer / Settings
//
// The books come from HOME_SHELF, a cached ordering of the whole /Books tree:
// currently reading first (most recently read of those at the very top), then
// never-opened books newest first. It is rebuilt only when
// BookLibraryIndex::fingerprint says the card changed, because building it opens
// every book's metadata cache and that is seconds on a large shelf.
//
// WHERE THE OLD VERBS WENT. Browse Files, Recent Books, File Transfer and
// Settings are all one row away, under More. Demoted rather than deleted: on
// this board the control centre (a power tap, from any screen) already carries
// Settings and Home tiles, so nothing here is the only route to anything.
class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool coversLoading = false;
  bool coversLoaded = false;
  bool firstRenderDone = false;
  // The shelf has been reconciled with the card this visit. Deferred past the
  // first render so home paints from the cache immediately and pays for the
  // directory walk behind an already-useful screen.
  bool shelfChecked = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  // The books this screen draws, in shelf order. RecentBook rather than a type
  // of its own because the theme's cover tile takes exactly that.
  std::vector<RecentBook> homeBooks;
  // How many of homeBooks the theme's cover tile takes; the rest become menu
  // rows, as many as the band has room for.
  int coverCount = 0;
  int bookRowCount = 0;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // The rows that always follow the books. The Store needs the phone app over
  // BLE and nothing else -- there is no WiFi on this board -- so on a build
  // without the BLE transfer service the row is not built at all rather than
  // offered and refused.
// Retired. Browsing and searching a Calibre library belongs in the phone app,
// which has a keyboard, a screen that scrolls and the whole catalogue; the
// reader shows the finite set that was actually saved offline and pushed to it.
// The shelf below already IS that library, in the order this device wants:
// currently reading first, then newest arrival (see BookLibraryIndex::shelfLess).
//
// BleStoreActivity is left in the tree but unreachable, like UsbDriveActivity.
static constexpr bool HAS_STORE = false;
// Nothing but books. Settings and Home both live in the Action Centre, and the
// browsing screens More used to hold are what this list now is, so a menu row
// underneath the shelf would be a third route to somewhere already reachable.
static constexpr int FIXED_MENU_ROWS = 0;
// Filled in by render() from the height actually available, so a theme with
// taller rows simply pages sooner.
mutable int rowsPerPage = 1;
  // Which screenful of the shelf is showing. The shelf itself is ordered once
  // (currently reading, then newest arrival); paging never reorders it.
  int pageIndex = 0;
  // Plain ints: Rect is not complete in this header, and the pager only needs
  // the band's geometry to hit-test against.
  mutable int pagerBarY = 0;
  mutable int pagerBarHeight = 0;
  mutable int pagerSplitX = 0;
  int pageCount() const;
  void goToPage(int index);
  // Index into homeBooks of this page's first row. selectorIndex is relative to
  // the page, so every conversion to a shelf index goes through here.
  int firstVisibleBook() const;
  // Moves the selector one row, spilling onto the next/previous page at the
  // ends instead of wrapping inside the page.
  void moveSelection(int delta);

  void onSelectBook(const std::string& path);
  void onStoreOpen();
  void onMoreOpen();

  int getMenuItemCount() const { return coverCount + bookRowCount + FIXED_MENU_ROWS; }
  // Menu rows actually drawn: the book rows and the two fixed ones, plus the
  // theme's own Continue Reading row when it puts the cover book in the menu.
  int renderedMenuRowCount() const;
  // Rows the menu band can draw without running off the bottom of the screen.
  // drawButtonMenu does not clip, so this is the caller's job.
  int menuRowCapacity() const;
  // Recomputes coverCount/bookRowCount from homeBooks and the current theme.
  void layoutShelf();
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  // Reads the cached shelf into homeBooks (no SD walk, no book opened).
  void loadShelfFromCache();
  // Reconciles the cache with the card: cheap read-time refresh always, full
  // BookLibraryIndex::collectShelf rebuild only on a fingerprint mismatch.
  void reconcileShelf();
  void loadCovers(int coverHeight);
  // The one place that decides what an empty shelf says.
  void renderEmptyShelf(Rect tile) const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
