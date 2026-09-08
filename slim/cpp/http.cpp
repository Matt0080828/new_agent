#include "http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static bool parse_url(const std::string& url, std::string& host, int& port, std::string& path) {
  std::string u = url;
  if (u.compare(0, 8, "https://") == 0)
    return false;
  if (u.compare(0, 7, "http://") == 0)
    u = u.substr(7);
  port = 80;
  std::string::size_type slash = u.find('/');
  std::string hp = slash == std::string::npos ? u : u.substr(0, slash);
  path = slash == std::string::npos ? "/" : u.substr(slash);
  std::string::size_type colon = hp.rfind(':');
  if (colon != std::string::npos) {
    host = hp.substr(0, colon);
    port = atoi(hp.substr(colon + 1).c_str());
  } else {
    host = hp;
  }
  return !host.empty() && port > 0;
}

HttpResult http_post_json(const std::string& url, const std::string& json,
                          const std::string& bearer, int timeout_sec) {
  HttpResult out;
  out.status = 0;
  std::string host, path;
  int port = 80;
  if (!parse_url(url, host, port, path)) {
    out.error = "only http:// URLs supported";
    return out;
  }
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  struct addrinfo* res = 0;
  std::ostringstream ps;
  ps << port;
  int gai = getaddrinfo(host.c_str(), ps.str().c_str(), &hints, &res);
  if (gai != 0) {
    out.error = gai_strerror(gai);
    return out;
  }
  int fd = -1;
  for (struct addrinfo* p = res; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0)
      continue;
    struct timeval tv;
    tv.tv_sec = timeout_sec > 0 ? timeout_sec : 120;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    out.error = "connect failed";
    return out;
  }
  std::ostringstream req;
  req << "POST " << path << " HTTP/1.0\r\n"
      << "Host: " << host << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Accept: application/json\r\n"
      << "Content-Length: " << json.size() << "\r\n"
      << "Connection: close\r\n";
  if (!bearer.empty())
    req << "Authorization: Bearer " << bearer << "\r\n";
  req << "\r\n" << json;
  std::string wire = req.str();
  size_t sent = 0;
  while (sent < wire.size()) {
    ssize_t n = send(fd, wire.data() + sent, wire.size() - sent, 0);
    if (n <= 0) {
      out.error = "send failed";
      close(fd);
      return out;
    }
    sent += (size_t)n;
  }
  std::string resp;
  char buf[4096];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      out.error = strerror(errno);
      close(fd);
      return out;
    }
    if (n == 0)
      break;
    resp.append(buf, (size_t)n);
    if (resp.size() > 256 * 1024) {
      out.error = "response too large";
      close(fd);
      return out;
    }
  }
  close(fd);
  std::string::size_type sp = resp.find(' ');
  if (sp != std::string::npos)
    out.status = atoi(resp.c_str() + sp + 1);
  std::string::size_type sep = resp.find("\r\n\r\n");
  if (sep == std::string::npos)
    sep = resp.find("\n\n");
  if (sep == std::string::npos) {
    out.error = "bad HTTP response";
    return out;
  }
  if (sep == resp.find("\r\n\r\n"))
    out.body = resp.substr(sep + 4);
  else
    out.body = resp.substr(sep + 2);
  if (out.status < 200 || out.status >= 300)
    out.error = "HTTP status";
  return out;
}
