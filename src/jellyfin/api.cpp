#include "api.h"

#include "../cjson/cJSON.h"

#include <cctype>
#include <cstring>

namespace jellyfin_api {

namespace {

bool ieq_char(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

std::size_t find_last_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty() || needle.size() > hay.size())
        return std::string_view::npos;
    std::size_t last = std::string_view::npos;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (!ieq_char(hay[i + j], needle[j])) {
                match = false;
                break;
            }
        }
        if (match) last = i;
    }
    return last;
}

void lowercase_prefix_if_match(std::string& s, const char* lower) {
    std::size_t n = std::strlen(lower);
    if (s.size() < n) return;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != lower[i])
            return;
    }
    for (std::size_t i = 0; i < n; ++i) s[i] = lower[i];
}

}  // namespace

std::string normalize_input(std::string_view user_input) {
    std::size_t start = 0;
    std::size_t end = user_input.size();
    while (start < end && std::isspace(static_cast<unsigned char>(user_input[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(user_input[end - 1])))
        --end;
    std::string s(user_input.substr(start, end - start));

    lowercase_prefix_if_match(s, "http:");
    lowercase_prefix_if_match(s, "https:");

    if (s.find("://") == std::string::npos) {
        s.insert(0, "http://");
    }
    return s;
}

std::string extract_base_url(std::string_view url) {
    const std::size_t web = find_last_ci(url, "/web");
    if (web != std::string_view::npos) {
        return std::string(url.substr(0, web));
    }
    const std::size_t scheme_end = url.find("://");
    const std::size_t host_start = (scheme_end == std::string_view::npos)
        ? 0 : scheme_end + 3;
    const std::size_t path_start = url.find('/', host_start);
    if (path_start == std::string_view::npos) {
        return std::string(url);
    }
    return std::string(url.substr(0, path_start));
}

bool is_valid_public_info(std::string_view body) {
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root) return false;
    bool ok = false;
    if (cJSON_IsObject(root)) {
        cJSON* id = cJSON_GetObjectItem(root, "Id");
        if (id && cJSON_IsString(id) && id->valuestring && id->valuestring[0] != '\0') {
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

}  // namespace jellyfin_api
