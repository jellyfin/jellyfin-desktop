#include "cef/resource_handler.h"
#include <cstring>
#include "logging.h"

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

    auto it = embedded_resources.find(url);
    if (it != embedded_resources.end()) {
        return new EmbeddedResourceHandler(it->second);
    }

    LOG_WARN(LOG_RESOURCE, "EmbeddedScheme not found: %s", url.c_str());
    return nullptr;
}

EmbeddedResourceHandler::EmbeddedResourceHandler(const EmbeddedResource& resource)
    : resource_(resource) {}

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
    response->SetMimeType(resource_.mime_type);
    response_length = static_cast<int64_t>(resource_.size);
}

bool EmbeddedResourceHandler::Read(void* data_out,
                                   int bytes_to_read,
                                   int& bytes_read,
                                   CefRefPtr<CefResourceReadCallback> callback) {
    if (offset_ >= resource_.size) {
        bytes_read = 0;
        return false;
    }

    size_t remaining = resource_.size - offset_;
    size_t to_copy = (std::min)(remaining, static_cast<size_t>(bytes_to_read));
    memcpy(data_out, resource_.data + offset_, to_copy);
    offset_ += to_copy;
    bytes_read = static_cast<int>(to_copy);
    return true;
}
