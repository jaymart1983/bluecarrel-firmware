#include "FirmwareWatcher.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "FirmwareStaging.h"

namespace {

// An idle look costs two existence checks, so it can be frequent without being
// felt. It is not more frequent than this because there is nothing to gain: a
// drop that arrives over USB is followed by a reboot anyway, and one that
// arrives over BLE is minutes of transfer.
constexpr unsigned long POLL_INTERVAL_MS = 30UL * 1000UL;
// Do not go looking during the boot sequence. The splash, the font cache and the
// shelf are all competing for the same SD bus in the first seconds.
constexpr unsigned long STARTUP_GRACE_MS = 20UL * 1000UL;
// One bite of hashing per main-loop tick. 4 KB is a couple of SD reads and a
// SHA-256 block run -- single-digit milliseconds -- so a page turn never waits on
// it, and a 3 MB image is still done inside a few hundred ticks. The buffer is
// static rather than automatic: the Arduino loop task's stack is not the place
// for kilobytes of scratch, and there is exactly one watcher on exactly one task.
constexpr size_t HASH_CHUNK_BYTES = 4UL * 1024UL;
uint8_t gHashBuffer[HASH_CHUNK_BYTES];
// Nothing smaller than this is a plausible ESP32 application image; refusing it
// here saves hashing a stray text file that happens to be called firmware.bin.
constexpr size_t MIN_IMAGE_BYTES = 64UL * 1024UL;

std::string toHex(const uint8_t digest[32]) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(64);
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  return out;
}

}  // namespace

FirmwareWatcher& FirmwareWatcher::getInstance() {
  static FirmwareWatcher instance;
  return instance;
}

void FirmwareWatcher::tick() {
  if (phase_ == Phase::HASHING) {
    const size_t wanted = std::min(HASH_CHUNK_BYTES, imageSize_ - hashedBytes_);
    const int read = image_.read(gHashBuffer, wanted);
    if (read <= 0) {
      abandon("could not read the staged image");
      return;
    }
    mbedtls_sha256_update(&sha_, gHashBuffer, static_cast<size_t>(read));
    hashedBytes_ += static_cast<size_t>(read);
    if (hashedBytes_ >= imageSize_) finishHash();
    return;
  }

  if (millis() < STARTUP_GRACE_MS) return;
  if (lastPollMs_ != 0 && millis() - lastPollMs_ < POLL_INTERVAL_MS) return;
  lastPollMs_ = millis();

  if (!firmware_staging::imageStaged()) {
    // The drop went away (flashed, or deleted by hand). Forget the verdict so a
    // future file with the same size is still looked at.
    if (phase_ != Phase::IDLE) {
      phase_ = Phase::IDLE;
      verdictSize_ = 0;
      verified_ = false;
    }
    return;
  }
  if (phase_ == Phase::READY) return;

  HalFile probe;
  if (!Storage.openFileForRead("FWDROP", firmware_staging::IMAGE_PATH, probe)) return;
  const size_t size = probe.fileSize();
  probe.close();

  // Already ruled on this exact drop. A rewritten image is a different size in
  // practice; when it is not, the next boot looks again.
  if (phase_ == Phase::DECLINED && size == verdictSize_) return;
  if (size < MIN_IMAGE_BYTES) {
    verdictSize_ = size;
    phase_ = Phase::DECLINED;
    LOG_ERR("FWDROP", "%s is %u bytes, too small to be a firmware image", firmware_staging::IMAGE_PATH,
            static_cast<unsigned>(size));
    return;
  }

  imageSize_ = size;
  beginHash();
}

void FirmwareWatcher::beginHash() {
  if (!firmware_staging::readExpectedHash(expectedHash_)) {
    abandon("no usable hash file beside the image");
    return;
  }
  if (!Storage.openFileForRead("FWDROP", firmware_staging::IMAGE_PATH, image_)) {
    abandon("could not open the staged image");
    return;
  }
  mbedtls_sha256_init(&sha_);
  mbedtls_sha256_starts(&sha_, 0);
  shaActive_ = true;
  hashedBytes_ = 0;
  verified_ = false;
  phase_ = Phase::HASHING;
  LOG_INF("FWDROP", "hashing %s (%u bytes) against %s", firmware_staging::IMAGE_PATH,
          static_cast<unsigned>(imageSize_), firmware_staging::HASH_PATH);
}

void FirmwareWatcher::finishHash() {
  uint8_t digest[32] = {};
  mbedtls_sha256_finish(&sha_, digest);
  mbedtls_sha256_free(&sha_);
  shaActive_ = false;
  image_.close();

  const std::string actual = toHex(digest);
  verdictSize_ = imageSize_;
  if (actual != expectedHash_) {
    // Not deleted. A digest that does not match is far more likely to be a copy
    // that was interrupted or a hash file someone forgot to update than an
    // attack, and throwing away the user's file is not this code's call to make.
    verified_ = false;
    phase_ = Phase::DECLINED;
    LOG_ERR("FWDROP", "%s does not match %s -- ignoring this image", firmware_staging::IMAGE_PATH,
            firmware_staging::HASH_PATH);
    return;
  }
  verified_ = true;
  phase_ = Phase::READY;
  LOG_INF("FWDROP", "staged firmware verified; offering the update");
}

void FirmwareWatcher::abandon(const char* reason) {
  if (shaActive_) {
    mbedtls_sha256_free(&sha_);
    shaActive_ = false;
  }
  if (image_) image_.close();
  LOG_ERR("FWDROP", "%s", reason);
  verified_ = false;
  verdictSize_ = imageSize_;
  phase_ = Phase::DECLINED;
}

void FirmwareWatcher::standDown() {
  if (phase_ == Phase::READY) phase_ = Phase::DECLINED;
}

FirmwareWatcher::StageState FirmwareWatcher::stageState() const {
  if (!firmware_staging::imageStaged()) return StageState::NONE;
  if (verified_) return StageState::READY;
  if (phase_ == Phase::DECLINED && verdictSize_ != 0) return StageState::INVALID;
  return StageState::CHECKING;
}

bool FirmwareWatcher::installAtSleep() const { return installAtSleep_ && firmware_staging::imageStaged(); }

std::string firmwareStageStatusText() {
  std::string version;
  firmware_staging::readVersion(version);
  const std::string prefix = version.empty() ? "" : version + " ";
  if (FIRMWARE_WATCHER.installAtSleep()) return prefix + tr(STR_FIRMWARE_AT_SLEEP);
  switch (FIRMWARE_WATCHER.stageState()) {
    case FirmwareWatcher::StageState::READY:
      return prefix + tr(STR_FIRMWARE_READY_TO_INSTALL);
    case FirmwareWatcher::StageState::CHECKING:
      return tr(STR_FIRMWARE_CHECKING);
    case FirmwareWatcher::StageState::INVALID:
      return tr(STR_FIRMWARE_INVALID);
    default:
      return tr(STR_FIRMWARE_UP_TO_DATE);
  }
}
