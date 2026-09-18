#pragma once
#include <cctype>
#include <string>

namespace LivingPartyPersistence {
inline std::string ActivityToken(const std::string& value) {
    std::string result;
    for (unsigned char c : value) {
        if (result.size() >= 48) break;
        if (std::isalnum(c)) result.push_back(static_cast<char>(std::tolower(c)));
        else if (!result.empty() && result.back() != '_') result.push_back('_');
    }
    while (!result.empty() && result.back() == '_') result.pop_back();
    return result.empty() ? "none" : result;
}
inline std::string Token(const std::string& value, size_t limit) {
    auto token = ActivityToken(value);
    if (token.size() > limit) token.resize(limit);
    while (!token.empty() && token.back() == '_') token.pop_back();
    return token;
}
inline bool ValidSavedToken(const std::string& value, size_t limit) {
    if (value.empty() || value.size() > limit) return false;
    const auto canonical = Token(value, limit);
    if (value == canonical) return true;
    // The v1 writer could cut exactly at one separator. Accept its legitimate
    // existing records without accepting arbitrary malformed/oversized tokens.
    return value.size() == limit && value == canonical + '_';
}
}
