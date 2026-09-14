#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Firmware image signatures (docs/security-v2.md section 3).
//
// Signed message, ASCII, no terminator: "X4FW1|" + version + "|" + sha256hex
//   version    yyyyMMdd.HHmm build stamp
//   sha256hex  64 lowercase hex characters, the SHA-256 of the image file
// Signature: ECDSA P-256 over SHA-256(message), DER encoded, carried as hex.
// Key: FIRMWARE_SIGNING_PUBKEY_DER (FirmwareSigningKey.h).
namespace firmware_signature {

// DER ECDSA P-256 signatures are at most 72 bytes; the transport cap is 256 hex characters.
constexpr size_t MAX_SIGNATURE_HEX = 256;

// Why a staged image is or is not installable, in the order the checks run.
enum class Verdict : uint8_t {
  OK,
  BAD_IMAGE,      // no usable .sha256, unreadable, or too small to be an image
  HASH_MISMATCH,  // image bytes do not hash to .sha256
  UNSIGNED,       // no .sig
  NO_VERSION,     // .version missing or not yyyyMMdd.HHmm
  BAD_SIGNATURE,  // .sig malformed or does not verify
  NOT_NEWER,      // signed, but not newer than the running build
};

const char* verdictName(Verdict verdict);

// yyyyMMdd.HHmm: eight digits, a dot, four digits.
bool isWellFormedVersion(const std::string& version);

// `version` is well-formed and sorts after X4_BUILD_STAMP.
bool isNewerThanRunning(const std::string& version);

// Non-empty, even length, at most `maxChars`, hex digits only (either case).
bool isHex(const std::string& text, size_t maxChars);

// Lowercase hex of `len` bytes.
std::string toHex(const uint8_t* data, size_t len);

// True when `signatureHex` is a valid signature by the firmware key over the message
// built from `version` and `sha256Hex`. Both inputs must already be well-formed.
bool verify(const std::string& version, const std::string& sha256Hex, const std::string& signatureHex);

// All install checks for an image whose bytes hashed to `actualSha256Hex`, given the
// staged companion values (empty string for a missing file).
Verdict check(const std::string& actualSha256Hex, const std::string& expectedSha256Hex, const std::string& version,
              const std::string& signatureHex);

}  // namespace firmware_signature
