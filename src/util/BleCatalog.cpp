#include "BleCatalog.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "TaskWatchdog.h"

namespace {

constexpr const char* TAG = "BCAT";
constexpr size_t HEADER_BYTES = 8;
constexpr size_t COPY_CHUNK_BYTES = 512;

// Trims a UTF-8 string to at most `maxBytes` without splitting a code point.
// The app is supposed to have truncated already; this is the device refusing to
// trust that, and it must not leave half a character behind for the renderer.
std::string clampUtf8(std::string value, const size_t maxBytes) {
  if (value.size() <= maxBytes) return value;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) end--;
  value.resize(end);
  return value;
}

// The same shape the `book` upload kind enforces. A catalogue entry names the
// file the app would upload, so it has to clear the same bar -- a page must
// never be able to propose a path that leaves /Books.
bool isSafeCatalogFilename(const std::string& value) {
  if (value.empty() || value.length() > BleCatalog::MAX_FILENAME_BYTES || value[0] == '.') return false;
  for (const char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-' || c == ' ') continue;
    return false;
  }
  return true;
}

// Ids come back to the app verbatim in the next request, so they are kept to
// printable ASCII with no JSON-hostile characters.
bool isSafeCatalogId(const std::string& value) {
  if (value.empty() || value.length() > BleCatalog::MAX_ID_BYTES) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    const auto uc = static_cast<unsigned char>(c);
    return uc > 32 && uc < 127 && c != '"' && c != '\\';
  });
}

bool readExact(HalFile& file, uint8_t* out, const size_t bytes) {
  size_t done = 0;
  while (done < bytes) {
    const int read = file.read(out + done, bytes - done);
    if (read <= 0) return false;
    done += static_cast<size_t>(read);
  }
  return true;
}

// Copies `bytes` from the current position of `in` into a new file, 512 bytes at
// a time. This is the whole reason a page never needs to be resident: the
// thumbnails go card-to-card and only the copy buffer lives in RAM.
bool extractThumbnail(HalFile& in, const std::string& outPath, const size_t bytes) {
  if (Storage.exists(outPath.c_str())) Storage.remove(outPath.c_str());
  HalFile out;
  if (!Storage.openFileForWrite(TAG, outPath, out)) return false;

  std::array<uint8_t, COPY_CHUNK_BYTES> buffer = {};
  size_t remaining = bytes;
  bool sawMagic = false;
  while (remaining > 0) {
    const size_t wanted = std::min(remaining, buffer.size());
    const int read = in.read(buffer.data(), wanted);
    if (read <= 0) {
      out.close();
      Storage.remove(outPath.c_str());
      return false;
    }
    if (!sawMagic) {
      sawMagic = true;
      // Cheap sanity check so a garbled page fails here rather than in the
      // renderer; Bitmap::parseHeaders does the real validation at draw time.
      if (read < 2 || buffer[0] != 'B' || buffer[1] != 'M') {
        out.close();
        Storage.remove(outPath.c_str());
        return false;
      }
    }
    if (out.write(buffer.data(), static_cast<size_t>(read)) != static_cast<size_t>(read)) {
      out.close();
      Storage.remove(outPath.c_str());
      return false;
    }
    remaining -= static_cast<size_t>(read);
  }
  out.flush();
  out.close();
  return true;
}

bool readEntry(JsonObjectConst obj, const size_t descriptionCap, const char* booksRoot, BleCatalog::Entry& entry) {
  entry.id = obj["id"] | "";
  if (!isSafeCatalogId(entry.id)) return false;
  entry.title = clampUtf8(obj["title"] | "", BleCatalog::MAX_TITLE_BYTES);
  if (entry.title.empty()) entry.title = entry.id;
  entry.author = clampUtf8(obj["author"] | "", BleCatalog::MAX_AUTHOR_BYTES);
  entry.description = clampUtf8(obj["description"] | "", descriptionCap);
  entry.format = clampUtf8(obj["format"] | "", 16);
  entry.size = obj["size"] | 0u;
  // Optional throughout: absent means "not supplied", never an error.
  entry.series = clampUtf8(obj["series"] | "", BleCatalog::MAX_META_BYTES);
  entry.publisher = clampUtf8(obj["publisher"] | "", BleCatalog::MAX_META_BYTES);
  entry.published = clampUtf8(obj["published"] | "", 16);
  entry.language = clampUtf8(obj["language"] | "", 16);
  entry.tags = clampUtf8(obj["tags"] | "", BleCatalog::MAX_TAGS_BYTES);
  const char* calibreUuid = obj["calibre_uuid"] | "";
  entry.calibreUuid.assign(calibreUuid, strnlen(calibreUuid, 65));

  const std::string filename = obj["filename"] | "";
  // An entry with no usable filename is still listed -- the user can read about
  // it -- but the detail view will not offer to fetch it.
  entry.filename = isSafeCatalogFilename(filename) ? filename : std::string();
  entry.onDevice = false;
  if (!entry.filename.empty()) {
    const std::string target = std::string(booksRoot) + "/" + entry.filename;
    entry.onDevice = Storage.exists(target.c_str());
  }
  return true;
}

}  // namespace

void BleCatalog::clearThumbnails(const char* thumbDir) {
  if (!thumbDir || !Storage.exists(thumbDir)) return;
  for (size_t i = 0; i < PAGE_LIMIT; i++) {
    char path[96];
    snprintf(path, sizeof(path), "%s/t%u.bmp", thumbDir, static_cast<unsigned>(i));
    if (Storage.exists(path)) Storage.remove(path);
  }
}

bool BleCatalog::parseContainer(const char* path, const char* thumbDir, const uint32_t expectedReq, const bool detail,
                                const char* booksRoot, Page& page, std::string& error) {
  page = Page{};
  error.clear();

  if (!Storage.ensureDirectoryExists(thumbDir)) {
    error = "no catalog dir";
    return false;
  }
  clearThumbnails(thumbDir);

  HalFile in;
  if (!Storage.openFileForRead(TAG, path, in)) {
    error = "catalog unreadable";
    return false;
  }

  uint8_t header[HEADER_BYTES] = {};
  if (!readExact(in, header, HEADER_BYTES)) {
    in.close();
    error = "catalog truncated";
    return false;
  }
  if (header[0] != 'C' || header[1] != 'P' || header[2] != 'C' || header[3] != 'T') {
    in.close();
    error = "not a catalog page";
    return false;
  }
  if (header[4] != 1) {
    in.close();
    error = "catalog version";
    return false;
  }
  const size_t jsonLen = static_cast<size_t>(header[6]) | (static_cast<size_t>(header[7]) << 8);
  if (jsonLen == 0 || jsonLen > MAX_JSON_BYTES) {
    in.close();
    error = "catalog header size";
    return false;
  }

  // The only thing this parser holds: the header text, capped at 6 KB.
  std::string json;
  json.resize(jsonLen);
  if (!readExact(in, reinterpret_cast<uint8_t*>(&json[0]), jsonLen)) {
    in.close();
    error = "catalog truncated";
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    in.close();
    error = "catalog json";
    return false;
  }
  json.clear();
  json.shrink_to_fit();

  const uint32_t req = doc["req"] | 0u;
  if (req != expectedReq) {
    in.close();
    // The device asks one question at a time and refuses answers to any other,
    // so a slow reply that arrives after a timeout cannot repaint the screen
    // under a user who has already moved on.
    error = "stale request";
    return false;
  }
  page.req = req;
  page.offset = doc["offset"] | 0u;
  page.total = doc["total"] | 0u;

  const size_t descriptionCap = detail ? MAX_DETAIL_DESCRIPTION_BYTES : MAX_LIST_DESCRIPTION_BYTES;
  std::vector<uint32_t> thumbBytes;

  if (detail) {
    Entry entry;
    if (!readEntry(doc["item"].as<JsonObjectConst>(), descriptionCap, booksRoot, entry)) {
      in.close();
      error = "catalog item";
      return false;
    }
    const uint32_t bytes = doc["item"]["thumb"] | 0u;
    if (bytes > MAX_THUMB_BYTES) {
      in.close();
      error = "cover too large";
      return false;
    }
    thumbBytes.push_back(bytes);
    page.entries.push_back(std::move(entry));
  } else {
    JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    for (JsonObjectConst obj : items) {
      if (page.entries.size() >= PAGE_LIMIT) break;
      Entry entry;
      if (!readEntry(obj, descriptionCap, booksRoot, entry)) {
        in.close();
        error = "catalog item";
        return false;
      }
      const uint32_t bytes = obj["thumb"] | 0u;
      if (bytes > MAX_THUMB_BYTES) {
        in.close();
        error = "cover too large";
        return false;
      }
      thumbBytes.push_back(bytes);
      page.entries.push_back(std::move(entry));
    }
    if (page.entries.empty() && page.total > 0) {
      in.close();
      error = "empty catalog page";
      return false;
    }
  }

  // The thumbnails follow the header in item order. Each is seeked to by its
  // own running offset rather than read straight through, so one cover that
  // fails to extract cannot desynchronise every cover after it.
  const size_t fileSize = in.fileSize();
  size_t cursor = HEADER_BYTES + jsonLen;
  for (size_t i = 0; i < page.entries.size(); i++) {
    resetTaskWatchdogIfSubscribed();
    const size_t bytes = thumbBytes[i];
    if (bytes == 0) continue;
    if (cursor + bytes > fileSize) {
      in.close();
      error = "catalog truncated";
      return false;
    }
    char outPath[96];
    snprintf(outPath, sizeof(outPath), "%s/t%u.bmp", thumbDir, static_cast<unsigned>(i));
    if (!in.seek(cursor)) {
      in.close();
      error = "catalog seek";
      return false;
    }
    if (extractThumbnail(in, outPath, bytes)) {
      page.entries[i].thumbPath = outPath;
    } else {
      // A bad cover costs its own row, not the page: the entry simply draws
      // without art, which is also what an entry with no cover at all does.
      LOG_ERR(TAG, "Could not extract cover %u", static_cast<unsigned>(i));
    }
    cursor += bytes;
  }

  in.close();
  LOG_DBG(TAG, "Catalog page req=%u offset=%u total=%u entries=%u", static_cast<unsigned>(page.req),
          static_cast<unsigned>(page.offset), static_cast<unsigned>(page.total),
          static_cast<unsigned>(page.entries.size()));
  return true;
}
