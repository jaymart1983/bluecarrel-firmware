#include "BleLink.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <ArduinoJson.h>
#include <HalClock.h>
#include <Logging.h>
#include <Memory.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <freertos/task.h>
#include <mbedtls/md.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <utility>

#include "BleTrustedHostStore.h"
#include "BuildStamp.h"
#include "CrossPointSettings.h"
#include "HomeShelfStore.h"
#include "components/UITheme.h"
#include "FirmwareFlasher.h"
#include "FirmwareStaging.h"
#include "FirmwareWatcher.h"
#include "activities/Activity.h"  // pulls ActivityManager.h with Activity complete
#include "activities/network/BleStoreController.h"
#include "util/BleCatalog.h"
#include "util/BookCacheUtils.h"
#include "util/BookLibraryIndex.h"
#include <vector>

#include "CrossPointState.h"
#include "activities/reader/ProgressFile.h"
#include "util/BookProgressSync.h"
#include "util/TaskWatchdog.h"

namespace {

constexpr const char* BLE_DEVICE_NAME = "Bluecarrel";
constexpr const char* BLE_SERVICE_UUID = "6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_CONTROL_UUID = "6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_IN_UUID = "6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_STATUS_UUID = "6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_OUT_UUID = "6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BOOKS_ROOT = "/Books";

// Where the reader IS in a book, as opposed to caches rebuilt from the book.
//
// A BOOK upload that replaces an existing file keeps these across the
// replacement, so when Calibre updates a book (new metadata, a cover swap, a
// baseline re-encode) and the phone re-sends it, the reader keeps its place
// without having to be told again by kosync. Safe because progress.bin records spine plus
// VISIBLE TEXT OFFSET, not a byte position in the file, so it still points at
// the same words after a rewrite that did not change the text; the reader
// clamps a spine index that no longer exists (see EpubReaderActivity).
constexpr const char* kPositionFiles[] = {"/progress.bin", "/progress.time", "/syncjump.bin"};

struct KeptPositionFile {
  const char* name;
  uint8_t bytes[64];  // the largest of these is 13 bytes
  int length;
};

std::vector<KeptPositionFile> takePositionFiles(const std::string& cachePath) {
  std::vector<KeptPositionFile> kept;
  for (const char* name : kPositionFiles) {
    HalFile f;
    if (!Storage.openFileForRead("BLE", cachePath + name, f)) continue;
    KeptPositionFile k{name, {}, 0};
    k.length = f.read(k.bytes, sizeof(k.bytes));
    f.close();
    // A file that filled the buffer is not one of ours; do not truncate it back.
    if (k.length > 0 && k.length < static_cast<int>(sizeof(k.bytes))) kept.push_back(k);
  }
  return kept;
}

void restorePositionFiles(const std::string& cachePath, const std::vector<KeptPositionFile>& kept) {
  if (kept.empty()) return;
  if (!Storage.exists(cachePath.c_str())) Storage.mkdir(cachePath.c_str());
  for (const auto& k : kept) {
    HalFile f;
    if (!Storage.openFileForWrite("BLE", cachePath + k.name, f)) continue;
    f.write(k.bytes, static_cast<size_t>(k.length));
    f.close();
  }
}
constexpr const char* PICTURES_ROOT = "/Pictures";
constexpr const char* CRASH_REPORT_PATH = "/crash_report.txt";
constexpr const char* CRASH_REPORT_NAME = "crash_report.txt";
// The library listing is staged on SD rather than held in RAM, then served
// through the same frame/ack path as any other download -- which is also what
// gives it a known size and a resumable offset.
constexpr const char* LIBRARY_INDEX_PATH = "/.crosspoint/ble-library.json";
constexpr const char* LIBRARY_INDEX_NAME = "library.json";
// Build identity, for the app's firmware screen. A download rather than a status
// field: the status READ already sits at ~503 of its 512 bytes and sheds, and a
// version string there would push the capability lists out.
constexpr const char* ABOUT_PATH = "/.crosspoint/ble-about.json";
constexpr const char* ABOUT_NAME = "about.json";
constexpr const char* CROSSPOINT_ROOT = "/.crosspoint";
// Settings move over the link as one JSON document, the same shape
// CrossPointSettings already persists -- toJson()/fromJson() are the single
// definition of what a setting is, so the transport adds no second schema to
// keep in step.
// One cover and one metadata file per book, keyed by the book's filename. The
// app builds both from Calibre -- it already has the cover art and the metadata
// -- so the reader never opens a book to draw its shelf. Sent BEFORE the book
// itself, so the row can show a cover and a blurb while the file is still
// copying.
constexpr const char* BOOK_META_DIR = "/.crosspoint/bookmeta";
constexpr const char* BOOK_META_PART_PATH = "/.crosspoint/.bookmeta.part";
constexpr const char* BOOK_META_INBOX_PATH = "/.crosspoint/bookmeta-in.cpct";
constexpr size_t MAX_BLE_BOOK_META_BYTES = 96 * 1024;
constexpr const char* SETTINGS_SNAPSHOT_PATH = "/.crosspoint/ble-settings.json";
constexpr const char* SETTINGS_SNAPSHOT_NAME = "settings.json";
constexpr const char* SETTINGS_INBOX_PATH = "/.crosspoint/ble-settings-in.json";
constexpr const char* SETTINGS_INBOX_NAME = "settings-in.json";
constexpr size_t MAX_BLE_SETTINGS_BYTES = 64 * 1024;
// A `progress` batch is staged like any other upload -- part file, SHA-256 over
// the whole thing, rename on commit -- and only then parsed. Verifying before
// touching a single book means a truncated batch cannot half-apply.
constexpr const char* PROGRESS_BATCH_PART_PATH = "/.crosspoint/ble-progress.json.part";
constexpr const char* PROGRESS_BATCH_PATH = "/.crosspoint/ble-progress.json";
constexpr const char* PROGRESS_BATCH_NAME = "progress.json";
// Per-entry outcomes go to SD and are served as an ordinary download. They do
// not fit in `status`: a notification carries at most ATT_MTU-3 bytes, so a
// shelf-sized result array would be silently truncated on the wire.
constexpr const char* PROGRESS_RESULT_PATH = "/.crosspoint/ble-progress-result.json";
constexpr const char* PROGRESS_RESULT_NAME = "progress-result.json";
// A catalogue page or book detail is staged exactly like every other upload --
// part file, SHA-256 over the whole thing, rename on commit -- and only then
// unpacked. The store's own scratch lives under /.crosspoint/store so closing
// the Store screen can clear the whole lot in one place.
constexpr const char* STORE_ROOT = "/.crosspoint/store";
constexpr const char* CATALOG_PART_PATH = "/.crosspoint/store/catalog.bin.part";
constexpr const char* CATALOG_PATH = "/.crosspoint/store/catalog.bin";
constexpr const char* CATALOG_NAME = "catalog.bin";
constexpr size_t MIN_BLE_FIRMWARE_BYTES = 64UL * 1024UL;
constexpr size_t MAX_BLE_BOOK_BYTES = 32UL * 1024UL * 1024UL;
constexpr size_t MAX_BLE_BMP_BYTES = 8UL * 1024UL * 1024UL;
// ~120 bytes per entry, so this is a shelf of a few thousand books with room to
// spare, and still a bounded amount of SD scratch.
constexpr size_t MAX_BLE_PROGRESS_BYTES = 512UL * 1024UL;
// One entry is parsed at a time and never exceeds this; the cap is what keeps a
// hostile or corrupt document from growing a std::string without bound.
constexpr size_t MAX_PROGRESS_ENTRY_BYTES = 640;
constexpr uint32_t MAX_PROGRESS_ENTRIES = 8192;
// A book path relative to /Books. Longer than MAX_FILENAME_BYTES because the
// `library` listing emits sub-folder paths and this must round-trip them.
constexpr size_t MAX_BOOK_PATH_BYTES = 255;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES = 160;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES_MIN = 20;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES_MAX = BLE_DOWNLOAD_CHUNK_BYTES;
constexpr size_t BLE_RESUME_HASH_CHUNK_BYTES = 512;
constexpr size_t MAX_FILENAME_BYTES = 96;
constexpr size_t BLE_HOST_ID_MAX_BYTES = 64;
constexpr size_t BLE_HOST_NAME_MAX_BYTES = 48;
constexpr size_t BLE_NONCE_BYTES = 16;
constexpr size_t BLE_PROGRESS_STATUS_INTERVAL_BYTES = 4UL * 1024UL;
constexpr size_t BLE_PROGRESS_DISPLAY_INTERVAL_BYTES = 128UL * 1024UL;
constexpr size_t BLE_FIRMWARE_PROGRESS_DISPLAY_INTERVAL_BYTES = 1024UL * 1024UL;
constexpr size_t BLE_UPLOAD_ACK_BYTES_MIN = 20;
constexpr size_t BLE_UPLOAD_ACK_BYTES_MAX = 64UL * 1024UL;
constexpr size_t MAX_QUEUED_BLE_EVENTS = 64;
constexpr size_t MAX_QUEUED_BLE_EVENT_BYTES = 8UL * 1024UL;
constexpr size_t EPUB_SUFFIX_LEN = 5;
constexpr size_t BMP_SUFFIX_LEN = 4;
constexpr size_t BIN_SUFFIX_LEN = 4;
// A GATT notification carries at most ATT_MTU-3 bytes, and the peer decides the
// MTU. Until it has exchanged one the only defensible assumption is the 23-byte
// BLE minimum -- 20 bytes of payload.
constexpr uint16_t BLE_ATT_MTU_MINIMUM = 23;
constexpr size_t BLE_ATT_NOTIFY_OVERHEAD = 3;
// What this peripheral answers an MTU exchange with. The Android client asks
// for 517, but the answer is ours to give and NimBLE refuses outright any value
// above the BLE_ATT_MTU_MAX its buffers were compiled for -- and a refused
// setMTU() leaves the previous value in place without saying so. So the
// preference is a ladder, tried richest first, and what actually took is read
// back and logged. 185 is the floor because it is what iOS settles on; below
// that there is nothing to gain over the default.
constexpr uint16_t BLE_ATT_MTU_PREFERENCES[] = {517, 256, 185};
// The advertising interval, in NimBLE's 0.625 ms units. The radio is now up for
// as long as the device is awake rather than for as long as one screen is open,
// so the interval is what decides what that costs. A reader is not a mouse: the
// phone syncs a shelf every few days, and nobody is waiting on a 30 ms
// reconnect. At ~1 s between events the radio is on for roughly 2 ms in every
// 1000 -- a 0.2% duty cycle -- and a scanning phone still finds the reader
// inside its first second or two of looking. Dropping to 30 ms would buy latency
// nothing here wants and cost ~30x the advertising energy.
constexpr uint16_t BLE_ADV_INTERVAL_MIN_UNITS = 1600;  // 1000 ms
constexpr uint16_t BLE_ADV_INTERVAL_MAX_UNITS = 2056;  // 1285 ms
// How long teardown will wait for the NimBLE host task, in 5 ms steps. Half a
// second is far longer than a clean stop needs and still bounded.
constexpr int BLE_TEARDOWN_WAIT_STEPS = 100;
constexpr unsigned long BLE_TEARDOWN_WAIT_STEP_MS = 5;
// Even when the peer grants 517 the status notification stays inside this. It is
// ATT_MTU-3 for the ~185-byte MTU that iOS and most Android stacks settle on, so
// the doorbell survives a re-negotiation downwards, a stack that reports the MTU
// it asked for rather than the one in force, and a reconnect that never
// exchanges at all. The whole document is on the READ; there is nothing to gain
// by filling 514 bytes of notification with it.
constexpr size_t BLE_STATUS_NOTIFY_MAX_BYTES = 180;
// The ATT ceiling on a single attribute value (Bluetooth Core, Vol 3 Part F).
// A characteristic value longer than this cannot be stored or served whole, so a
// READ document that exceeds it comes back truncated -- which is invalid JSON,
// and which the app reports as "reader returned an unreadable status". The
// notify path has always been bounded; the read path was not, and had been over
// this line at 533 bytes before `settings` was added to the capability lists.
constexpr size_t BLE_ATT_ATTR_MAX_BYTES = 512;
// Shrink levels for a NOTIFY document, richest first. Each level drops the next
// least useful group of fields; level 0 is `{"state":"..."}` alone, and the
// floor below that is the empty object. Nothing is ever cut mid-string.
//   5  everything a notification may carry
//   4  - protocol_version, store_supported, clock_supported, device_time
//   3  - trusted_host, paired, pairing, mode, name, path
//   2  - the pending block shrinks to the `req`/`op` an answer must quote back
//   1  - the transfer counters and the error text
//   0  - the pending block
// Below level 0 there is no document at all. An empty object would be worse
// than sending nothing: it parses, so the
// client accepts it as a status, finds no `state` in it, and reports the
// session unreadable. A notification is a doorbell for a read that always has
// the whole truth, so when even {"state":"..."} will not fit the link, the
// doorbell is skipped and the read stands.
// The two things a live session cannot lose sit at the bottom of the order on
// purpose. `received` IS the credit ack an upload waits on (see onDataWrite), so
// a notification that drops it stalls the transfer. The `pending` geometry is
// what the app builds its answer from, so it stays whole down to level 3 -- at
// 180 bytes every real store request still fits there, and only a book arriving
// while a fetch is outstanding pushes as far as level 2.
constexpr unsigned STATUS_DETAIL_MAX = 5;
// The cap must itself be sendable on the best link this server will ever ask
// for, or "never truncate" is a promise the code cannot keep.
static_assert(BLE_STATUS_NOTIFY_MAX_BYTES <= 517 - BLE_ATT_NOTIFY_OVERHEAD,
              "the notify cap must fit the largest MTU this server asks for");

static_assert(BleLink::NO_CONNECTION == BLE_HS_CONN_HANDLE_NONE, "NO_CONNECTION must match NimBLE");

constexpr int BLE_PROTOCOL_VERSION = 2;
constexpr size_t BLE_CLIENT_NONCE_HEX_CHARS = 32;
constexpr size_t BLE_HMAC_HEX_CHARS = 64;
constexpr size_t BLE_FIRMWARE_SIGNATURE_MAX_HEX_CHARS = 256;
constexpr size_t BLE_BUILD_STAMP_CHARS = 13;  // yyyyMMdd.HHmm
constexpr uint32_t BLE_PASSKEY_RANGE = 1000000UL;
constexpr const char* HOST_PROOF_PREFIX = "X4AUTH2|host|";
constexpr const char* READER_PROOF_PREFIX = "X4AUTH2|reader|";

// Six digits from esp_random(), without modulo bias. Only called while the
// radio is on, when esp_random() is a true RNG.
uint32_t makePasskey() {
  constexpr uint32_t limit = UINT32_MAX - (UINT32_MAX % BLE_PASSKEY_RANGE);
  uint32_t value = esp_random();
  while (value >= limit) value = esp_random();
  return value % BLE_PASSKEY_RANGE;
}

std::string bytesToHex(const uint8_t* data, const size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

std::string makeDeviceId() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  return bytesToHex(mac, sizeof(mac));
}

std::string makeNonceHex() {
  uint8_t nonce[BLE_NONCE_BYTES] = {};
  for (auto& byte : nonce) byte = static_cast<uint8_t>(esp_random() & 0xFF);
  return bytesToHex(nonce, sizeof(nonce));
}

std::string toLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool isHexString(const std::string& value, const size_t length) {
  if (value.length() != length) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

bool isHexSha256(const std::string& value) { return isHexString(value, 64); }

bool isLowerHex(const std::string& value, const size_t length) {
  if (value.length() != length) return false;
  return std::all_of(value.begin(), value.end(),
                     [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Lowercase hex of exactly out.size() bytes.
bool hexToBytes(const std::string& hex, std::array<uint8_t, BleTrustedHostStore::SECRET_BYTES>& out) {
  if (hex.size() != out.size() * 2) return false;
  for (size_t i = 0; i < out.size(); i++) {
    const int high = hexNibble(hex[i * 2]);
    const int low = hexNibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool isBuildStamp(const std::string& value) {
  if (value.size() != BLE_BUILD_STAMP_CHARS || value[8] != '.') return false;
  for (size_t i = 0; i < value.size(); i++) {
    if (i != 8 && !std::isdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool isFirmwareSignatureHex(const std::string& value) {
  return !value.empty() && value.size() <= BLE_FIRMWARE_SIGNATURE_MAX_HEX_CHARS && value.size() % 2 == 0 &&
         isLowerHex(value, value.size());
}

bool endsWithSuffix(const std::string& value, const char* suffix, const size_t suffixLen) {
  if (value.length() < suffixLen) return false;
  return toLowerAscii(value.substr(value.length() - suffixLen)) == suffix;
}

bool isSafeBleFileName(const std::string& value) {
  if (value.empty() || value.length() > MAX_FILENAME_BYTES || value[0] == '.') return false;
  for (const char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool isSafeHostId(const std::string& value) {
  if (value.empty() || value.length() > BLE_HOST_ID_MAX_BYTES) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '-' || c == '_';
  });
}

std::string sanitizeHostName(std::string value) {
  if (value.empty()) return "Trusted host";
  if (value.length() > BLE_HOST_NAME_MAX_BYTES) value.resize(BLE_HOST_NAME_MAX_BYTES);
  for (char& c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 32 || uc > 126) c = '?';
  }
  return value;
}

bool isSafeBleBookName(const std::string& value) {
  return isSafeBleFileName(value) && endsWithSuffix(value, ".epub", EPUB_SUFFIX_LEN);
}

bool isSafeBleBmpName(const std::string& value) {
  return isSafeBleFileName(value) && endsWithSuffix(value, ".bmp", BMP_SUFFIX_LEN);
}

bool isSafeBleFirmwareName(const std::string& value) {
  return isSafeBleFileName(value) && endsWithSuffix(value, ".bin", BIN_SUFFIX_LEN);
}

// A path relative to the books root, as the `library` listing emits it
// ("Sub/Folder/Book.epub"). Deliberately more permissive than
// isSafeBleFileName(): that guards a name the client invents for a new file,
// whereas this must accept every name already on the card, including UTF-8 and
// the punctuation real book titles carry. What it does not accept is anything
// that could leave /Books or name a hidden entry.
bool isSafeBleBookRelativePath(const std::string& value) {
  if (value.empty() || value.length() > MAX_BOOK_PATH_BYTES) return false;
  size_t segmentStart = 0;
  for (size_t i = 0; i <= value.size(); i++) {
    if (i < value.size()) {
      const auto uc = static_cast<unsigned char>(value[i]);
      if (uc < 0x20 || uc == 0x7F) return false;  // control characters
      if (value[i] == '\\') return false;         // never a separator here; confuses hosts
      if (value[i] != '/') continue;
    }
    // An empty segment is a leading '/', a trailing '/', or "//".
    if (i == segmentStart) return false;
    // Rejects "." and ".." -- which would walk out of the books root -- along
    // with the reader's own dot-caches and host-OS litter, none of which the
    // listing ever emits.
    if (value[segmentStart] == '.') return false;
    segmentStart = i + 1;
  }
  return true;
}

// The `position` of a book start_put: one `progress` batch entry without
// `filename`. Unlike a batch entry, anything malformed refuses it outright.
struct BookPosition {
  std::string location;
  uint32_t timestamp = 0;
  BookProgressSync::SpineJump jump;
  uint16_t percentBp = 0;
};

bool parseBookPosition(const JsonVariantConst position, const std::string& fileName, BookPosition& out) {
  if (!position.is<JsonObjectConst>()) return false;

  if (!position["timestamp"].is<int64_t>()) return false;
  const int64_t timestamp = position["timestamp"].as<int64_t>();
  if (timestamp <= 0 || timestamp > static_cast<int64_t>(UINT32_MAX) ||
      !HalClock::isPlausibleEpoch(static_cast<uint32_t>(timestamp))) {
    return false;
  }
  out.timestamp = static_cast<uint32_t>(timestamp);

  const bool hasSpine = !position["spine"].isNull() || !position["spine_n"].isNull();
  if (hasSpine) {
    if (!position["spine"].is<int>() || !position["spine_n"].is<int>()) return false;
    if (!position["spine_frac"].isNull() && !position["spine_frac"].is<float>()) return false;
    const int spineIndex = position["spine"].as<int>();
    const int spineCount = position["spine_n"].as<int>();
    const float fraction = position["spine_frac"] | 0.0f;
    if (spineIndex < 0 || spineCount <= 0 || spineIndex >= spineCount || spineCount > UINT16_MAX ||
        !(fraction >= 0.0f && fraction <= 1.0f)) {
      return false;
    }
    out.jump.present = true;
    out.jump.spineIndex = static_cast<uint16_t>(spineIndex);
    out.jump.fraction = fraction;
    out.jump.spineCount = static_cast<uint16_t>(spineCount);
  } else if (!position["spine_frac"].isNull()) {
    return false;
  }

  if (!position["location"].isNull()) {
    if (!position["location"].is<const char*>()) return false;
    out.location = toLowerAscii(position["location"].as<const char*>());
    if (!out.location.empty()) {
      uint8_t bytes[BookProgressSync::MAX_PROGRESS_BYTES] = {};
      size_t len = 0;
      if (!BookProgressSync::decodeLocation(out.location, bytes, len) ||
          !BookProgressSync::isValidLocationLength(fileName, len)) {
        return false;
      }
    }
  }
  if (out.location.empty() && !out.jump.present) return false;

  if (!position["pct"].isNull()) {
    if (!position["pct"].is<float>()) return false;
    const float pct = position["pct"].as<float>();
    if (!(pct >= 0.0f && pct <= 1.0f)) return false;
    out.percentBp = static_cast<uint16_t>(pct * 10000.0f + 0.5f);
  }
  return true;
}

// Pulls one top-level object at a time out of a JSON array held in a file.
//
// The batch is parsed incrementally on purpose: a few thousand entries is
// hundreds of kilobytes, and a reading session has no such heap to spare. Only
// the current object's text is in RAM, capped at MAX_PROGRESS_ENTRY_BYTES, and
// ArduinoJson is handed that one object rather than the document.
//
// This is a brace matcher, not a JSON parser -- it only needs to find where each
// object ends, which means tracking strings and their escapes so a '}' inside a
// filename does not end the object early. Everything inside is then parsed
// properly by ArduinoJson, which is what rejects malformed entries.
class ProgressBatchReader {
 public:
  enum class Next { OBJECT, END, PARSE_ERROR };

  explicit ProgressBatchReader(HalFile& file) : file_(file) {}

  Next next(std::string& objectText) {
    objectText.clear();
    if (!sawArrayStart_) {
      if (skipWhitespace() != '[') return Next::PARSE_ERROR;
      sawArrayStart_ = true;
    }
    int c = skipWhitespace();
    if (c == ']') return Next::END;
    if (!firstEntry_) {
      if (c != ',') return Next::PARSE_ERROR;
      c = skipWhitespace();
    }
    firstEntry_ = false;
    if (c != '{') return Next::PARSE_ERROR;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    while (true) {
      if (objectText.size() >= MAX_PROGRESS_ENTRY_BYTES) return Next::PARSE_ERROR;
      objectText.push_back(static_cast<char>(c));
      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          inString = false;
        }
      } else if (c == '"') {
        inString = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        depth--;
        if (depth == 0) return Next::OBJECT;
      }
      c = readByte();
      if (c < 0) return Next::PARSE_ERROR;
    }
  }

 private:
  // Buffered: reading a few hundred entries one SD call per byte would be
  // minutes of SPI overhead.
  int readByte() {
    if (bufferPos_ >= bufferLen_) {
      const int read = file_.read(buffer_.data(), buffer_.size());
      if (read <= 0) return -1;
      bufferLen_ = static_cast<size_t>(read);
      bufferPos_ = 0;
    }
    return buffer_[bufferPos_++];
  }

  int skipWhitespace() {
    while (true) {
      const int c = readByte();
      if (c < 0) return -1;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      return c;
    }
  }

  HalFile& file_;
  std::array<uint8_t, 256> buffer_ = {};
  size_t bufferLen_ = 0;
  size_t bufferPos_ = 0;
  bool sawArrayStart_ = false;
  bool firstEntry_ = true;
};

std::string transferKindName(const BleLink::TransferKind kind) {
  switch (kind) {
    case BleLink::TransferKind::BOOK:
      return "book";
    case BleLink::TransferKind::BMP:
      return "bmp";
    case BleLink::TransferKind::FIRMWARE:
      return "firmware";
    case BleLink::TransferKind::PROGRESS:
      return "progress";
    case BleLink::TransferKind::PROGRESS_RESULT:
      return "progress_result";
    case BleLink::TransferKind::CRASH_REPORT:
      return "crash_report";
    case BleLink::TransferKind::LIBRARY:
      return "library";
    case BleLink::TransferKind::ABOUT:
      return "about";
    case BleLink::TransferKind::CATALOG_PAGE:
      return "catalog_page";
    case BleLink::TransferKind::CATALOG_DETAIL:
      return "catalog_detail";
    case BleLink::TransferKind::NONE:
      return "";
  }
  return "";
}

std::string sha256ToHex(const uint8_t digest[32]) { return bytesToHex(digest, 32); }

std::string hmacSha256Hex(const std::array<uint8_t, BleTrustedHostStore::SECRET_BYTES>& key,
                          const std::string& message) {
  uint8_t output[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return "";
  const int ret = mbedtls_md_hmac(md, key.data(), key.size(), reinterpret_cast<const uint8_t*>(message.data()),
                                  message.size(), output);
  if (ret != 0) return "";
  return bytesToHex(output, sizeof(output));
}

std::string stateName(BleLink::State state) {
  switch (state) {
    case BleLink::State::STARTING:
      return "starting";
    case BleLink::State::ADVERTISING:
      return "advertising";
    case BleLink::State::CONNECTED:
      return "connected";
    case BleLink::State::RECEIVING:
      return "receiving";
    case BleLink::State::VERIFYING:
      return "verifying";
    case BleLink::State::SAVED:
      return "saved";
    case BleLink::State::PREPARING:
      return "preparing";
    case BleLink::State::SENDING:
      return "sending";
    case BleLink::State::SENT:
      return "sent";
    case BleLink::State::ERROR:
      return "error";
  }
  return "unknown";
}

uint32_t readLe32(const std::string& value) {
  assert(value.size() >= sizeof(uint32_t));
  const auto* b = reinterpret_cast<const uint8_t*>(value.data());
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < left.size(); i++) {
    diff |= static_cast<uint8_t>(left[i]) ^ static_cast<uint8_t>(right[i]);
  }
  return diff == 0;
}

bool hashExistingPrefix(const std::string& path, size_t bytes, mbedtls_sha256_context& context) {
  HalFile file;
  if (!Storage.openFileForRead("BLE", path, file)) return false;

  std::array<uint8_t, BLE_RESUME_HASH_CHUNK_BYTES> buffer = {};
  while (bytes > 0) {
    const size_t wanted = std::min(bytes, buffer.size());
    const int read = file.read(buffer.data(), wanted);
    if (read <= 0) {
      file.close();
      return false;
    }
    mbedtls_sha256_update(&context, buffer.data(), static_cast<size_t>(read));
    bytes -= static_cast<size_t>(read);
  }

  file.close();
  return true;
}

bool linkIsSecure(const NimBLEConnInfo& connInfo) {
  return connInfo.isEncrypted() && connInfo.isAuthenticated() && connInfo.isBonded();
}

// Runs on the NimBLE host task. Every event is bound to the one connection the
// link holds; events for any other handle are ignored.
class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleLink& link) : link_(link) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    const uint16_t handle = connInfo.getConnHandle();
    if (!link_.bindConnection(handle)) {
      LOG_ERR("BLE", "refusing a second connection (%u)", static_cast<unsigned>(handle));
      server->disconnect(handle);
      return;
    }
    LOG_INF("BLE", "connected (handle=%u encrypted=%d bonded=%d)", static_cast<unsigned>(handle),
            connInfo.isEncrypted(), connInfo.isBonded());
    // 7.5-15 ms interval, no slave latency, 4 s supervision timeout.
    //
    // The timeout was 1.2 s (120 units). That is legal but tight: it is the
    // window in which the link layer's own heartbeat -- a packet every
    // connection interval, empty if there is nothing to say -- must succeed at
    // least once, and phones deprioritise BLE scheduling routinely for Wi-Fi
    // coexistence and doze. A gap that costs nothing at 4 s dropped the link at
    // 1.2 s, and a dropped link is what the app then has to notice, reconnect
    // and re-authenticate through.
    //
    // Latency stays 0: the peripheral answers every event, which is what keeps
    // a notification prompt and a transfer fast. The cost is the modem floor
    // while awake, which is already the price of the radio being always on.
    server->updateConnParams(handle, 6, 12, 0, 400);
    server->setDataLen(handle, 251);
    // BLE 5.0 2M PHY: double the symbol rate, which is the only throughput lever
    // left on this link. The interval is already at the 7.5 ms spec minimum and
    // the PDU is already the 251-byte maximum, so everything else is spent.
    //
    // Both masks are offered rather than 2M alone: a peer that cannot do 2M then
    // negotiates 1M instead of failing the procedure. Nothing depends on the
    // outcome -- it is a speed optimisation, and a phone that stays on 1M simply
    // transfers at the 1M rate. onPhyUpdate logs what was actually agreed.
    server->updatePhy(handle, BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
                      BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK, 0);
    // Still the 23-byte default at this point on most stacks; onMTUChange
    // corrects it a moment later. Recorded either way so a peer that never
    // exchanges is sized for honestly rather than optimistically.
    link_.noteBleMtu(connInfo.getMTU());
    link_.enqueueBleConnected(handle);
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
    if (connInfo.getConnHandle() == link_.boundConnection()) link_.noteBleMtu(mtu);
  }

  void onPhyUpdate(NimBLEConnInfo&, const uint8_t txPhy, const uint8_t rxPhy) override {
    // Logged because it is otherwise invisible: a transfer that runs at half the
    // expected rate looks like a slow phone rather than a link that quietly
    // stayed on 1M.
    LOG_INF("BLE", "PHY now tx=%s rx=%s", txPhy == BLE_GAP_LE_PHY_2M ? "2M" : "1M",
            rxPhy == BLE_GAP_LE_PHY_2M ? "2M" : "1M");
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& connInfo, int reason) override {
    const uint16_t handle = connInfo.getConnHandle();
    LOG_INF("BLE", "disconnected (handle=%u reason=0x%x)", static_cast<unsigned>(handle),
            static_cast<unsigned>(reason));
    if (!link_.releaseConnection(handle)) return;
    link_.noteBleMtu(0);
    link_.enqueueBleDisconnected(handle);
  }

  // Called for each pairing attempt. NimBLE passes no connection; with one
  // connection allowed it is the bound one. A closed window still gets a random
  // passkey (the stack injects whatever is returned) and loses the link.
  uint32_t onPassKeyDisplay() override {
    const uint32_t passkey = makePasskey();
    const uint16_t handle = link_.boundConnection();
    if (handle == BleLink::NO_CONNECTION) return passkey;
    if (!link_.pairingWindowOpen()) {
      LOG_INF("BLE", "pairing refused: the pairing window is closed");
      NimBLEDevice::getServer()->disconnect(handle);
      return passkey;
    }
    link_.notePairingStarted(passkey);
    LOG_INF("BLE", "pairing started: passkey shown");
    return passkey;
  }

  // Fires on every encryption change, successful or not.
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    const uint16_t handle = connInfo.getConnHandle();
    if (handle != link_.boundConnection()) return;

    const bool newBond = link_.takePairingStarted();
    const bool windowOpen = link_.pairingWindowOpen();
    const bool secure = linkIsSecure(connInfo);
    // Encrypted but unauthenticated is Just Works: a pairing attempt too.
    const bool attempted = newBond || (connInfo.isEncrypted() && !connInfo.isAuthenticated());
    // A bond without a passkey on this connection is one the reader already had.
    const bool accepted = secure && (!newBond || windowOpen);

    if (!accepted) {
      if (attempted && windowOpen) link_.notePairingFailed();
      if (connInfo.isEncrypted()) {
        // The keys this pairing produced must not outlive the connection.
        NimBLEDevice::deleteBond(connInfo.getIdAddress());
      }
      LOG_INF("BLE", "link refused (encrypted=%d authenticated=%d bonded=%d new=%d window=%d)",
              connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded(), newBond, windowOpen);
      NimBLEDevice::getServer()->disconnect(handle);
      link_.enqueueSecurityResult(handle, false, newBond, {});
      return;
    }

    LOG_INF("BLE", "link accepted (encrypted=%d authenticated=%d bonded=%d new=%d window=%d)",
            connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded(), newBond, windowOpen);
    link_.markConnectionSecure();
    std::string peerIdAddress;
    if (newBond) {
      const NimBLEAddress id = connInfo.getIdAddress();
      peerIdAddress.assign(reinterpret_cast<const char*>(id.getBase()), sizeof(ble_addr_t));
    }
    link_.enqueueSecurityResult(handle, true, newBond, peerIdAddress);
  }

 private:
  BleLink& link_;
};

// Writes count only from the bound connection once it is secure.
class ControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleLink& link) : link_(link) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    const uint16_t handle = connInfo.getConnHandle();
    if (!linkIsSecure(connInfo) || !link_.boundConnectionSecure(handle)) {
      LOG_INF("BLE", "control write ignored (encrypted=%d authenticated=%d bonded=%d handle=%u bound=%u)",
              connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded(), static_cast<unsigned>(handle),
              static_cast<unsigned>(link_.boundConnection()));
      return;
    }
    link_.enqueueControlWrite(handle, characteristic->getValue());
  }

 private:
  BleLink& link_;
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit DataCallbacks(BleLink& link) : link_(link) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    const uint16_t handle = connInfo.getConnHandle();
    if (!linkIsSecure(connInfo) || !link_.boundConnectionSecure(handle)) return;
    link_.enqueueDataWrite(handle, characteristic->getValue());
  }

 private:
  BleLink& link_;
};

}  // namespace

struct BleLinkRuntime {
  explicit BleLinkRuntime(BleLink& owner)
      : link(owner), serverCallbacks(owner), controlCallbacks(owner), dataCallbacks(owner) {}

  BleLink& link;
  NimBLEServer* server = nullptr;
  NimBLEService* service = nullptr;
  NimBLECharacteristic* control = nullptr;
  NimBLECharacteristic* dataIn = nullptr;
  NimBLECharacteristic* status = nullptr;
  NimBLECharacteristic* dataOut = nullptr;
  ServerCallbacks serverCallbacks;
  ControlCallbacks controlCallbacks;
  DataCallbacks dataCallbacks;

  bool begin() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    // LE Secure Connections with bonding and MITM protection by passkey entry:
    // the reader displays the passkey, the phone types it.
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    // The radio is on now, so esp_random() draws from the true RNG.
    link.deviceNonce_ = makeNonceHex();
    // Before the server starts, and before any peer can connect: the preferred
    // MTU is what an exchange is answered with, and NimBLE latches it into a
    // connection when the connection is made. Setting it after a peer is on the
    // link changes nothing for that peer.
    for (const uint16_t wanted : BLE_ATT_MTU_PREFERENCES) {
      if (NimBLEDevice::setMTU(wanted)) break;
      LOG_DBG("BLE", "preferred ATT MTU %u refused by the stack", static_cast<unsigned>(wanted));
    }
    LOG_INF("BLE", "preferred ATT MTU is %u", static_cast<unsigned>(NimBLEDevice::getMTU()));
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks, false);

    service = server->createService(BLE_SERVICE_UUID);
    if (!service) return false;

    // The stack refuses these reads and writes on a link that is not encrypted
    // with an authenticated key; the callbacks check the bond as well.
    control = service->createCharacteristic(
        BLE_CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
    dataIn = service->createCharacteristic(BLE_DATA_IN_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                                                                 NIMBLE_PROPERTY::WRITE_ENC |
                                                                 NIMBLE_PROPERTY::WRITE_AUTHEN);
    status = service->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                                                                NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
    dataOut = service->createCharacteristic(BLE_DATA_OUT_UUID, NIMBLE_PROPERTY::NOTIFY);
    if (!control || !dataIn || !status || !dataOut) return false;

    control->setCallbacks(&controlCallbacks);
    dataIn->setCallbacks(&dataCallbacks);
    // The stored value is the authoritative document from the first moment: a
    // client that reads before it ever sees a notification still gets the whole
    // truth.
    status->setValue(link.buildReadJson());

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setName(BLE_DEVICE_NAME);
    advertising->setMinInterval(BLE_ADV_INTERVAL_MIN_UNITS);
    advertising->setMaxInterval(BLE_ADV_INTERVAL_MAX_UNITS);
    advertising->start();
    return true;
  }

  // The ATT MTU actually in force on the live link. onMTUChange() is a *report*
  // that an exchange happened, not the source of truth, and treating it as the
  // truth is what pinned this server at 20 usable bytes: a peer that exchanges
  // before the server callbacks are attached, a stack that raises no event, or a
  // cached value cleared on disconnect all leave it reading as the 23-byte floor
  // while the connection is carrying hundreds. ble_att_mtu(), behind
  // getPeerMTU(), is the number the ATT layer will use for the next PDU, so ask
  // that and keep the callback only as a fallback for the moment between connect
  // and the first exchange.
  uint16_t peerMtu() const {
    if (!server) return 0;
    uint16_t best = 0;
    for (const uint16_t handle : server->getPeerDevices()) {
      const uint16_t mtu = server->getPeerMTU(handle);
      if (mtu > best) best = mtu;
    }
    return best;
  }

  bool hasPeer() const { return server != nullptr && server->getConnectedCount() > 0; }

  void publish(const std::string& readJson, const std::string& notifyJson) {
    if (!status) return;
    // Two different payloads on one characteristic. setValue() is what a GATT
    // read returns; notify(buffer, length) sends *that* buffer instead of the
    // stored value, so the doorbell can be small while the read stays whole.
    // Confirmed present in the pinned NimBLE-Arduino:
    //   bool notify(const uint8_t* value, size_t length, uint16_t connHandle) const
    //
    // The stored value is refreshed whether or not anyone is listening: it costs
    // nothing and it means the next client to read gets the truth immediately
    // rather than waiting for the next thing to happen.
    status->setValue(readJson);

    const size_t notifyCap = link.notifyCapBytes();
    const uint16_t mtu = peerMtu();
    const uint16_t handle = link.boundConnection();
    if (!hasPeer() || !link.boundConnectionSecure(handle)) {
      // Nobody to ring, or a link that is not yet encrypted, authenticated and
      // bonded: treated as not subscribed.
      LOG_DBG("BLE", "status: read %u bytes, no secure peer -- notify skipped",
              static_cast<unsigned>(readJson.size()));
      return;
    }
    if (notifyJson.empty()) {
      // buildNotifyJson() could not fit even {"state":"..."} in the cap. See the
      // shrink ladder: an empty object is a well-formed lie and a skipped
      // notification is not, and the whole document is one GATT read away.
      LOG_DBG("BLE", "status: read %u bytes, cap %u (mtu %u) -- too small to notify",
              static_cast<unsigned>(readJson.size()), static_cast<unsigned>(notifyCap),
              static_cast<unsigned>(mtu));
      return;
    }
    LOG_DBG("BLE", "status: notify %u bytes, read %u bytes, cap %u (mtu %u)",
            static_cast<unsigned>(notifyJson.size()), static_cast<unsigned>(readJson.size()),
            static_cast<unsigned>(notifyCap), static_cast<unsigned>(mtu));
    status->notify(reinterpret_cast<const uint8_t*>(notifyJson.data()), notifyJson.size(), handle);
  }

  void notifyData(const uint8_t* data, const size_t length) {
    if (!dataOut) return;
    const uint16_t handle = link.boundConnection();
    if (!link.boundConnectionSecure(handle)) return;
    const size_t notifyCap = link.notifyCapBytes();
    // The client picks the chunk size and its resume arithmetic depends on it,
    // so this is never silently shrunk -- but a frame the link cannot carry is
    // the same class of bug as an overlong status, and must not be silent.
    if (length > notifyCap) {
      LOG_DBG("BLE", "data frame %u bytes exceeds notify cap %u", static_cast<unsigned>(length),
              static_cast<unsigned>(notifyCap));
    }
    dataOut->notify(data, length, handle);
  }

  void disconnect(const uint16_t handle) const {
    if (server) server->disconnect(handle);
  }

  void startAdvertising() {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising) advertising->start();
  }

  // Teardown runs on the main loop task while the NimBLE host task is still
  // live on the other core, so the order below is the whole point of it.
  //
  // THE CRASH THIS FIXES. NimBLEDevice::deinit() is nimble_port_stop() followed
  // immediately by nimble_port_deinit(). nimble_port_stop() returns as soon as
  // its stop event has been *dispatched* by the host task -- not when that task
  // has left nimble_port_run(). nimble_port_deinit() then frees the default
  // event queue (vQueueDelete on g_eventq_dflt) and deinits the controller,
  // while the host task may still be going round its loop reading that queue.
  // Every event still in flight when deinit() is called widens that window,
  // and an advertising restart or a live connection is exactly such an event.
  // So: stop making work, wait for the host to go quiet, and only then deinit.
  void end() {
    if (server) {
      // Nothing may re-enter this runtime or the link from the host task once
      // end() returns: the callback objects are members of this struct and the
      // link's event queue is torn down moments later. Detaching first is
      // what makes that safe rather than merely likely -- NimBLE swaps in its own
      // do-nothing defaults when handed nullptr.
      server->setCallbacks(nullptr, false);
      // Otherwise the disconnect below immediately re-arms the advertiser, which
      // is one more thing the host task has to unwind while deinit() runs.
      server->advertiseOnDisconnect(false);
      for (const uint16_t handle : server->getPeerDevices()) server->disconnect(handle);
    }
    for (NimBLECharacteristic* characteristic : {control, dataIn, status, dataOut}) {
      if (characteristic) characteristic->setCallbacks(nullptr);
    }
    NimBLEDevice::stopAdvertising();
    waitForHostQuiet();
    NimBLEDevice::deinit(true);
    waitForHostTaskGone();
    server = nullptr;
    service = nullptr;
    control = nullptr;
    dataIn = nullptr;
    status = nullptr;
    dataOut = nullptr;
  }

  // Wait for the advertiser to be down and the last peer gone, so the host task
  // has nothing left queued when deinit() pulls the queue out from under it.
  void waitForHostQuiet() const {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    for (int step = 0; step < BLE_TEARDOWN_WAIT_STEPS; step++) {
      const bool stillAdvertising = advertising != nullptr && advertising->isAdvertising();
      if (!stillAdvertising && !hasPeer()) return;
      resetTaskWatchdogIfSubscribed();
      delay(BLE_TEARDOWN_WAIT_STEP_MS);
    }
    LOG_DBG("BLE", "host still busy at teardown; deinitialising anyway");
  }

  // And wait for the task itself to be gone before this runtime -- and with it
  // the callback objects and the link that owns them -- is freed.
  static void waitForHostTaskGone() {
    for (int step = 0; step < BLE_TEARDOWN_WAIT_STEPS; step++) {
      if (xTaskGetHandle("nimble_host") == nullptr) return;
      resetTaskWatchdogIfSubscribed();
      delay(BLE_TEARDOWN_WAIT_STEP_MS);
    }
    LOG_DBG("BLE", "nimble host task outlived deinit");
  }
};

BleLink& BleLink::getInstance() {
  static BleLink instance;
  return instance;
}

void BleLink::begin() {
  if (ble_) return;

  // A wake from deep sleep is a chip reset, so this IS the "just woke up" path.
  // Take the snapshot now so the first status a phone reads or is notified with
  // already describes the library and the position, rather than being empty
  // until the first heartbeat sixty seconds later.
  refreshPingSnapshot(true);

  if (!eventMutex_) {
    eventMutex_ = xSemaphoreCreateMutex();
    if (!eventMutex_) {
      LOG_ERR("BLE", "could not create the BLE event mutex; the link stays down");
      return;
    }
  }

  deviceId_ = makeDeviceId();
  // Set by BleLinkRuntime::begin() once the radio is up.
  deviceNonce_.clear();
  BLE_TRUSTED_HOSTS.load();
  connHandle_.store(NO_CONNECTION);
  linkSecure_.store(false);
  pairingInProgress_.store(false);
  helloAccepted_ = false;
  authHandle_ = NO_CONNECTION;
  mbedtls_sha256_init(&shaContext_);
  state_ = State::STARTING;
  errorMessage_.clear();
  authErrorMessage_.clear();

  ble_ = makeUniqueNoThrow<BleLinkRuntime>(*this);
  if (!ble_) {
    LOG_ERR("BLE", "OOM: BLE runtime");
    setError("Could not start BLE");
    return;
  }
  if (!ble_->begin()) {
    ble_.reset();
    setError("Could not start BLE");
    return;
  }

  LOG_INF("BLE", "advertising as '%s' (paired: %s)", BLE_DEVICE_NAME, BLE_TRUSTED_HOSTS.hasHosts() ? "yes" : "no");
  setState(State::ADVERTISING);
  publishStatus();
}

void BleLink::end() {
  if (!ble_) return;
  // Close the files first, then take the radio down, and only then clear the
  // card. The order matters: clearing the staged scratch documents is a second
  // or so of SD work, and it must not happen with the server still advertising
  // and its callbacks still pointing at state on its way out. Nothing may arrive
  // over the air after this point.
  resetTransfer(true);
  ble_->end();
  ble_.reset();
  // Scratch for one wake only. The Store's own thumbnails are cleared by the
  // Store screen; these are the link's.
  if (Storage.exists(CATALOG_PATH)) Storage.remove(CATALOG_PATH);
  if (Storage.exists(CATALOG_PART_PATH)) Storage.remove(CATALOG_PART_PATH);
  if (Storage.exists(LIBRARY_INDEX_PATH)) Storage.remove(LIBRARY_INDEX_PATH);
  if (Storage.exists(PROGRESS_BATCH_PATH)) Storage.remove(PROGRESS_BATCH_PATH);
  if (Storage.exists(PROGRESS_RESULT_PATH)) Storage.remove(PROGRESS_RESULT_PATH);
  mbedtls_sha256_free(&shaContext_);
  helloAccepted_ = false;
  authHandle_ = NO_CONNECTION;
  hostPaired_ = false;
  trustedHostName_.clear();
  readerProof_.clear();
  connHandle_.store(NO_CONNECTION);
  linkSecure_.store(false);
  pairingInProgress_.store(false);
  connectedAtMs_ = 0;
  securedAtMs_ = 0;
  state_ = State::STARTING;
  LOG_INF("BLE", "link stopped");
}

void BleLink::tick() {
  // Heartbeat. Only while a phone is actually listening: notifying into an
  // empty room costs radio and tells nobody anything.
  if (isPeerConnected()) {
    const unsigned long now = millis();
    if (now - lastHeartbeatMs_ >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs_ = now;
      refreshPingSnapshot(pingLibDirty_);
      statusDirty_ = true;
    }
  }

  if (!ble_) return;

  processBleEvents();
  checkConnectionDeadlines();

  // A delete waiting on its book to close (delete_book with close:true).
  if (!pendingDeletePath_.empty()) {
    if (!activityManager.isReaderActivity()) {
      const std::string name = pendingDeleteName_;
      const std::string path = pendingDeletePath_;
      pendingDeleteName_.clear();
      pendingDeletePath_.clear();
      deleteBookNow(name, path);
    } else if (millis() - pendingDeleteAt_ > 5000) {
      pendingDeleteName_.clear();
      pendingDeletePath_.clear();
      setError("book open");
    }
  }

  if (pendingCommit_) {
    pendingCommit_ = false;
    processCommit();
    return;
  }
  // Only the Store has work of its own to do on a tick -- deadlines, republishes
  // of an outstanding request. Its input is handled by its own activity, which
  // is the thing that has a screen.
  if (store_) store_->tick();
  if (state_ == State::SENDING && downloadOpen_) {
    if (statusDirty_) publishStatus();
    pumpDownload();
    return;
  }
  if (statusDirty_) publishStatus();
}

void BleLink::deleteBookNow(const std::string& name, const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    // Already absent is the requested state, so this is a success: the app must
    // not have to distinguish "I deleted it" from "it was not there".
    LOG_INF("BLE", "delete_book: %s already absent", name.c_str());
    setState(State::SAVED);
    return;
  }
  if (!Storage.remove(path.c_str())) {
    setError("could not delete the book");
    return;
  }
  clearBookCache(path);
  HomeShelfStore::markStale();
  noteLibraryChanged();
  // ...and ask for a repaint. Marking the shelf stale only sets a flag that is
  // read while rendering, so a deleted book sat on screen until something else
  // caused a draw -- tapping the ghost row was what finally removed it.
  if (!activityManager.isReaderActivity()) activityManager.requestUpdate();
  LOG_INF("BLE", "deleted %s", name.c_str());
  setState(State::SAVED);
}

void BleLink::attachStore(BleStoreController* store) {
  store_ = store;
  storeExpectedBook_.clear();
  if (!store_) return;
  // The Store screen opened onto a link that may already be through the gate --
  // which is the entire point of the radio outliving the screen. Tell it so,
  // rather than making it wait for a reconnect that is not coming.
  if (sessionAuthenticated()) store_->onAppReady();
}

void BleLink::detachStore(const BleStoreController* store) {
  if (store_ != store) return;
  store_ = nullptr;
  storeExpectedBook_.clear();
}

void BleLink::notifyObserver() {
  if (observer_) observer_->onBleLinkChanged();
}

bool BleLink::hasTrustedHost() const { return BLE_TRUSTED_HOSTS.hasHosts(); }

std::string BleLink::trustedHostLabel() const {
  const BleTrustedHost* host = BLE_TRUSTED_HOSTS.host();
  if (!host) return {};
  return host->name.empty() ? host->hostId : host->name;
}

bool BleLink::forgetTrustedHost() {
  if (!BLE_TRUSTED_HOSTS.clearAll()) return false;
  // The bond without the host record is useless; unpairing also drops the link.
  if (ble_ && !NimBLEDevice::deleteAllBonds()) LOG_ERR("BLE", "could not delete every bond");
  helloAccepted_ = false;
  authHandle_ = NO_CONNECTION;
  trustedHostName_.clear();
  readerProof_.clear();
  hostPaired_ = false;
  authErrorMessage_.clear();
  deviceNonce_ = makeNonceHex();
  LOG_INF("BLE", "forgot the trusted host and every bond");
  setState(isPeerConnected() ? State::CONNECTED : State::ADVERTISING);
  publishStatus();
  return true;
}

void BleLink::clearAuthError() {
  if (authErrorMessage_.empty()) return;
  authErrorMessage_.clear();
  statusDirty_ = true;
  notifyObserver();
}

void BleLink::openPairingWindow() {
  // A fresh window starts with a clean attempt count; a running lockout stays.
  if (!pairingWindowRequested_.exchange(true)) failedPairings_.store(0);
  authErrorMessage_.clear();
  LOG_INF("BLE", "pairing window open");
  statusDirty_ = true;
  notifyObserver();
}

void BleLink::closePairingWindow() {
  if (pairingWindowRequested_.exchange(false)) LOG_INF("BLE", "pairing window closed");
  statusDirty_ = true;
}

bool BleLink::pairingWindowOpen() const {
  if (!pairingWindowRequested_.load()) return false;
  return !(pairingLocked_.load() && millis() - pairingLockedAtMs_.load() < PAIRING_LOCK_MS);
}

uint32_t BleLink::pairingLockSecondsLeft() const {
  if (!pairingLocked_.load()) return 0;
  const unsigned long elapsed = millis() - pairingLockedAtMs_.load();
  if (elapsed >= PAIRING_LOCK_MS) return 0;
  return static_cast<uint32_t>((PAIRING_LOCK_MS - elapsed + 999UL) / 1000UL);
}

bool BleLink::pairingPasskey(uint32_t& passkey) const {
  if (!pairingInProgress_.load() || !pairingWindowOpen()) return false;
  passkey = passkey_.load();
  return true;
}

bool BleLink::bindConnection(const uint16_t connHandle) {
  uint16_t expected = NO_CONNECTION;
  if (connHandle == NO_CONNECTION || !connHandle_.compare_exchange_strong(expected, connHandle)) return false;
  linkSecure_.store(false);
  pairingInProgress_.store(false);
  return true;
}

bool BleLink::releaseConnection(const uint16_t connHandle) {
  uint16_t expected = connHandle;
  if (connHandle == NO_CONNECTION || !connHandle_.compare_exchange_strong(expected, NO_CONNECTION)) return false;
  linkSecure_.store(false);
  // Gone with a passkey still on screen: the attempt failed.
  if (pairingInProgress_.exchange(false) && pairingWindowRequested_.load()) notePairingFailed();
  return true;
}

void BleLink::notePairingStarted(const uint32_t passkey) {
  passkey_.store(passkey);
  pairingInProgress_.store(true);
  enqueueBleEvent({BleEventType::PAIRING, {}});
}

void BleLink::notePairingFailed() {
  const auto failures = static_cast<uint8_t>(failedPairings_.load() + 1);
  failedPairings_.store(failures);
  if (failures >= MAX_FAILED_PAIRINGS) {
    pairingLockedAtMs_.store(millis());
    pairingLocked_.store(true);
    failedPairings_.store(0);
    LOG_INF("BLE", "pairing locked for %lu s after %u failed attempts", PAIRING_LOCK_MS / 1000UL,
            static_cast<unsigned>(failures));
  }
  enqueueBleEvent({BleEventType::PAIRING, {}});
}

void BleLink::checkConnectionDeadlines() {
  const unsigned long now = millis();
  if (pairingLocked_.load() && now - pairingLockedAtMs_.load() >= PAIRING_LOCK_MS) {
    pairingLocked_.store(false);
    failedPairings_.store(0);
    LOG_INF("BLE", "pairing lockout over");
    statusDirty_ = true;
    notifyObserver();
  }

  if (connHandle_.load() == NO_CONNECTION || sessionAuthenticated() || connectedAtMs_ == 0) return;
  if (linkSecure_.load()) {
    if (securedAtMs_ != 0 && now - securedAtMs_ >= HELLO_TIMEOUT_MS) disconnectPeer("no hello after encryption");
  } else if (now - connectedAtMs_ >= SECURE_LINK_TIMEOUT_MS) {
    disconnectPeer("link not secured in time");
  }
}

void BleLink::disconnectPeer(const char* reason) {
  const uint16_t handle = connHandle_.load();
  // Once per connection: the deadlines stop counting until the next connect.
  connectedAtMs_ = 0;
  securedAtMs_ = 0;
  if (!ble_ || handle == NO_CONNECTION) return;
  LOG_INF("BLE", "disconnecting: %s", reason);
  ble_->disconnect(handle);
}

bool BleLink::isPeerConnected() const { return ble_ && ble_->hasPeer(); }

void BleLink::publishStatusNow() {
  statusDirty_ = true;
  publishStatus();
  notifyObserver();
}

void BleLink::enqueueBleEvent(BleEvent event) {
  if (!eventMutex_) return;
  const size_t eventBytes = event.value.size();
  xSemaphoreTake(eventMutex_, portMAX_DELAY);
  if (bleEventOverflow_ || bleEvents_.size() >= MAX_QUEUED_BLE_EVENTS ||
      queuedBleEventBytes_ + eventBytes > MAX_QUEUED_BLE_EVENT_BYTES) {
    bleEventOverflow_ = true;
    queuedBleEventBytes_ = 0;
    bleEvents_.clear();
  } else {
    queuedBleEventBytes_ += eventBytes;
    bleEvents_.push_back(std::move(event));
  }
  xSemaphoreGive(eventMutex_);
}

void BleLink::enqueueBleConnected(const uint16_t connHandle) {
  BleEvent event{BleEventType::CONNECTED, {}};
  event.connHandle = connHandle;
  enqueueBleEvent(std::move(event));
}

void BleLink::enqueueBleDisconnected(const uint16_t connHandle) {
  BleEvent event{BleEventType::DISCONNECTED, {}};
  event.connHandle = connHandle;
  enqueueBleEvent(std::move(event));
}

void BleLink::enqueueSecurityResult(const uint16_t connHandle, const bool accepted, const bool newBond,
                                    const std::string& peerIdAddress) {
  BleEvent event{BleEventType::SECURITY, peerIdAddress};
  event.connHandle = connHandle;
  event.accepted = accepted;
  event.newBond = newBond;
  enqueueBleEvent(std::move(event));
}

void BleLink::enqueueControlWrite(const uint16_t connHandle, const std::string& value) {
  BleEvent event{BleEventType::CONTROL, value};
  event.connHandle = connHandle;
  enqueueBleEvent(std::move(event));
}

void BleLink::enqueueDataWrite(const uint16_t connHandle, const std::string& value) {
  BleEvent event{BleEventType::DATA, value};
  event.connHandle = connHandle;
  enqueueBleEvent(std::move(event));
}

void BleLink::processBleEvents() {
  while (true) {
    BleEvent event;
    bool hasEvent = false;
    bool hasOverflow = false;
    if (eventMutex_) {
      xSemaphoreTake(eventMutex_, portMAX_DELAY);
      if (bleEventOverflow_) {
        bleEventOverflow_ = false;
        queuedBleEventBytes_ = 0;
        bleEvents_.clear();
        hasOverflow = true;
      }
      if (!bleEvents_.empty()) {
        event = std::move(bleEvents_.front());
        queuedBleEventBytes_ -= event.value.size();
        bleEvents_.pop_front();
        hasEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }
    if (hasOverflow) {
      // Dropped events may include a connect or disconnect, so the session can no
      // longer be trusted to belong to the connection that is up.
      resetTransfer(true);
      helloAccepted_ = false;
      authHandle_ = NO_CONNECTION;
      readerProof_.clear();
      setError("BLE event queue overflow");
      disconnectPeer("event queue overflow");
      return;
    }
    if (!hasEvent) return;

    const bool fromSecureLink = event.connHandle == connHandle_.load() && linkSecure_.load();
    switch (event.type) {
      case BleEventType::CONNECTED:
        onBleConnected(event.connHandle);
        break;
      case BleEventType::DISCONNECTED:
        onBleDisconnected(event.connHandle);
        break;
      case BleEventType::SECURITY:
        onSecurityResult(event);
        break;
      case BleEventType::PAIRING:
        statusDirty_ = true;
        notifyObserver();
        break;
      case BleEventType::CONTROL:
        if (fromSecureLink) onControlWrite(event.value);
        break;
      case BleEventType::DATA:
        if (fromSecureLink) onDataWrite(event.value);
        break;
    }
  }
}

void BleLink::onBleConnected(const uint16_t connHandle) {
  // A connection that has already gone again; its disconnect follows.
  if (connHandle != connHandle_.load()) return;
  helloAccepted_ = false;
  authHandle_ = NO_CONNECTION;
  hostPaired_ = false;
  trustedHostName_.clear();
  readerProof_.clear();
  authErrorMessage_.clear();
  invalidHellos_ = 0;
  connectedAtMs_ = millis();
  securedAtMs_ = 0;
  setState(State::CONNECTED);
}

void BleLink::onSecurityResult(const BleEvent& event) {
  statusDirty_ = true;
  // The passkey, if one was showing, leaves the screen either way.
  notifyObserver();
  if (!event.accepted || event.connHandle != connHandle_.load()) return;
  securedAtMs_ = millis();
  if (event.newBond) adoptNewBond(event.value);
}

void BleLink::adoptNewBond(const std::string& peerIdAddress) {
  // One bond is enforced by the store (MYNEWT_VAL_BLE_STORE_MAX_BONDS=1): saving
  // this pairing already dropped any older bond. Deleting bonds here by address is
  // unsafe -- the stored identity address can differ from the connection's in type,
  // and ble_gap_unpair() on the new bond also terminates the link it just secured.
  // The stored host stays; `pair` replaces it.
  (void)peerIdAddress;
  failedPairings_.store(0);
  LOG_INF("BLE", "new bond adopted (bonds stored: %d, host stored: %s)", NimBLEDevice::getNumBonds(),
          BLE_TRUSTED_HOSTS.hasHosts() ? "yes" : "no");
}

void BleLink::onBleDisconnected(const uint16_t) {
  // The Store is live or it is nothing: with the link gone there is no
  // catalogue to show, so it drops what it had rather than leaving a page on
  // screen that no longer describes anything reachable.
  if (store_) store_->onAppGone();

  if (transferOpen_ || downloadOpen_) {
    const bool keepPartialUpload = transferOpen_ && uploadResumable_;
    resetTransfer(!keepPartialUpload);
    if (keepPartialUpload) {
      helloAccepted_ = false;
      authHandle_ = NO_CONNECTION;
      hostPaired_ = false;
      trustedHostName_.clear();
      readerProof_.clear();
      connectedAtMs_ = 0;
      securedAtMs_ = 0;
      deviceNonce_ = makeNonceHex();
      setState(State::ADVERTISING);
      if (ble_) ble_->startAdvertising();
      return;
    }
    // Report it, then fall through to the same cleanup every disconnect gets.
    // Returning here left the session believing it was still authenticated and,
    // worse, never restarted advertising -- so the reader went invisible to the
    // phone while its own header still showed BLE, and only a reboot fixed it.
    // A disconnect during a transfer is still a disconnect.
    setError("client disconnected");
  }
  helloAccepted_ = false;
  authHandle_ = NO_CONNECTION;
  hostPaired_ = false;
  trustedHostName_.clear();
  readerProof_.clear();
  connectedAtMs_ = 0;
  securedAtMs_ = 0;
  deviceNonce_ = makeNonceHex();
  setState(State::ADVERTISING);
  if (ble_) ble_->startAdvertising();
  // The header carries the BLE mark; with the peer gone it must stop saying so.
  if (!activityManager.isReaderActivity()) activityManager.requestUpdate();
}

void BleLink::onControlWrite(const std::string& value) {
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, value.data(), value.size());
  if (parseError) {
    setError("invalid control JSON");
    return;
  }

  const std::string op = doc["op"] | "";
  if (op == "hello") {
    // Mutual HMAC over the reader's nonce D, the client's nonce C, the host id H
    // and the device id I, keyed with the 32 raw secret bytes.
    const int version = doc["version"] | 0;
    const std::string hostId = doc["host_id"] | "";
    const std::string clientNonce = doc["client_nonce"] | "";
    const std::string response = doc["response"] | "";
    // Host ids are identifiers, not secrets; the first 8 characters are enough to
    // tell whether the phone and the reader agree on who is paired.
    const auto logHello = [&](const char* outcome) {
      const BleTrustedHost* stored = BLE_TRUSTED_HOSTS.host();
      LOG_INF("BLE", "hello %s (offered host %.8s, host stored: %s %.8s)", outcome,
              isSafeHostId(hostId) ? hostId.c_str() : "?", stored ? "yes" : "no",
              stored ? stored->hostId.c_str() : "");
    };
    const auto refuse = [&](const char* reason) {
      logHello(reason);
      refuseHello(reason);
    };
    if (version != BLE_PROTOCOL_VERSION) {
      refuse("unsupported protocol version");
      return;
    }
    if (!isSafeHostId(hostId) || !isLowerHex(clientNonce, BLE_CLIENT_NONCE_HEX_CHARS) ||
        !isLowerHex(response, BLE_HMAC_HEX_CHARS)) {
      refuse("invalid hello");
      return;
    }
    const BleTrustedHost* host = BLE_TRUSTED_HOSTS.findHost(hostId);
    if (!host) {
      refuse("unknown trusted host");
      return;
    }
    const std::string fields = deviceNonce_ + "|" + clientNonce + "|" + hostId + "|" + deviceId_;
    const std::string expected = hmacSha256Hex(host->secret, HOST_PROOF_PREFIX + fields);
    if (expected.empty() || !constantTimeEquals(expected, response)) {
      refuse("invalid trusted host auth");
      return;
    }
    const std::string proof = hmacSha256Hex(host->secret, READER_PROOF_PREFIX + fields);
    if (proof.empty()) {
      refuse("invalid trusted host auth");
      return;
    }
    logHello("accepted");

    readerProof_ = proof;
    helloAccepted_ = true;
    authHandle_ = connHandle_.load();
    invalidHellos_ = 0;
    trustedHostName_ = host->name.empty() ? hostId : host->name;
    // A phone that renamed itself says so on every hello; keep the stored
    // label in step so Settings shows the current name.
    const std::string offeredName = sanitizeHostName(doc["host_name"] | "");
    if (offeredName != host->name) {
      BleTrustedHost renamed = *host;
      renamed.name = offeredName;
      if (BLE_TRUSTED_HOSTS.addOrReplaceHost(renamed)) trustedHostName_ = offeredName;
    }
    authErrorMessage_.clear();
    deviceNonce_ = makeNonceHex();
    LOG_INF("BLE", "trusted host '%s' accepted", trustedHostName_.c_str());
    setState(State::CONNECTED);
    // The gate is the only thing the Store was waiting for: ask for page one.
    if (store_) store_->onAppReady();
    return;
  }

  if (op == "pair") {
    // The link is already bonded (onControlWrite only runs on a secure link);
    // the window is the user's consent to store this phone's secret.
    const auto refusePair = [&](const char* reason) {
      LOG_INF("BLE", "pair refused: %s", reason);
      setAuthError(reason);
    };
    if (!pairingWindowOpen()) {
      refusePair("pairing window closed");
      return;
    }
    const int version = doc["version"] | 0;
    const std::string hostId = doc["host_id"] | "";
    const std::string secretHex = doc["secret"] | "";
    BleTrustedHost host;
    if (version != BLE_PROTOCOL_VERSION || !isSafeHostId(hostId) ||
        !isLowerHex(secretHex, BleTrustedHostStore::SECRET_BYTES * 2) || !hexToBytes(secretHex, host.secret)) {
      refusePair("invalid pair request");
      return;
    }
    host.hostId = hostId;
    host.name = sanitizeHostName(doc["host_name"] | "");
    if (!BLE_TRUSTED_HOSTS.addOrReplaceHost(host)) {
      refusePair("could not save the pairing");
      return;
    }
    closePairingWindow();
    helloAccepted_ = true;
    authHandle_ = connHandle_.load();
    hostPaired_ = true;
    invalidHellos_ = 0;
    trustedHostName_ = host.name;
    readerProof_.clear();
    authErrorMessage_.clear();
    LOG_INF("BLE", "pair accepted: '%s' (host %.8s) saved", host.name.c_str(), host.hostId.c_str());
    setState(State::CONNECTED);
    if (store_) store_->onAppReady();
    return;
  }

  if (!sessionAuthenticated()) {
    setAuthError("hello required");
    return;
  }

  if (op == "catalog_error") {
    // The app giving up early rather than letting the device sit out the whole
    // timeout: Calibre unreachable, the book withdrawn, a query that failed. It
    // must name the request it is failing, or it is ignored.
    if (!store_) {
      setError("store not open", false);
      return;
    }
    const uint32_t req = doc["req"] | 0u;
    std::string message = doc["error"] | "";
    if (message.size() > 96) message.resize(96);
    store_->onAppError(req, message);
    return;
  }

  if (op == "set_time") {
    // There is no NTP in a build without the network stack, so this is the only
    // way the device learns the *real* date: HalClock::begin() has already
    // started the RTC from the firmware's build epoch, which is a lower bound
    // that keeps saves stamped but drifts behind wall time until this arrives.
    // A client's time always wins -- it is the more accurate of the two.
    if (!halClock.isAvailable()) {
      setError("no clock on this device");
      return;
    }
    const int64_t epoch = doc["epoch"] | static_cast<int64_t>(0);
    if (epoch < static_cast<int64_t>(HalClock::MIN_VALID_EPOCH) ||
        epoch >= static_cast<int64_t>(HalClock::MAX_VALID_EPOCH)) {
      setError("invalid epoch");
      return;
    }
    // The offset comes with the instant, or the clock is right and the CLOCK IS
    // WRONG: the device was showing UTC while the user was six hours west of it,
    // which reads as a six-hour error rather than as a missing time zone.
    // Quarter-hours, biased by 48, matching CrossPointSettings::clockUtcOffsetQ
    // (48 = UTC+0) -- quarters because not every zone is a whole hour.
    if (doc["utc_offset_q"].is<int>()) {
      const int offsetQ = doc["utc_offset_q"].as<int>();
      if (offsetQ >= 0 && offsetQ <= 96) {
        SETTINGS.clockUtcOffsetQ = static_cast<uint8_t>(offsetQ);
        SETTINGS.saveToFile();
      } else {
        LOG_ERR("BLE", "ignoring out-of-range utc_offset_q %d", offsetQ);
      }
    }
    if (!halClock.setEpoch(static_cast<uint32_t>(epoch))) {
      setError("could not set clock");
      return;
    }
    LOG_INF("BLE", "Clock set by client to %lu", static_cast<unsigned long>(epoch));
    // No state change: the acknowledgement is `device_time` in the status the
    // client is already subscribed to, which is also how it detects drift.
    statusDirty_ = true;
    notifyObserver();
    return;
  }

  if (op == "delete_book") {
    // The offline shelf is a two-way mirror: a book removed in the app is
    // removed here. Progress is not lost by doing so -- it lives in kosync, so
    // re-saving the book restores the position with it.
    //
    // Destructive, so it is narrow by construction: one file, by name, under
    // /Books, with the same name rule an upload has to satisfy (no separators,
    // no traversal, .epub only). There is no recursive form and no wildcard.
    const std::string name = doc["name"] | "";
    if (!isSafeBleBookName(name)) {
      setError("unsafe book filename");
      return;
    }
    const std::string path = std::string(BOOKS_ROOT) + "/" + name;
    // Only the book that is OPEN blocks its own deletion; any other book can be
    // removed while a book is on screen.
    if (activityManager.isReaderActivity() && APP_STATE.openEpubPath == path) {
      if (!(doc["close"] | false)) {
        setError("book open");
        return;
      }
      // The user confirmed in the app: leave the book (its position is saved on
      // the way out) and delete once the reader has gone -- see tick().
      pendingDeleteName_ = name;
      pendingDeletePath_ = path;
      pendingDeleteAt_ = millis();
      LOG_INF("BLE", "delete_book: closing %s first", name.c_str());
      activityManager.goHome();
      return;
    }
    deleteBookNow(name, path);
    return;
  }

  if (op == "start_put") {
    resetTransfer(true);

    const std::string kind = doc["kind"] | "";
    // Present only on an answer to a `pending` request. Zero everywhere else,
    // which is exactly what an ordinary Bluetooth Transfer upload sends.
    const uint32_t responseReq = doc["req"] | 0u;
    fileName_ = doc["name"] | "";
    expectedSize_ = doc["size"] | 0;
    // Asked for explicitly, never inferred. Replacing a book is how a Calibre
    // update reaches the card; "exists" is still the answer for an ordinary
    // send, because that is the only cheap way the app learns a book is here.
    replaceExisting_ = doc["replace"] | false;
    expectedSha256_ = toLowerAscii(doc["sha256"] | "");
    uploadResumable_ = doc["resume"] | false;
    uploadChunkSize_ = doc["chunk_size"] | 0;
    uploadAckBytes_ = doc["ack_bytes"] | BLE_PROGRESS_STATUS_INTERVAL_BYTES;
    transferKind_ = TransferKind::NONE;

    if (!isHexSha256(expectedSha256_)) {
      setError("invalid sha256");
      return;
    }
    if (uploadResumable_ && uploadChunkSize_ == 0) {
      setError("invalid resume chunk size");
      return;
    }
    if (uploadAckBytes_ < BLE_UPLOAD_ACK_BYTES_MIN || uploadAckBytes_ > BLE_UPLOAD_ACK_BYTES_MAX) {
      setError("invalid ack window");
      return;
    }

    if (kind == "book") {
      if (!isSafeBleBookName(fileName_)) {
        setError("unsafe book filename");
        return;
      }
      if (store_) {
        // In the Store, a book upload is only ever the answer to a
        // `catalog_fetch` the device published. It must name that request and
        // that exact file: the user asked for one book, and an unsolicited push
        // must not land on the card in its place.
        if (!store_->acceptsResponse(responseReq, BleStoreController::PendingOp::FETCH)) {
          setError("stale request", false);
          return;
        }
        if (storeExpectedBook_.empty() || fileName_ != storeExpectedBook_) {
          setError("unexpected book", false);
          return;
        }
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_BOOK_BYTES) {
        setError("invalid book size");
        return;
      }
      if (!Storage.exists(BOOKS_ROOT) && !Storage.mkdir(BOOKS_ROOT)) {
        setError("could not create books directory");
        return;
      }
      transferKind_ = TransferKind::BOOK;
      partPath_ = std::string(BOOKS_ROOT) + "/.ble-" + fileName_ + ".part";
      finalPath_ = std::string(BOOKS_ROOT) + "/" + fileName_;
      if (Storage.exists(finalPath_.c_str())) {
        if (!replaceExisting_) {
          setError("exists");
          return;
        }
        // Not underneath an open reader: it holds this file and would write its
        // position back over the replacement on exit. Refused at begin, before
        // megabytes cross the link, for the same reason progress is.
        if (activityManager.isReaderActivity() && APP_STATE.openEpubPath == finalPath_) {
          setError("book open");
          return;
        }
      }
      if (!doc["position"].isNull()) {
        BookPosition position;
        if (!parseBookPosition(doc["position"].as<JsonVariantConst>(), fileName_, position)) {
          setError("invalid position");
          return;
        }
        positionGiven_ = true;
        positionLocation_ = std::move(position.location);
        positionTimestamp_ = position.timestamp;
        positionJump_ = position.jump;
        positionPercentBp_ = position.percentBp;
      }
    } else if (kind == "bmp") {
      if (!isSafeBleBmpName(fileName_)) {
        setError("unsafe bmp filename");
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_BMP_BYTES) {
        setError("invalid bmp size");
        return;
      }
      if (!Storage.exists(PICTURES_ROOT) && !Storage.mkdir(PICTURES_ROOT)) {
        setError("could not create pictures directory");
        return;
      }
      transferKind_ = TransferKind::BMP;
      partPath_ = std::string(PICTURES_ROOT) + "/.ble-" + fileName_ + ".part";
      finalPath_ = std::string(PICTURES_ROOT) + "/" + fileName_;
      if (Storage.exists(finalPath_.c_str())) {
        setError("exists");
        return;
      }
    } else if (kind == "progress") {
      // NOT while a book is open. The radio runs whatever is on screen, and a
      // batch applied underneath a live reader would be silently overwritten by
      // that reader's own position when it exits, which is worse than not syncing
      // at all because the phone would have been told it succeeded.
      //
      // Refused rather than deferred: the app knows how to retry, and a queue of
      // pending shelf writes is a far larger thing to get right than a retry.
      if (activityManager.isReaderActivity()) {
        setError("book open");
        return;
      }
      // The batch has no user-facing name and never lands on the shelf: it is
      // staged at a fixed scratch path, parsed on commit, and deleted.
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_PROGRESS_BYTES) {
        setError("invalid progress size");
        return;
      }
      if (!Storage.ensureDirectoryExists(CROSSPOINT_ROOT)) {
        setError("could not create data directory");
        return;
      }
      transferKind_ = TransferKind::PROGRESS;
      fileName_ = PROGRESS_BATCH_NAME;
      partPath_ = PROGRESS_BATCH_PART_PATH;
      finalPath_ = PROGRESS_BATCH_PATH;
    } else if (kind == "book_meta") {
      // Small by construction: a 1-bit cover at the row geometry plus a short
      // blurb. The cap is generous enough for a detail-sized cover and mean
      // enough that a malformed size cannot fill the card.
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_BOOK_META_BYTES) {
        setError("invalid book metadata size");
        return;
      }
      if (!Storage.ensureDirectoryExists(CROSSPOINT_ROOT)) {
        setError("could not create data directory");
        return;
      }
      transferKind_ = TransferKind::BOOK_META;
      bookMetaReq_ = responseReq;
      fileName_ = "bookmeta";
      partPath_ = BOOK_META_PART_PATH;
      finalPath_ = BOOK_META_INBOX_PATH;
    } else if (kind == "settings") {
      // Settings are the device's own state, and several of them (orientation,
      // theme, sleep timeout) change what is on screen the moment they land.
      // Refused while a book is open for the same reason a progress batch is:
      // the reader holds state that would be written back over the top on exit.
      if (activityManager.isReaderActivity()) {
        setError("book open");
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_SETTINGS_BYTES) {
        setError("invalid settings size");
        return;
      }
      if (!Storage.ensureDirectoryExists(CROSSPOINT_ROOT)) {
        setError("could not create data directory");
        return;
      }
      // Staged at a fixed scratch path, parsed on commit and deleted -- it has
      // no user-facing name and never lands on the shelf.
      transferKind_ = TransferKind::SETTINGS_INBOX;
      fileName_ = SETTINGS_INBOX_NAME;
      partPath_ = std::string(SETTINGS_INBOX_PATH) + ".part";
      finalPath_ = SETTINGS_INBOX_PATH;
    } else if (kind == "catalog_page" || kind == "catalog_detail") {
      // The answer to the question in the last `status` notification. It rides
      // the ordinary upload path -- framing, credit flow control, SHA-256,
      // commit -- and adds only the request id that ties it to the question.
      const bool detail = kind == "catalog_detail";
      if (!store_) {
        setError("store not open", false);
        return;
      }
      if (!store_->acceptsResponse(
              responseReq, detail ? BleStoreController::PendingOp::DETAIL : BleStoreController::PendingOp::PAGE)) {
        // A reply to a question the device has already given up on or moved past.
        // Refused without disturbing whatever is on screen now.
        setError("stale request", false);
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > BleCatalog::MAX_CONTAINER_BYTES) {
        setError("invalid catalog size");
        return;
      }
      if (!Storage.ensureDirectoryExists(STORE_ROOT)) {
        setError("could not create store directory");
        return;
      }
      transferKind_ = detail ? TransferKind::CATALOG_DETAIL : TransferKind::CATALOG_PAGE;
      fileName_ = CATALOG_NAME;
      partPath_ = CATALOG_PART_PATH;
      finalPath_ = CATALOG_PATH;
    } else if (kind == "firmware") {
      // Only signed images are staged. Both are written beside the image at
      // commit, where FirmwareWatcher checks the signature over them.
      const std::string version = doc["version"] | "";
      const std::string signature = doc["signature"] | "";
      if (!isBuildStamp(version) || !isFirmwareSignatureHex(signature)) {
        setError("signature required");
        return;
      }
      firmwareVersion_ = version;
      firmwareSignature_ = signature;
      if (!isSafeBleFirmwareName(fileName_)) {
        setError("unsafe firmware filename");
        return;
      }
      const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
      if (!dest) {
        setError("no update partition");
        return;
      }
      if (expectedSize_ < MIN_BLE_FIRMWARE_BYTES || expectedSize_ > dest->size) {
        setError("invalid firmware size");
        return;
      }
      if (!Storage.ensureDirectoryExists(firmware_staging::DIR)) {
        setError("could not create the firmware directory");
        return;
      }
      // Straight into the watched folder under its one fixed name. The client
      // may call the file whatever it likes -- isSafeBleFirmwareName() still has
      // to pass, and the name is echoed back in `status` -- but what lands on the
      // card is /firmware/firmware.bin, because that is the only path the
      // watcher, the app and a human with the card in a laptop all agree on.
      transferKind_ = TransferKind::FIRMWARE;
      partPath_ = firmware_staging::PART_PATH;
      finalPath_ = firmware_staging::IMAGE_PATH;
      // A previous drop must not be what gets flashed if this one fails halfway.
      firmware_staging::clearStaged();
    } else {
      setError("unsupported transfer kind");
      return;
    }

    mbedtls_sha256_starts(&shaContext_, 0);
    shaActive_ = true;
    receivedBytes_ = 0;
    expectedSequence_ = 0;
    if (uploadResumable_ && Storage.exists(partPath_.c_str())) {
      HalFile partialFile;
      if (!Storage.openFileForRead("BLE", partPath_, partialFile)) {
        setError("could not inspect partial transfer");
        resetTransfer(true);
        return;
      }
      const size_t partialSize = partialFile.fileSize();
      partialFile.close();
      if (partialSize > expectedSize_ ||
          (partialSize > 0 && partialSize < expectedSize_ && (partialSize % uploadChunkSize_) != 0)) {
        setError("partial transfer mismatch");
        resetTransfer(true);
        return;
      }
      if (!hashExistingPrefix(partPath_, partialSize, shaContext_)) {
        setError("could not hash partial transfer");
        resetTransfer(true);
        return;
      }
      uploadFile_ = Storage.open(partPath_.c_str(), O_RDWR);
      if (!uploadFile_ || !uploadFile_.seek(partialSize)) {
        setError("could not resume transfer file");
        resetTransfer(true);
        return;
      }
      receivedBytes_ = partialSize;
      expectedSequence_ = static_cast<uint32_t>(partialSize / uploadChunkSize_);
    } else {
      if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
      if (!Storage.openFileForWrite("BLE", partPath_, uploadFile_)) {
        setError("could not open transfer file");
        return;
      }
    }
    transferOpen_ = true;
    removePartOnExit_ = true;
    lastProgressStatusBytes_ = receivedBytes_;
    lastDisplayProgressBytes_ = receivedBytes_;
    setState(State::RECEIVING);
    // The store's own deadline ends here: from now on the transfer path reports
    // progress and owns the failure, so the screen shows bytes rather than a
    // countdown.
    if (store_ && transferKind_ == TransferKind::BOOK) store_->onFetchStarted();
    return;
  }

  if (op == "start_get") {
    resetTransfer(true);
    const int64_t offsetValue = doc["offset"] | 0;
    const int64_t chunkSizeValue = doc["chunk_size"] | static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES);
    if (offsetValue < 0 ||
        static_cast<uint64_t>(offsetValue) > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      setError("invalid download offset");
      return;
    }
    if (chunkSizeValue < static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES_MIN) ||
        chunkSizeValue > static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES_MAX)) {
      setError("invalid download chunk size");
      return;
    }
    const std::string kind = doc["kind"] | "";
    if (kind == "crash_report") {
      startCrashReportDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    if (kind == "library") {
      startLibraryDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    if (kind == "progress_result") {
      startProgressResultDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    if (kind == "settings") {
      startSettingsDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    if (kind == "about") {
      startAboutDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    setError("unsupported transfer kind");
    return;
  }

  if (op == "get_ack") {
    if (!downloadOpen_ || !downloadAwaitingAck_) {
      setError("no download pending");
      return;
    }
    const uint32_t sequence = doc["sequence"] | UINT32_MAX;
    if (sequence != pendingDownloadAck_) {
      setError("unexpected download ack");
      return;
    }
    downloadAwaitingAck_ = false;
    statusDirty_ = true;
    notifyObserver();
    return;
  }

  if (op == "commit") {
    if (!transferOpen_) {
      setError("no transfer open");
      return;
    }
    pendingCommit_ = true;
    setState(State::VERIFYING);
    return;
  }

  if (op == "cancel") {
    resetTransfer(true);
    setState(State::CONNECTED);
    return;
  }

  setError("unknown control op");
}

void BleLink::onDataWrite(const std::string& value) {
  if (!transferOpen_ || state_ != State::RECEIVING) return;
  if (value.size() <= sizeof(uint32_t)) {
    setError("invalid data frame");
    resetTransfer(true);
    return;
  }

  const uint32_t sequence = readLe32(value);
  if (sequence != expectedSequence_) {
    setError("unexpected data sequence");
    resetTransfer(true);
    return;
  }

  const uint8_t* payload = reinterpret_cast<const uint8_t*>(value.data() + sizeof(uint32_t));
  const size_t payloadSize = value.size() - sizeof(uint32_t);
  if (receivedBytes_ + payloadSize > expectedSize_) {
    setError("transfer too large");
    resetTransfer(true);
    return;
  }
  if (uploadFile_.write(payload, payloadSize) != payloadSize) {
    setError("transfer write failed");
    resetTransfer(true);
    return;
  }

  mbedtls_sha256_update(&shaContext_, payload, payloadSize);
  receivedBytes_ += payloadSize;
  expectedSequence_++;
  if (receivedBytes_ == expectedSize_ || receivedBytes_ - lastProgressStatusBytes_ >= uploadAckBytes_) {
    lastProgressStatusBytes_ = receivedBytes_;
    statusDirty_ = true;
  }
  const size_t displayInterval = transferKind_ == TransferKind::FIRMWARE ? BLE_FIRMWARE_PROGRESS_DISPLAY_INTERVAL_BYTES
                                                                         : BLE_PROGRESS_DISPLAY_INTERVAL_BYTES;
  if (receivedBytes_ == expectedSize_ || receivedBytes_ - lastDisplayProgressBytes_ >= displayInterval) {
    lastDisplayProgressBytes_ = receivedBytes_;
    // The store paints its own progress bar, on the same cadence the transfer
    // screen repaints on.
    if (store_ && transferKind_ == TransferKind::BOOK) store_->onFetchProgress(receivedBytes_, expectedSize_);
    notifyObserver();
  }
}

void BleLink::processCommit() {
  if (!transferOpen_) return;
  setState(State::VERIFYING);

  uploadFile_.flush();
  uploadFile_.close();
  transferOpen_ = false;

  if (receivedBytes_ != expectedSize_) {
    setError("size mismatch");
    resetTransfer(true);
    return;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&shaContext_, digest);
  shaActive_ = false;
  if (sha256ToHex(digest) != expectedSha256_) {
    setError("sha256 mismatch");
    resetTransfer(true);
    return;
  }

  if ((transferKind_ == TransferKind::BOOK || transferKind_ == TransferKind::BMP) &&
      Storage.exists(finalPath_.c_str())) {
    // A BOOK may replace its predecessor only when the sender asked to (a
    // Calibre update). Anything else that already exists is refused.
    if (transferKind_ != TransferKind::BOOK || !replaceExisting_) {
      setError("exists");
      resetTransfer(true);
      return;
    }
    if (activityManager.isReaderActivity() && APP_STATE.openEpubPath == finalPath_) {
      setError("book open");
      resetTransfer(true);
      return;
    }
    // FAT will not rename onto an existing name. The verified .part is already
    // complete on the card at this point, so the window between this remove and
    // the rename below is the only moment the book is absent.
    if (!Storage.remove(finalPath_.c_str())) {
      setError("could not replace book");
      resetTransfer(true);
      return;
    }
    LOG_INF("BLE", "Replacing %s with the updated copy", fileName_.c_str());
  }
  if ((transferKind_ == TransferKind::FIRMWARE || transferKind_ == TransferKind::PROGRESS ||
       transferKind_ == TransferKind::SETTINGS_INBOX || transferKind_ == TransferKind::BOOK_META ||
       transferKind_ == TransferKind::CATALOG_PAGE ||
       transferKind_ == TransferKind::CATALOG_DETAIL) &&
      Storage.exists(finalPath_.c_str()) && !Storage.remove(finalPath_.c_str())) {
    setError("could not replace staged upload");
    resetTransfer(true);
    return;
  }
  if (!Storage.rename(partPath_.c_str(), finalPath_.c_str())) {
    setError("could not finalize transfer");
    resetTransfer(true);
    return;
  }
  removePartOnExit_ = false;

  if (transferKind_ == TransferKind::CATALOG_PAGE || transferKind_ == TransferKind::CATALOG_DETAIL) {
    // Only now that the whole container is on the card and its SHA-256 checks
    // out is it unpacked: the header is read (kilobytes), each cover is copied
    // out to its own small file, and the staged blob is deleted. Nothing larger
    // than one 512-byte buffer is ever resident.
    const bool detail = transferKind_ == TransferKind::CATALOG_DETAIL;
    removePartOnExit_ = false;
    if (store_) store_->onCatalogCommitted(finalPath_.c_str(), detail);
    resetTransfer(false);
    return;
  }

  if (transferKind_ == TransferKind::BOOK || transferKind_ == TransferKind::BMP) {
    savedPath_ = finalPath_;
    if (transferKind_ == TransferKind::BOOK) {
      const std::string positionCache = BookProgressSync::cachePathForBook(savedPath_);
      const auto keptPosition = takePositionFiles(positionCache);
      clearBookCache(savedPath_);
      restorePositionFiles(positionCache, keptPosition);
      if (!keptPosition.empty()) LOG_INF("BLE", "Book replaced; kept its reading position");
      // Before the shelf hears of the book, so nothing can open it first.
      if (positionGiven_) {
        const auto result = BookProgressSync::applyProgress(BOOKS_ROOT, fileName_, positionLocation_,
                                                            positionTimestamp_, positionJump_, positionPercentBp_);
        positionApplied_ = result == BookProgressSync::ApplyResult::APPLIED;
        LOG_INF("BLE", "Position for %s: %s", fileName_.c_str(), BookProgressSync::applyResultName(result));
      }
      // The Library reconciles once per visit, so a book that lands while the
      // shelf is on screen would otherwise not appear until a restart.
      //
      // The repaint request is the half that was missing: marking the shelf
      // stale only sets a flag, and the flag is read while rendering -- so with
      // nothing asking for a render, an idle Library sat there never noticing.
      HomeShelfStore::markStale();
    noteLibraryChanged();
      if (!activityManager.isReaderActivity()) activityManager.requestUpdate();
    }
    if (store_ && transferKind_ == TransferKind::BOOK) {
      storeExpectedBook_.clear();
      store_->onFetchSaved(savedPath_);
    }
    setState(State::SAVED);
    return;
  }

  if (transferKind_ == TransferKind::PROGRESS) {
    // Only now that the whole batch is on disk and its SHA-256 checks out does
    // anything get written underneath a book.
    processProgressBatch();
    return;
  }

  if (transferKind_ == TransferKind::BOOK_META) {
    if (!applyBookMetaDocument()) {
      resetTransfer(true);
      return;
    }
    HomeShelfStore::markStale();
    noteLibraryChanged();
    if (!activityManager.isReaderActivity()) activityManager.requestUpdate();
    setState(State::SAVED);
    return;
  }

  if (transferKind_ == TransferKind::SETTINGS_INBOX) {
    // Only now that the whole document is on disk and its SHA-256 checks out is
    // anything applied: a half-received settings file must never be able to
    // half-configure the device.
    if (!applySettingsDocument()) {
      resetTransfer(true);
      return;
    }
    setState(State::SAVED);
    return;
  }

  // Firmware. A BLE push never flashes anything: it drops the image into the
  // watched folder and writes the companion hash beside it, exactly as a person
  // with the card mounted over USB would. FirmwareWatcher finds it, re-hashes it
  // off the card, and asks the user. So the radio's job ends here, and an
  // interactive flash can never be something that happens to a reader because a
  // phone came into range.
  //
  // Validating now anyway is worth the second or two: it lets the app hear
  // "invalid firmware: BAD_CHIP" while it is still connected and can say so,
  // instead of the image sitting on the card being silently declined later.
  // The watcher validates again before it flashes -- the SD card is removable
  // and that gap is real, so neither check is redundant.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    setError("no update partition");
    return;
  }
  LOG_INF("BLE", "validating staged firmware: %s (%u bytes)", finalPath_.c_str(), static_cast<unsigned>(expectedSize_));
  const firmware_flash::Result validateRes = firmware_flash::validateImageFile(finalPath_.c_str(), dest->size);
  if (validateRes != firmware_flash::Result::OK) {
    firmware_staging::clearStaged();
    setError(std::string("invalid firmware: ") + firmware_flash::resultName(validateRes));
    return;
  }
  // expectedSha256_ is the digest this transfer just verified the bytes against,
  // so the companion file can be written from it rather than asking the app to
  // send the same number twice.
  if (!firmware_staging::writeExpectedHash(expectedSha256_)) {
    firmware_staging::clearStaged();
    setError("could not write the firmware hash file");
    return;
  }
  // Validated at start_put; FirmwareWatcher verifies the signature over both.
  if (!firmware_staging::writeVersion(firmwareVersion_) || !firmware_staging::writeSignature(firmwareSignature_)) {
    firmware_staging::clearStaged();
    setError("could not write the firmware signature");
    return;
  }
  LOG_INF("BLE", "firmware staged at %s; the update prompt is the watcher's", firmware_staging::IMAGE_PATH);
  savedPath_ = finalPath_;
  setState(State::SAVED);
}

void BleLink::startFileDownload(const char* path, const char* name, const TransferKind kind,
                                            const size_t offset, const size_t chunkSize) {
  if (!Storage.exists(path)) {
    setError("not_found");
    return;
  }
  if (!Storage.openFileForRead("BLE", path, downloadFile_)) {
    setError("could not open download");
    return;
  }

  fileName_ = name;
  transferKind_ = kind;
  expectedSize_ = downloadFile_.fileSize();
  if (offset > expectedSize_) {
    downloadFile_.close();
    setError("invalid download offset");
    return;
  }
  if (offset < expectedSize_ && offset % chunkSize != 0) {
    downloadFile_.close();
    setError("unaligned download offset");
    return;
  }
  if (!downloadFile_.seek(offset)) {
    downloadFile_.close();
    setError("could not seek download");
    return;
  }
  sentBytes_ = offset;
  downloadSequence_ = static_cast<uint32_t>(offset / chunkSize);
  pendingDownloadAck_ = 0;
  downloadAwaitingAck_ = false;
  downloadChunkSize_ = chunkSize;
  lastProgressStatusBytes_ = sentBytes_;
  downloadOpen_ = true;
  setState(State::SENDING);
}

void BleLink::startCrashReportDownload(const size_t offset, const size_t chunkSize) {
  startFileDownload(CRASH_REPORT_PATH, CRASH_REPORT_NAME, TransferKind::CRASH_REPORT, offset, chunkSize);
}

void BleLink::startLibraryDownload(const size_t offset, const size_t chunkSize) {
  // Offset 0 means a fresh listing, so rebuild it: a client must never resume
  // onto a document that changed underneath it. A non-zero offset can only refer
  // to the file staged by that same start_get.
  if (offset == 0) {
    // Walking the shelf is seconds of SD work -- one metadata cache open per
    // book -- so say so on screen and over BLE before blocking on it.
    setState(State::PREPARING);
    publishStatus();
    notifyObserver();

    BookLibraryIndex::Stats stats;
    if (!BookLibraryIndex::build(BOOKS_ROOT, LIBRARY_INDEX_PATH, &stats)) {
      setError("could not build library index");
      return;
    }
    LOG_DBG("BLE", "Library index staged: %u books", static_cast<unsigned>(stats.books));
  } else if (!Storage.exists(LIBRARY_INDEX_PATH)) {
    setError("not_found");
    return;
  }
  startFileDownload(LIBRARY_INDEX_PATH, LIBRARY_INDEX_NAME, TransferKind::LIBRARY, offset, chunkSize);
}

void BleLink::startProgressResultDownload(const size_t offset, const size_t chunkSize) {
  startFileDownload(PROGRESS_RESULT_PATH, PROGRESS_RESULT_NAME, TransferKind::PROGRESS_RESULT, offset, chunkSize);
}

void BleLink::startSettingsDownload(const size_t offset, const size_t chunkSize) {
  // Re-serialised on every start_get rather than cached: settings change from
  // the device's own screens too, and a snapshot the app fetched from a stale
  // file would be silently wrong in exactly the case that matters -- the user
  // changed something on the reader and then opened the app.
  //
  // Only on a fresh request, though: a resumed transfer (offset > 0) must keep
  // reading the bytes the earlier chunks came from, or the document the app
  // reassembles is a splice of two different snapshots.
  if (offset == 0) {
    if (!Storage.ensureDirectoryExists(CROSSPOINT_ROOT)) {
      setError("could not create data directory");
      return;
    }
    JsonDocument doc;
    SETTINGS.toJson(doc);
    if (Storage.exists(SETTINGS_SNAPSHOT_PATH)) Storage.remove(SETTINGS_SNAPSHOT_PATH);
    HalFile out;
    if (!Storage.openFileForWrite("BLE", SETTINGS_SNAPSHOT_PATH, out)) {
      setError("could not stage settings");
      return;
    }
    // Through a String, as BookLibraryIndex does: the settings document is a few
    // KB, so there is nothing to gain from streaming it and a good deal to lose
    // in a half-written file if the write fails partway.
    String json;
    serializeJson(doc, json);
    const bool ok = json.length() > 0 && out.print(json) == json.length();
    out.close();
    if (!ok) {
      Storage.remove(SETTINGS_SNAPSHOT_PATH);
      setError("could not serialise settings");
      return;
    }
  }
  startFileDownload(SETTINGS_SNAPSHOT_PATH, SETTINGS_SNAPSHOT_NAME, TransferKind::SETTINGS_SNAPSHOT, offset,
                    chunkSize);
}

void BleLink::startAboutDownload(const size_t offset, const size_t chunkSize) {
  // Which build is running, so the app can say whether the update page has a
  // newer one. X4_BUILD_STAMP is written by the build host (BuildStamp.h) and the
  // same stamp names the published image in firmware.json. The running slot is
  // there for diagnosis: after the first Bluetooth update the reader boots app1,
  // and a USB flash to 0x10000 then lands in the slot it is NOT booting.
  if (offset == 0) {
    if (!Storage.ensureDirectoryExists(CROSSPOINT_ROOT)) {
      setError("could not create data directory");
      return;
    }
    JsonDocument doc;
    doc["firmware_version"] = X4_BUILD_STAMP;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) doc["running_partition"] = running->label;
    // Whether an image already waits on the card, and whether the reader means
    // to install it at its next sleep -- so the app neither re-sends an update
    // the reader holds nor offers one as if it did not.
    doc["update_staged"] = firmware_staging::imageStaged();
    doc["install_at_sleep"] = FIRMWARE_WATCHER.installAtSleep();
    std::string stagedVersion;
    if (firmware_staging::readVersion(stagedVersion)) doc["staged_version"] = stagedVersion;
    // Protocol features beyond the upload and download kinds. Here rather than in
    // `status`, whose read already sheds its capability lists to fit 512 bytes.
    JsonArray features = doc["features"].to<JsonArray>();
    features.add("book_position");
    if (Storage.exists(ABOUT_PATH)) Storage.remove(ABOUT_PATH);
    HalFile out;
    if (!Storage.openFileForWrite("BLE", ABOUT_PATH, out)) {
      setError("could not stage about");
      return;
    }
    String json;
    serializeJson(doc, json);
    const bool ok = json.length() > 0 && out.print(json) == json.length();
    out.close();
    if (!ok) {
      Storage.remove(ABOUT_PATH);
      setError("could not serialise about");
      return;
    }
  }
  startFileDownload(ABOUT_PATH, ABOUT_NAME, TransferKind::ABOUT, offset, chunkSize);
}

bool BleLink::applyBookMetaDocument() {
  // The container is the Store's own format, so this reuses the parser that
  // already knows how to split a header from its thumbnails rather than adding
  // a second wire format to keep in step.
  if (!Storage.ensureDirectoryExists(BOOK_META_DIR)) {
    setError("could not create the book metadata directory");
    return false;
  }
  BleCatalog::Page parsed;
  std::string error;
  const bool ok = BleCatalog::parseContainer(BOOK_META_INBOX_PATH, BOOK_META_DIR, bookMetaReq_, /*detail=*/true,
                                             BOOKS_ROOT, parsed, error);
  Storage.remove(BOOK_META_INBOX_PATH);
  if (!ok || parsed.entries.empty()) {
    setError(error.empty() ? "invalid book metadata" : error);
    return false;
  }

  const auto& entry = parsed.entries.front();
  // Keyed by the book's filename, because that is the only name the shelf and
  // the app agree on -- the reader has no catalogue ids.
  if (!isSafeBleBookName(entry.filename)) {
    setError("book metadata names no usable book");
    return false;
  }
  const std::string stem = std::string(BOOK_META_DIR) + "/" + entry.filename;

  // The cover lands wherever parseContainer put it; move it to the stable name
  // the Library looks for, so a redelivery replaces rather than accumulates.
  const std::string coverPath = stem + ".bmp";
  if (!entry.thumbPath.empty() && entry.thumbPath != coverPath) {
    if (Storage.exists(coverPath.c_str())) Storage.remove(coverPath.c_str());
    if (!Storage.rename(entry.thumbPath.c_str(), coverPath.c_str())) {
      LOG_ERR("BLE", "could not place the cover for %s", entry.filename.c_str());
    }
  }

  JsonDocument doc;
  doc["title"] = entry.title.c_str();
  doc["author"] = entry.author.c_str();
  doc["description"] = entry.description.c_str();
  doc["series"] = entry.series.c_str();
  doc["publisher"] = entry.publisher.c_str();
  doc["published"] = entry.published.c_str();
  doc["language"] = entry.language.c_str();
  doc["tags"] = entry.tags.c_str();
  String json;
  serializeJson(doc, json);
  const std::string metaPath = stem + ".json";
  if (Storage.exists(metaPath.c_str())) Storage.remove(metaPath.c_str());
  HalFile out;
  if (!Storage.openFileForWrite("BLE", metaPath, out)) {
    setError("could not write the book metadata");
    return false;
  }
  const bool written = out.print(json) == json.length();
  out.close();
  if (!written) {
    Storage.remove(metaPath.c_str());
    setError("could not write the book metadata");
    return false;
  }
  LOG_INF("BLE", "book metadata stored for %s", entry.filename.c_str());
  return true;
}

bool BleLink::applySettingsDocument() {
  HalFile in;
  if (!Storage.openFileForRead("BLE", SETTINGS_INBOX_PATH, in)) {
    setError("could not read settings");
    return false;
  }
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, in);
  in.close();
  Storage.remove(SETTINGS_INBOX_PATH);
  if (parseError) {
    setError(std::string("invalid settings: ") + parseError.c_str());
    return false;
  }
  // fromJson() is the same path a settings file read at boot goes through, so
  // an app-sent document gets the identical validation, clamping and revision
  // migration -- there is no second, laxer way into the settings store.
  //
  // Auto-install is a device-only setting: whatever the document says, the
  // device keeps its own value.
  const auto autoInstallFirmware = SETTINGS.autoInstallFirmware;
  const bool accepted = SETTINGS.fromJson(doc.as<JsonVariantConst>());
  SETTINGS.autoInstallFirmware = autoInstallFirmware;
  if (!accepted) {
    setError("settings rejected");
    return false;
  }
  if (!SETTINGS.saveToFile()) {
    setError("could not persist settings");
    return false;
  }

  // Persisting is not applying. Most settings are read live at draw time, but
  // the theme and the metrics derived from it are built once and cached, so a
  // document that changes uiTheme did nothing visible until the next boot --
  // which is exactly what a settings screen written over BLE looked like from
  // the outside: "it saved, but nothing happened".
  //
  // This is the same pair of steps SettingsActivity performs when the user
  // changes a value on the device itself (reload the theme, then repaint); the
  // BLE path simply never did them.
  UITheme::getInstance().reload();
  activityManager.requestUpdate();
  LOG_INF("BLE", "settings applied and re-rendered");
  return true;
}

void BleLink::processProgressBatch() {
  progressEntries_ = 0;
  progressApplied_ = 0;

  // The open-book hazard is handled at start_put, not here: a batch is refused
  // outright while the reader has a book in memory. See the `progress` branch of
  // onControlWrite(). Anything that reaches this point has no in-memory reader
  // position to fight, so every write below is the last word on that book until
  // the user opens it again.
  HalFile in;
  if (!Storage.openFileForRead("BLE", PROGRESS_BATCH_PATH, in)) {
    setError("could not read progress batch");
    resetTransfer(true);
    return;
  }
  if (Storage.exists(PROGRESS_RESULT_PATH)) Storage.remove(PROGRESS_RESULT_PATH);
  HalFile out;
  if (!Storage.openFileForWrite("BLE", PROGRESS_RESULT_PATH, out)) {
    in.close();
    setError("could not stage progress results");
    resetTransfer(true);
    return;
  }

  ProgressBatchReader reader(in);
  std::string objectText;
  bool ok = out.print("[") == 1;
  bool first = true;
  bool malformed = false;

  while (ok) {
    const auto next = reader.next(objectText);
    if (next == ProgressBatchReader::Next::PARSE_ERROR) {
      malformed = true;
      break;
    }
    if (next == ProgressBatchReader::Next::END) break;
    if (progressEntries_ >= MAX_PROGRESS_ENTRIES) {
      malformed = true;
      break;
    }
    progressEntries_++;
    // Each entry is several SD operations (existence check, sidecar read,
    // atomic write); a large batch would otherwise outlast the watchdog window.
    resetTaskWatchdogIfSubscribed();

    std::string filename;
    auto result = BookProgressSync::ApplyResult::INVALID;
    JsonDocument entry;
    if (deserializeJson(entry, objectText) == DeserializationError::Ok) {
      filename = entry["filename"] | "";
      const std::string location = toLowerAscii(entry["location"] | "");
      const int64_t timestamp = entry["timestamp"] | static_cast<int64_t>(0);

      // Optional, and absent means exactly today's behaviour. A position from
      // another reading system arrives as a spine item plus a fraction through
      // it, because that system's own position encoding indexes a file this
      // device will never hold. `spine_n` is the spine item count the sender
      // measured; the reader checks it against its own copy when it opens the
      // book, so a re-converted or different edition falls back rather than
      // jumping somewhere confidently wrong.
      BookProgressSync::SpineJump jump;
      if (entry["spine"].is<int>() && entry["spine_n"].is<int>()) {
        const int spineIndex = entry["spine"].as<int>();
        const int spineCount = entry["spine_n"].as<int>();
        const float fraction = entry["spine_frac"] | 0.0f;
        if (spineIndex >= 0 && spineCount > 0 && spineIndex < spineCount && spineCount <= UINT16_MAX &&
            fraction >= 0.0f && fraction <= 1.0f) {
          jump.present = true;
          jump.spineIndex = static_cast<uint16_t>(spineIndex);
          jump.fraction = fraction;
          jump.spineCount = static_cast<uint16_t>(spineCount);
        }
      }

      // How far through the book, 0..1, stored beside the position so the
      // library screen can show it without opening the book. Absent means "not
      // supplied" and leaves whatever the reader already had.
      const float pct = entry["pct"] | -1.0f;
      const uint16_t percentBp =
          (pct >= 0.0f && pct <= 1.0f) ? static_cast<uint16_t>(pct * 10000.0f + 0.5f) : 0;

      // One bad entry costs that entry only. A book the phone knows about but
      // this card does not is the ordinary case, not a failed batch.
      if (isSafeBleBookRelativePath(filename) && timestamp > 0 &&
          timestamp <= static_cast<int64_t>(UINT32_MAX)) {
        result = BookProgressSync::applyProgress(BOOKS_ROOT, filename, location,
                                                 static_cast<uint32_t>(timestamp), jump, percentBp);
      }
    }
    if (result == BookProgressSync::ApplyResult::APPLIED) progressApplied_++;

    JsonDocument resultDoc;
    // Echoed back so the client can match outcomes to entries without relying on
    // array position; empty when the entry was too malformed to name a book.
    resultDoc["filename"] = filename;
    resultDoc["result"] = BookProgressSync::applyResultName(result);
    String json;
    serializeJson(resultDoc, json);
    if (!first && out.print(",") != 1) {
      ok = false;
      break;
    }
    first = false;
    if (out.print(json) != json.length()) ok = false;
  }

  if (ok) ok = out.print("]") == 1;
  out.flush();
  out.close();
  in.close();
  // The uploaded batch is scratch; the result document stays until the link stops
  // (deep sleep) so the client can fetch it.
  Storage.remove(PROGRESS_BATCH_PATH);
  removePartOnExit_ = false;

  if (!ok || malformed) {
    Storage.remove(PROGRESS_RESULT_PATH);
    progressEntries_ = 0;
    progressApplied_ = 0;
    setError(malformed ? "malformed progress batch" : "could not write progress results");
    resetTransfer(true);
    return;
  }

  LOG_INF("BLE", "Progress batch: %u entries, %u applied", static_cast<unsigned>(progressEntries_),
          static_cast<unsigned>(progressApplied_));
  setState(State::SAVED);
}

void BleLink::pumpDownload() {
  if (!downloadOpen_ || downloadAwaitingAck_) return;

  std::array<uint8_t, sizeof(uint32_t) + BLE_DOWNLOAD_CHUNK_BYTES> frame = {};
  frame[0] = static_cast<uint8_t>(downloadSequence_ & 0xFF);
  frame[1] = static_cast<uint8_t>((downloadSequence_ >> 8) & 0xFF);
  frame[2] = static_cast<uint8_t>((downloadSequence_ >> 16) & 0xFF);
  frame[3] = static_cast<uint8_t>((downloadSequence_ >> 24) & 0xFF);

  const int read = downloadFile_.read(frame.data() + sizeof(uint32_t), downloadChunkSize_);
  if (read < 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setError("download read failed");
    return;
  }
  if (read == 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setState(State::SENT);
    return;
  }

  ble_->notifyData(frame.data(), sizeof(uint32_t) + static_cast<size_t>(read));
  sentBytes_ += static_cast<size_t>(read);
  pendingDownloadAck_ = downloadSequence_;
  downloadAwaitingAck_ = true;
  downloadSequence_++;
  if (sentBytes_ == expectedSize_ || sentBytes_ - lastProgressStatusBytes_ >= BLE_PROGRESS_STATUS_INTERVAL_BYTES) {
    lastProgressStatusBytes_ = sentBytes_;
    statusDirty_ = true;
    notifyObserver();
  }
}

void BleLink::resetTransfer(const bool removePart) {
  replaceExisting_ = false;
  positionGiven_ = false;
  positionApplied_ = false;
  positionLocation_.clear();
  positionTimestamp_ = 0;
  positionPercentBp_ = 0;
  positionJump_ = {};
  firmwareVersion_.clear();
  firmwareSignature_.clear();
  if (shaActive_) {
    mbedtls_sha256_free(&shaContext_);
    mbedtls_sha256_init(&shaContext_);
    shaActive_ = false;
  }
  if (uploadFile_) uploadFile_.close();
  if (downloadFile_) downloadFile_.close();
  if (removePart && removePartOnExit_ && !partPath_.empty() && Storage.exists(partPath_.c_str())) {
    Storage.remove(partPath_.c_str());
  }

  fileName_.clear();
  partPath_.clear();
  finalPath_.clear();
  expectedSha256_.clear();
  savedPath_.clear();
  transferKind_ = TransferKind::NONE;
  expectedSize_ = 0;
  receivedBytes_ = 0;
  sentBytes_ = 0;
  lastProgressStatusBytes_ = 0;
  lastDisplayProgressBytes_ = 0;
  uploadChunkSize_ = 0;
  uploadAckBytes_ = BLE_PROGRESS_STATUS_INTERVAL_BYTES;
  downloadChunkSize_ = BLE_DOWNLOAD_CHUNK_BYTES;
  expectedSequence_ = 0;
  downloadSequence_ = 0;
  pendingDownloadAck_ = 0;
  transferOpen_ = false;
  downloadOpen_ = false;
  downloadAwaitingAck_ = false;
  pendingCommit_ = false;
  removePartOnExit_ = false;
  uploadResumable_ = false;
}

void BleLink::setState(const State state) {
  state_ = state;
  statusDirty_ = true;
  notifyObserver();
}

void BleLink::setError(const std::string& error) { setError(error, true); }

void BleLink::setError(const std::string& error, const bool notifyStore) {
  errorMessage_ = error;
  state_ = State::ERROR;
  statusDirty_ = true;
  notifyObserver();
  // A refused answer to a question the device is no longer asking is a normal
  // race, reported to the app in `status` and nowhere else. Everything else the
  // Store user needs to see.
  if (notifyStore && store_) store_->onTransferError(error);
}

void BleLink::setAuthError(const std::string& error) {
  authErrorMessage_ = error;
  // Not State::ERROR: a refused hello is not a failed session and the link stays
  // up. Whether a host is stored is half of any diagnosis, so it is logged too.
  LOG_ERR("BLE", "auth refused: %s (a trusted host is stored: %s)", error.c_str(),
          BLE_TRUSTED_HOSTS.hasHosts() ? "yes" : "no");
  helloAccepted_ = false;
  authHandle_ = NO_CONNECTION;
  trustedHostName_.clear();
  readerProof_.clear();
  statusDirty_ = true;
  publishStatus();
  notifyObserver();
}

void BleLink::refuseHello(const std::string& error) {
  setAuthError(error);
  if (++invalidHellos_ >= MAX_INVALID_HELLOS) disconnectPeer("too many invalid hellos");
}

void BleLink::noteBleMtu(const uint16_t mtu) { negotiatedMtu_.store(mtu, std::memory_order_relaxed); }

size_t BleLink::notifyCapBytes() const {
  // The live link is authoritative; the value onMTUChange() cached is the
  // fallback for the moment between connect and the first exchange; the 23-byte
  // BLE floor is the last resort. Reading the cache first would cap every
  // notification at 20 bytes on a link that had negotiated far more.
  uint16_t mtu = ble_ ? ble_->peerMtu() : 0;
  if (mtu == 0) mtu = negotiatedMtu_.load(std::memory_order_relaxed);
  // Still 0 means no exchange has happened (or the peer has gone). Assume the
  // floor rather than the 517 this server asked for: an optimistic guess here is
  // exactly how a document ends up truncated on the wire.
  if (mtu < BLE_ATT_MTU_MINIMUM) mtu = BLE_ATT_MTU_MINIMUM;
  const size_t cap = static_cast<size_t>(mtu) - BLE_ATT_NOTIFY_OVERHEAD;
  return cap < BLE_STATUS_NOTIFY_MAX_BYTES ? cap : BLE_STATUS_NOTIFY_MAX_BYTES;
}

std::string BleLink::buildReadJson() const {
  // Bounded for the same reason the notification is: a value over the ATT
  // ceiling is served truncated, and truncated JSON is indistinguishable to the
  // client from a broken reader. Sheds decoration, then capability lists, then
  // the clock -- all re-derivable -- and never touches identity, the nonce, or
  // auth_error.
  for (unsigned detail = STATUS_DETAIL_MAX;; --detail) {
    std::string json = buildStatusJson(StatusScope::READ, detail);
    if (json.size() <= BLE_ATT_ATTR_MAX_BYTES || detail == 0) {
      if (json.size() > BLE_ATT_ATTR_MAX_BYTES) {
        // Nothing sheddable is left and it still does not fit. Log it loudly
        // rather than hand the stack a value it will silently cut in half.
        LOG_ERR("BLE", "status read is %u bytes, over the %u-byte ATT ceiling",
                static_cast<unsigned>(json.size()), static_cast<unsigned>(BLE_ATT_ATTR_MAX_BYTES));
      } else if (detail < STATUS_DETAIL_MAX) {
        LOG_DBG("BLE", "status read shed to detail %u (%u bytes)", detail, static_cast<unsigned>(json.size()));
      }
      return json;
    }
  }
}

std::string BleLink::buildNotifyJson(const size_t capBytes) const {
  for (unsigned detail = STATUS_DETAIL_MAX;; --detail) {
    std::string json = buildStatusJson(StatusScope::NOTIFY, detail);
    if (json.size() <= capBytes) return json;
    if (detail == 0) break;
  }
  // Not even `{"state":"..."}` fits -- a 23-byte MTU with a long state name.
  // Returning `{}` here was worse than returning nothing: it is a valid document
  // that says nothing, so the client parsed it, found no `state`, and reported
  // the reader unreadable. An empty string means "do not ring the doorbell"; the
  // GATT read still carries the whole session, and truncating is still never an
  // option. See publish().
  return {};
}

void BleLink::refreshPingSnapshot(const bool withLibrary) {
  if (withLibrary) {
    BookLibraryIndex::Fingerprint fp;
    if (BookLibraryIndex::fingerprint(BOOKS_ROOT, fp)) {
      pingLibBooks_ = fp.books;
      pingLibHash_ = fp.hash;
      pingLibDirty_ = false;
    }
  }

  // One sidecar read, no book opened. See ProgressFile.h: the record is eleven
  // bytes and carries the percentage precisely so it does not have to be
  // re-derived by opening the book.
  pingBook_.clear();
  pingPct_ = -1.0f;
  pingOpen_ = false;
  const std::string& openPath = APP_STATE.openEpubPath;
  if (openPath.empty()) return;
  const auto slash = openPath.find_last_of('/');
  pingBook_ = slash == std::string::npos ? openPath : openPath.substr(slash + 1);
  // openEpubPath survives closing the book (it is what a wake resumes), so it
  // cannot say on its own whether the book is open; the activity stack can.
  pingOpen_ = activityManager.isReaderActivity();

  uint32_t epoch = 0;
  uint16_t percentBp = 0;
  bool hasPercent = false;
  if (ProgressFile::readSidecar(BookProgressSync::cachePathForBook(openPath), epoch, percentBp, hasPercent) && hasPercent) {
    pingPct_ = static_cast<float>(percentBp) / 10000.0f;
  }
}

void BleLink::notePositionChanged() {
  refreshPingSnapshot(false);
  statusDirty_ = true;
}

void BleLink::noteLibraryChanged() {
  // Flagged, not walked. The caller is usually finishing a transfer and the
  // heartbeat is a second away; doing the directory walk on this thread would
  // charge the walk to whatever just wrote to the card.
  pingLibDirty_ = true;
  statusDirty_ = true;
}

void BleLink::notifySleeping() {
  refreshPingSnapshot(pingLibDirty_);
  sleeping_ = true;
  publishStatusNow();
}

void BleLink::publishStatus() {
  statusDirty_ = false;
  if (!ble_) return;
  ble_->publish(buildReadJson(), buildNotifyJson(notifyCapBytes()));

  // Repaint whatever is on screen when the BLE indicator would change. Every
  // header draws that indicator, but observers are single-slot and the Store or
  // the pairing page usually holds it, so most screens never hear about the
  // link at all. Not while a book is open: a page of text is the one place a
  // repaint costs the user something, and the indicator is not worth it.
  const bool authenticated = sessionAuthenticated();
  if (authenticated != lastPublishedAuth_) {
    lastPublishedAuth_ = authenticated;
    if (!activityManager.isReaderActivity()) activityManager.requestUpdate();
  }
}

std::string BleLink::buildStatusJson(const StatusScope scope, const unsigned detail) const {
  JsonDocument doc;
  const std::string state = stateName(state_);
  // The notification is a doorbell, the read is authoritative. A READ carries
  // the whole session; a NOTIFY carries what the client cannot cheaply re-derive
  // and drops the rest until it fits the ATT payload. Everything dropped here is
  // still one GATT read away, and the client already has to read to get the
  // capability lists it saw at connect time.
  const bool full = (scope == StatusScope::READ);
  // READ sheds too, or it grows past the 512-byte ATT ceiling and comes back
  // truncated. What it sheds is only ever re-derivable: the trims below drop
  // decoration and capability lists, never identity, never the nonce, and never
  // the reason a hello was refused -- those are what a client cannot recover
  // without them, and the pairing path needs all three.
  const bool wantDecoration = !full || detail >= 5;  // firmware_name, ota/resume flags
  const bool wantKinds = !full || detail >= 4;       // upload_kinds / download_kinds
  const bool wantClock = !full || detail >= 3;       // clock_supported, device_time
  const bool wantSession = full || detail >= 5;   // session-constant capability facts
  const bool wantIdentity = full || detail >= 4;  // who this device is, and to whom
  const bool wantProgress = full || detail >= 2;  // byte counters and the error text
  // NEVER shed. The reader is a peripheral and cannot call out, so a notification
  // carrying `pending` is the ONLY way it can ask the phone anything. Shedding it
  // to make the document fit produces a doorbell with no question behind it: the
  // reader waits out its whole budget and reports that the app did not answer,
  // while the app was never told there was anything to answer. That is the same
  // class of silent loss as the request ids that restarted per screen.
  //
  // Everything else in a notification is re-readable; this is not.
  const bool wantPending = true;  // the store's request channel
  // Only below level 3 does the request shed the geometry and the deadline the
  // app builds its answer from; a GATT read still has them.
  const bool pendingTerse = !full && detail < 3;

  doc["state"] = state.c_str();
  if (!sessionAuthenticated()) {
    // Before hello or pair: only what authentication needs.
    if (full || detail >= 4) doc["protocol_version"] = BLE_PROTOCOL_VERSION;
    if (full) {
      doc["device_id"] = deviceId_.c_str();
      doc["device_nonce"] = deviceNonce_.c_str();
    }
    if (full || detail >= 3) {
      doc["has_trusted_host"] = BLE_TRUSTED_HOSTS.hasHosts();
      doc["pairing_window"] = pairingWindowOpen();
    }
    if ((full || detail >= 2) && !authErrorMessage_.empty()) doc["auth_error"] = authErrorMessage_.c_str();
    String preAuth;
    serializeJson(doc, preAuth);
    return preAuth.c_str();
  }
  // The heartbeat payload. Small and deliberately never shed: it is the whole
  // reason a notification is worth sending at all, and everything the shrink
  // ladder drops below is re-readable while these are what tell the app there
  // is anything to re-read.
  //
  // The fingerprint answers "is your idea of my library still right?" without
  // sending the library: BookLibraryIndex::Fingerprint is name+size+mtime and
  // exists to answer exactly that. A mismatch is the app's cue to run a full
  // mirror; a match means it can skip one entirely.
  if (pingLibBooks_ > 0 || pingLibHash_ != 0) {
    doc["lib_n"] = pingLibBooks_;
    doc["lib_h"] = pingLibHash_;
  }
  // Position, so the app can update its row without a kosync round trip.
  if (pingPct_ >= 0.0f) doc["pct"] = pingPct_;
  if (pingOpen_) doc["open"] = true;
  if (sleeping_) doc["sleeping"] = true;
  if (wantSession) doc["protocol_version"] = BLE_PROTOCOL_VERSION;
  if (full) {
    if (wantDecoration) {
      doc["firmware_name"] = "Bluecarrel";
      doc["firmware_ota_supported"] = true;
      doc["resume_supported"] = true;
    }
    if (wantKinds) {
    JsonArray uploadKinds = doc["upload_kinds"].to<JsonArray>();
    uploadKinds.add("book");
    uploadKinds.add("bmp");
    uploadKinds.add("firmware");
    uploadKinds.add("progress");
    uploadKinds.add("catalog_page");
    uploadKinds.add("catalog_detail");
    uploadKinds.add("settings");
    uploadKinds.add("book_meta");
    JsonArray downloadKinds = doc["download_kinds"].to<JsonArray>();
    downloadKinds.add("about");
    downloadKinds.add("crash_report");
    downloadKinds.add("library");
    downloadKinds.add("progress_result");
    downloadKinds.add("settings");
    }
  }
  // The store is a capability of this firmware, not of this screen: an app can
  // see it is supported while the user is still on the transfer screen.
  if (wantSession) doc["store_supported"] = true;
  if (wantSession && wantClock) {
    doc["clock_supported"] = halClock.isAvailable();
    // Omitted, never zeroed, when the device does not know the time: the client
    // uses its absence to decide it must send `set_time`, and its value to notice
    // drift. A `0` here would read as a real 1970 instant.
    uint32_t deviceEpoch = 0;
    if (halClock.getEpoch(deviceEpoch)) doc["device_time"] = deviceEpoch;
  }
  if (full) {
    doc["device_id"] = deviceId_.c_str();
    doc["device_nonce"] = deviceNonce_.c_str();
  }
  if (wantIdentity) {
    doc["has_trusted_host"] = BLE_TRUSTED_HOSTS.hasHosts();
    if (!trustedHostName_.empty()) doc["trusted_host"] = trustedHostName_.c_str();
    // The app trusts this reader only once it has checked this proof.
    if (!readerProof_.empty()) doc["reader_proof"] = readerProof_.c_str();
    if (hostPaired_) doc["paired"] = true;
  }
  if (wantProgress && (expectedSize_ > 0 || state_ == State::SENDING || state_ == State::SENT)) {
    const std::string kind = transferKindName(transferKind_);
    if (!kind.empty()) doc["kind"] = kind.c_str();
    if (state_ == State::SAVED && transferKind_ == TransferKind::PROGRESS) {
      // A finished batch reports its outcome, not its byte count. Kept this
      // short deliberately: the per-entry outcomes are a `progress_result`
      // download precisely because a shelf-sized array fits in no notification.
      doc["entries"] = progressEntries_;
      doc["applied"] = progressApplied_;
    } else if (state_ == State::SENDING || state_ == State::SENT) {
      doc["sent"] = sentBytes_;
      doc["size"] = expectedSize_;
    } else {
      doc["received"] = receivedBytes_;
      if (uploadResumable_) doc["resumable"] = true;
      doc["ack_bytes"] = uploadAckBytes_;
      doc["size"] = expectedSize_;
      // Only for a saved book whose start_put carried `position`.
      if (state_ == State::SAVED && transferKind_ == TransferKind::BOOK && positionGiven_) {
        doc["position_applied"] = positionApplied_;
      }
    }
  }
  if (wantIdentity && state_ == State::SAVED && !savedPath_.empty()) {
    doc["name"] = fileName_.c_str();
    doc["path"] = savedPath_.c_str();
  }
  if (wantIdentity && state_ == State::SENT) doc["name"] = fileName_.c_str();
  // Which book `pct` is about. Gated with the other identity fields because a
  // filename is the one variable-length part of the heartbeat; when it is shed
  // the app still learns THAT the position moved and can read for the rest.
  if (wantIdentity && !pingBook_.empty()) doc["book"] = pingBook_.c_str();
  // `state` already says ERROR; the message is the part that can be any length,
  // so it is the part that goes when the payload is tight.
  if (wantProgress && state_ == State::ERROR && !errorMessage_.empty()) doc["error"] = errorMessage_.c_str();
  // Why the last hello or pair was refused. Separate from `error` and not tied to
  // State::ERROR, because a refused hello leaves the session healthy. Cleared by
  // the next accepted hello.
  if (wantProgress && !authErrorMessage_.empty()) doc["auth_error"] = authErrorMessage_.c_str();
  // The request channel. When the device wants something from the app it says so
  // here, and the app answers with an upload naming the same `req`. Absent
  // whenever nothing is outstanding.
  if (wantPending && store_) store_->describePending(doc, pendingTerse);

  String output;
  serializeJson(doc, output);
  return output.c_str();
}

#endif  // FREEINK_CAP_BLE_TRANSFER
