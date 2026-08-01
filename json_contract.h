#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ss {

inline const nlohmann::json* JsonField(const nlohmann::json& object, const std::string_view key) {
    if (!object.is_object()) throw std::runtime_error("json_contract:root_not_object");
    const auto iterator = object.find(std::string(key));
    return iterator == object.end() ? nullptr : &*iterator;
}

inline std::string JsonStringOr(const nlohmann::json& object, const std::string_view key,
                                std::string fallback = {}) {
    const nlohmann::json* value = JsonField(object, key);
    if (value == nullptr || value->is_null()) return fallback;
    if (!value->is_string()) throw std::runtime_error("json_contract:" + std::string(key) + ":string");
    return value->get<std::string>();
}

inline bool JsonBooleanOr(const nlohmann::json& object, const std::string_view key, const bool fallback = false) {
    const nlohmann::json* value = JsonField(object, key);
    if (value == nullptr || value->is_null()) return fallback;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number_integer()) {
        const auto converted = value->get<std::int64_t>();
        if (converted == 0 || converted == 1) return converted == 1;
    }
    if (value->is_number_unsigned()) {
        const auto converted = value->get<std::uint64_t>();
        if (converted == 0 || converted == 1) return converted == 1;
    }
    if (value->is_string()) {
        const std::string converted = value->get<std::string>();
        if (converted == "1" || converted == "true") return true;
        if (converted == "0" || converted == "false" || converted.empty()) return false;
    }
    throw std::runtime_error("json_contract:" + std::string(key) + ":boolean");
}

template <typename Integer>
inline Integer JsonIntegerOr(const nlohmann::json& object, const std::string_view key, const Integer fallback = {}) {
    static_assert(std::numeric_limits<Integer>::is_integer);
    const nlohmann::json* value = JsonField(object, key);
    if (value == nullptr || value->is_null()) return fallback;

    std::int64_t converted{};
    if (value->is_number_integer()) {
        converted = value->get<std::int64_t>();
    } else if (value->is_number_unsigned()) {
        const auto unsignedValue = value->get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
            throw std::runtime_error("json_contract:" + std::string(key) + ":integer_range");
        }
        return static_cast<Integer>(unsignedValue);
    } else if (value->is_string()) {
        const std::string text = value->get<std::string>();
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), converted);
        if (error != std::errc{} || end != text.data() + text.size()) {
            throw std::runtime_error("json_contract:" + std::string(key) + ":integer");
        }
    } else {
        throw std::runtime_error("json_contract:" + std::string(key) + ":integer");
    }

    if constexpr (std::numeric_limits<Integer>::is_signed) {
        if (converted < static_cast<std::int64_t>(std::numeric_limits<Integer>::min())
            || converted > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
            throw std::runtime_error("json_contract:" + std::string(key) + ":integer_range");
        }
    } else {
        if (converted < 0 || static_cast<std::uint64_t>(converted) > std::numeric_limits<Integer>::max()) {
            throw std::runtime_error("json_contract:" + std::string(key) + ":integer_range");
        }
    }
    return static_cast<Integer>(converted);
}

inline std::vector<std::string> JsonStringArrayOrEmpty(const nlohmann::json& object, const std::string_view key) {
    const nlohmann::json* value = JsonField(object, key);
    if (value == nullptr || value->is_null()) return {};
    if (!value->is_array()) throw std::runtime_error("json_contract:" + std::string(key) + ":array");
    std::vector<std::string> result;
    result.reserve(value->size());
    for (const auto& entry : *value) {
        if (!entry.is_string()) throw std::runtime_error("json_contract:" + std::string(key) + ":string_array");
        result.push_back(entry.get<std::string>());
    }
    return result;
}

}  // namespace ss
