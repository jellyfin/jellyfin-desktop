#pragma once

#include <string>

namespace open_url_linux {

// Caller must ensure the URL is non-empty and doesn't start with '-'.
void open(const std::string& url);

}
