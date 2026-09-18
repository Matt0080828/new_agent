#include "http.hpp"

#include "sse.hpp"
#include "util.hpp"

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

static const size_t kMaxResponse = 256 * 1024;

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

namespace {

// Result of the connection handshake, shared by the buffered and the streaming
// request paths so both classify failures identically.
struct OpenResult {
  int fd;
  std::string host;
  std::string error;
  bool permanent;  // retrying cannot help (bad scheme, ...)
  OpenResult() : fd(-1), permanent(false) {}
};

struct ReadResult {
  bool ok;
  std::string error;
  bool permanent;
  ReadResult() : ok(false), permanent(false) {}
};

}  // namespace

// Parse the URL, connect, and send the request body. The caller owns the fd.
static OpenResult http_open_send(const std::string& url, const std::string& json,
                                 const std::string& bearer, int timeout_sec) {
  OpenResult out;
  std::string path;
  int port = 80;
  if (!parse_url(url, out.host, port, path)) {
    out.error = "only http:// URLs supported";
    out.permanent = true;
    return out;
  }
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  struct addrinfo* res = 0;
  std::ostringstream ps;
  ps << port;
  int gai = getaddrinfo(out.host.c_str(), ps.str().c_str(), &hints, &res);
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
      << "Host: " << out.host << "\r\n"
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
  out.fd = fd;
  return out;
}

static ReadResult http_read_all(int fd, std::string* out) {
  ReadResult r;
  char buf[4096];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      r.error = strerror(errno);
      return r;
    }
    if (n == 0)
      break;
    out->append(buf, (size_t)n);
    if (out->size() > kMaxResponse) {
      r.error = "response too large";
      r.permanent = true;
      return r;
    }
  }
  r.ok = true;
  return r;
}

// Split a raw response into status and body.
static void http_parse_response(const std::string& raw, HttpResult* out) {
  std::string::size_type sp = raw.find(' ');
  if (sp != std::string::npos)
    out->status = atoi(raw.c_str() + sp + 1);
  std::string::size_type sep = raw.find("\r\n\r\n");
  if (sep == std::string::npos)
    sep = raw.find("\n\n");
  if (sep == std::string::npos) {
    out->error = "bad HTTP response";
    out->status = -1;  // permanent: the peer is not speaking HTTP in a usable way
    return;
  }
  if (sep == raw.find("\r\n\r\n"))
    out->body = raw.substr(sep + 4);
  else
    out->body = raw.substr(sep + 2);
  if (out->status < 200 || out->status >= 300)
    out->error = "HTTP status";
}

HttpResult http_post_json(const std::string& url, const std::string& json,
                          const std::string& bearer, int timeout_sec) {
  HttpResult out;
  out.status = 0;
  OpenResult o = http_open_send(url, json, bearer, timeout_sec);
  if (o.fd < 0) {
    out.error = o.error;
    out.status = o.permanent ? -1 : 0;
    return out;
  }
  std::string raw;
  ReadResult r = http_read_all(o.fd, &raw);
  close(o.fd);
  if (!r.ok) {
    out.error = r.error;
    out.status = r.permanent ? -1 : 0;
    return out;
  }
  http_parse_response(raw, &out);
  return out;
}

// ---------------------------------------------------------------------------
// Retry policy
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Streaming (text/event-stream)
// ---------------------------------------------------------------------------

namespace {

struct StreamState {
  SseParser parser;
  std::string acc;   // assembled assistant text
  std::string raw;   // raw bytes, kept for status + non-SSE fallbacks
  DeltaFn on_delta;
  void* ctx;
  bool emitted;      // at least one delta reached the user
  bool too_large;
  bool header_done;  // HTTP header/body boundary located
  size_t fed;        // raw bytes already handed to the parser

  StreamState(DeltaFn d, void* c)
      : on_delta(d), ctx(c), emitted(false), too_large(false), header_done(false), fed(0) {}
};

// Drain every complete payload currently queued in the parser.
void drain_stream(StreamState* s) {
  std::string payload;
  while (s->parser.next(&payload)) {
    std::string delta;
    if (!sse_delta_content(payload, delta) || delta.empty())
      continue;
    s->acc += delta;
    s->emitted = true;
    if (s->on_delta)
      s->on_delta(s->ctx, delta);
  }
}

// One streaming attempt. Returns true when a usable response arrived.
bool stream_attempt(const std::string& url, const std::string& json, const std::string& bearer,
                    int timeout_sec, StreamState* s, HttpResult* out) {
  s->parser.reset();
  s->acc.clear();
  s->raw.clear();
  s->emitted = false;
  s->too_large = false;
  s->header_done = false;
  s->fed = 0;

  OpenResult o = http_open_send(url, json, bearer, timeout_sec);
  if (o.fd < 0) {
    out->error = o.error;
    out->status = o.permanent ? -1 : 0;
    return false;
  }
  char buf[4096];
  for (;;) {
    ssize_t n = recv(o.fd, buf, sizeof(buf), 0);
    if (n < 0) {
      out->error = strerror(errno);
      out->status = 0;
      close(o.fd);
      return false;
    }
    if (n == 0)
      break;
    s->raw.append(buf, (size_t)n);
    if (s->raw.size() > kMaxResponse) {
      s->too_large = true;
      close(o.fd);
      return false;
    }
    // Only the bytes after the HTTP header reach the event parser, so the status
    // line can never be mistaken for a data line. The boundary is located once
    // and `fed` tracks how much of the body has already been parsed.
    if (!s->header_done) {
      std::string::size_type sep = s->raw.find("\r\n\r\n");
      size_t hlen = 0;
      if (sep != std::string::npos) {
        hlen = sep + 4;
      } else {
        sep = s->raw.find("\n\n");
        if (sep != std::string::npos)
          hlen = sep + 2;
      }
      if (hlen == 0)
        continue;  // headers still incomplete: keep reading
      s->header_done = true;
      s->fed = hlen;
    }
    if (s->raw.size() > s->fed) {
      s->parser.feed(s->raw.substr(s->fed));
      s->fed = s->raw.size();
      drain_stream(s);
    }
  }
  close(o.fd);
  s->parser.flush();
  drain_stream(s);
  http_parse_response(s->raw, out);
  if (s->too_large) {
    out->error = "response too large";
    out->status = -1;
    return false;
  }
  if (out->status < 200 || out->status >= 300) {
    out->body = s->raw;
    return false;
  }
  out->body = s->acc;
  if (out->body.empty()) {
    // A server that ignored "stream": true answers with one plain JSON body.
    // Fall back to it rather than reporting an empty successful reply.
    std::string content;
    if (json_extract_string(s->raw, "content", content) && !content.empty())
      out->body = content;
  }
  return true;
}

}  // namespace

HttpResult http_post_json_stream(const std::string& url, const std::string& json,
                                 const std::string& bearer, int timeout_sec,
                                 const RetryPolicy& policy, bool verbose,
                                 DeltaFn on_delta, void* ctx) {
  StreamState s(on_delta, ctx);
  HttpResult out;
  out.status = 0;
  int attempts = policy.attempts > 0 ? policy.attempts : 1;
  std::string log;
  for (int i = 1; i <= attempts; ++i) {
    bool ok = stream_attempt(url, json, bearer, timeout_sec, &s, &out);
    if (ok) {
      if (!s.parser.done())
        out.error = "stream ended without [DONE]";
      if (verbose) {
        std::ostringstream line;
        line << "[slim] stream attempt " << i << "/" << attempts << " status=" << out.status
             << " deltas=" << (s.emitted ? "yes" : "no") << " bytes=" << s.acc.size() << "\n";
        log += line.str();
      }
      break;
    }
    bool retryable = is_retryable_status(out.status);
    if (verbose) {
      std::ostringstream line;
      line << "[slim] stream attempt " << i << "/" << attempts << " status=" << out.status
           << (retryable ? " retryable" : " final");
      if (!out.error.empty())
        line << ": " << out.error;
      line << "\n";
      log += line.str();
    }
    // Once a delta has been shown, a retry would restart generation behind the
    // user's back: the failure is final.
    if (!retryable || s.emitted || i == attempts)
      break;
    int delay = retry_delay_ms(i, policy);
    http_sleep_ms(delay);
  }
  if (verbose && !log.empty())
    std::fputs(log.c_str(), stderr);
  return out;
}
