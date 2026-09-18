// Tests for the session store (slim/cpp/session.cpp).
//
// The format is shared with the Python client, so the exact line shape and the
// tolerant reader are both pinned here.
#include "../session.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
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

static std::string g_dir;

static bool append_ok(const std::string& path, const std::string& role, const std::string& text) {
  std::string why;
  return session_append(path, role, text, why);
}

static void append_raw(const std::string& path, const std::string& bytes) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd >= 0) {
    ssize_t n = write(fd, bytes.data(), bytes.size());
    (void)n;
    close(fd);
  }
}

static std::string read_raw(const std::string& path) {
  std::string out;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return out;
  char buf[512];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    out.append(buf, (size_t)n);
  close(fd);
  return out;
}

static void test_names() {
  std::cout << "-- session name validation\n";
  const char* good[] = {"default", "2026-09-18", "a.b_c-1"};
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
    std::string why;
    check(valid_session_name(good[i], why), std::string("accepts ") + good[i]);
  }
  std::string long_name(65, 'a');
  const char* bad[] = {"", ".hidden", "a/b", "..", "../etc/passwd", "a b", "sessions/x", "a\\b"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    std::string why;
    check(!valid_session_name(bad[i], why) && !why.empty(), std::string("rejects ") + bad[i]);
  }
  std::string why;
  check(!valid_session_name(long_name, why), "rejects a 65-character name");
}

static void test_paths() {
  std::cout << "-- session path\n";
  check(session_path("/data", "default") == "/data/sessions/default.jsonl", "join");
  check(session_path("/data/", "default") == "/data/sessions/default.jsonl",
        "trailing slash is not doubled");
}

static void test_round_trip() {
  std::cout << "-- append and load\n";
  std::string path = session_path(g_dir, "roundtrip");
  check(append_ok(path, "user", "hello"), "first append");
  check(append_ok(path, "assistant", "ok"), "second append");
  std::vector<Turn> turns = session_load(path, -1);
  check(turns.size() == 2, "two turns loaded");
  if (turns.size() == 2) {
    check(turns[0].role == "user" && turns[0].text == "hello", "oldest first");
    check(turns[1].role == "assistant" && turns[1].text == "ok", "then the newest");
    check(turns[0].ts.size() >= 20, "a timestamp was written");
  }
  // The directory is created by the writer.
  check(append_ok(session_path(g_dir, "nested/deep"), "user", "x"), "new dir");
}

static void test_awkward_text() {
  std::cout << "-- escaping round trip\n";
  std::string path = session_path(g_dir, "escape");
  std::string text = "multi\nline \"quoted\" back\\slash\ttab";
  append_ok(path, "user", text);
  std::vector<Turn> turns = session_load(path, -1);
  check(turns.size() == 1 && turns[0].text == text, "newlines/quotes/backslashes survive");

  // A hand-edited line: an unknown escape keeps the character itself, and a \u escape
  // is decoded as UTF-8. Written next to the temp dir because this helper does not
  // create directories (session_append does).
  std::string manual = g_dir + "/manual.jsonl";
  append_raw(manual, "{\"role\":\"user\",\"text\":\"a\\qb\"}\n");
  append_raw(manual, "{\"role\":\"user\",\"text\":\"a\\u00e9b\"}\n");
  std::vector<Turn> manual_turns = session_load(manual, -1);
  check(manual_turns.size() == 2, "hand-edited lines still parse");
  if (manual_turns.size() == 2) {
    check(manual_turns[0].text == "aqb", "an unknown escape keeps the character");
    check(manual_turns[1].text == "a\xc3\xa9" "b", "\\uXXXX decodes to UTF-8, not to 'u00e9'");
  }
}

static void test_limits_and_missing() {
  std::cout << "-- limits and a missing file\n";
  std::string path = session_path(g_dir, "limits");
  for (int i = 0; i < 5; ++i) {
    std::string why;
    session_append(path, "user", std::string("turn ") + (char)('0' + i), why);
  }
  std::vector<Turn> two = session_load(path, 2);
  check(two.size() == 2 && two[0].text == "turn 3" && two[1].text == "turn 4",
        "limit keeps the newest, oldest first");
  std::vector<Turn> missing = session_load(session_path(g_dir, "never-used"), 4);
  check(missing.empty(), "a missing file is an empty session");
  check(session_load(g_dir, 4).empty(), "a directory is an empty session, not a crash");
}

static void test_tolerant_reader() {
  std::cout << "-- broken lines are skipped\n";
  std::string path = session_path(g_dir, "broken");
  std::string why;
  session_append(path, "assistant", "good one", why);
  append_raw(path, "{\"ts\":\"x\",\"role\":\"user\"\n");            // truncated
  append_raw(path, "not json at all\n");                           // garbage
  append_raw(path, "{\"role\":1,\"text\":\"wrong types\"}\n");     // wrong types
  append_raw(path, "[\"a\",\"list\"]\n");                          // not an object
  append_raw(path, "\n");                                          // empty line
  session_append(path, "user", "good two", why);
  std::vector<Turn> turns = session_load(path, -1);
  check(turns.size() == 2, "only the two readable turns survive");
  if (turns.size() == 2)
    check(turns[1].text == "good two", "the turn after the broken lines is still read");

  std::string big = session_path(g_dir, "big");
  session_append(big, "assistant", "short", why);
  append_raw(big, "{\"ts\":\"x\",\"role\":\"user\",\"text\":\"" + std::string(20000, 'a') + "\"}\n");
  std::vector<Turn> bigturns = session_load(big, -1);
  check(bigturns.size() == 1 && bigturns[0].text == "short", "an oversized line is skipped");
  std::vector<Turn> every = session_load(path, -1);
  check(every.size() == 2, "a negative limit loads every readable turn");
  std::vector<Turn> no_turns = session_load(path, 0);
  check(no_turns.empty(), "limit 0 replays nothing");
}

static void test_line_shape_and_permissions() {
  std::cout << "-- on-disk shape\n";
  std::string path = session_path(g_dir, "shape");
  std::string why;
  session_append(path, "user", "hi", why);
  std::string raw = read_raw(path);
  check(raw.substr(0, 1) == "{", "one JSON object per line");
  check(raw.find("\"role\":\"user\"") != std::string::npos, "compact JSON (no space after ':')");
  check(raw.find("\"text\":\"hi\"") != std::string::npos, "the text field is on the same line");
  check(raw[raw.size() - 1] == '\n', "every turn ends with a newline");
  check(raw.find('\n') == raw.size() - 1, "exactly one line per turn");
  struct stat st;
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "mode 0600");
}

int main() {
  char tmpl[] = "/tmp/slim-session-test-XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (!dir) {
    std::cout << "cannot create a temp directory\n";
    return 1;
  }
  g_dir = dir;
  test_names();
  test_paths();
  test_round_trip();
  test_awkward_text();
  test_limits_and_missing();
  test_tolerant_reader();
  test_line_shape_and_permissions();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  if (g_failures) {
    std::cout << g_failures << " FAILED\n";
    return 1;
  }
  std::cout << "all good\n";
  return 0;
}
