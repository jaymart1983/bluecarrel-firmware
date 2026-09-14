#include "FirmwareSignature.h"

#include <Logging.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#include <cstring>

#include "BuildStamp.h"
#include "FirmwareSigningKey.h"

namespace firmware_signature {
namespace {

constexpr size_t SHA256_HEX_LEN = 64;
constexpr size_t VERSION_LEN = 13;  // yyyyMMdd.HHmm
constexpr const char* MESSAGE_PREFIX = "X4FW1|";

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool isLowerHex(const std::string& text) {
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// Frees the key context on every return path.
struct PkContext {
  mbedtls_pk_context ctx;
  PkContext() { mbedtls_pk_init(&ctx); }
  ~PkContext() { mbedtls_pk_free(&ctx); }
  PkContext(const PkContext&) = delete;
  PkContext& operator=(const PkContext&) = delete;
};

}  // namespace

const char* verdictName(const Verdict verdict) {
  switch (verdict) {
    case Verdict::OK:
      return "OK";
    case Verdict::BAD_IMAGE:
      return "BAD_IMAGE";
    case Verdict::HASH_MISMATCH:
      return "HASH_MISMATCH";
    case Verdict::UNSIGNED:
      return "UNSIGNED";
    case Verdict::NO_VERSION:
      return "NO_VERSION";
    case Verdict::BAD_SIGNATURE:
      return "BAD_SIGNATURE";
    case Verdict::NOT_NEWER:
      return "NOT_NEWER";
  }
  return "?";
}

bool isWellFormedVersion(const std::string& version) {
  if (version.length() != VERSION_LEN) return false;
  for (size_t i = 0; i < VERSION_LEN; i++) {
    const char c = version[i];
    if (i == 8) {
      if (c != '.') return false;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

bool isNewerThanRunning(const std::string& version) {
  // Fixed-width digits, so string order is build order.
  return isWellFormedVersion(version) && std::strcmp(version.c_str(), X4_BUILD_STAMP) > 0;
}

bool isHex(const std::string& text, const size_t maxChars) {
  if (text.empty() || text.length() > maxChars || (text.length() % 2) != 0) return false;
  for (const char c : text) {
    if (hexValue(c) < 0) return false;
  }
  return true;
}

std::string toHex(const uint8_t* data, const size_t len) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 0x0F];
  }
  return out;
}

bool verify(const std::string& version, const std::string& sha256Hex, const std::string& signatureHex) {
  if (!isWellFormedVersion(version)) {
    LOG_ERR("FWSIG", "malformed version");
    return false;
  }
  if (sha256Hex.length() != SHA256_HEX_LEN || !isLowerHex(sha256Hex)) {
    LOG_ERR("FWSIG", "malformed image digest");
    return false;
  }
  if (!isHex(signatureHex, MAX_SIGNATURE_HEX)) {
    LOG_ERR("FWSIG", "malformed signature hex (%u chars)", static_cast<unsigned>(signatureHex.length()));
    return false;
  }

  uint8_t signature[MAX_SIGNATURE_HEX / 2];
  const size_t signatureLen = signatureHex.length() / 2;
  for (size_t i = 0; i < signatureLen; i++) {
    signature[i] = static_cast<uint8_t>((hexValue(signatureHex[i * 2]) << 4) | hexValue(signatureHex[i * 2 + 1]));
  }

  const std::string message = std::string(MESSAGE_PREFIX) + version + "|" + sha256Hex;
  uint8_t digest[32];
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(message.data()), message.length(), digest, 0) != 0) {
    LOG_ERR("FWSIG", "sha256 failed");
    return false;
  }

#if defined(MBEDTLS_PSA_CRYPTO_C)
  // With MBEDTLS_USE_PSA_CRYPTO the pk layer runs on PSA; init is idempotent.
  const psa_status_t psaStatus = psa_crypto_init();
  if (psaStatus != PSA_SUCCESS) {
    LOG_ERR("FWSIG", "psa_crypto_init failed: %d", static_cast<int>(psaStatus));
    return false;
  }
#endif

  PkContext pk;
  int ret = mbedtls_pk_parse_public_key(&pk.ctx, FIRMWARE_SIGNING_PUBKEY_DER, FIRMWARE_SIGNING_PUBKEY_DER_LEN);
  if (ret != 0) {
    LOG_ERR("FWSIG", "public key parse failed: -0x%04X", static_cast<unsigned>(-ret));
    return false;
  }
  if (!mbedtls_pk_can_do(&pk.ctx, MBEDTLS_PK_ECDSA) || mbedtls_pk_get_bitlen(&pk.ctx) != 256) {
    LOG_ERR("FWSIG", "firmware key is not ECDSA P-256");
    return false;
  }
  ret = mbedtls_pk_verify(&pk.ctx, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature, signatureLen);
  if (ret != 0) {
    LOG_ERR("FWSIG", "signature does not verify: -0x%04X", static_cast<unsigned>(-ret));
    return false;
  }
  return true;
}

Verdict check(const std::string& actualSha256Hex, const std::string& expectedSha256Hex, const std::string& version,
              const std::string& signatureHex) {
  if (expectedSha256Hex.empty()) return Verdict::BAD_IMAGE;
  if (actualSha256Hex != expectedSha256Hex) return Verdict::HASH_MISMATCH;
  if (signatureHex.empty()) return Verdict::UNSIGNED;
  if (!isWellFormedVersion(version)) return Verdict::NO_VERSION;
  if (!verify(version, actualSha256Hex, signatureHex)) return Verdict::BAD_SIGNATURE;
  if (!isNewerThanRunning(version)) return Verdict::NOT_NEWER;
  return Verdict::OK;
}

}  // namespace firmware_signature
