#include "FirmwareStaging.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace firmware_staging {
namespace {

constexpr size_t SHA256_HEX_LEN = 64;

bool isHexDigit(const char c) {
  return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

bool imageStaged() { return Storage.exists(IMAGE_PATH) && Storage.exists(HASH_PATH); }

bool readExpectedHash(std::string& outHex) {
  outHex.clear();
  // A hash file is 64 bytes plus whatever `sha256sum` appended; anything longer
  // than a short line is not one, and reading it whole is bounded on purpose.
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
  for (char& c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  outHex = std::move(hex);
  return true;
}

bool writeExpectedHash(const std::string& hex) {
  if (hex.length() != SHA256_HEX_LEN) return false;
  // `sha256sum` layout: digest, two spaces, filename. A human checking the card
  // with `sha256sum -c` gets a file that works, and readExpectedHash() only ever
  // looks at the first token either way.
  return Storage.writeFile(HASH_PATH, String((hex + "  " + IMAGE_NAME + "\n").c_str()));
}

bool readVersion(std::string& out) {
  out.clear();
  char buffer[48] = {};
  const size_t read = Storage.readFileToBuffer(VERSION_PATH, buffer, sizeof(buffer) - 1);
  if (read == 0) return false;
  std::string value(buffer);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  if (value.empty()) return false;
  out = std::move(value);
  return true;
}

bool writeVersion(const std::string& version) {
  if (version.empty()) return false;
  return Storage.writeFile(VERSION_PATH, String((version + "\n").c_str()));
}

void clearStaged() {
  if (Storage.exists(IMAGE_PATH) && !Storage.remove(IMAGE_PATH)) {
    LOG_ERR("FWDROP", "could not remove %s", IMAGE_PATH);
  }
  if (Storage.exists(HASH_PATH) && !Storage.remove(HASH_PATH)) {
    LOG_ERR("FWDROP", "could not remove %s", HASH_PATH);
  }
  if (Storage.exists(PART_PATH)) Storage.remove(PART_PATH);
  if (Storage.exists(VERSION_PATH)) Storage.remove(VERSION_PATH);
}

}  // namespace firmware_staging
