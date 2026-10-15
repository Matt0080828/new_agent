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

// CA bundle used to verify TLS peers, set once at startup from --ca-file / SLIM_CA_FILE.
// Empty means "no bundle": TLS is then refused rather than silently unverified.
static std::string g_ca_file = "/etc/ssl/certs/ca-certificates.crt";

void http_set_ca_file(const std::string& path) { g_ca_file = path; }

#ifdef SLIM_WITH_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

namespace {

// One connection: a socket, plus the mbedTLS state when it is a TLS session. Both the
// buffered and the streaming paths go through conn_read/conn_write_all, so failures are
// classified identically whether or not TLS is in play.
struct Conn {
  int fd;
#ifdef SLIM_WITH_TLS
  bool tls;
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  bool ssl_setup;
  bool ssl_conf;
  bool ca_inited;
  bool entropy_inited;
  bool drbg_inited;
#endif
  Conn()
      : fd(-1)
#ifdef SLIM_WITH_TLS
        , tls(false), ssl_setup(false), ssl_conf(false), ca_inited(false),
        entropy_inited(false), drbg_inited(false)
#endif
  {
  }
};

// Result of the connection handshake, shared by the buffered and the streaming
// request paths so both classify failures identically.
struct OpenResult {
  Conn conn;
  std::string host;
  std::string error;
  bool permanent;  // retrying cannot help (bad scheme, ...)
  OpenResult() : permanent(false) {}
};

struct ReadResult {
  bool ok;
  std::string error;
  bool permanent;
  ReadResult() : ok(false), permanent(false) {}
};

}  // namespace

// Parse "<scheme>://host[:port]/path". A bare host[:port]/path is treated as http.
// An explicit scheme other than http/https is refused; https is only *usable* when the
// build has TLS, which http_open_send enforces with a clear error.
static bool parse_url(const std::string& url, std::string& host, int& port, std::string& path,
                      bool& tls) {
  std::string u = url;
  tls = false;
  port = 80;
  if (url_is_tls(u)) {
    tls = true;
    port = 443;
    u = u.substr(8);
  } else if (u.size() >= 7 && u.compare(0, 7, "http://") == 0) {
    u = u.substr(7);
  } else if (u.find("://") != std::string::npos) {
    return false;
  }
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

#ifdef SLIM_WITH_TLS
// Entropy -> DRBG -> client config -> handshake, with MBEDTLS_SSL_VERIFY_REQUIRED and the
// hostname for SNI. Anything that cannot be verified is a failure, never a warning.
static bool tls_handshake(Conn* c, const std::string& host, std::string* err) {
  mbedtls_ssl_init(&c->ssl);
  mbedtls_ssl_config_init(&c->conf);
  c->ssl_conf = true;
  mbedtls_x509_crt_init(&c->ca);
  c->ca_inited = true;
  mbedtls_entropy_init(&c->entropy);
  c->entropy_inited = true;
  mbedtls_ctr_drbg_init(&c->ctr_drbg);
  c->drbg_inited = true;
  const char* pers = "slim-agent";
  if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                            (const unsigned char*)pers, strlen(pers)) != 0) {
    *err = "tls: rng seed failed";
    return false;
  }
  if (g_ca_file.empty() || mbedtls_x509_crt_parse_file(&c->ca, g_ca_file.c_str()) != 0) {
    *err = "tls: cannot load CA bundle (set --ca-file / SLIM_CA_FILE)";
    return false;
  }
  if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    *err = "tls: config failed";
    return false;
  }
  mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, 0);
  mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
  mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) {
    *err = "tls: setup failed";
    return false;
  }
  c->ssl_setup = true;
  if (mbedtls_ssl_set_hostname(&c->ssl, host.c_str()) != 0) {
    *err = "tls: set hostname failed";
    return false;
  }
  c->net.fd = c->fd;
  mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, 0);
  int ret;
  while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      char buf[128];
      mbedtls_strerror(ret, buf, sizeof(buf));
      *err = std::string("tls: handshake failed: ") + buf;
      return false;
    }
  }
  if (mbedtls_ssl_get_verify_result(&c->ssl) != 0) {
    *err = "tls: certificate verification failed";
    return false;
  }
  c->tls = true;
  return true;
}
#endif  // SLIM_WITH_TLS

// -1 error, 0 EOF, >0 bytes. The caller distinguishes error from EOF via the flags.
static ssize_t conn_read(Conn* c, char* buf, size_t n, bool* eof, bool* error) {
  *eof = false;
  *error = false;
#ifdef SLIM_WITH_TLS
  if (c->tls) {
    for (;;) {
      int r = mbedtls_ssl_read(&c->ssl, (unsigned char*)buf, n);
      if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
        *eof = true;
        return 0;
      }
      if (r < 0) {
        *error = true;
        return -1;
      }
      return (ssize_t)r;
    }
  }
#endif
  ssize_t r = recv(c->fd, buf, n, 0);
  if (r == 0)
    *eof = true;
  else if (r < 0)
    *error = true;
  return r;
}

static bool conn_write_all(Conn* c, const std::string& data, std::string* err) {
  size_t sent = 0;
  while (sent < data.size()) {
#ifdef SLIM_WITH_TLS
    if (c->tls) {
      int n = mbedtls_ssl_write(&c->ssl, (const unsigned char*)data.data() + sent,
                                data.size() - sent);
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      if (n < 0) {
        *err = "tls send failed";
        return false;
      }
      sent += (size_t)n;
      continue;
    }
#endif
    ssize_t n = send(c->fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) {
      *err = "send failed";
      return false;
    }
    sent += (size_t)n;
  }
  return true;
}

static void conn_close(Conn* c) {
#ifdef SLIM_WITH_TLS
  if (c->tls)
    mbedtls_ssl_close_notify(&c->ssl);
  if (c->ssl_setup)
    mbedtls_ssl_free(&c->ssl);
  if (c->ssl_conf)
    mbedtls_ssl_config_free(&c->conf);
  if (c->ca_inited)
    mbedtls_x509_crt_free(&c->ca);
  if (c->drbg_inited)
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
  if (c->entropy_inited)
    mbedtls_entropy_free(&c->entropy);
#endif
  if (c->fd >= 0)
    close(c->fd);
  c->fd = -1;
}

// Parse the URL, connect, and send the request body. The caller owns the fd.
static OpenResult http_open_send(const std::string& url, const std::string& json,
                                 const std::string& bearer, int timeout_sec) {
  OpenResult out;
  std::string path;
  int port = 80;
  bool tls = false;
  if (!parse_url(url, out.host, port, path, tls)) {
    out.error = "unsupported URL (only http:// and https:// are spoken)";
    out.permanent = true;
    return out;
  }
#ifndef SLIM_WITH_TLS
  if (tls) {
    out.error = "https:// requires a TLS-enabled build (make tls / tls-static)";
    out.permanent = true;
    return out;
  }
#endif
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
  out.conn.fd = fd;
#ifdef SLIM_WITH_TLS
  if (tls) {
    std::string terr;
    if (!tls_handshake(&out.conn, out.host, &terr)) {
      out.error = terr;
      out.permanent = true;
      conn_close(&out.conn);
      return out;
    }
  }
#endif
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
  std::string werr;
  if (!conn_write_all(&out.conn, req.str(), &werr)) {
    out.error = werr;
    conn_close(&out.conn);
    return out;
  }
  return out;
}

static ReadResult http_read_all(Conn* c, std::string* out) {
  ReadResult r;
  char buf[4096];
  for (;;) {
    bool eof = false, err = false;
    ssize_t n = conn_read(c, buf, sizeof(buf), &eof, &err);
    if (err) {
#ifdef SLIM_WITH_TLS
      r.error = c->tls ? std::string("tls read failed") : std::string(strerror(errno));
#else
      r.error = strerror(errno);
#endif
      return r;
    }
    if (eof || n == 0)
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
  if (o.conn.fd < 0) {
    out.error = o.error;
    out.status = o.permanent ? -1 : 0;
    return out;
  }
  std::string raw;
  ReadResult r = http_read_all(&o.conn, &raw);
  conn_close(&o.conn);
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
  size_t body_off;   // offset of the first body byte (for error diagnostics)
  size_t fed;        // raw bytes already handed to the parser

  StreamState(DeltaFn d, void* c)
      : on_delta(d), ctx(c), emitted(false), too_large(false), header_done(false), body_off(0), fed(0) {}
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
  s->body_off = 0;
  s->fed = 0;

  OpenResult o = http_open_send(url, json, bearer, timeout_sec);
  if (o.conn.fd < 0) {
    out->error = o.error;
    out->status = o.permanent ? -1 : 0;
    return false;
  }
  char buf[4096];
  for (;;) {
    bool eof = false, err = false;
    ssize_t n = conn_read(&o.conn, buf, sizeof(buf), &eof, &err);
    if (err) {
      out->error = "read failed";
      out->status = 0;
      conn_close(&o.conn);
      return false;
    }
    if (eof || n == 0)
      break;
    s->raw.append(buf, (size_t)n);
    if (s->raw.size() > kMaxResponse) {
      s->too_large = true;
      conn_close(&o.conn);
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
      s->body_off = hlen;
      s->fed = hlen;
    }
    if (s->raw.size() > s->fed) {
      s->parser.feed(s->raw.substr(s->fed));
      s->fed = s->raw.size();
      drain_stream(s);
    }
  }
  conn_close(&o.conn);
  s->parser.flush();
  drain_stream(s);
  http_parse_response(s->raw, out);
  if (s->too_large) {
    out->error = "response too large";
    out->status = -1;
    return false;
  }
  if (out->status < 200 || out->status >= 300) {
    // Never hand the raw response (status line + headers + body) back as the answer:
    // on the streaming path that text was printed to the user and appended to the
    // session history as if the model had said it. Report a bounded server message.
    std::ostringstream err;
    err << "HTTP status " << out->status;
    std::string msg;
    if (s->body_off <= s->raw.size() &&
        json_extract_string(s->raw.substr(s->body_off), "message", msg) && !msg.empty())
      err << ": " << cap_prompt(msg, 200);
    out->error = err.str();
    out->body.clear();
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
