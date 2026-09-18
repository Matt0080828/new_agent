#include "http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
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
    // Negative status marks a permanent/config error. Transport failures use 0,
    // which the retry policy treats as retryable; this must not be retried.
    out.status = -1;
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
      out.status = -1;  // permanent: a retry returns the same oversized body
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
    out.status = -1;  // permanent: the peer is not speaking HTTP in a usable way
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


void http_sleep_ms(int ms);  // defined below; forward declared for run_with_retry

bool is_retryable_status(int status) {
  if (status == 0)
    return true;  // transport-level failure: no response at all
  if (status == 429)
    return true;  // rate limited
  return status >= 500 && status <= 599;
}

int retry_delay_ms(int attempt_index, const RetryPolicy& p) {
  if (attempt_index < 1)
    attempt_index = 1;
  int delay = p.base_delay_ms;
  for (int i = 1; i < attempt_index; ++i) {
    if (delay >= p.max_delay_ms)
      return p.max_delay_ms;
    delay *= 2;
  }
  if (p.max_delay_ms > 0 && delay > p.max_delay_ms)
    delay = p.max_delay_ms;
  return delay < 0 ? 0 : delay;
}

bool run_with_retry(const RetryPolicy& policy, RetryAttemptFn fn, void* ctx, HttpResult* out,
                    SleepFn sleeper, std::string* log) {
  int attempts = policy.attempts > 0 ? policy.attempts : 1;
  for (int i = 1; i <= attempts; ++i) {
    out->status = 0;
    out->body.clear();
    out->error.clear();
    bool delivered = fn(ctx, out);
    bool retryable = is_retryable_status(out->status);
    if (log) {
      std::ostringstream line;
      line << "[slim] http attempt " << i << "/" << attempts << " status=" << out->status
           << (delivered ? " delivered" : " failed") << (retryable ? " retryable" : " final");
      if (!out->error.empty())
        line << ": " << out->error;
      *log += line.str() + "\n";
    }
    if (!retryable)
      return true;
    if (i == attempts)
      break;
    int delay = retry_delay_ms(i, policy);
    if (sleeper)
      sleeper(delay);
    else
      http_sleep_ms(delay);
  }
  return false;
}

void http_sleep_ms(int ms) {
  if (ms <= 0)
    return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, 0);
}

namespace {
struct PostCtx {
  const std::string* url;
  const std::string* json;
  const std::string* bearer;
  int timeout_sec;
};

bool post_attempt(void* ctx, HttpResult* out) {
  PostCtx* c = static_cast<PostCtx*>(ctx);
  *out = http_post_json(*c->url, *c->json, *c->bearer, c->timeout_sec);
  return out->status >= 200 && out->status < 300;
}
}  // namespace

HttpResult http_post_json_retry(const std::string& url, const std::string& json,
                                const std::string& bearer, int timeout_sec,
                                const RetryPolicy& policy, bool verbose) {
  PostCtx ctx;
  ctx.url = &url;
  ctx.json = &json;
  ctx.bearer = &bearer;
  ctx.timeout_sec = timeout_sec;
  HttpResult out;
  out.status = 0;
  std::string log;
  run_with_retry(policy, post_attempt, &ctx, &out, 0, verbose ? &log : 0);
  if (verbose && !log.empty())
    std::fputs(log.c_str(), stderr);
  return out;
}
