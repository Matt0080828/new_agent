// Tests for the SSE parser and the streaming transport.
//
// The parser tests drive arbitrary chunk boundaries in-process (deterministic),
// and the integration tests talk to a fake HTTP server that forks on loopback so
// real sockets, real header/body framing and real reset behaviour are exercised.
#include "../http.hpp"
#include "../sse.hpp"
#include "../util.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::cout << "FAIL  " << what << "\n";
  } else {
    std::cout << "ok    " << what << "\n";
  }
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

static void test_parser_single_event() {
  std::cout << "-- sse parser: one event\n";
  SseParser p;
  p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n");
  std::string payload;
  check(p.next(&payload), "one payload ready");
  check(payload.find("\"content\":\"hi\"") != std::string::npos, "payload is the data line");
  check(!p.next(&payload), "no second payload");
  check(!p.done(), "not done");
}

static void test_parser_split_everywhere() {
  std::cout << "-- sse parser: chunk boundaries\n";
  const std::string full =
      "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
      "data: [DONE]\n\n";
  SseParser p;
  // Feed one byte at a time: every possible split point, including inside
  // "data:" and inside the JSON.
  for (size_t i = 0; i < full.size(); ++i)
    p.feed(full.substr(i, 1));
  std::string payload, first, second, delta;
  check(p.next(&payload), "first payload after byte-by-byte feed");
  check(sse_delta_content(payload, delta) && delta == "hello", "first delta is hello");
  check(p.next(&payload), "second payload");
  check(sse_delta_content(payload, delta) && delta == " world", "second delta is ' world'");
  check(!p.next(&payload), "nothing left");
  check(p.done(), "[DONE] observed");
}

static void test_parser_two_events_one_chunk() {
  std::cout << "-- sse parser: two events in one chunk\n";
  SseParser p;
  p.feed("data: {\"a\":1}\n\ndata: {\"b\":2}\n\n");
  std::string payload;
  check(p.next(&payload) && payload == "{\"a\":1}", "first event");
  check(p.next(&payload) && payload == "{\"b\":2}", "second event");
  check(!p.next(&payload), "no third event");
}

static void test_parser_crlf_and_noise() {
  std::cout << "-- sse parser: CRLF, comments, other fields\n";
  SseParser p;
  p.feed(": keep-alive comment\r\n"
         "event: message\r\n"
         "id: 42\r\n"
         "retry: 1000\r\n"
         "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\r\n"
         "\r\n");
  std::string payload, delta;
  check(p.next(&payload), "CRLF event parsed");
  check(sse_delta_content(payload, delta) && delta == "x", "delta survives CRLF");
  check(!p.next(&payload), "noise produced no payload");
}

static void test_parser_multiline_data() {
  std::cout << "-- sse parser: multi-line data\n";
  SseParser p;
  p.feed("data: line one\ndata: line two\n\n");
  std::string payload;
  check(p.next(&payload) && payload == "line one\nline two", "data lines join with a newline");
}

static void test_parser_flush_and_reset() {
  std::cout << "-- sse parser: flush and reset\n";
  SseParser p;
  p.feed("data: {\"unterminated\":1}");
  std::string payload;
  check(!p.next(&payload), "nothing emitted without a terminating blank line");
  p.flush();
  check(p.next(&payload) && payload == "{\"unterminated\":1}", "flush emits the pending event");
  p.feed("data: [DONE]\n\n");
  check(p.done(), "done flag set");
  p.reset();
  check(!p.done(), "reset clears done");
  p.feed("data: {\"after\":\"reset\"}\n\n");
  check(p.next(&payload) && payload == "{\"after\":\"reset\"}", "parser usable after reset");
}

static void test_delta_content() {
  std::cout << "-- sse delta extraction\n";
  std::string out;
  check(sse_delta_content("{\"choices\":[{\"delta\":{\"content\":\"abc\"}}]}", out) && out == "abc",
        "delta.content");
  check(sse_delta_content("{\"choices\":[{\"message\":{\"content\":\"full\"}}]}", out) && out == "full",
        "message.content (non-streaming answer)");
  check(!sse_delta_content("{\"choices\":[{\"delta\":{\"content\":null}}]}", out),
        "null content is not a delta");
  check(!sse_delta_content("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}", out),
        "role-only chunk yields no delta");
  check(!sse_delta_content("", out), "empty payload");
}

static void test_assemble_whole_body() {
  std::cout << "-- sse_assemble (whole body)\n";
  std::string out;
  check(sse_assemble("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                     "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n"
                     "data: [DONE]\n\n",
                     out) && out == "ab",
        "assembles deltas in order");
  check(!sse_assemble("{\"choices\":[{\"message\":{\"content\":\"x\"}}]}", out),
        "a plain JSON body is not mistaken for a stream");
  check(!sse_assemble("", out), "empty body");
}

static void test_unicode_escapes_in_a_delta() {
  std::cout << "-- \\u escapes in a delta\n";
  // A server that escapes non-ASCII (Python's json.dumps does by default) must still
  // deliver the characters. Before this was handled, the backslash was consumed and
  // the text arrived as the literal "u6eab".
  SseParser p;
  p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"\\u6eab\\u5ea6 25.5\\u00b0C\"}}]}\n\n");
  std::string payload, delta;
  check(p.next(&payload) && sse_delta_content(payload, delta), "escaped delta extracted");
  check(delta == "\xe6\xba\xab\xe5\xba\xa6 25.5\xc2\xb0" "C",
        "\\uXXXX arrives as UTF-8, not as 'u6eabu5ea6'");

  SseParser q;
  q.feed("data: {\"choices\":[{\"delta\":{\"content\":\"\\ud83d\\ude00\"}}]}\n\n");
  check(q.next(&payload) && sse_delta_content(payload, delta) && delta == "\xf0\x9f\x98\x80",
        "a surrogate pair decodes to one code point");
}

// ---------------------------------------------------------------------------
// Fake HTTP server (loopback, forked)
// ---------------------------------------------------------------------------

struct FakeResponse {
  std::string bytes;
  bool reset;              // RST on close instead of FIN
  int delay_before_close_ms;
  FakeResponse() : reset(false), delay_before_close_ms(0) {}
};

static int start_fake_server(const std::vector<FakeResponse>& responses, int* port_out) {
  int lsock = socket(AF_INET, SOCK_STREAM, 0);
  if (lsock < 0)
    return -1;
  int one = 1;
  setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(lsock, 4) != 0) {
    close(lsock);
    return -1;
  }
  socklen_t alen = sizeof(addr);
  if (getsockname(lsock, (struct sockaddr*)&addr, &alen) != 0) {
    close(lsock);
    return -1;
  }
  *port_out = ntohs(addr.sin_port);

  pid_t pid = fork();
  if (pid == 0) {
    for (size_t i = 0; i < responses.size(); ++i) {
      int c = accept(lsock, 0, 0);
      if (c < 0)
        break;
      std::string req;
      char buf[1024];
      std::string::size_type header_end = std::string::npos;
      size_t body_len = 0;
      for (;;) {
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0)
          break;
        req.append(buf, (size_t)n);
        if (header_end == std::string::npos) {
          std::string::size_type p = req.find("\r\n\r\n");
          if (p != std::string::npos) {
            header_end = p + 4;
            std::string::size_type cl = req.find("Content-Length:");
            if (cl != std::string::npos && cl < header_end)
              body_len = (size_t)atoi(req.c_str() + cl + 15);
          }
        }
        if (header_end != std::string::npos && req.size() >= header_end + body_len)
          break;
      }
      size_t written = 0;
      while (written < responses[i].bytes.size()) {
        ssize_t n = send(c, responses[i].bytes.data() + written, responses[i].bytes.size() - written, 0);
        if (n <= 0)
          break;
        written += (size_t)n;
      }
      if (responses[i].delay_before_close_ms > 0) {
        struct timespec ts;
        ts.tv_sec = responses[i].delay_before_close_ms / 1000;
        ts.tv_nsec = (long)(responses[i].delay_before_close_ms % 1000) * 1000000L;
        nanosleep(&ts, 0);
      }
      if (responses[i].reset) {
        struct linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
      }
      close(c);
    }
    close(lsock);
    _exit(0);
  }
  close(lsock);
  return pid;
}

static void reap_fake_server(int pid) {
  for (int i = 0; i < 60; ++i) {  // up to ~3s
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000;
    nanosleep(&ts, 0);
  }
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
}

static std::string respond(int code, const std::string& status, const std::string& extra_headers,
                           const std::string& body) {
  char head[512];
  snprintf(head, sizeof(head), "HTTP/1.0 %d %s\r\nContent-Length: %u\r\n%s\r\n",
           code, status.c_str(), (unsigned)body.size(), extra_headers.c_str());
  return std::string(head) + body;
}

// ---------------------------------------------------------------------------
// Integration
// ---------------------------------------------------------------------------

struct DeltaCollector {
  std::string text;
  int calls;
  DeltaCollector() : calls(0) {}
};

static void collect_delta(void* ctx, const std::string& delta) {
  DeltaCollector* c = static_cast<DeltaCollector*>(ctx);
  c->text += delta;
  ++c->calls;
}

static void test_plain_json_request_round_trip() {
  std::cout << "-- http_post_json against a real socket\n";
  std::vector<FakeResponse> resps(1);
  resps[0].bytes = respond(200, "OK", "Content-Type: application/json\r\n",
                           "{\"choices\":[{\"message\":{\"content\":\"plain answer\"}}]}");
  int port = 0;
  int pid = start_fake_server(resps, &port);
  check(pid > 0 && port > 0, "fake server started");
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);
  HttpResult r = http_post_json(url, "{\"stream\":false}", "", 3);
  check(r.status == 200, "status 200");
  std::string content;
  check(json_extract_string(r.body, "content", content) && content == "plain answer",
        "body parsed from a plain response");
  reap_fake_server(pid);
}

static void test_stream_error_status_is_not_an_answer() {
  std::cout << "-- http_post_json_stream: an error status must not become the answer\n";
  std::vector<FakeResponse> resps(1);
  resps[0].bytes = respond(400, "Bad Request", "Content-Type: application/json\r\n",
                           "{\"error\":{\"message\":\"Failed to load model \\\"nvidia/nemotron-3-nano-4b\\\"\"}}");
  int port = 0;
  int pid = start_fake_server(resps, &port);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);
  DeltaCollector c;
  RetryPolicy p;
  HttpResult r = http_post_json_stream(url, "{\"stream\":true}", "", 3, p, false, collect_delta, &c);
  check(r.status == 400, "status 400 surfaced to the caller");
  check(c.calls == 0, "no delta printed for a failed request");
  check(r.body.empty(), "the raw response never becomes the body/answer");
  check(r.error.find("HTTP status 400") != std::string::npos, "error names the status");
  check(r.error.find("HTTP/1.1") == std::string::npos && r.error.find("X-Powered") == std::string::npos,
        "error carries no header bytes");
  check(r.error.find("Failed to load model") != std::string::npos,
        "error keeps the server message (bounded)");
  reap_fake_server(pid);
}

static void test_stream_round_trip() {
  std::cout << "-- http_post_json_stream: deltas and accumulation\n";
  std::string body =
      "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\", world\"}}]}\n\n"
      "data: [DONE]\n\n";
  std::vector<FakeResponse> resps(1);
  resps[0].bytes = respond(200, "OK", "Content-Type: text/event-stream\r\n", body);
  int port = 0;
  int pid = start_fake_server(resps, &port);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);
  DeltaCollector c;
  RetryPolicy p;
  HttpResult r = http_post_json_stream(url, "{\"stream\":true}", "", 3, p, false, collect_delta, &c);
  check(r.status == 200, "status 200");
  check(c.text == "Hello, world", "deltas arrived in order");
  check(c.calls == 2, "two deltas (the role-only chunk produced none)");
  check(r.body == "Hello, world", "assembled body");
  check(r.error.empty(), "no error when [DONE] is present");
  reap_fake_server(pid);
}

static void test_stream_without_done() {
  std::cout << "-- http_post_json_stream: missing [DONE]\n";
  std::vector<FakeResponse> resps(1);
  resps[0].bytes = respond(200, "OK", "Content-Type: text/event-stream\r\n",
                           "data: {\"choices\":[{\"delta\":{\"content\":\"trunc\"}}]}\n\n");
  int port = 0;
  int pid = start_fake_server(resps, &port);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
  DeltaCollector c;
  RetryPolicy p;
  HttpResult r = http_post_json_stream(url, "{}", "", 3, p, false, collect_delta, &c);
  check(c.text == "trunc", "text still delivered");
  check(r.error.find("[DONE]") != std::string::npos, "missing [DONE] is reported");
  reap_fake_server(pid);
}

static void test_stream_server_ignores_stream_flag() {
  std::cout << "-- http_post_json_stream: server answers in one shot\n";
  std::vector<FakeResponse> resps(1);
  resps[0].bytes = respond(200, "OK", "Content-Type: application/json\r\n",
                           "{\"choices\":[{\"message\":{\"content\":\"one shot\"}}]}");
  int port = 0;
  int pid = start_fake_server(resps, &port);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
  DeltaCollector c;
  RetryPolicy p;
  HttpResult r = http_post_json_stream(url, "{}", "", 3, p, false, collect_delta, &c);
  check(r.body == "one shot", "falls back to the plain JSON body");
  reap_fake_server(pid);
}

static void test_stream_retries_before_any_delta() {
  std::cout << "-- http_post_json_stream: retry before the first delta\n";
  std::vector<FakeResponse> resps(2);
  resps[0].bytes = respond(500, "Server Error", "", "busy");
  resps[1].bytes = respond(200, "OK", "Content-Type: text/event-stream\r\n",
                           "data: {\"choices\":[{\"delta\":{\"content\":\"second try\"}}]}\n\n"
                           "data: [DONE]\n\n");
  int port = 0;
  int pid = start_fake_server(resps, &port);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
  DeltaCollector c;
  RetryPolicy p;
  p.base_delay_ms = 10;
  HttpResult r = http_post_json_stream(url, "{}", "", 3, p, false, collect_delta, &c);
  check(r.status == 200 && c.text == "second try", "500 is retried, second attempt streamed");
  reap_fake_server(pid);
}

static void test_stream_never_retries_after_a_delta() {
  std::cout << "-- http_post_json_stream: no retry once output was shown\n";
  std::vector<FakeResponse> resps(2);
  // First attempt: one delta, then RST after a pause so the client has certainly
  // processed it. The second response must never be requested.
  resps[0].bytes = respond(200, "OK", "Content-Type: text/event-stream\r\n",
                           "data: {\"choices\":[{\"delta\":{\"content\":\"one\"}}]}\n\n");
  resps[0].reset = true;
  resps[0].delay_before_close_ms = 200;
  resps[1].bytes = respond(200, "OK", "Content-Type: text/event-stream\r\n",
                           "data: {\"choices\":[{\"delta\":{\"content\":\"SECOND\"}}]}\n\n"
                           "data: [DONE]\n\n");
  int port = 0;
  int pid = start_fake_server(resps, &port);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
  DeltaCollector c;
  RetryPolicy p;
  p.base_delay_ms = 10;
  HttpResult r = http_post_json_stream(url, "{}", "", 3, p, false, collect_delta, &c);
  check(c.text == "one", "only the first attempt's delta was delivered");
  check(c.text.find("SECOND") == std::string::npos, "the second response was never requested");
  check(!r.error.empty(), "the mid-stream failure is reported");
  reap_fake_server(pid);
}

int main() {
  test_parser_single_event();
  test_parser_split_everywhere();
  test_parser_two_events_one_chunk();
  test_parser_crlf_and_noise();
  test_parser_multiline_data();
  test_parser_flush_and_reset();
  test_delta_content();
  test_assemble_whole_body();
  test_unicode_escapes_in_a_delta();
  test_plain_json_request_round_trip();
  test_stream_error_status_is_not_an_answer();
  test_stream_round_trip();
  test_stream_without_done();
  test_stream_server_ignores_stream_flag();
  test_stream_retries_before_any_delta();
  test_stream_never_retries_after_a_delta();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  if (g_failures) {
    std::cout << g_failures << " FAILED\n";
    return 1;
  }
  std::cout << "all good\n";
  return 0;
}
