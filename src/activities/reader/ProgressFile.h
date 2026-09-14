#pragma once

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <string>

namespace ProgressFile {

// The saved position itself lives in `<cachePath>/progress.bin`, whose layout is
// private to each reader activity (EPUB writes 4/6/10 bytes, XTC and TXT write
// 4). "When was that position saved" lives in a sidecar next to it rather than
// inside it, for two reasons:
//
//  1. All three readers dispatch on the exact byte length of progress.bin --
//     EpubReaderActivity accepts 4, 6 or 10 and reads the 10-byte form as
//     "position + visible-text offset". Appending four timestamp bytes would
//     turn a 6-byte EPUB position into the 10-byte form and resume the book at a
//     text offset that is really a clock reading. Length is load-bearing; it
//     cannot carry a trailer.
//  2. Firmware that predates this file must keep opening books written by
//     firmware that has it. An ignored extra file is compatible in both
//     directions; a longer progress.bin is not.
//
// Backwards compatibility, therefore: every progress.bin written before this
// existed has no sidecar, and readSavedTime() reports that as "unknown". Unknown
// is NOT epoch 0 -- callers must treat it as older than any real timestamp
// rather than as a 1970 save that everything beats by definition.
constexpr const char* SIDECAR_NAME = "/progress.time";

// magic "CPPT" (CrossPoint Progress Time) + version, so a truncated or
// scribbled-on sidecar reads as unknown instead of as a wrong instant.
constexpr uint8_t SIDECAR_MAGIC[4] = {'C', 'P', 'P', 'T'};
// v1: magic(4) + version(1) + epoch LE(4).
// v2: the same, plus percent-of-book in basis points LE(2).
//
// The percentage is here rather than derived on demand because deriving it means
// OPENING THE BOOK: ProgressMapper::toPercentage needs a loaded Epub. The home
// screen's cheap path (HomeShelfStore::refreshReadTimes) runs on every visit and
// deliberately touches nothing but these sidecars, so a book read to 40% listed
// at 0% forever -- the percentage existed only inside the reader that already
// knew it. Writing it down where the cheap path can see it costs two bytes.
//
// Version 1 files stay readable and simply have no percentage; readSavedTime
// accepts either length, so a downgrade keeps working and an upgrade fills the
// percentage in the first time each book is next saved.
constexpr uint8_t SIDECAR_VERSION = 2;
constexpr size_t SIDECAR_BYTES_V1 = 9;
constexpr size_t SIDECAR_BYTES = 11;

// --- Synced jump ------------------------------------------------------------
//
// A position that arrived from ANOTHER device, expressed as a spine item plus a
// fraction through it, waiting for the book to be opened so it can be resolved.
//
// Its own file, deliberately NOT appended to progress.bin. The three readers
// dispatch on progress.bin's exact byte length -- EpubReaderActivity accepts 4,
// 6 or 10 and reads the 10-byte form as "position + visible-text offset". Adding
// bytes there would turn a 6-byte EPUB position into the 10-byte form and resume
// the book at a text offset that is really a spine index. Length is load-bearing;
// it cannot carry a trailer. The same reasoning as the progress.time sidecar.
//
// Why a spine index and a fraction rather than a percentage: a percentage has to
// be converted back into a position through cumulative spine FILE sizes, and
// markup density varies enough between chapters (measured at 1.23 to 4.41 bytes
// per text character within a single book) that the conversion can select the
// wrong chapter outright. The sender already knows which item it meant.
//
// `spineCount` is a same-file check. The sender measured a specific EPUB; if this
// reader holds one with a different spine count -- a re-conversion, a different
// edition -- the fields describe a book that is not this one and are discarded
// in favour of the percentage the sender also supplied.
constexpr const char* JUMP_NAME = "/syncjump.bin";
constexpr uint8_t JUMP_MAGIC[4] = {'C', 'P', 'S', 'J'};  // CrossPoint Sync Jump
constexpr uint8_t JUMP_VERSION = 1;
constexpr size_t JUMP_BYTES = 13;  // magic(4) + version(1) + spine(2) + frac(4) + count(2)

inline bool writeSyncJump(const std::string& cachePath, const uint16_t spineIndex, const float fraction,
                          const uint16_t spineCount) {
  if (spineCount == 0) return false;
  if (spineIndex >= spineCount) return false;
  if (!(fraction >= 0.0f) || fraction > 1.0f) return false;  // also rejects NaN
  uint32_t fracBits = 0;
  memcpy(&fracBits, &fraction, sizeof(fracBits));
  const uint8_t record[JUMP_BYTES] = {JUMP_MAGIC[0],
                                      JUMP_MAGIC[1],
                                      JUMP_MAGIC[2],
                                      JUMP_MAGIC[3],
                                      JUMP_VERSION,
                                      static_cast<uint8_t>(spineIndex & 0xFF),
                                      static_cast<uint8_t>((spineIndex >> 8) & 0xFF),
                                      static_cast<uint8_t>(fracBits & 0xFF),
                                      static_cast<uint8_t>((fracBits >> 8) & 0xFF),
                                      static_cast<uint8_t>((fracBits >> 16) & 0xFF),
                                      static_cast<uint8_t>((fracBits >> 24) & 0xFF),
                                      static_cast<uint8_t>(spineCount & 0xFF),
                                      static_cast<uint8_t>((spineCount >> 8) & 0xFF)};
  const std::string path = cachePath + JUMP_NAME;
  HalFile f;
  if (!Storage.openFileForWrite("PRG", path, f)) {
    LOG_ERR("PRG", "Could not open sync jump for write: %s", path.c_str());
    return false;
  }
  const size_t written = f.write(record, sizeof(record));
  f.flush();
  f.close();
  if (written != sizeof(record)) {
    Storage.remove(path.c_str());
    return false;
  }
  return true;
}

/// Reads a pending jump. False when there is none, or it is unreadable.
inline bool readSyncJump(const std::string& cachePath, uint16_t& spineIndex, float& fraction,
                         uint16_t& spineCount) {
  HalFile f;
  if (!Storage.openFileForRead("PRG", cachePath + JUMP_NAME, f)) return false;
  uint8_t buf[JUMP_BYTES] = {};
  const int read = f.read(buf, sizeof(buf));
  f.close();
  if (read != static_cast<int>(JUMP_BYTES)) return false;
  for (size_t i = 0; i < sizeof(JUMP_MAGIC); i++) {
    if (buf[i] != JUMP_MAGIC[i]) return false;
  }
  if (buf[4] != JUMP_VERSION) return false;
  spineIndex = static_cast<uint16_t>(buf[5] | (buf[6] << 8));
  const uint32_t fracBits = static_cast<uint32_t>(buf[7]) | (static_cast<uint32_t>(buf[8]) << 8) |
                            (static_cast<uint32_t>(buf[9]) << 16) | (static_cast<uint32_t>(buf[10]) << 24);
  memcpy(&fraction, &fracBits, sizeof(fraction));
  spineCount = static_cast<uint16_t>(buf[11] | (buf[12] << 8));
  if (!(fraction >= 0.0f) || fraction > 1.0f) return false;
  return spineCount > 0 && spineIndex < spineCount;
}

/// Consumed exactly once. The jump describes where another device HAD got to,
/// which stops being true the moment this reader moves; leaving it behind would
/// drag the book back there on every open.
inline void clearSyncJump(const std::string& cachePath) {
  const std::string path = cachePath + JUMP_NAME;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
}

// Reads the UTC epoch the position in `<cachePath>/progress.bin` was saved at.
//
// Returns false for "unknown": no sidecar (a pre-existing book, or one saved
// while the device had no clock), a short read, a bad magic/version, or an
// implausible epoch. Never yields 0 as a timestamp.
// Reads the record, accepting either version. `percentBp` is set to the stored
// per-mille-of-a-percent value for v2 and left untouched for v1; `hasPercent`
// says which happened.
inline bool readSidecar(const std::string& cachePath, uint32_t& epochUtc, uint16_t& percentBp, bool& hasPercent) {
  hasPercent = false;
  HalFile f;
  if (!Storage.openFileForRead("PRG", cachePath + SIDECAR_NAME, f)) return false;
  uint8_t buf[SIDECAR_BYTES] = {};
  const int read = f.read(buf, sizeof(buf));
  f.close();
  if (read < static_cast<int>(SIDECAR_BYTES_V1)) return false;
  for (size_t i = 0; i < sizeof(SIDECAR_MAGIC); i++) {
    if (buf[i] != SIDECAR_MAGIC[i]) return false;
  }
  // Any version this build knows. Unknown (newer) versions read as no record at
  // all rather than as a misparsed one.
  if (buf[4] != 1 && buf[4] != 2) return false;
  const uint32_t epoch = static_cast<uint32_t>(buf[5]) | (static_cast<uint32_t>(buf[6]) << 8) |
                         (static_cast<uint32_t>(buf[7]) << 16) | (static_cast<uint32_t>(buf[8]) << 24);
  if (!HalClock::isPlausibleEpoch(epoch)) return false;
  epochUtc = epoch;
  if (buf[4] == 2 && read >= static_cast<int>(SIDECAR_BYTES)) {
    const uint16_t bp = static_cast<uint16_t>(buf[9]) | static_cast<uint16_t>(buf[10] << 8);
    if (bp <= 10000) {
      percentBp = bp;
      hasPercent = true;
    }
  }
  return true;
}

inline bool readSavedTime(const std::string& cachePath, uint32_t& epochUtc) {
  uint16_t percentBp = 0;
  bool hasPercent = false;
  return readSidecar(cachePath, epochUtc, percentBp, hasPercent);
}

// The fraction of the book the saved position sits at, 0..1. False when the
// sidecar is missing, unreadable, or was written by a build that predates the
// percentage -- never a fabricated 0, which would list a half-read book as
// untouched.
inline bool readSavedPercent(const std::string& cachePath, float& percent) {
  uint32_t epoch = 0;
  uint16_t percentBp = 0;
  bool hasPercent = false;
  if (!readSidecar(cachePath, epoch, percentBp, hasPercent) || !hasPercent) return false;
  percent = static_cast<float>(percentBp) / 10000.0f;
  return true;
}

// Drops the sidecar, so the saved position reads as "saved at an unknown time".
//
// This is what a save made while the device has no working clock does. Leaving a
// stale sidecar behind would be actively wrong: it would claim a position the
// user reached just now was reached at whatever the last known time was, and an
// incoming sync newer than that would silently overwrite genuinely fresher
// reading.
inline void clearSavedTime(const std::string& cachePath) {
  const std::string path = cachePath + SIDECAR_NAME;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
}

// Stamps `<cachePath>/progress.bin` as having been saved at `epochUtc`.
//
// Written in place rather than through a temp-and-rename: the record is nine
// bytes inside a single sector, and -- unlike progress.bin, whose corruption
// stranded books (issue #2275) -- a torn write here fails the magic/length check
// and degrades to "unknown", which every caller already handles as the oldest
// possible timestamp. The failure mode is a skipped sync, not a lost book.
inline bool writeSavedTime(const std::string& cachePath, const uint32_t epochUtc, const uint16_t percentBp = 0) {
  if (!HalClock::isPlausibleEpoch(epochUtc)) return false;
  const uint16_t bp = percentBp > 10000 ? 10000 : percentBp;
  const uint8_t record[SIDECAR_BYTES] = {SIDECAR_MAGIC[0],
                                         SIDECAR_MAGIC[1],
                                         SIDECAR_MAGIC[2],
                                         SIDECAR_MAGIC[3],
                                         SIDECAR_VERSION,
                                         static_cast<uint8_t>(epochUtc & 0xFF),
                                         static_cast<uint8_t>((epochUtc >> 8) & 0xFF),
                                         static_cast<uint8_t>((epochUtc >> 16) & 0xFF),
                                         static_cast<uint8_t>((epochUtc >> 24) & 0xFF),
                                         static_cast<uint8_t>(bp & 0xFF),
                                         static_cast<uint8_t>((bp >> 8) & 0xFF)};
  const std::string path = cachePath + SIDECAR_NAME;
  HalFile f;
  if (!Storage.openFileForWrite("PRG", path, f)) {
    LOG_ERR("PRG", "Could not open progress timestamp for write: %s", path.c_str());
    return false;
  }
  const size_t written = f.write(record, sizeof(record));
  f.flush();
  f.close();
  if (written != sizeof(record)) {
    LOG_ERR("PRG", "Short write saving progress timestamp to %s", path.c_str());
    Storage.remove(path.c_str());
    return false;
  }
  return true;
}

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeBytesAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

// Saves progress and stamps it with `savedAtEpoch`.
//
// The stamp is written after progress.bin is in place, so a crash in between
// leaves a real position with an unknown time -- which reads as "oldest", the
// conservative direction: a sync will decline to overwrite it only when it is
// itself older, and the user loses a comparison, never a page.
inline bool writeAtomicAt(const std::string& cachePath, const uint8_t* data, const size_t len,
                          const uint32_t savedAtEpoch, const uint16_t percentBp = 0) {
  if (!writeBytesAtomic(cachePath, data, len)) return false;
  if (!writeSavedTime(cachePath, savedAtEpoch, percentBp)) clearSavedTime(cachePath);
  return true;
}

// Saves progress, stamped with the device's own clock.
//
// This is the reader's entry point, and on any board with an RTC it always
// stamps: HalClock::begin() starts a stopped clock at the firmware's build
// epoch, so the device knows *a* time from its very first boot even before an
// app sends `set_time`. The stamp may be behind real time until then; it is
// never absent.
//
// The clearSavedTime() path is the honest answer for what remains: a board with
// no RTC at all, or an RTC that has failed. The position is new, the time is not
// known, and a stale stamp left in place would claim the user reached this page
// back whenever the clock last worked -- letting an incoming sync overwrite
// genuinely fresher reading.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, const size_t len,
                        const uint16_t percentBp = 0) {
  if (!writeBytesAtomic(cachePath, data, len)) return false;
  uint32_t epoch = 0;
  if (halClock.getEpoch(epoch)) {
    writeSavedTime(cachePath, epoch, percentBp);
  } else {
    clearSavedTime(cachePath);
  }
  return true;
}

}  // namespace ProgressFile
