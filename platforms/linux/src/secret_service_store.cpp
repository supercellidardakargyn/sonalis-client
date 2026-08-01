#include "sonalis/linux/secret_service_store.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <glib.h>
#include <libsecret/secret.h>

namespace sonalis::linux_platform {
namespace {

const SecretSchema kSchema{
    "tr.sonalis.desktop", SECRET_SCHEMA_NONE,
    {{"key", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

bool ValidKey(const std::string& key) noexcept {
    return !key.empty() && key.size() <= 128
        && std::all_of(key.begin(), key.end(), [](const unsigned char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
        });
}

void Wipe(void* data, const std::size_t size) noexcept {
    auto* bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = 0;
}

void AssignError(std::string& output, GError* error, const char* fallback) {
    output = error != nullptr && error->message != nullptr ? error->message : fallback;
    if (error != nullptr) g_error_free(error);
}

}  // namespace

bool SecretServiceStore::Put(std::string key, const std::span<const std::uint8_t> secret,
                             std::string& error) {
    error.clear();
    if (!ValidKey(key) || secret.empty() || secret.size() > 64 * 1024) {
        error = "secure_store_input_invalid";
        return false;
    }
    gchar* encoded = g_base64_encode(secret.data(), secret.size());
    if (encoded == nullptr) { error = "secure_store_encode_failed"; return false; }
    GError* failure = nullptr;
    const gboolean stored = secret_password_store_sync(
        &kSchema, SECRET_COLLECTION_DEFAULT, "Sonalis", encoded, nullptr, &failure,
        "key", key.c_str(), nullptr);
    Wipe(encoded, std::char_traits<char>::length(encoded));
    g_free(encoded);
    if (!stored) AssignError(error, failure, "secure_store_write_failed");
    return stored != FALSE;
}

std::vector<std::uint8_t> SecretServiceStore::Get(std::string key, std::string& error) {
    error.clear();
    if (!ValidKey(key)) { error = "secure_store_key_invalid"; return {}; }
    GError* failure = nullptr;
    gchar* encoded = secret_password_lookup_sync(&kSchema, nullptr, &failure,
                                                  "key", key.c_str(), nullptr);
    if (encoded == nullptr) {
        if (failure != nullptr) AssignError(error, failure, "secure_store_read_failed");
        return {};
    }
    gsize decodedSize = 0;
    guchar* decoded = g_base64_decode(encoded, &decodedSize);
    secret_password_free(encoded);
    if (decoded == nullptr || decodedSize == 0 || decodedSize > 64 * 1024) {
        if (decoded != nullptr) { Wipe(decoded, decodedSize); g_free(decoded); }
        error = "secure_store_value_invalid";
        return {};
    }
    std::vector<std::uint8_t> result(decoded, decoded + decodedSize);
    Wipe(decoded, decodedSize);
    g_free(decoded);
    return result;
}

bool SecretServiceStore::Erase(std::string key, std::string& error) {
    error.clear();
    if (!ValidKey(key)) { error = "secure_store_key_invalid"; return false; }
    GError* failure = nullptr;
    const gboolean removed = secret_password_clear_sync(&kSchema, nullptr, &failure,
                                                         "key", key.c_str(), nullptr);
    if (!removed && failure != nullptr) AssignError(error, failure, "secure_store_delete_failed");
    return removed != FALSE;
}

}  // namespace sonalis::linux_platform
