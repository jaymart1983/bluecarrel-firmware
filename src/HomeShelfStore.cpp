#include "HomeShelfStore.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "activities/reader/ProgressFile.h"
#include "util/BookProgressSync.h"

std::atomic<bool> HomeShelfStore::shelfStale{false};

void HomeShelfStore::toJson(JsonDocument& doc) const {
  doc["fpBooks"] = fingerprintBooks;
  doc["fpHash"] = fingerprintHash;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["readAt"] = book.readAt;
    obj["addedAt"] = book.addedAt;
    obj["inProgress"] = book.inProgress;
    obj["percent"] = book.percent;
  }
}

bool HomeShelfStore::fromJson(JsonVariantConst doc) {
  books.clear();
  valid = false;
  fingerprintBooks = doc["fpBooks"] | 0u;
  fingerprintHash = doc["fpHash"] | 0u;

  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  books.reserve(std::min(arr.size(), MAX_SHELF_BOOKS));
  for (JsonObjectConst obj : arr) {
    if (books.size() >= MAX_SHELF_BOOKS) break;
    HomeShelfBook book;
    book.path = obj["path"] | "";
    if (book.path.empty()) continue;
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.readAt = obj["readAt"] | 0u;
    book.addedAt = obj["addedAt"] | 0u;
    book.inProgress = obj["inProgress"] | false;
    book.percent = obj["percent"] | 0.0f;
    if (book.title.empty()) {
      // Never draw a blank row: the filename is always something to show.
      const size_t slash = book.path.find_last_of('/');
      book.title = slash == std::string::npos ? book.path : book.path.substr(slash + 1);
    }
    books.push_back(std::move(book));
  }

  // A cache with no fingerprint is one written by something that did not know
  // which shelf it described; treat it as absent rather than trust it.
  valid = fingerprintBooks > 0 || !books.empty();
  LOG_DBG("HSS", "Home shelf loaded (%d entries, fp %u/%08x)", static_cast<int>(books.size()),
          static_cast<unsigned>(fingerprintBooks), static_cast<unsigned>(fingerprintHash));
  return true;
}

bool HomeShelfStore::orderBefore(const HomeShelfBook& a, const HomeShelfBook& b) {
  if (a.inProgress != b.inProgress) return a.inProgress;
  const uint32_t keyA = a.inProgress ? a.readAt : a.addedAt;
  const uint32_t keyB = b.inProgress ? b.readAt : b.addedAt;
  if (keyA != keyB) return keyA > keyB;  // newer first; 0 (unknown) sorts last
  return FsHelpers::naturalLess(a.path, b.path);
}

void HomeShelfStore::replace(std::vector<HomeShelfBook>&& newBooks, const uint32_t fpBooks, const uint32_t fpHash) {
  books = std::move(newBooks);
  if (books.size() > MAX_SHELF_BOOKS) books.resize(MAX_SHELF_BOOKS);
  fingerprintBooks = fpBooks;
  fingerprintHash = fpHash;
  valid = true;
}

bool HomeShelfStore::refreshReadTimes() {
  bool changed = false;
  for (auto& book : books) {
    // A book removed from the card between visits keeps its cached row until the
    // next fingerprint mismatch rebuilds the shelf; the home screen filters it
    // out of the drawn list on its own.
    const std::string cachePath = BookProgressSync::cachePathForBook(book.path);
    if (cachePath.empty()) continue;
    uint32_t readAt = 0;
    const bool hasTime = ProgressFile::readSavedTime(cachePath, readAt);
    if (!hasTime) readAt = 0;
    // Never older than what the shelf already holds. promote() stamps a book
    // when it is opened; one opened and closed without a page turn still has
    // its OLD saved time, and taking that would drop it back down the shelf --
    // the reshuffle-on-return promote() exists to prevent.
    readAt = std::max(readAt, book.readAt);
    // A book gains "in progress" the first time it is opened past the start,
    // which is exactly when a progress.bin appears beside it.
    const bool inProgress = book.inProgress || readAt != 0;
    // The percentage the reader recorded when it last saved. This used to be
    // left alone here, so a book indexed before it was ever opened kept
    // percent = 0 and listed without a percentage no matter how far it was
    // read -- the only path that recomputed it was the expensive full rebuild,
    // which a page turn never triggers because the card has not changed.
    float percent = book.percent;
    float saved = 0.0f;
    if (ProgressFile::readSavedPercent(cachePath, saved)) percent = saved;
    if (readAt != book.readAt || inProgress != book.inProgress || percent != book.percent) {
      book.readAt = readAt;
      book.inProgress = inProgress;
      book.percent = percent;
      changed = true;
    }
  }
  if (!changed) return false;
  std::stable_sort(books.begin(), books.end(), orderBefore);
  return true;
}

bool HomeShelfStore::promote(const std::string& path) {
  auto it = std::find_if(books.begin(), books.end(), [&](const HomeShelfBook& b) { return b.path == path; });
  if (it == books.end()) return false;
  uint32_t newest = 0;
  for (auto other = books.begin(); other != books.end(); ++other) {
    if (other != it && other->inProgress) newest = std::max(newest, other->readAt);
  }
  // Already first: no reorder, and no card write on every reopen of the book
  // being read.
  if (it == books.begin() && it->inProgress && it->readAt > newest) return false;
  uint32_t now = 0;
  if (!halClock.getEpoch(now)) now = 0;
  it->readAt = std::max(now, newest + 1);
  it->inProgress = true;
  std::stable_sort(books.begin(), books.end(), orderBefore);
  return true;
}
