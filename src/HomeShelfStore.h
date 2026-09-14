#pragma once

#include <atomic>
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// The home screen's ordered shelf, cached on the card.
//
// WHY THIS EXISTS. Home is the screen the device returns to constantly, and the
// order it wants -- currently reading first by when it was last read, then
// newest added -- can only be computed by walking /Books and opening every
// book's metadata cache (BookLibraryIndex::collectShelf). That is seconds on a
// large shelf, and paying it on every trip home would be intolerable. So the
// walk's result is cached here and rebuilt only when the card actually changed,
// which BookLibraryIndex::fingerprint answers in milliseconds because it never
// opens a book.
//
// The cache holds only what home draws: no percentages (nothing on the home
// screen renders one) and no more entries than the screen can show.
struct HomeShelfBook {
  std::string path;    // absolute, e.g. "/Books/Classics/Ulysses.epub"
  std::string title;   // never empty
  std::string author;  // empty when unknown
  // When the saved position was last written; 0 means unknown, which sorts
  // oldest. Refreshed cheaply on every visit (one small sidecar read per entry)
  // so returning from a book re-sorts the shelf without a rebuild.
  uint32_t readAt = 0;
  // FAT modification time; 0 means unknown. Only meaningful for books that have
  // never been opened, which are ordered by it.
  uint32_t addedAt = 0;
  bool inProgress = false;
  // 0..1, from the same walk that set inProgress. Carried so the Library can
  // show how far in a book is without reopening it; 0 means "not known", which
  // is also what a .txt with a saved position but no recoverable percentage has.
  float percent = 0.0f;
};

class HomeShelfStore : public PersistableStore<HomeShelfStore> {
 private:
  static std::atomic<bool> shelfStale;
  std::vector<HomeShelfBook> books;
  // BookLibraryIndex::Fingerprint of the shelf these entries were built from.
  uint32_t fingerprintBooks = 0;
  uint32_t fingerprintHash = 0;
  bool valid = false;

  HomeShelfStore() = default;
  ~HomeShelfStore() = default;

  friend class PersistableStore<HomeShelfStore>;

 public:
  // The home menu can draw a handful of rows at most (see
  // HomeActivity::bookRowCapacity), and the cover strip takes the first few.
  // Twelve leaves headroom for the tallest theme without turning the cache into
  // a second library index.
  // The Library pages through this, so it is the size of the whole shelf the
  // device shows rather than one screenful. Each entry is a few short strings;
  // 40 is a handful of KB against 8 MB of PSRAM.
  static constexpr size_t MAX_SHELF_BOOKS = 40;

  // Set when something outside the Library changes what is on the card -- a book
  // arriving over BLE, today. The Library reconciles once per visit, so without
  // this a book pushed from the phone while the shelf was on screen did not
  // appear until the device was restarted, which read as "the transfer failed".
  //
  // Atomic because the writer is the BLE event pump and the reader is the
  // render loop.
  static void markStale() { shelfStale.store(true, std::memory_order_relaxed); }
  static bool consumeStale() { return shelfStale.exchange(false, std::memory_order_relaxed); }

  static const char* getFilePath() { return "/.crosspoint/home-shelf.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<HomeShelfBook>& getBooks() const { return books; }
  bool isValid() const { return valid; }

  // True when `books` was built from a shelf that still looks like this one.
  bool matches(uint32_t fpBooks, uint32_t fpHash) const {
    return valid && fingerprintBooks == fpBooks && fingerprintHash == fpHash;
  }

  // Replaces the cached shelf. Does not persist; the caller decides.
  void replace(std::vector<HomeShelfBook>&& newBooks, uint32_t fpBooks, uint32_t fpHash);

  // Re-reads each entry's saved-position timestamp and re-sorts. Cheap: one
  // nine-byte sidecar per entry, no book is opened. Returns true when the order
  // or any timestamp changed, so the caller knows whether to persist and repaint.
  bool refreshReadTimes();

  // Moves `path` to the front the moment it is OPENED, rather than when a page
  // turn later writes its saved time. Home used to draw the old order on return
  // and then re-sort under the user's finger, so the book they were reaching
  // for had moved by the time they pressed it. Stamps the book newer than every
  // other in-progress book (the clock may be unset). Returns true when the
  // shelf changed and should be saved; false when it was already first or is
  // not on the shelf.
  bool promote(const std::string& path);

  // The shared ordering, exposed so the rebuild and the cheap refresh cannot
  // drift apart: in-progress first (most recently read first), then never-opened
  // (most recently added first), unknown timestamps last within their group.
  static bool orderBefore(const HomeShelfBook& a, const HomeShelfBook& b);
};

#define HOME_SHELF HomeShelfStore::getInstance()
