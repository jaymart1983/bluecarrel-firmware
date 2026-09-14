#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct BleTrustedHost {
  std::string hostId;
  std::string name;
  // HMAC-SHA256 key: the raw bytes, not their hex spelling.
  std::array<uint8_t, 32> secret{};
};

/**
 * The one phone this reader trusts, kept in NVS (Preferences namespace
 * "bleauth") so it is not readable from the SD card.
 *
 * Main loop only: nothing here is locked.
 */
class BleTrustedHostStore {
 public:
  static constexpr size_t SECRET_BYTES = 32;

  static BleTrustedHostStore& getInstance();

  // Reads the record from NVS, and removes the v1 SD-card file if it is still
  // on the card (v1 pairings do not carry over).
  void load();

  bool addOrReplaceHost(const BleTrustedHost& host);
  const BleTrustedHost* findHost(const std::string& hostId) const;
  // Null when nobody is paired.
  const BleTrustedHost* host() const { return hasHost_ ? &host_ : nullptr; }
  bool hasHosts() const { return hasHost_; }
  bool clearAll();

 private:
  BleTrustedHostStore() = default;

  BleTrustedHost host_;
  bool hasHost_ = false;
};

#define BLE_TRUSTED_HOSTS BleTrustedHostStore::getInstance()
