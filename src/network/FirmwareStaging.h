#pragma once

#include <cstdint>
#include <string>

#include "FirmwareSignature.h"

// The firmware drop folder: the one place on the SD card an update image may be
// staged, and the one contract the phone app has to match.
//
//   /firmware/firmware.bin           the ESP32 application image
//   /firmware/firmware.bin.sha256    its SHA-256, as text
//   /firmware/firmware.bin.version   its build stamp, yyyyMMdd.HHmm
//   /firmware/firmware.bin.sig       ECDSA signature, hex (FirmwareSignature.h)
//
// The .sha256 file's first whitespace-delimited token is a 64-character hex
// SHA-256 of the image, the first field of `sha256sum firmware.bin` output. It is
// an integrity check only: anyone who can write the image can write it. The
// signature over version and digest is what makes an image installable.
namespace firmware_staging {

constexpr const char* DIR = "/firmware";
constexpr const char* IMAGE_PATH = "/firmware/firmware.bin";
constexpr const char* IMAGE_NAME = "firmware.bin";
constexpr const char* HASH_PATH = "/firmware/firmware.bin.sha256";
// Where a BLE upload accumulates before it is renamed into place. Dot-prefixed
// so a half-finished push is not mistaken for a staged image.
constexpr const char* PART_PATH = "/firmware/.firmware.bin.part";
constexpr const char* VERSION_PATH = "/firmware/firmware.bin.version";
constexpr const char* SIG_PATH = "/firmware/firmware.bin.sig";

// True when both the image and its companion hash file are present.
bool imageStaged();

// Reads the expected digest out of HASH_PATH. Returns false when the file is
// missing, unreadable, or does not begin with 64 hex characters. `outHex` is
// lowercased on success.
bool readExpectedHash(std::string& outHex);

// Writes `hex` to HASH_PATH in `sha256sum` layout.
bool writeExpectedHash(const std::string& hex);

// Reads the build stamp at VERSION_PATH into `out`, surrounding whitespace
// trimmed. Returns false (leaving `out` empty) when there is none.
bool readVersion(std::string& out);

// Writes `version` to VERSION_PATH. Returns false for an empty stamp or a failed
// write.
bool writeVersion(const std::string& version);

// Reads the signature at SIG_PATH into `hex` (lowercased, whitespace trimmed).
// Returns false (leaving `hex` empty) when the file is missing, too long or not hex.
bool readSignature(std::string& hex);

// Writes `hex` to SIG_PATH, lowercased. Returns false for malformed hex (empty,
// odd length, over 256 characters) or a failed write.
bool writeSignature(const std::string& hex);

// SHA-256 of IMAGE_PATH as lowercase hex, read in one pass.
bool hashImage(std::string& outHex);

// Every install check for the staged image, given the digest its bytes hashed to:
// .sha256 match, .sig present, .version well-formed, signature valid, newer build.
firmware_signature::Verdict checkStaged(const std::string& actualSha256Hex);

// Bumped by every write and clear above, so watchers notice a restage made on
// this device even when the new files look the same size.
uint32_t stageGeneration();

// Remove the image, its companion files and any partial upload. Missing files
// are not an error.
void clearStaged();

}  // namespace firmware_staging
