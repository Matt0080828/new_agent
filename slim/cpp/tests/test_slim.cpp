// Host-side tests for the slim agent core (util + policy). No test framework:
// plain assertions, exit code 1 on any failure, so it works on a bare toolchain.
//
// Build+run:  make test        (from slim/cpp)
#include "../http.hpp"
#include "../policy.hpp"
#include "../util.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
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

static std::string tmpdir() {
  char tmpl[] = "/tmp/slim-test-XXXXXX";
  char* p = mkdtemp(tmpl);
  if (!p)
    return "";
  return std::string(p);
}

static void touch(const std::string& path, const std::string& body = "body") {
  std::ofstream f(path.c_str(), std::ios::binary);
  f << body;
}

static void test_suffix_helper() {
  std::cout << "-- has_suffix\n";
  check(has_suffix("a.md", ".md"), "a.md ends with .md");
  check(!has_suffix("md", ".md"), "md (too short) does not end with .md");
  check(!has_suffix("abc", ".md"), "abc does not end with .md");
  check(has_suffix(".md", ".md"), ".md ends with .md");
  check(has_suffix("note.txt", ".txt"), "note.txt ends with .txt");
  check(!has_suffix("note.tx", ".txt"), "note.tx does not end with .txt");
  check(!has_suffix("", ".md"), "empty string matches nothing");
  check(has_suffix("x.md", ""), "empty suffix always matches");
}

// Regression: walk() used name.substr(name.size() - 4) behind a size() >= 3
// guard, so any 3-character file name threw std::out_of_range and killed the
// process (exit 134) on ingest and on every RAG search.
static void test_list_text_files_short_names() {
  std::cout << "-- list_text_files with short names (regression)\n";
  std::string dir = tmpdir();
  if (dir.empty()) {
    check(false, "mkdtemp");
    return;
  }
  touch(dir + "/abc");    // 3 chars: exploded before the fix
  touch(dir + "/ab");     // 2 chars
  touch(dir + "/a");      // 1 char
  touch(dir + "/.md");    // exactly the suffix
  touch(dir + "/x.md");   // should be indexed
  touch(dir + "/y.txt");  // should be indexed
  touch(dir + "/z.json"); // not indexed
  mkdir((dir + "/sub").c_str(), 0755);
  touch(dir + "/sub/n.md");  // nested, should be indexed

  std::vector<std::pair<std::string, std::string> > files;
  list_text_files(dir, "data", files);  // must not throw / abort

  check(files.size() == 3, "indexes exactly 3 files (x.md, y.txt, sub/n.md)");
  bool have_md = false, have_txt = false, have_nested = false;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].first == "data/x.md") have_md = true;
    if (files[i].first == "data/y.txt") have_txt = true;
    if (files[i].first == "data/sub/n.md") have_nested = true;
  }
  check(have_md, "x.md indexed with prefix");
  check(have_txt, "y.txt indexed with prefix");
  check(have_nested, "nested sub/n.md indexed");
}

static void test_jail_path() {
  std::cout << "-- jail_path\n";
  std::string full, err;
  check(jail_path("/data", "notes.md", full, err), "relative path accepted");
  check(full == "/data/notes.md", "absolute result joins under root");
  check(!jail_path("/data", "/etc/passwd", full, err), "absolute path rejected");
  check(!jail_path("/data", "../x", full, err), ".. rejected");
  check(!jail_path("/data", "a/../../x", full, err), "nested .. rejected");
  check(!jail_path("/data", "", full, err), "empty path rejected");
  check(jail_path("/data", "sub/dir/x.md", full, err), "nested relative path accepted");
}

static void test_protected_path() {
  std::cout << "-- protected_path\n";
  std::string why;
  check(protected_path("slim.sqlite", why), "slim.sqlite protected");
  check(protected_path("state.db", why), "state.db protected");
  check(protected_path("sub/x.sqlite-wal", why), "sqlite WAL protected");
  check(protected_path("x.db-journal", why), "sqlite journal protected");
  check(protected_path(".hidden", why), "dotfiles protected");
  check(protected_path("slim-agent", why), "agent binary name protected");
  // The session store is append-only agent state and lives inside the write jail.
  check(protected_path("sessions/default.jsonl", why), "session file protected");
  check(protected_path("sessions", why), "session directory protected");
  check(protected_path("sub/sessions/x.jsonl", why), "a nested sessions path is protected");
  check(!protected_path("notes.md", why), "notes.md allowed");
  check(!protected_path("sub/dir/notes.txt", why), "nested .txt allowed");
  check(!protected_path("data.bin", why), "data.bin allowed");
  check(!protected_path("my-sessions.md", why), "a name containing 'sessions' is not a session dir");
}

static void test_approval() {
  std::cout << "-- approve_mutation\n";
  std::string why;

  Policy closed;  // defaults: no allow flags, no terminal
  check(!approve_mutation(closed, "write_file", "notes.md", false, why),
        "fail-closed: model write denied by default");
  check(why.find("blocked by policy") != std::string::npos, "denial explains itself");
  check(!approve_mutation(closed, "mqtt_publish", "a/b", false, why),
        "fail-closed: model publish denied by default");
  check(approve_mutation(closed, "write_file", "notes.md", true, why),
        "human-initiated write allowed");

  Policy write_only;
  write_only.allow_write = true;
  check(approve_mutation(write_only, "write_file", "notes.md", false, why), "allow_write permits writes");
  check(!approve_mutation(write_only, "mqtt_publish", "a/b", false, why), "allow_write does not permit publishes");

  Policy mqtt_only;
  mqtt_only.allow_mqtt = true;
  check(approve_mutation(mqtt_only, "mqtt_publish", "a/b", false, why), "allow_mqtt permits publishes");
  check(!approve_mutation(mqtt_only, "write_file", "notes.md", false, why), "allow_mqtt does not permit writes");

  // run_command: the allowlist is the gate for everyone; --allow-exec only decides whether
  // the model (rather than the operator) may run an allowlisted name.
  Policy exec_listed;
  exec_listed.exec_allow.push_back("uptime");
  check(!approve_mutation(exec_listed, "run_command", "uptime", false, why),
        "an allowlisted command still needs --allow-exec for the model");
  Policy exec_open = exec_listed;
  exec_open.allow_exec = true;
  check(approve_mutation(exec_open, "run_command", "uptime", false, why),
        "--allow-exec permits an allowlisted command");
  check(approve_mutation(exec_open, "run_command", "uptime", true, why),
        "the operator may run an allowlisted command");
  check(!exec_allowed(exec_listed, "uptime"), "exec_allowed needs the opt-in");
  check(exec_allowed(exec_open, "uptime"), "exec_allowed accepts an allowlisted name");
  check(!exec_allowed(exec_open, "reboot"), "exec_allowed refuses a name outside the list");
  check(is_mutating_tool("run_command"), "run_command is classified as mutating");

  check(is_mutating_tool("write_file") && is_mutating_tool("mqtt_publish"), "mutating tools recognised");
  check(!is_mutating_tool("read_file") && !is_mutating_tool("rag_search"), "read-only tools not mutating");
}

// --- retry policy: deterministic (fake attempt function + recording sleeper) ---

struct RetryCtx {
  int* statuses;  // sequence of statuses handed out, last one repeats
  int count;
  int calls;
};

static bool fake_attempt(void* ctx, HttpResult* out) {
  RetryCtx* c = static_cast<RetryCtx*>(ctx);
  int idx = c->calls < c->count ? c->calls : c->count - 1;
  out->status = c->statuses[idx];
  out->error = (out->status >= 200 && out->status < 300) ? "" : "synthetic";
  ++c->calls;
  return out->status >= 200 && out->status < 300;
}

struct SleepLog {
  int total_ms;
  int calls;
  int last_ms;
};

// run_with_retry takes a plain function pointer, so the recorder is reached
// through one static slot; the tests are single threaded.
static SleepLog*& sleep_log_slot();

static void recording_sleeper(int ms) {
  SleepLog* log = sleep_log_slot();
  if (!log)
    return;
  log->total_ms += ms;
  log->last_ms = ms;
  ++log->calls;
}

static SleepLog*& sleep_log_slot() {
  static SleepLog* slot = 0;
  return slot;
}

static void test_retry() {
  std::cout << "-- retry policy\n";
  check(is_retryable_status(0), "transport error is retryable");
  check(is_retryable_status(429), "429 is retryable");
  check(is_retryable_status(500) && is_retryable_status(503), "5xx is retryable");
  check(!is_retryable_status(400) && !is_retryable_status(404), "4xx is not retryable");
  check(!is_retryable_status(-1), "negative status (permanent error) is not retryable");
  check(!is_retryable_status(200), "success is not retryable");

  RetryPolicy p;
  check(p.attempts == 3 && p.base_delay_ms == 500 && p.max_delay_ms == 4000, "default policy");
  check(retry_delay_ms(1, p) == 500, "first backoff is the base");
  check(retry_delay_ms(2, p) == 1000, "backoff doubles");
  check(retry_delay_ms(3, p) == 2000, "backoff doubles again");
  check(retry_delay_ms(9, p) == 4000, "backoff is capped");

  {
    int statuses[] = {500, 500, 200};
    RetryCtx ctx = {statuses, 3, 0};
    HttpResult out;
    SleepLog sleep = {0, 0, 0};
    sleep_log_slot() = &sleep;
    bool delivered = run_with_retry(p, fake_attempt, &ctx, &out, recording_sleeper, 0);
    sleep_log_slot() = 0;
    check(delivered && out.status == 200, "recovers after two 5xx");
    check(ctx.calls == 3, "exactly three attempts");
    check(sleep.calls == 2 && sleep.total_ms == 1500, "backoff 500 then 1000");
  }

  {
    int statuses[] = {400};
    RetryCtx ctx = {statuses, 1, 0};
    HttpResult out;
    SleepLog sleep = {0, 0, 0};
    sleep_log_slot() = &sleep;
    bool delivered = run_with_retry(p, fake_attempt, &ctx, &out, recording_sleeper, 0);
    sleep_log_slot() = 0;
    check(delivered && out.status == 400, "4xx is returned as-is");
    check(ctx.calls == 1 && sleep.calls == 0, "no retry and no sleep for 4xx");
  }

  {
    int statuses[] = {503};
    RetryCtx ctx = {statuses, 1, 0};
    HttpResult out;
    SleepLog sleep = {0, 0, 0};
    sleep_log_slot() = &sleep;
    bool delivered = run_with_retry(p, fake_attempt, &ctx, &out, recording_sleeper, 0);
    sleep_log_slot() = 0;
    check(!delivered && out.status == 503, "exhausted retries surface the failure");
    check(ctx.calls == 3 && sleep.calls == 2, "attempts and sleeps are bounded");
  }

  {
    RetryPolicy once;
    once.attempts = 1;
    int statuses[] = {500};
    RetryCtx ctx = {statuses, 1, 0};
    HttpResult out;
    SleepLog sleep = {0, 0, 0};
    sleep_log_slot() = &sleep;
    run_with_retry(once, fake_attempt, &ctx, &out, recording_sleeper, 0);
    sleep_log_slot() = 0;
    check(ctx.calls == 1 && sleep.calls == 0, "attempts=1 means a single try");
  }

  {
    int statuses[] = {502, 200};
    RetryCtx ctx = {statuses, 2, 0};
    HttpResult out;
    std::string log;
    run_with_retry(p, fake_attempt, &ctx, &out, recording_sleeper, &log);
    check(log.find("attempt 1/3 status=502") != std::string::npos, "log records the first attempt");
    check(log.find("attempt 2/3 status=200") != std::string::npos, "log records the recovery");
  }
}

static void test_cap_prompt() {
  std::cout << "-- cap_prompt\n";
  check(cap_prompt("hello", 6000) == "hello", "short prompt untouched");
  check(cap_prompt("", 10).empty(), "empty prompt untouched");
  std::string big(100, 'x');
  std::string capped = cap_prompt(big, 10);
  check(capped.size() == 10 + std::string("\n[truncated]").size(), "long prompt truncated to the cap");
  check(capped.compare(capped.size() - 12, 12, "[truncated]") == 0 ||
            capped.find("[truncated]") != std::string::npos,
        "truncation is marked");
  check(capped.compare(0, 10, big.substr(0, 10)) == 0, "prefix preserved");
}

static void test_json_helpers() {
  std::cout << "-- json helpers\n";
  std::string v;
  check(json_extract_string("{\"tool\":\"rag_search\",\"query\":\"cpe\"}", "query", v) && v == "cpe",
        "json_extract_string reads a field");
  check(!json_extract_string("{\"tool\":\"x\"}", "content", v), "missing field returns false");
  check(json_escape("a\"b\n") == "a\\\"b\\n", "json_escape escapes quote and newline");
  std::string obj;
  check(json_extract_object("prefix {\"tool\":\"read_file\"} suffix", obj) && obj == "{\"tool\":\"read_file\"}",
        "json_extract_object trims to first { and last }");
  check(join_url("http://host:1234", "/chat/completions") == "http://host:1234/v1/chat/completions",
        "join_url appends /v1");
  check(join_url("http://host:1234/v1/", "/chat/completions") == "http://host:1234/v1/chat/completions",
        "join_url keeps existing /v1");
}

static void test_format_skills() {
  std::cout << "-- format_skills (top-level .md, sorted, bounded)\n";
  std::string dir = tmpdir();
  if (dir.empty()) {
    check(false, "mkdtemp");
    return;
  }
  touch(dir + "/b.md", "second skill");
  touch(dir + "/a.md", "first skill");
  touch(dir + "/notes.txt", "not a skill");
  mkdir((dir + "/sub").c_str(), 0755);
  touch(dir + "/sub/c.md", "nested, not a skill");

  std::string s = format_skills(dir, 8, 1200);
  check(s.find("Available skills (markdown only, do not execute):") == 0,
        "the header says skills are guidance, not code");
  check(s.find("### a.md") < s.find("### b.md"), "skills are sorted by name");
  check(s.find("first skill") != std::string::npos, "and the body is included");
  check(s.find("notes.txt") == std::string::npos, ".md only: a .txt file is not a skill");
  check(s.find("sub/c.md") == std::string::npos, "top-level only: subdirectories are not scanned");
  check(format_skills(tmpdir(), 8, 1200) == "", "an empty directory yields no skills block");

  // Limits: the limit and the per-file cap both bind.
  std::string dir2 = tmpdir();
  touch(dir2 + "/1.md", "one");
  touch(dir2 + "/2.md", "two");
  std::string limited = format_skills(dir2, 1, 1200);
  check(limited.find("### 1.md") != std::string::npos && limited.find("### 2.md") == std::string::npos,
        "the file limit is applied (first by name wins)");
  touch(dir2 + "/big.md", std::string(3000, 'x'));
  std::string dir3 = tmpdir();
  touch(dir3 + "/big.md", std::string(3000, 'x'));
  std::string capped = format_skills(dir3, 8, 100);
  size_t body_at = capped.find("### big.md\n");
  size_t x_count = 0;
  for (size_t i = body_at + 11; i < capped.size(); ++i)
    if (capped[i] == 'x')
      ++x_count;
  check(x_count == 100, "the body is cut to max_chars");
}

int main() {
  test_suffix_helper();
  test_list_text_files_short_names();
  test_format_skills();
  test_jail_path();
  test_protected_path();
  test_approval();
  test_cap_prompt();
  test_retry();
  test_json_helpers();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  if (g_failures) {
    std::cout << g_failures << " FAILED\n";
    return 1;
  }
  std::cout << "all good\n";
  return 0;
}
