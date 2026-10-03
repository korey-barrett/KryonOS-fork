#include "CryptoEngine.h"
#include "FileSystem/FileSystem.h"
// mbedTLS 4.x (what ESP-IDF v6.1 ships) moved the per-algorithm headers -- md5.h, sha256.h,
// sha512.h, aes.h, rsa.h -- under the driver's private/ directory and left only the generic
// layers public. So the algorithms are reached through PSA Crypto now, and the three headers
// that remain here are the three this file genuinely still needs directly: the generic
// message-digest API (for HMAC), the public-key API (for RSA verification) and base64.
#include <psa/crypto.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <esp_random.h>
#include <Preferences.h>
#include <ArduinoJson.h>

namespace {
// PSA requires psa_crypto_init() before any operation. It is idempotent and near-free after the
// first call, so every entry point below routes through here rather than assuming some other
// subsystem (TLS, WiFi) has already initialised it -- on a cold boot the first file hash can
// easily be the first PSA user in the system.
bool psaReady() {
    static const bool ready = (psa_crypto_init() == PSA_SUCCESS);
    return ready;
}
} // namespace

bool CryptoEngine::hexToBytes(const String& hex, std::vector<uint8_t>& out) {
    out.clear();
    if (hex.length() % 2 != 0) return false;

    out.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        char h1 = hex.charAt(i);
        char h2 = hex.charAt(i + 1);

        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        int v1 = hexVal(h1);
        int v2 = hexVal(h2);
        if (v1 < 0 || v2 < 0) {
            out.clear();
            return false;
        }
        out.push_back((uint8_t)((v1 << 4) | v2));
    }
    return true;
}

String CryptoEngine::bytesToHex(const uint8_t* data, size_t length) {
    if (!data || length == 0) return "";
    static const char hexChars[] = "0123456789abcdef";
    String hexStr = "";
    hexStr.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        hexStr += hexChars[(data[i] >> 4) & 0x0F];
        hexStr += hexChars[data[i] & 0x0F];
    }
    return hexStr;
}

String CryptoEngine::sha256(const String& data) {
    if (!psaReady()) return "";
    uint8_t hash[32];
    size_t hashLen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)data.c_str(), data.length(),
                         hash, sizeof(hash), &hashLen) != PSA_SUCCESS) {
        return "";
    }
    return bytesToHex(hash, hashLen);
}

String CryptoEngine::sha512(const String& data) {
    if (!psaReady()) return "";
    uint8_t hash[64];
    size_t hashLen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_512, (const uint8_t*)data.c_str(), data.length(),
                         hash, sizeof(hash), &hashLen) != PSA_SUCCESS) {
        return "";
    }
    return bytesToHex(hash, hashLen);
}

String CryptoEngine::hmacSha256(const String& key, const String& message) {
    // PSA rather than the legacy mbedtls_md_hmac_* family: in mbedTLS 4.x those three declarations
    // sit behind MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS, so they are no longer public API even though
    // md.h itself still is. HMAC through PSA is the supported path, and it takes the key through
    // the ordinary import route.
    if (!psaReady()) return "";

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, key.length() * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attr, (const uint8_t*)key.c_str(), key.length(), &keyId) != PSA_SUCCESS) {
        return "";
    }

    uint8_t hmacOut[32];
    size_t hmacLen = 0;
    psa_status_t st = psa_mac_compute(keyId, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                      (const uint8_t*)message.c_str(), message.length(),
                                      hmacOut, sizeof(hmacOut), &hmacLen);
    psa_destroy_key(keyId);
    if (st != PSA_SUCCESS) return "";

    return bytesToHex(hmacOut, hmacLen);
}

String CryptoEngine::aesEncrypt(const String& plainText, const String& keyHex, const String& ivHex) {
    // 1. Strict Input Key & IV Length Validation
    if (keyHex.length() != 32 && keyHex.length() != 64) {
        Serial.println("[Crypto] Error: AES keyHex must be 32 (128-bit) or 64 (256-bit) hex characters.");
        return "";
    }
    if (ivHex.length() != 32) {
        Serial.println("[Crypto] Error: AES ivHex must be 32 hex characters (16 bytes).");
        return "";
    }

    std::vector<uint8_t> keyBytes, ivBytes;
    if (!hexToBytes(keyHex, keyBytes) || !hexToBytes(ivHex, ivBytes) || ivBytes.size() != 16) {
        return "";
    }

    // 2. PKCS#7 Padding Alignment
    size_t plainLen = plainText.length();
    size_t padLen = 16 - (plainLen % 16);
    size_t totalLen = plainLen + padLen;

    std::vector<uint8_t> padded(totalLen);
    memcpy(padded.data(), plainText.c_str(), plainLen);
    memset(padded.data() + plainLen, (uint8_t)padLen, padLen);

    // 3. Hardware AES-CBC Encryption
    //
    // The multipart PSA interface is used rather than the one-shot psa_cipher_encrypt(), because
    // that one-shot generates its OWN random IV and prepends it to the output. This file's
    // contract is the opposite: the caller supplies the IV and keeps it beside the ciphertext (see
    // deviceEncrypt(), which stores it in the JSON envelope), so the IV must be set explicitly.
    // PSA copies the IV, which also removes the in-place mutation hazard the mbedTLS CBC call had.
    if (!psaReady()) return "";
    if (totalLen == 0 || totalLen % 16 != 0) return "";

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, keyBytes.size() * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);

    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attr, keyBytes.data(), keyBytes.size(), &keyId) != PSA_SUCCESS) {
        return "";
    }

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    std::vector<uint8_t> cipher(totalLen);
    size_t outLen = 0;

    psa_status_t st = psa_cipher_encrypt_setup(&op, keyId, PSA_ALG_CBC_NO_PADDING);
    if (st == PSA_SUCCESS) st = psa_cipher_set_iv(&op, ivBytes.data(), ivBytes.size());
    if (st == PSA_SUCCESS) {
        st = psa_cipher_update(&op, padded.data(), padded.size(), cipher.data(), cipher.size(), &outLen);
    }

    // CBC with no padding has nothing left over, so the finish call should write zero bytes -- but
    // it must still be made, because it is what commits the operation.
    size_t tailLen = 0;
    if (st == PSA_SUCCESS) {
        st = psa_cipher_finish(&op, cipher.data() + outLen, cipher.size() - outLen, &tailLen);
    }
    if (st != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        psa_destroy_key(keyId);
        return "";
    }
    psa_destroy_key(keyId);
    cipher.resize(outLen + tailLen);

    // 4. Base64 Encode Ciphertext
    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, cipher.data(), cipher.size());
    if (b64Len == 0) return "";

    std::vector<unsigned char> b64Out(b64Len + 1, 0);
    size_t actualLen = 0;
    if (mbedtls_base64_encode(b64Out.data(), b64Out.size(), &actualLen, cipher.data(), cipher.size()) != 0) {
        return "";
    }

    return String((const char*)b64Out.data());
}

String CryptoEngine::aesDecrypt(const String& base64Cipher, const String& keyHex, const String& ivHex) {
    // 1. Strict Input Key & IV Length Validation
    if (keyHex.length() != 32 && keyHex.length() != 64) {
        Serial.println("[Crypto] Error: AES keyHex must be 32 (128-bit) or 64 (256-bit) hex characters.");
        return "";
    }
    if (ivHex.length() != 32) {
        Serial.println("[Crypto] Error: AES ivHex must be 32 hex characters (16 bytes).");
        return "";
    }

    std::vector<uint8_t> keyBytes, ivBytes;
    if (!hexToBytes(keyHex, keyBytes) || !hexToBytes(ivHex, ivBytes) || ivBytes.size() != 16) {
        return "";
    }

    // 2. Base64 Decode Ciphertext
    size_t b64Len = 0;
    mbedtls_base64_decode(nullptr, 0, &b64Len, (const unsigned char*)base64Cipher.c_str(), base64Cipher.length());
    if (b64Len == 0) return "";

    std::vector<uint8_t> cipherBytes(b64Len);
    size_t actualLen = 0;
    if (mbedtls_base64_decode(cipherBytes.data(), cipherBytes.size(), &actualLen, (const unsigned char*)base64Cipher.c_str(), base64Cipher.length()) != 0) {
        return "";
    }
    cipherBytes.resize(actualLen);

    // 3. Ciphertext Block Alignment Check (Must be multiple of 16)
    if (cipherBytes.empty() || (cipherBytes.size() % 16 != 0)) {
        Serial.println("[Crypto] Error: Decoded ciphertext length is not a multiple of 16 bytes.");
        return "";
    }

    // 4. Hardware AES-CBC Decryption -- same multipart shape as the encrypt side, and for the same
    // reason: the IV comes from the caller (it was read back out of the JSON envelope) rather than
    // from PSA's random-IV one-shot.
    if (!psaReady()) return "";

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, keyBytes.size() * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);

    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attr, keyBytes.data(), keyBytes.size(), &keyId) != PSA_SUCCESS) {
        return "";
    }

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    std::vector<uint8_t> plain(cipherBytes.size());
    size_t outLen = 0;

    psa_status_t st = psa_cipher_decrypt_setup(&op, keyId, PSA_ALG_CBC_NO_PADDING);
    if (st == PSA_SUCCESS) st = psa_cipher_set_iv(&op, ivBytes.data(), ivBytes.size());
    if (st == PSA_SUCCESS) {
        st = psa_cipher_update(&op, cipherBytes.data(), cipherBytes.size(), plain.data(), plain.size(), &outLen);
    }

    size_t tailLen = 0;
    if (st == PSA_SUCCESS) {
        st = psa_cipher_finish(&op, plain.data() + outLen, plain.size() - outLen, &tailLen);
    }
    if (st != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        psa_destroy_key(keyId);
        return "";
    }
    psa_destroy_key(keyId);
    plain.resize(outLen + tailLen);

    // 5. Strict PKCS#7 Unpadding Validation & Underflow Guard
    size_t plainLen = plain.size();
    if (plainLen == 0) return "";

    uint8_t pad = plain[plainLen - 1];
    if (pad == 0 || pad > 16 || pad > plainLen) {
        return ""; // Invalid PKCS#7 padding
    }

    for (size_t i = 0; i < pad; i++) {
        if (plain[plainLen - 1 - i] != pad) {
            return ""; // Padding mismatch
        }
    }
    plainLen -= pad;

    return String((const char*)plain.data(), plainLen);
}

String CryptoEngine::randomBytes(size_t length) {
    if (length == 0 || length > 4096) return "";
    std::vector<uint8_t> buf(length);
    esp_fill_random(buf.data(), length);
    return bytesToHex(buf.data(), length);
}

bool CryptoEngine::rsaVerify(const String& pubKeyPem, const String& message, const String& sigBase64) {
    if (pubKeyPem.length() == 0 || message.length() == 0 || sigBase64.length() == 0) {
        return false;
    }

    // 1. Decode Base64 Signature
    size_t sigLen = 0;
    mbedtls_base64_decode(nullptr, 0, &sigLen, (const unsigned char*)sigBase64.c_str(), sigBase64.length());
    if (sigLen == 0) return false;

    std::vector<uint8_t> sigBytes(sigLen);
    size_t actualSigLen = 0;
    if (mbedtls_base64_decode(sigBytes.data(), sigBytes.size(), &actualSigLen, (const unsigned char*)sigBase64.c_str(), sigBase64.length()) != 0) {
        return false;
    }
    sigBytes.resize(actualSigLen);

    // 2. Parse PEM Public Key (Length MUST include null-terminator + 1)
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)pubKeyPem.c_str(), pubKeyPem.length() + 1);
    if (ret != 0) {
        Serial.printf("[Crypto] Error: mbedtls_pk_parse_public_key failed (ret = -0x%04X)\n", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    // 3. Compute SHA-256 Hash of Message (PSA; mbedtls_sha256() is private in mbedTLS 4.x)
    uint8_t hash[32];
    size_t hashLen = 0;
    if (!psaReady() ||
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)message.c_str(), message.length(),
                         hash, sizeof(hash), &hashLen) != PSA_SUCCESS) {
        mbedtls_pk_free(&pk);
        return false;
    }

    // 4. Hardware RSA Signature Verification (PKCS#1 v1.5 with SHA-256)
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sigBytes.data(), sigBytes.size());
    mbedtls_pk_free(&pk);

    return (ret == 0);
}

bool CryptoEngine::getDeviceKey(uint8_t keyOut[32]) {
    Preferences prefs;
    if (!prefs.begin("kryon_sec", false)) {
        Serial.println("[Crypto] Error opening NVS kryon_sec namespace");
        return false;
    }
    if (!prefs.isKey("enc_key")) {
        uint8_t rawKey[32];
        esp_fill_random(rawKey, sizeof(rawKey));
        prefs.putBytes("enc_key", rawKey, sizeof(rawKey));
        Serial.println("[Crypto] Generated new device encryption key via hardware TRNG");
    }
    size_t len = prefs.getBytes("enc_key", keyOut, 32);
    prefs.end();
    return (len == 32);
}

String CryptoEngine::deviceEncrypt(const String& plaintext) {
    if (plaintext.length() == 0) return "";
    uint8_t key[32];
    if (!getDeviceKey(key)) return "";

    String keyHex = bytesToHex(key, 32);
    String ivHex = randomBytes(16); // 16 bytes = 32 hex chars

    String cipherBase64 = aesEncrypt(plaintext, keyHex, ivHex);
    if (cipherBase64.length() == 0) return "";

    JsonDocument doc;
    doc["v"] = 1;
    doc["iv"] = ivHex;
    doc["data"] = cipherBase64;

    String output;
    serializeJson(doc, output);
    return output;
}

String CryptoEngine::deviceDecrypt(const String& ciphertext) {
    if (ciphertext.length() == 0) return "";
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, ciphertext);
    if (err) {
        Serial.printf("[Crypto] JSON parse error in deviceDecrypt: %s\n", err.c_str());
        return "";
    }

    const char* ivHex = doc["iv"];
    const char* dataBase64 = doc["data"];
    if (!ivHex || !dataBase64) return "";

    uint8_t key[32];
    if (!getDeviceKey(key)) return "";

    String keyHex = bytesToHex(key, 32);
    return aesDecrypt(String(dataBase64), keyHex, String(ivHex));
}

String CryptoEngine::sha256File(const char* path) {
    File f = FileSystem::openFile(path, "r");
    if (!f || f.isDirectory()) {
        return "";
    }

    // Streaming PSA hash. The point of this function is that a file of any size is read in 512-byte
    // chunks and never held in memory, which is what psa_hash_setup/update/finish preserves -- the
    // one-shot psa_hash_compute() would require the whole file as one buffer and defeat it.
    if (!psaReady()) {
        f.close();
        return "";
    }

    psa_hash_operation_t shaCtx = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&shaCtx, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        f.close();
        return "";
    }

    uint8_t buffer[512];
    while (f.available()) {
        size_t bytesRead = f.read(buffer, sizeof(buffer));
        if (bytesRead > 0 && psa_hash_update(&shaCtx, buffer, bytesRead) != PSA_SUCCESS) {
            psa_hash_abort(&shaCtx);
            f.close();
            return "";
        }
    }
    f.close();

    uint8_t hash[32];
    size_t hashLen = 0;
    if (psa_hash_finish(&shaCtx, hash, sizeof(hash), &hashLen) != PSA_SUCCESS) {
        psa_hash_abort(&shaCtx);
        return "";
    }

    return bytesToHex(hash, hashLen);
}

