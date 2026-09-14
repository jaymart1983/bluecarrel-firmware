#pragma once

#include <string>

// The firmware drop folder: the one place on the SD card an update image may be
// staged, and the one contract the phone app has to match.
//
//   /firmware/firmware.bin           the ESP32 application image
//   /firmware/firmware.bin.sha256    its SHA-256, as text
//
// The companion file's first whitespace-delimited token is a 64-character
// lowercase hex SHA-256 of the image. That is exactly the first field of
// `sha256sum firmware.bin` output, so the file a person would produce by hand
// and the file the app writes are the same file. Case is not significant and a
// trailing newline is fine.
//
// WHAT THE HASH DOES AND DOES NOT DO. It proves the image on the card is the
// image whoever wrote the hash meant to put there -- it catches a truncated
// copy, a half-finished BLE upload, a card that went bad. It proves NOTHING
// about who wrote it. Anyone who can write firmware.bin can write
// firmware.bin.sha256 in the same breath, so this is an INTEGRITY check, not an
// authenticity check, and it is not a signature. That is an accepted trade on a
// device whose SD card is already writable by anyone holding it and whose USB
// port already gives full control. The checks that actually stop a bad image
// bricking the reader are firmware_flash::validateImageFile() and the on-device
// confirmation prompt, and neither of those is weakened by any of this.
namespace firmware_staging {

constexpr const char* DIR = "/firmware";
constexpr const char* IMAGE_PATH = "/firmware/firmware.bin";
constexpr const char* IMAGE_NAME = "firmware.bin";
constexpr const char* HASH_PATH = "/firmware/firmware.bin.sha256";
// Where a BLE upload accumulates before it is renamed into place. Dot-prefixed
// so a half-finished push is not mistaken for a staged image by anything that
// merely lists the folder.
constexpr const char* PART_PATH = "/firmware/.firmware.bin.part";
// The build stamp the app sent with the image, for the update screens. Optional:
// an image dropped by hand has none, and nothing depends on it being there.
constexpr const char* VERSION_PATH = "/firmware/firmware.bin.version";

// True when both the image and its companion hash file are present.
bool imageStaged();

// Reads the expected digest out of HASH_PATH. Returns false when the file is
// missing, unreadable, or does not begin with 64 hex characters. `outHex` is
// lowercased on success.
bool readExpectedHash(std::string& outHex);

// Writes `hex` to HASH_PATH in `sha256sum` layout. Used by the BLE `firmware`
// upload, which has already hashed the bytes it received and so can produce the
// companion file itself rather than making the app send a second one.
bool writeExpectedHash(const std::string& hex);

// Reads the build stamp at VERSION_PATH into `out`, trailing whitespace trimmed.
// Returns false (leaving `out` empty) when there is none.
bool readVersion(std::string& out);

// Writes `version` to VERSION_PATH. Returns false for an empty stamp or a failed
// write.
bool writeVersion(const std::string& version);

// Remove the staged image, its companion hash, any partial upload and the build
// stamp after a flash (or a rejection), so the next boot does not offer the same
// update again forever. Missing files are not an error.
void clearStaged();

}  // namespace firmware_staging
