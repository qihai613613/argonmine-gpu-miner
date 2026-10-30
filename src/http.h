// Minimal WinHTTP client (Windows only) for the Argonmine work protocol.
#pragma once
#include <string>

namespace ab {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

bool http_request(const std::string &url, const std::string &method,
                  const std::string &body, const std::string &content_type,
                  HttpResponse &resp);

std::string url_encode(const std::string &s);

}  // namespace ab
