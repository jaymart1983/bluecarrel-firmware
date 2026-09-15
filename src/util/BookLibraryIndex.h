#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Builds the on-device library listing that the BLE `library` download serves:
// one JSON array describing every book under the books root, with the reading
// progress the device already has on disk.
//
// The document is written straight to SD, one book at a time, and never exists
// as a whole in RAM -- a shelf of a few hundred books would otherwise be tens of
// kilobytes of String at exactly the moment a reading session has the least heap
// to spare. The BLE download then streams that file through the ordinary
// frame/ack path, which also gives the transfer a known size and resumability.
namespace BookLibraryIndex {

constexpr size_t CALIBRE_UUID_MAX_BYTES = 64;

// A Calibre book UUID as the BLE link carries it: 1-64 characters of
// [0-9A-Za-z-]. Checked wherever one comes from the link or the card.
bool isValidCalibreUuid(std::string_view value);

struct Stats {
  uint32_t books = 0;         // entries written
  uint32_t withMetadata = 0;  // entries whose title came from a cached book metadata record
  uint32_t withProgress = 0;  // entries with a non-zero percent
};

// Writes the JSON array for `booksRoot` to `outPath`, replacing anything already
// there. Returns false (and removes the partial file) if the document could not
// be written in full; an individual unreadable folder or book is logged and
// skipped over, never dropped silently and never fatal.
//
// Blocking and SD-bound: it opens each book's metadata cache in turn, so expect
// tens of milliseconds per book. Callers on the UI task should show something
// first. The task watchdog is fed between books.
bool build(const char* booksRoot, const char* outPath, Stats* stats = nullptr);

// One book, as the home screen wants it. Deliberately the same walk and the same
// per-book description the `library` download uses -- there is exactly one
// /Books scanner in this firmware and this is it.
struct ShelfBook {
  std::string relPath;  // relative to booksRoot, e.g. "Classics/Ulysses.epub"
  std::string title;    // never empty; falls back to the filename
  std::string author;   // empty when unknown
  float percent = 0.0f;
  // When the saved position was written, from the progress.time sidecar.
  // 0 means unknown, which sorts oldest -- never treat it as epoch 0.
  uint32_t readAt = 0;
  // FAT modification time (see HalFile::modifiedEpoch). 0 means unknown.
  uint32_t addedAt = 0;
  // The book has a saved position on disk, i.e. the user has opened it and read
  // past the start. This rather than `percent > 0` because .txt/.md books have
  // no recoverable percentage but do have an exact saved position -- judging
  // them by percent would file a half-read text file under "never opened".
  bool inProgress = false;
};

// Collects at most `limit` books for the home screen, already ordered:
//
//   1. books with a saved position, most recently read first (unknown read
//      times last within that group),
//   2. then books never opened, most recently added first (unknown add times
//      last),
//   3. ties broken by the same natural path order the file browser shows.
//
// Only `limit` entries are ever held: the walk keeps a sorted top-N and drops
// anything that cannot make the cut, so a thousand-book shelf costs the same RAM
// as a ten-book one. Blocking and SD-bound exactly like build() -- one metadata
// cache open per book -- so show something first and expect seconds on a large
// shelf. The task watchdog is fed between books.
//
// Returns false only when the shelf could not be walked at all; an individual
// unreadable folder or book is logged and skipped.
bool collectShelf(const char* booksRoot, size_t limit, std::vector<ShelfBook>& out);

// A cheap "has the shelf changed" summary: the directory walk WITHOUT the
// per-book metadata load that makes collectShelf slow. Nothing here opens a
// book -- it hashes what the directory entries already carry (path, size,
// modification time), so a few hundred books cost tens of milliseconds rather
// than seconds.
//
// This exists so the home screen can keep a cached, ordered shelf and rebuild it
// only when the card actually changed. It answers "different?" and nothing else:
// a collision is possible in principle and costs a stale row, not a wrong book.
struct Fingerprint {
  uint32_t books = 0;
  uint32_t hash = 0;

  bool operator==(const Fingerprint& other) const { return books == other.books && hash == other.hash; }
  bool operator!=(const Fingerprint& other) const { return !(*this == other); }
};

bool fingerprint(const char* booksRoot, Fingerprint& out);

}  // namespace BookLibraryIndex
