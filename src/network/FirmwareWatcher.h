#pragma once

#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "FirmwareSignature.h"

// Watches the firmware drop folder (see FirmwareStaging.h) and offers an update
// when what is there is a signed, newer image whose bytes match its hash.
//
// A drop is just a file: the app writes it over BLE, or a person copies it with
// the card mounted over USB, and both routes end here.
//
// Hashing an image off SD takes seconds, so it runs in HASH_CHUNK_BYTES bites
// across main-loop ticks, and only when the staged files change. With nothing
// staged a poll is two Storage::exists() calls; with something staged it also
// reads the image size and the three small companion files.
class FirmwareWatcher {
 public:
  static FirmwareWatcher& getInstance();

  // Called once per main loop. Never called while USB Drive owns the card.
  void tick();

  // A verified image is waiting for the prompt.
  bool updateReady() const { return phase_ == Phase::READY; }

  // Stop offering the current image (declined, or handed to the prompt). The
  // offer comes back if the staged files change, or at the next boot.
  void standDown();

  // "Later" or auto-install: install the verified image at the next sleep. Binds
  // the deferral to that image's digest; returns false when nothing is verified.
  // RAM only, so a restart forgets it.
  bool deferToSleep();
  // Deferred, and the stage is still the verified image that was approved.
  bool installAtSleep() const;
  void clearDeferral();

  // What the card holds, for the Settings row.
  enum class StageState : uint8_t { NONE, CHECKING, READY, INVALID };
  StageState stageState() const;
  // Why the stage is INVALID.
  firmware_signature::Verdict verdict() const { return verdict_; }
  // SHA-256 (lowercase hex) of the verified staged image; empty when none. The
  // installer requires the image it flashes to hash to this.
  const std::string& verifiedHash() const { return verifiedHash_; }

 private:
  enum class Phase : uint8_t {
    IDLE,      // nothing staged, or not looked at yet
    HASHING,   // reading the image a chunk per tick
    READY,     // verified; waiting for the prompt
    DECLINED,  // verified but declined, or failed a check
  };

  // What the current verdict was reached on; any difference means a new stage.
  struct Fingerprint {
    size_t size = 0;
    uint32_t generation = 0;
    std::string hash;
    std::string version;
    std::string signature;
    bool operator==(const Fingerprint& other) const {
      return size == other.size && generation == other.generation && hash == other.hash &&
             version == other.version && signature == other.signature;
    }
  };

  FirmwareWatcher() = default;

  bool readFingerprint(Fingerprint& out) const;
  void resetStage();
  void beginHash();
  void finishHash();
  void reject(firmware_signature::Verdict verdict, const char* reason);

  Phase phase_ = Phase::IDLE;
  unsigned long lastPollMs_ = 0;
  Fingerprint print_;

  // Hashing state, live only in Phase::HASHING.
  HalFile image_;
  size_t hashedBytes_ = 0;
  size_t imageSize_ = 0;
  mbedtls_sha256_context sha_{};
  bool shaActive_ = false;

  // Verdict on the current stage. Survives standDown().
  bool verified_ = false;
  std::string verifiedHash_;
  firmware_signature::Verdict verdict_ = firmware_signature::Verdict::OK;

  // Deferral. Cleared whenever the verdict is reset, so while installAtSleep_ is
  // set approvedHash_ equals verifiedHash_.
  bool installAtSleep_ = false;
  std::string approvedHash_;
  uint32_t approvedGeneration_ = 0;
};

#define FIRMWARE_WATCHER FirmwareWatcher::getInstance()

// Short UI text for a failed check ("Bad signature", "Not newer", ...).
const char* firmwareVerdictText(firmware_signature::Verdict verdict);

// One-line firmware status for the settings screens: the staged build stamp (if
// any) with "installs at sleep" / "ready to install", or checking, the reason an
// image is refused, or up to date.
std::string firmwareStageStatusText();
