#include "cef/resource_handler.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include "common.h"
#include "logging.h"
#include "paths/paths.h"
#include "version.h"
#include "include/cef_parser.h"
#include "include/cef_values.h"

// Absolute-but-not-resolved: prepends CWD if path is relative, leaves
// symlinks and ".."/"." components intact. Falls back to the input when
// the lookup fails.
static std::string absPath(const std::string& p) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) return p;
    return abs.string();
}

// Generated at startup from kBgColor; served as app://resources/theme.css.
static char g_theme_css[64];
static size_t g_theme_css_len = 0;

static void init_theme_css() {
    if (g_theme_css_len == 0)
        g_theme_css_len = snprintf(g_theme_css, sizeof(g_theme_css),
            ":root{--bg-color:#%06x}", kBgColor.rgb);
}

CefRefPtr<CefResourceHandler> EmbeddedSchemeHandlerFactory::Create(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const CefString& scheme_name,
    CefRefPtr<CefRequest> request) {

    std::string url = request->GetURL().ToString();

    // Strip scheme: "app://resources/foo.html" -> "resources/foo.html"
    size_t pos = url.find("://");
    if (pos != std::string::npos) {
        url = url.substr(pos + 3);
    }

    // Strip query string and fragment (e.g. "?foo=bar" or "#playlist-data")
    pos = url.find_first_of("?#");
    if (pos != std::string::npos) {
        url = url.substr(0, pos);
    }

    if (url == "resources/theme.css") {
        init_theme_css();
        static const EmbeddedResource theme = {
            reinterpret_cast<const uint8_t*>(g_theme_css),
            g_theme_css_len, "text/css"
        };
        return new EmbeddedResourceHandler(theme);
    }

    if (url == "resources/about.js") {
        auto it = embedded_resources.find(url);
        if (it == embedded_resources.end()) {
            LOG_WARN(LOG_RESOURCE, "about.js missing from embedded_resources");
            return nullptr;
        }

        // Assemble the data blob with CefWriteJSON (per CLAUDE.md: no
        // hand-rolled JSON). Log paths are omitted when file logging is
        // disabled — the panel renders only the rows present in the data.
        auto dict = CefDictionaryValue::Create();
        dict->SetString("app", APP_VERSION_FULL);
        dict->SetString("cef", APP_CEF_VERSION);
        dict->SetString("configDir", absPath(paths::getConfigDir()));
        const std::string& log_path = activeLogPath();
        if (!log_path.empty())
            dict->SetString("logFile", absPath(log_path));
        auto val = CefValue::Create();
        val->SetDictionary(dict);
        CefString json = CefWriteJSON(val, JSON_WRITER_DEFAULT);

        std::string prefix = "var _aboutData = " + json.ToString() + ";\n";
        std::string payload;
        payload.reserve(prefix.size() + it->second.size);
        payload.append(prefix);
        payload.append(reinterpret_cast<const char*>(it->second.data), it->second.size);
        return new EmbeddedResourceHandler(std::move(payload), it->second.mime_type);
    }

    auto it = embedded_resources.find(url);
    if (it != embedded_resources.end()) {
        return new EmbeddedResourceHandler(it->second);
    }

    LOG_WARN(LOG_RESOURCE, "EmbeddedScheme not found: {}", url.c_str());
    return nullptr;
}

EmbeddedResourceHandler::EmbeddedResourceHandler(const EmbeddedResource& resource)
    : bytes_(resource.data), size_(resource.size), mime_type_(resource.mime_type) {}

EmbeddedResourceHandler::EmbeddedResourceHandler(std::string owned_bytes, const char* mime_type)
    : owned_(std::move(owned_bytes)),
      bytes_(reinterpret_cast<const uint8_t*>(owned_.data())),
      size_(owned_.size()),
      mime_type_(mime_type) {}

bool EmbeddedResourceHandler::Open(CefRefPtr<CefRequest> request,
                                    bool& handle_request,
                                    CefRefPtr<CefCallback> callback) {
    handle_request = true;
    return true;
}

void EmbeddedResourceHandler::GetResponseHeaders(CefRefPtr<CefResponse> response,
                                                  int64_t& response_length,
                                                  CefString& redirect_url) {
    response->SetStatus(200);
    response->SetStatusText("OK");
    response->SetMimeType(mime_type_);
    response_length = static_cast<int64_t>(size_);
}

bool EmbeddedResourceHandler::Read(void* data_out,
                                   int bytes_to_read,
                                   int& bytes_read,
                                   CefRefPtr<CefResourceReadCallback> callback) {
    if (offset_ >= size_) {
        bytes_read = 0;
        return false;
    }

    size_t remaining = size_ - offset_;
    size_t to_copy = (std::min)(remaining, static_cast<size_t>(bytes_to_read));
    memcpy(data_out, bytes_ + offset_, to_copy);
    offset_ += to_copy;
    bytes_read = static_cast<int>(to_copy);
    return true;
}
