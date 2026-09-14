#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HomeShelfStore.h"
#include "MappedInputManager.h"
#include <ArduinoJson.h>

#include "RecentBooksStore.h"
#include "util/BleCatalog.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookLibraryIndex.h"

namespace {
// Where the book list begins: under the status strip and the title bar, with no
// cover tile in between. One definition, because three call sites computing the
// same top independently is how a list ends up drawn over its own header.
// Tall enough for a cover to be legible on a 1-bit panel. A 44px thumbnail read
// as a smudge; the row is sized to the art rather than the art squeezed into the
// row. Used for BOTH the drawing and the touch grid, so they cannot disagree.
static int libraryRowHeight(const GfxRenderer& renderer) {
  const int fromCover = BleCatalog::ROW_COVER_HEIGHT + 12;
  return std::max(GUI.getMenuRowHeight(renderer), fromCover);
}

static int libraryListTop(const ThemeMetrics& metrics) {
  return metrics.topPadding + metrics.headerHeight + metrics.homeMenuTopOffset;
}

// The pager band across the bottom. Taller than the hint band it replaced: it is
// a touch target now, not a caption, and two arrows in 40px sat closer together
// than a thumb can reliably separate. ONE definition, because the drawing, the
// hit-test and the row capacity all have to agree on where the list stops --
// rows sized against a band that was not reserved are rows drawn underneath it.
static int libraryPagerHeight(const ThemeMetrics& metrics) { return metrics.buttonHintsHeight + 12; }

constexpr const char* BOOKS_ROOT = "/Books";
// Where the phone puts a book's cover: one BMP per book, keyed by filename.
constexpr const char* BOOK_META_DIR = "/.crosspoint/bookmeta";

// The cover the recents list already generated for this book, if any. Looked up
// by path in the ten-entry recents list rather than through
// RecentBooksStore::getDataFromBook(), which opens the book to rebuild metadata
// -- far too heavy for a screen that draws on every trip home. A book with no
// cached cover simply draws without one; the home screen never generates covers
// for books it is only listing.
std::string cachedCoverFor(const std::string& path) {
  for (const RecentBook& recent : RECENT_BOOKS.getBooks()) {
    if (recent.path == path) return recent.coverBmpPath;
  }
  return {};
}

}  // namespace

int HomeActivity::menuRowCapacity() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bandTop = libraryListTop(metrics);
  // The pager band is reserved on EVERY board, touch or not. Home draws no Back
  // chip, so this space used to be handed back to the menu on touch boards --
  // but the pager lives there now, and the bottom row was being drawn under it.
  const int bandBottom = renderer.getScreenHeight() - libraryPagerHeight(metrics);
  // The Library's own row height, not the theme's: rows that draw taller than
  // the capacity calculation assumes are rows that run off the bottom.
  const int rowPitch = libraryRowHeight(renderer) + metrics.menuSpacing;
  if (rowPitch <= 0 || bandBottom <= bandTop) return FIXED_MENU_ROWS;
  // drawButtonMenu draws every row it is given and clips nothing, so a row that
  // does not fit would be painted off the bottom of the panel.
  return std::max(FIXED_MENU_ROWS, (bandBottom - bandTop) / rowPitch);
}

void HomeActivity::layoutShelf() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int total = static_cast<int>(homeBooks.size());
  // A theme that puts the current book in the menu (RoundedRaff) shows exactly
  // one there, so the cover tile owns one book in that mode.
  // Rows, not a cover tile. The Library is a list: one line per book, the one
  // being read at the top with its percentage. A big cover for the first book
  // spent a third of the screen on a single row and pushed everything else into
  // a stub of a menu.
  coverCount = 0;
  const int room = menuRowCapacity();
  rowsPerPage = std::max(1, room);
  // Clamp the page before counting rows: a shelf that shrank (a book removed
  // from the phone) can leave pageIndex past the end, and every count below is
  // derived from it.
  const int listed = std::max(0, total - coverCount);
  const int pages = std::max(1, (listed + rowsPerPage - 1) / rowsPerPage);
  pageIndex = std::clamp(pageIndex, 0, pages - 1);
  // Rows on THIS page, not on page zero. The last page is usually short, and
  // sizing every page like the first let the selector land on -- and Confirm
  // open -- a row that was never drawn.
  bookRowCount = std::clamp(listed - pageIndex * rowsPerPage, 0, rowsPerPage);
}

// Index into homeBooks of the first row this page draws. selectorIndex is
// page-relative (it is also the row index handed to the theme), so every use of
// it as a shelf index has to come back through here. Skipping that is why a tap
// on page two opened a book from page one.
int HomeActivity::firstVisibleBook() const { return coverCount + pageIndex * std::max(1, rowsPerPage); }

int HomeActivity::renderedMenuRowCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return (metrics.homeContinueReadingInMenu ? coverCount : 0) + bookRowCount + FIXED_MENU_ROWS;
}

void HomeActivity::loadShelfFromCache() {
  homeBooks.clear();
  const auto& cached = HOME_SHELF.getBooks();
  homeBooks.reserve(cached.size());
  for (const HomeShelfBook& book : cached) {
    // The cache can outlive a book by one visit -- the fingerprint check below
    // is what removes it -- so never offer a row that opens nothing.
    if (!Storage.exists(book.path.c_str())) continue;
    RecentBook entry;
    entry.path = book.path;
    entry.title = book.title;
    entry.author = book.author;
    entry.coverBmpPath = cachedCoverFor(book.path);
    homeBooks.push_back(std::move(entry));
  }
  layoutShelf();
}

void HomeActivity::reconcileShelf() {
  shelfChecked = true;

  BookLibraryIndex::Fingerprint fp;
  if (!BookLibraryIndex::fingerprint(BOOKS_ROOT, fp)) {
    // No /Books at all (or an unreadable card): an empty shelf is the honest
    // answer, and the empty state says what to do about it.
    LOG_DBG("HOME", "No shelf to fingerprint");
    if (!HOME_SHELF.getBooks().empty()) {
      HOME_SHELF.replace({}, 0, 0);
      HOME_SHELF.saveToFile();
      loadShelfFromCache();
      requestUpdate();
    }
    return;
  }

  if (HOME_SHELF.matches(fp.books, fp.hash)) {
    // Same shelf. Only the reading positions can have moved, and those are one
    // nine-byte sidecar each -- cheap enough to re-read on every visit, which is
    // what makes "most recently read first" true the moment a book is closed.
    if (HOME_SHELF.refreshReadTimes()) {
      HOME_SHELF.saveToFile();
      loadShelfFromCache();
      requestUpdate();
    }
    return;
  }

  // The card changed. This is the expensive path -- one metadata cache open per
  // book -- so say so before blocking on it.
  LOG_DBG("HOME", "Shelf changed (%u books), rebuilding", static_cast<unsigned>(fp.books));
  // Painted straight into the buffer that is already on screen, not through
  // requestUpdateAndWait(): this runs ON the render task with the render lock
  // held (see render()), and waiting for a render from there would deadlock.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  std::vector<BookLibraryIndex::ShelfBook> shelf;
  if (!BookLibraryIndex::collectShelf(BOOKS_ROOT, HomeShelfStore::MAX_SHELF_BOOKS, shelf)) {
    LOG_ERR("HOME", "Could not rebuild the shelf");
    return;
  }

  std::vector<HomeShelfBook> cached;
  cached.reserve(shelf.size());
  const auto& previous = HOME_SHELF.getBooks();
  for (const auto& book : shelf) {
    HomeShelfBook entry;
    entry.path = std::string(BOOKS_ROOT) + "/" + book.relPath;
    entry.title = book.title;
    entry.author = book.author;
    entry.readAt = book.readAt;
    entry.addedAt = book.addedAt;
    entry.inProgress = book.inProgress;
    entry.percent = book.percent;
    // Keep the stamp promote() gave a book opened without a page turn: the
    // rebuild reads only the saved-position sidecar, which is older.
    for (const auto& old : previous) {
      if (old.path != entry.path) continue;
      entry.readAt = std::max(entry.readAt, old.readAt);
      entry.inProgress = entry.inProgress || old.inProgress;
      break;
    }
    cached.push_back(std::move(entry));
  }
  std::stable_sort(cached.begin(), cached.end(), HomeShelfStore::orderBefore);
  HOME_SHELF.replace(std::move(cached), fp.books, fp.hash);
  HOME_SHELF.saveToFile();

  loadShelfFromCache();
  // The rebuilt list may not contain the book the selection was on.
  if (selectorIndex >= getMenuItemCount()) selectorIndex = 0;
  coverRendered = false;
  coversLoaded = false;
  freeCoverBuffer();
  requestUpdate();
}

void HomeActivity::loadCovers(const int coverHeight) {
  coversLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Only the books the cover tile actually draws. Generating a thumbnail opens
  // the book, so doing it for every listed row would turn the home screen into
  // a bulk indexing pass.
  const int count = coverCount;
  for (int i = 0; i < count; i++) {
    RecentBook& book = homeBooks[i];
    if (book.coverBmpPath.empty()) continue;
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    if (Storage.exists(coverPath.c_str())) continue;

    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      // Skip loading css since we only need metadata here
      epub.load(false, true);
      if (!showingLoading) {
        showingLoading = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      GUI.fillPopupProgress(renderer, popupRect, 10 + i * (90 / count));
      if (!epub.generateThumbBmp(coverHeight)) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
        book.coverBmpPath = "";
      }
      coverRendered = false;
      requestUpdate();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (!xtc.load()) continue;
      if (!showingLoading) {
        showingLoading = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      GUI.fillPopupProgress(renderer, popupRect, 10 + i * (90 / count));
      if (!xtc.generateThumbBmp(coverHeight)) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
        book.coverBmpPath = "";
      }
      coverRendered = false;
      requestUpdate();
    }
  }

  coversLoaded = true;
  coversLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Straight from the cache: no directory walk, no book opened, so the first
  // paint is immediate. reconcileShelf() puts it right after that paint.
  HOME_SHELF.loadFromFile();
  loadShelfFromCache();

  switch (initialMenuItem) {
    case HomeMenuItem::STORE:
      // Without a Store row there is nothing at that index: the menu is
      // books + More, so the last valid index is coverCount + bookRowCount.
      // The old expression walked one PAST the end when HAS_STORE went false.
      selectorIndex = coverCount + bookRowCount;
      break;
    // Everything that used to be its own home row now lives one level down, so
    // coming back from any of them lands on the row that opens them.
    case HomeMenuItem::FILE_BROWSER:
    case HomeMenuItem::RECENTS:
    case HomeMenuItem::OPDS_BROWSER:
    case HomeMenuItem::FILE_TRANSFER:
    case HomeMenuItem::SETTINGS_MENU:
    case HomeMenuItem::MORE:
      selectorIndex = getMenuItemCount() - 1;
      break;
    default:
      selectorIndex = 0;
      break;
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    const int bookCount = coverCount + bookRowCount;
    if (selectorIndex < bookCount) {
      // firstVisibleBook(), not selectorIndex alone: the selector counts rows on
      // this page, homeBooks counts the whole shelf.
      const int shelfIndex = firstVisibleBook() + selectorIndex - coverCount;
      if (shelfIndex < 0 || shelfIndex >= static_cast<int>(homeBooks.size())) return;
      onSelectBook(homeBooks[shelfIndex].path);
      return;
    }
    if (HAS_STORE && selectorIndex == bookCount) {
      onStoreOpen();
      return;
    }
    onMoreOpen();
  };

  buttonNavigator.onNext([this] { moveSelection(1); });

  buttonNavigator.onPrevious([this] { moveSelection(-1); });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    moveSelection(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    moveSelection(-1);
    return;
  }

  // Back is otherwise unused on the home menu: open the book at the top of the
  // shelf, which is the one being read (homeBooks is shelf-ordered and already
  // pruned of files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !homeBooks.empty()) {
    onSelectBook(homeBooks[0].path);
    return;
  }

  const int coverColumnCount = std::max(1, coverCount);
  const int coverColumnWidth = (renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, metrics.contentSidePadding, coverColumnWidth, coverCount,
                                               metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      activateSelection();
    }
    return;
  }

  // The pager, hit-tested BEFORE the list. Two one-row grids rather than a rect
  // test, because that is the hit-testing the input manager offers -- each is
  // the full band high and half the panel wide. render() records the geometry;
  // until it has run once there is nothing to hit.
  if (pagerBarHeight > 0) {
    int pagerHit = -1;
    const auto upTouch = mappedInput.rowTouch(pagerHit, pagerBarY, pagerBarHeight, 1, 0, pagerSplitX, pagerBarHeight);
    if (upTouch != MappedInputManager::RowTouch::None) {
      // Only on release. Acting on the press would page the list out from under
      // a finger that has not lifted yet.
      if (upTouch == MappedInputManager::RowTouch::Tap) goToPage(pageIndex - 1);
      return;
    }
    const auto downTouch =
        mappedInput.rowTouch(pagerHit, pagerBarY, pagerBarHeight, 1, pagerSplitX, INT32_MAX, pagerBarHeight);
    if (downTouch != MappedInputManager::RowTouch::None) {
      if (downTouch == MappedInputManager::RowTouch::Tap) goToPage(pageIndex + 1);
      return;
    }
  }

  const int menuTop = libraryListTop(metrics);
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = libraryRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing,
                                              renderedMenuRowCount(), 0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    // A theme that draws the cover book as menu row 0 shares one index space
    // with the selection; every other theme's menu starts after the cover tiles.
    const int touchedIndex = metrics.homeContinueReadingInMenu ? menuRow : menuRow + coverCount;
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::renderEmptyShelf(const Rect tile) const {
  // What a brand-new device shows first: no books on the card and no phone
  // paired. Say both, and say where pairing lives -- the Store cannot help
  // here, because it needs the app connected before it can show anything.
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = tile.y + tile.height / 3;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_HOME_NO_BOOKS), true, EpdFontFamily::BOLD);
  y += lineHeight + 16;
  if (HAS_STORE) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_HOME_NO_BOOKS_HINT));
    y += lineHeight + 6;
    renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_HOME_PAIR_PATH));
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_HOME_NO_BOOKS_HINT_NO_APP));
  }
}

int HomeActivity::pageCount() const {
  // rowsPerPage (capacity), never bookRowCount (rows on THIS page). On a short
  // last page the two differ, and dividing by the short one invents pages.
  const int rows = std::max(1, rowsPerPage);
  const int listed = std::max(0, static_cast<int>(homeBooks.size()) - coverCount);
  return std::max(1, (listed + rows - 1) / rows);
}

void HomeActivity::goToPage(const int index) {
  const int clamped = std::clamp(index, 0, pageCount() - 1);
  if (clamped == pageIndex) return;
  pageIndex = clamped;
  // bookRowCount is per-page, so it is stale the moment the page changes.
  layoutShelf();
  // The selector belongs to the page, not to the shelf: landing on a row the
  // user cannot see would make the next button press jump somewhere unrelated.
  selectorIndex = 0;
  requestUpdate();
}

// One step of the selector, spilling onto the next or previous page at the ends.
//
// The rows are a window onto the shelf, not the whole of it. Wrapping modulo the
// rows on screen -- which is what ButtonNavigator does, and what this used to
// call -- means the pages past the first are unreachable by button or swipe: the
// selector runs to the bottom of page one and jumps back to its top. Only the
// true last row wraps, and only to the true first.
void HomeActivity::moveSelection(const int delta) {
  const int rows = getMenuItemCount();
  if (rows <= 0) return;
  int next = selectorIndex + delta;
  if (next >= rows) {
    if (pageIndex + 1 < pageCount()) {
      goToPage(pageIndex + 1);
      return;
    }
    next = 0;
  } else if (next < 0) {
    if (pageIndex > 0) {
      goToPage(pageIndex - 1);
      // Arriving from below: land on the last row, not the first, or paging up
      // and back down again would not return the user where they were.
      selectorIndex = std::max(0, getMenuItemCount() - 1);
      requestUpdate();
      return;
    }
    next = rows - 1;
  }
  selectorIndex = next;
  requestUpdate();
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // The page has a name now. This screen is the device's library -- the shelf
  // that mirrors what the phone saved offline -- and an untitled band read as
  // chrome rather than as a place.
  // The FULL header height, not the shortened home band. This header is two
  // rows -- the status strip (time, USB, BLE, battery) above, the page title
  // below -- and squeezing both into homeTopPadding left the status row with
  // nowhere to draw, which read as "the top bar is missing".
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LIBRARY));

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  // Nothing to snapshot: the cover tile is gone, so the partial-restore path
  // has no region to preserve.
  coverRectX = 0;
  coverRectY = 0;
  coverRectW = 0;
  coverRectH = 0;

  // No cover tile. The Library is a list, and the tile was still being painted
  // at homeTopPadding with its full height -- straight over the header, which
  // now occupies that space. That is the black block and the grey slab across
  // the top, and why the title bar could not be seen.
  //
  // The empty-shelf message moves into the list area, under the header, for the
  // same reason.
  if (homeBooks.empty()) {
    const int top = libraryListTop(metrics);
    renderEmptyShelf(Rect{0, top, pageWidth, pageHeight - top - metrics.buttonHintsHeight});
  }

  // Books first, then the two rows that are not books.
  std::vector<std::string> menuItems;
  std::vector<UIIcon> menuIcons;
  std::vector<std::string> menuCovers;
  menuItems.reserve(renderedMenuRowCount());
  menuIcons.reserve(renderedMenuRowCount());
  if (metrics.homeContinueReadingInMenu) {
    for (int i = 0; i < coverCount; i++) {
      menuItems.emplace_back(tr(STR_CONTINUE_READING));
      menuIcons.push_back(Book);
    }
  }
  // The book opened most recently, by the timestamp the reader stamps on every
  // save. Computed from the shelf rather than assumed to be row zero: the sort
  // already puts it first today, but a badge that is really just "the top row"
  // would start lying the moment the ordering changes.
  std::string lastReadPath;
  {
    uint32_t newest = 0;
    for (const auto& shelfBook : HOME_SHELF.getBooks()) {
      if (!shelfBook.inProgress || shelfBook.readAt == 0) continue;
      if (shelfBook.readAt > newest) {
        newest = shelfBook.readAt;
        lastReadPath = shelfBook.path;
      }
    }
  }
  std::vector<bool> menuMarked;
  menuMarked.reserve(renderedMenuRowCount());
  // -1 = no progress to show. Kept out of the label so the disc can own a fixed
  // corner of the row instead of trailing whatever the title happens to be.
  std::vector<float> menuPercent;
  menuPercent.reserve(renderedMenuRowCount());

  const int firstRow = firstVisibleBook();
  for (int i = 0; i < bookRowCount; i++) {
    const int idx = firstRow + i;
    if (idx >= static_cast<int>(homeBooks.size())) break;
    // homeBooks holds RecentBook, which has no progress; the percentage lives on
    // the shelf entry the same walk produced. Matched by path rather than by
    // index because the two lists are built separately.
    const auto& book = homeBooks[idx];
    float percent = 0.0f;
    for (const auto& shelfBook : HOME_SHELF.getBooks()) {
      if (shelfBook.path == book.path) {
        percent = shelfBook.inProgress ? shelfBook.percent : 0.0f;
        break;
      }
    }
    // The title Calibre knows, when the phone sent one. The shelf's own title
    // comes from the EPUB's metadata and falls back to the filename, which is
    // why rows read "The-Time-Machine-H-G-Wells" instead of "The Time Machine".
    const auto slashForTitle = book.path.find_last_of('/');
    const std::string fileForTitle =
        slashForTitle == std::string::npos ? book.path : book.path.substr(slashForTitle + 1);
    std::string title = book.title;
    {
      const std::string metaPath = std::string(BOOK_META_DIR) + "/" + fileForTitle + ".json";
      HalFile meta;
      if (Storage.openFileForRead("HOME", metaPath, meta)) {
        JsonDocument doc;
        if (deserializeJson(doc, meta) == DeserializationError::Ok) {
          const char* sent = doc["title"] | "";
          if (sent != nullptr && sent[0] != '\0') title = sent;
        }
        meta.close();
      }
    }
    menuItems.push_back(title);
    // Whole percent thrown away here was the only reason the row could not show
    // a decimal: the shelf stores 0..1 and the sidecar keeps basis points.
    menuPercent.push_back(percent > 0.0f ? percent * 100.0f : -1.0f);
    menuIcons.push_back(Book);
    menuMarked.push_back(!lastReadPath.empty() && book.path == lastReadPath);
    const auto slash = book.path.find_last_of('/');
    const std::string filename = slash == std::string::npos ? book.path : book.path.substr(slash + 1);
    const std::string stem = std::string(BOOK_META_DIR) + "/" + filename;
    const std::string cover = stem + ".bmp";
    menuCovers.push_back(Storage.exists(cover.c_str()) ? cover : std::string());
  }

  // Home is the navigation root and draws no Back chip, but the band at the
  // bottom is not free: the pager owns it, on touch boards and button boards
  // alike. Must match menuRowCapacity() or the rows and their touch grid
  // disagree about how many fit.
  const int menuBottomReserve = libraryPagerHeight(metrics);
  GUI.drawButtonMenu(
      renderer,
      // Height derived from the SAME top the list is drawn at. The old
      // expression still subtracted homeTopPadding as well as headerHeight,
      // double-counting a band that is no longer there and leaving the row
      // geometry disagreeing with the space it was given.
      Rect{0, libraryListTop(metrics), pageWidth,
           pageHeight - libraryListTop(metrics) - menuBottomReserve - metrics.verticalSpacing},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - coverCount,
      [&menuItems](int index) { return menuItems[index]; }, [&menuIcons](int index) { return menuIcons[index]; },
      // The cover the phone sent with the book, if it did. Built from the book's
      // filename, which is the only name the shelf and the app agree on -- the
      // reader has no catalogue ids. Nothing is opened or decoded here beyond
      // the BMP itself.
      [&menuCovers](int index) { return menuCovers[index]; }, libraryRowHeight(renderer),
      [&menuMarked](int index) { return index < static_cast<int>(menuMarked.size()) && menuMarked[index]; },
      [&menuPercent](int index) {
        return index < static_cast<int>(menuPercent.size()) ? menuPercent[index] : -1.0f;
      });

  // The pager, full width, across the bottom. Always drawn -- greyed rather than
  // hidden -- so the bar does not appear and disappear as the shelf grows past
  // one screen, which would move every row under the user's finger.
  {
    const int band = libraryPagerHeight(metrics);
    const int barY = pageHeight - band;
    const int half = pageWidth / 2;
    const int pages = pageCount();
    const bool canUp = pageIndex > 0;
    const bool canDown = pageIndex + 1 < pages;
    // pageWidth is one PAST the last valid column (0..pageWidth-1), so drawing to
    // it puts a pixel off-panel every frame -- which the renderer then reports,
    // and those reports flood the serial console.
    renderer.drawLine(0, barY, pageWidth - 1, barY, true);
    pagerBarY = barY;
    pagerBarHeight = band;
    pagerSplitX = half;
    const Rect pageUpRect{0, barY, half, band};
    const Rect pageDownRect{half, barY, pageWidth - half - 1, band};
    renderer.drawLine(half, barY + 4, half, barY + band - 4, true);

    // Disabled is drawn DIMMER, not struck through. A slash reads as "forbidden"
    // and drew the eye to the one arrow that does nothing; the panel is 1-bit and
    // has no grey, so the dimming is a dotted stroke -- every other pixel -- which
    // at this size reads as grey from any normal distance.
    const auto dottedLine = [&](int x0, int y0, const int x1, const int y1) {
      const int dx = std::abs(x1 - x0);
      const int dy = -std::abs(y1 - y0);
      const int sx = x0 < x1 ? 1 : -1;
      const int sy = y0 < y1 ? 1 : -1;
      int err = dx + dy;
      int n = 0;
      while (true) {
        if ((n++ % 2) == 0) renderer.drawPixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
      }
    };
    const auto arrow = [&](const Rect& r, const bool up, const bool enabled) {
      const int cx = r.x + r.width / 2;
      const int cy = r.y + r.height / 2;
      const int w = 12;
      const int h = 7;
      const int tipY = up ? cy - h : cy + h;
      const int baseY = up ? cy + h : cy - h;
      if (enabled) {
        // Three stacked chevrons, not one. A single-pixel stroke next to a
        // dotted one is a difference of degree the eye has to hunt for; on a
        // 1-bit panel the only way to say "this one is live" is more ink.
        for (int d = 0; d < 3; d++) {
          const int dy = up ? d : -d;
          renderer.drawLine(cx - w, baseY + dy, cx, tipY + dy, true);
          renderer.drawLine(cx + w, baseY + dy, cx, tipY + dy, true);
        }
      } else {
        dottedLine(cx - w, baseY, cx, tipY);
        dottedLine(cx + w, baseY, cx, tipY);
      }
    };
    arrow(pageUpRect, /*up=*/true, canUp);
    arrow(pageDownRect, /*up=*/false, canDown);
  }

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (HomeShelfStore::consumeStale()) {
    // Something landed on the card from outside this screen. Re-walk and repaint
    // so a book pushed from the phone appears where it belongs, at the top.
    shelfChecked = false;
    reconcileShelf();
    requestUpdate();
  } else if (!shelfChecked) {
    // Behind the first useful paint, never in front of it.
    reconcileShelf();
    // Unconditional: even when nothing changed, this pass ends with a popup or a
    // stale frame on the panel and the covers still to load.
    requestUpdate();
  } else if (!coversLoaded && !coversLoading) {
    coversLoading = true;
    loadCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onStoreOpen() {
#if FREEINK_CAP_BLE_TRANSFER
  activityManager.goToStore();
#endif
}

void HomeActivity::onMoreOpen() { activityManager.goToMoreMenu(); }
