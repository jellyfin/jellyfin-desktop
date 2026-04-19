#pragma once

#include <string>
#include "include/cef_scheme.h"
#include "include/cef_resource_handler.h"
#include "embedded_resources.h"

class EmbeddedSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
    CefRefPtr<CefResourceHandler> Create(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString& scheme_name,
        CefRefPtr<CefRequest> request) override;

    IMPLEMENT_REFCOUNTING(EmbeddedSchemeHandlerFactory);
};

class EmbeddedResourceHandler : public CefResourceHandler {
public:
    // Borrowed: wraps a static EmbeddedResource from the embedded_resources map.
    EmbeddedResourceHandler(const EmbeddedResource& resource);

    // Owned: takes a heap-allocated byte string and mime type.  Used for
    // dynamic resources (e.g. about.js with prepended data blob).
    EmbeddedResourceHandler(std::string owned_bytes, const char* mime_type);

    bool Open(CefRefPtr<CefRequest> request,
              bool& handle_request,
              CefRefPtr<CefCallback> callback) override;

    void GetResponseHeaders(CefRefPtr<CefResponse> response,
                           int64_t& response_length,
                           CefString& redirect_url) override;

    bool Read(void* data_out,
              int bytes_to_read,
              int& bytes_read,
              CefRefPtr<CefResourceReadCallback> callback) override;

    void Cancel() override {}

private:
    // When owned_.empty() is false, bytes_/size_ point into owned_ and
    // mime_type_ names a C-string that outlives this handler. Otherwise
    // bytes_/size_/mime_type_ come from a borrowed EmbeddedResource.
    std::string owned_;
    const uint8_t* bytes_;
    size_t size_;
    const char* mime_type_;
    size_t offset_ = 0;

    IMPLEMENT_REFCOUNTING(EmbeddedResourceHandler);
};
