#include "FirmwareStaging.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <cctype>

#include "FirmwareSignature.h"

namespace firmware_staging {
namespace {

constexpr size_t SHA256_HEX_LEN = 64;
constexpr size_t HASH_CHUNK_BYTES = 4UL * 1024UL;

std::atomic<uint32_t> gGeneration{1};

void bumpGeneration() { gGeneration.fetch_add(1, std::memory_order_relaxed); }

bool isHexDigit(const char c) {
  return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isSpace(const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

void trim(std::string& value) {
  while (!value.empty() && isSpace(value.back())) value.pop_back();
  size_t start = 0;
  while (start < value.size() && isSpace(value[start])) start++;
  value.erase(0, start);
}

void lowercase(std::string& value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

bool imageStaged() { return Storage.exists(IMAGE_PATH) && Storage.exists(HASH_PATH); }

bool readExpectedHash(std::string& outHex) {
  outHex.clear();
  // A hash file is 64 bytes plus whatever `sha256sum` appended; the read is bounded on purpose.
  char buffer[160] = {};
  const size_t read = Storage.readFileToBuffer(HASH_PATH, buffer, sizeof(buffer));
  if (read == 0) return false;

  // Skip leading whitespace, then take exactly the first token.
  const char* cursor = buffer;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
  size_t length = 0;
  while (cursor[length] != '\0' && cursor[length] != ' ' && cursor[length] != '\t' && cursor[length] != '\r' &&
         cursor[length] != '\n') {
    length++;
  }
  if (length != SHA256_HEX_LEN) {
    LOG_ERR("FWDROP", "%s: expected a 64-character hex digest, found %u characters", HASH_PATH,
            static_cast<unsigned>(length));
    return false;
  }
  std::string hex(cursor, length);
  if (!std::all_of(hex.begin(), hex.end(), isHexDigit)) {
    LOG_ERR("FWDROP", "%s: digest is not hexadecimal", HASH_PATH);
    return false;
  }
  lowercase(hex);
  outHex = std::move(hex);
  return true;
}

bool writeExpectedHash(const std::string& hex) {
  if (hex.length() != SHA256_HEX_LEN) return false;
  bumpGeneration();
  // `sha256sum` layout, so `sha256sum -c` works on the card.
  return Storage.writeFile(HASH_PATH, String((hex + "  " + IMAGE_NAME + "\n").c_str()));
}

bool readVersion(std::string& out) {
  out.clear();
  char buffer[48] = {};
  const size_t read = Storage.readFileToBuffer(VERSION_PATH, buffer, sizeof(buffer) - 1);
  if (read == 0) return false;
  std::string value(buffer);
  trim(value);
  if (value.empty()) return false;
  out = std::move(value);
  return true;
}

bool writeVersion(const std::string& version) {
  if (version.empty()) return false;
  bumpGeneration();
  return Storage.writeFile(VERSION_PATH, String((version + "\n").c_str()));
}

bool readSignature(std::string& hex) {
  hex.clear();
  char buffer[firmware_signature::MAX_SIGNATURE_HEX + 8] = {};
  const size_t read = Storage.readFileToBuffer(SIG_PATH, buffer, sizeof(buffer) - 1);
  if (read == 0) return false;
  // A buffer filled to the brim means the file may be longer than any signature.
  if (read >= sizeof(buffer) - 2) {
    LOG_ERR("FWDROP", "%s: too long to be a signature", SIG_PATH);
    return false;
  }
  std::string value(buffer);
  trim(value);
  if (!firmware_signature::isHex(value, firmware_signature::MAX_SIGNATURE_HEX)) {
    LOG_ERR("FWDROP", "%s: not a hex signature", SIG_PATH);
    return false;
  }
  lowercase(value);
  hex = std::move(value);
  return true;
}

bool writeSignature(const std::string& hex) {
  if (!firmware_signature::isHex(hex, firmware_signature::MAX_SIGNATURE_HEX)) return false;
  std::string value = hex;
  lowercase(value);
  bumpGeneration();
  return Storage.writeFile(SIG_PATH, String((value + "\n").c_str()));
}

bool hashImage(std::string& outHex) {
  outHex.clear();
  HalFile file;
  if (!Storage.openFileForRead("FWDROP", IMAGE_PATH, file)) return false;
  const size_t size = file.fileSize();
  auto buffer = makeUniqueNoThrow<uint8_t[]>(HASH_CHUNK_BYTES);
  if (!buffer) {
    LOG_ERR("FWDROP", "OOM: %u bytes", static_cast<unsigned>(HASH_CHUNK_BYTES));
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t hashed = 0;
  while (hashed < size) {
    const size_t wanted = std::min(HASH_CHUNK_BYTES, size - hashed);
    const int got = file.read(buffer.get(), wanted);
    if (got <= 0 || static_cast<size_t>(got) != wanted) {
      LOG_ERR("FWDROP", "read failed at %u while hashing %s", static_cast<unsigned>(hashed), IMAGE_PATH);
      mbedtls_sha256_free(&sha);
      return false;
    }
    mbedtls_sha256_update(&sha, buffer.get(), wanted);
    hashed += wanted;
  }
  uint8_t digest[32] = {};
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  outHex = firmware_signature::toHex(digest, sizeof(digest));
  return true;
}

firmware_signature::Verdict checkStaged(const std::string& actualSha256Hex) {
  std::string expected;
  std::string version;
  std::string signature;
  readExpectedHash(expected);
  readVersion(version);
  // A .sig that is present but malformed is a bad signature, not a missing one.
  if (!readSignature(signature) && Storage.exists(SIG_PATH)) signature = "?";
  return firmware_signature::check(actualSha256Hex, expected, version, signature);
}

uint32_t stageGeneration() { return gGeneration.load(std::memory_order_relaxed); }

void clearStaged() {
  bumpGeneration();
  if (Storage.exists(IMAGE_PATH) && !Storage.remove(IMAGE_PATH)) {
    LOG_ERR("FWDROP", "could not remove %s", IMAGE_PATH);
  }
  if (Storage.exists(HASH_PATH) && !Storage.remove(HASH_PATH)) {
    LOG_ERR("FWDROP", "could not remove %s", HASH_PATH);
  }
  if (Storage.exists(PART_PATH)) Storage.remove(PART_PATH);
  if (Storage.exists(VERSION_PATH)) Storage.remove(VERSION_PATH);
  if (Storage.exists(SIG_PATH) && !Storage.remove(SIG_PATH)) {
    LOG_ERR("FWDROP", "could not remove %s", SIG_PATH);
  }
}

}  // namespace firmware_staging
