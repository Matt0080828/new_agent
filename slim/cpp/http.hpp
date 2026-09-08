#ifndef SLIM_HTTP_HPP
#define SLIM_HTTP_HPP

#include <string>

struct HttpResult {
  int status;
  std::string body;
  std::string error;
};

HttpResult http_post_json(const std::string& url, const std::string& json,
                          const std::string& bearer, int timeout_sec);

#endif
