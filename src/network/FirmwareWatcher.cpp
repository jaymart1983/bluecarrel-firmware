#include "FirmwareWatcher.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <esp_attr.h>

#include <algorithm>
#include <cstring>

#include "FirmwareStaging.h"

namespace {

using firmware_signature::Verdict;

// An idle look is cheap; there is nothing to gain from looking more often.
constexpr unsigned long POLL_INTERVAL_MS = 30UL * 1000UL;
// Stay off the SD bus while boot is loading fonts and the shelf.
constexpr unsigned long STARTUP_GRACE_MS = 20UL * 1000UL;
// One bite of hashing per main-loop tick: single-digit milliseconds. Static, not
// on the loop task's stack; there is one watcher on one task.
constexpr size_t HASH_CHUNK_BYTES = 4UL * 1024UL;
uint8_t gHashBuffer[HASH_CHUNK_BYTES];
// Nothing smaller is a plausible ESP32 application image.
constexpr size_t MIN_IMAGE_BYTES = 64UL * 1024UL;
// Enough to tell companion files apart; anything longer fails its own parse.
constexpr size_t COMPANION_READ_BYTES = 400;

// The last digest computed, kept in RTC slow memory: it survives deep sleep (a
// wake is a reset, which would otherwise lose it) and is zeroed at power-on. The
// key is what can be read without reading the image. It only spares the wake
// re-hash: the verdict is still recomputed from it (hash file, version,
// signature), and SdFirmwareUpdateActivity::verifyStagedDrop() hashes the image
// again and compares before anything is written to flash, so a same-size image
// swapped in under a stale key is refused at install. 120 bytes of RTC memory.
struct DigestCache {
  uint32_t magic;
  uint32_t size;
  uint32_t mtime;
  char expected[65];  // the first 64 bytes of the companion .sha256 file
  char digest[65];    // hex SHA-256 of the image
};
constexpr uint32_t DIGEST_CACHE_MAGIC = 0x46574443;  // "FWDC"
RTC_DATA_ATTR DigestCache gDigestCache;

void keyOf(const std::string& expectedRaw, char (&out)[65]) {
  std::fill(out, out + sizeof(out), '\0');
  const size_t n = std::min(expectedRaw.size(), sizeof(out) - 1);
  std::copy(expectedRaw.begin(), expectedRaw.begin() + static_cast<std::ptrdiff_t>(n), out);
}

bool cachedDigest(const size_t size, const uint32_t mtime, const std::string& expectedRaw, std::string& digest) {
  if (gDigestCache.magic != DIGEST_CACHE_MAGIC || gDigestCache.size != size || gDigestCache.mtime != mtime) {
    return false;
  }
  char key[65];
  keyOf(expectedRaw, key);
  if (std::memcmp(key, gDigestCache.expected, sizeof(key)) != 0) return false;
  gDigestCache.digest[64] = '\0';
  digest.assign(gDigestCache.digest);
  return digest.size() == 64;
}

void rememberDigest(const size_t size, const uint32_t mtime, const std::string& expectedRaw,
                    const std::string& digest) {
  if (digest.size() != 64) return;
  gDigestCache.magic = 0;
  gDigestCache.size = static_cast<uint32_t>(size);
  gDigestCache.mtime = mtime;
  keyOf(expectedRaw, gDigestCache.expected);
  std::memcpy(gDigestCache.digest, digest.c_str(), 65);
  gDigestCache.magic = DIGEST_CACHE_MAGIC;
}

// Main-loop task only, so the buffer is static rather than on the stack.
std::string readRaw(const char* path) {
  static char buffer[COMPANION_READ_BYTES + 1];
  std::fill(buffer, buffer + sizeof(buffer), '\0');
  const size_t read = Storage.readFileToBuffer(path, buffer, COMPANION_READ_BYTES);
  return read == 0 ? std::string() : std::string(buffer);
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
      reject(Verdict::BAD_IMAGE, "could not read the staged image");
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
    if (phase_ != Phase::IDLE || verified_ || installAtSleep_) resetStage();
    return;
  }

  Fingerprint current;
  if (!readFingerprint(current)) return;
  if (phase_ != Phase::IDLE && current == print_) return;

  if (phase_ != Phase::IDLE) LOG_INF("FWDROP", "staged firmware changed; checking it again");
  resetStage();
  print_ = std::move(current);
  imageSize_ = print_.size;
  if (imageSize_ < MIN_IMAGE_BYTES) {
    LOG_ERR("FWDROP", "%s is %u bytes, too small to be a firmware image", firmware_staging::IMAGE_PATH,
            static_cast<unsigned>(imageSize_));
    reject(Verdict::BAD_IMAGE, "image too small");
    return;
  }
  std::string remembered;
  if (cachedDigest(print_.size, print_.mtime, print_.hash, remembered)) {
    LOG_INF("FWDROP", "staged image unchanged since it was last hashed; not re-reading it");
    concludeWithDigest(remembered);
    return;
  }
  beginHash();
}

bool FirmwareWatcher::readFingerprint(Fingerprint& out) const {
  HalFile probe;
  if (!Storage.openFileForRead("FWDROP", firmware_staging::IMAGE_PATH, probe)) return false;
  out.size = probe.fileSize();
  if (!probe.modifiedEpoch(out.mtime)) out.mtime = 0;
  probe.close();
  out.generation = firmware_staging::stageGeneration();
  out.hash = readRaw(firmware_staging::HASH_PATH);
  out.version = readRaw(firmware_staging::VERSION_PATH);
  out.signature = readRaw(firmware_staging::SIG_PATH);
  return true;
}

void FirmwareWatcher::resetStage() {
  if (shaActive_) {
    mbedtls_sha256_free(&sha_);
    shaActive_ = false;
  }
  if (image_) image_.close();
  clearDeferral();
  approvedHash_.clear();
  verified_ = false;
  verifiedHash_.clear();
  verdict_ = Verdict::OK;
  phase_ = Phase::IDLE;
}

void FirmwareWatcher::beginHash() {
  std::string expected;
  if (!firmware_staging::readExpectedHash(expected)) {
    reject(Verdict::BAD_IMAGE, "no usable hash file beside the image");
    return;
  }
  if (!Storage.openFileForRead("FWDROP", firmware_staging::IMAGE_PATH, image_)) {
    reject(Verdict::BAD_IMAGE, "could not open the staged image");
    return;
  }
  mbedtls_sha256_init(&sha_);
  mbedtls_sha256_starts(&sha_, 0);
  shaActive_ = true;
  hashedBytes_ = 0;
  phase_ = Phase::HASHING;
  LOG_INF("FWDROP", "hashing %s (%u bytes)", firmware_staging::IMAGE_PATH, static_cast<unsigned>(imageSize_));
}

void FirmwareWatcher::finishHash() {
  uint8_t digest[32] = {};
  mbedtls_sha256_finish(&sha_, digest);
  mbedtls_sha256_free(&sha_);
  shaActive_ = false;
  image_.close();

  const std::string actual = firmware_signature::toHex(digest, sizeof(digest));
  rememberDigest(print_.size, print_.mtime, print_.hash, actual);
  concludeWithDigest(actual);
}

void FirmwareWatcher::concludeWithDigest(const std::string& actual) {
  const Verdict verdict = firmware_staging::checkStaged(actual);
  if (verdict != Verdict::OK) {
    // The file is left alone; deleting the user's image is not this code's call.
    reject(verdict, firmware_signature::verdictName(verdict));
    return;
  }
  verified_ = true;
  verifiedHash_ = actual;
  verdict_ = Verdict::OK;
  phase_ = Phase::READY;
  LOG_INF("FWDROP", "staged firmware is signed, newer and intact; offering the update");
}

void FirmwareWatcher::reject(const Verdict verdict, const char* reason) {
  if (shaActive_) {
    mbedtls_sha256_free(&sha_);
    shaActive_ = false;
  }
  if (image_) image_.close();
  LOG_ERR("FWDROP", "not offering %s: %s", firmware_staging::IMAGE_PATH, reason);
  clearDeferral();
  verified_ = false;
  verifiedHash_.clear();
  verdict_ = verdict;
  phase_ = Phase::DECLINED;
}

void FirmwareWatcher::standDown() {
  if (phase_ == Phase::READY) phase_ = Phase::DECLINED;
}

bool FirmwareWatcher::deferToSleep() {
  if (!verified_ || verifiedHash_.empty()) {
    LOG_ERR("FWDROP", "no verified image to install at sleep");
    return false;
  }
  approvedHash_ = verifiedHash_;
  approvedGeneration_ = firmware_staging::stageGeneration();
  installAtSleep_ = true;
  return true;
}

void FirmwareWatcher::clearDeferral() {
  installAtSleep_ = false;
  approvedGeneration_ = 0;
}

bool FirmwareWatcher::installAtSleep() const {
  // Scalars only: BLE status reads this too. The installer re-hashes the image
  // and compares it with verifiedHash() before it writes flash.
  return installAtSleep_ && verified_ && approvedGeneration_ == firmware_staging::stageGeneration() &&
         firmware_staging::imageStaged();
}

FirmwareWatcher::StageState FirmwareWatcher::stageState() const {
  if (!firmware_staging::imageStaged()) return StageState::NONE;
  if (verified_) return StageState::READY;
  if (phase_ == Phase::DECLINED) return StageState::INVALID;
  return StageState::CHECKING;
}

const char* firmwareVerdictText(const Verdict verdict) {
  switch (verdict) {
    case Verdict::HASH_MISMATCH:
      return tr(STR_FIRMWARE_HASH_MISMATCH);
    case Verdict::UNSIGNED:
      return tr(STR_FIRMWARE_UNSIGNED);
    case Verdict::NO_VERSION:
      return tr(STR_FIRMWARE_NO_VERSION);
    case Verdict::BAD_SIGNATURE:
      return tr(STR_FIRMWARE_BAD_SIGNATURE);
    case Verdict::NOT_NEWER:
      return tr(STR_FIRMWARE_NOT_NEWER);
    case Verdict::OK:
    case Verdict::BAD_IMAGE:
      break;
  }
  return tr(STR_FIRMWARE_INVALID);
}

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
      return firmwareVerdictText(FIRMWARE_WATCHER.verdict());
    default:
      return tr(STR_FIRMWARE_UP_TO_DATE);
  }
}
