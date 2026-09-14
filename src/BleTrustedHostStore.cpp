#include "BleTrustedHostStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>

#include <utility>

namespace {

constexpr const char* NVS_NAMESPACE = "bleauth";
constexpr const char* KEY_HOST_ID = "host_id";
constexpr const char* KEY_NAME = "name";
constexpr const char* KEY_SECRET = "secret";
constexpr const char* LEGACY_FILE_PATH = "/.crosspoint/ble_trusted_hosts.json";
// Longest host_id BleLink accepts, plus the terminator.
constexpr size_t MAX_STRING_BYTES = 65;

bool readString(Preferences& prefs, const char* key, std::string& out) {
  out.clear();
  if (!prefs.isKey(key)) return false;
  char buffer[MAX_STRING_BYTES] = {};
  if (prefs.getString(key, buffer, sizeof(buffer)) == 0) return false;
  out.assign(buffer);
  return true;
}

}  // namespace

BleTrustedHostStore& BleTrustedHostStore::getInstance() {
  static BleTrustedHostStore instance;
  return instance;
}

void BleTrustedHostStore::load() {
  if (Storage.exists(LEGACY_FILE_PATH)) {
    if (Storage.remove(LEGACY_FILE_PATH)) {
      LOG_INF("BTH", "Removed the v1 trusted host file");
    } else {
      LOG_ERR("BTH", "Could not remove the v1 trusted host file");
    }
  }

  host_ = BleTrustedHost{};
  hasHost_ = false;

  Preferences prefs;
  // Fails when the namespace does not exist yet, which is the unpaired state.
  if (!prefs.begin(NVS_NAMESPACE, true)) return;

  BleTrustedHost loaded;
  const bool haveId = readString(prefs, KEY_HOST_ID, loaded.hostId) && !loaded.hostId.empty();
  readString(prefs, KEY_NAME, loaded.name);
  const bool haveSecret = prefs.isKey(KEY_SECRET) && prefs.getBytesLength(KEY_SECRET) == SECRET_BYTES &&
                          prefs.getBytes(KEY_SECRET, loaded.secret.data(), loaded.secret.size()) == SECRET_BYTES;
  prefs.end();

  if (!haveId || !haveSecret) return;
  host_ = std::move(loaded);
  hasHost_ = true;
  LOG_DBG("BTH", "Loaded BLE trusted host");
}

bool BleTrustedHostStore::addOrReplaceHost(const BleTrustedHost& host) {
  if (host.hostId.empty() || host.hostId.size() >= MAX_STRING_BYTES || host.name.size() >= MAX_STRING_BYTES) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("BTH", "Could not open NVS namespace");
    return false;
  }
  prefs.clear();
  const bool ok = prefs.putString(KEY_HOST_ID, host.hostId.c_str()) > 0 &&
                  prefs.putString(KEY_NAME, host.name.c_str()) == host.name.size() &&
                  prefs.putBytes(KEY_SECRET, host.secret.data(), host.secret.size()) == SECRET_BYTES;
  if (!ok) {
    // A half-written record must not authenticate anyone on the next boot.
    prefs.clear();
    prefs.end();
    host_ = BleTrustedHost{};
    hasHost_ = false;
    LOG_ERR("BTH", "Could not save BLE trusted host");
    return false;
  }
  prefs.end();

  host_ = host;
  hasHost_ = true;
  LOG_DBG("BTH", "Saved BLE trusted host");
  return true;
}

const BleTrustedHost* BleTrustedHostStore::findHost(const std::string& hostId) const {
  return hasHost_ && host_.hostId == hostId ? &host_ : nullptr;
}

bool BleTrustedHostStore::clearAll() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("BTH", "Could not open NVS namespace");
    return false;
  }
  const bool ok = prefs.clear();
  prefs.end();
  if (!ok) {
    LOG_ERR("BTH", "Could not clear BLE trusted host");
    return false;
  }
  host_ = BleTrustedHost{};
  hasHost_ = false;
  LOG_DBG("BTH", "Cleared BLE trusted hosts");
  return true;
}
