// Host-side tests for the slim agent core (util + policy). No test framework:
// plain assertions, exit code 1 on any failure, so it works on a bare toolchain.
//
// Build+run:  make test        (from slim/cpp)
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
  check(!protected_path("notes.md", why), "notes.md allowed");
  check(!protected_path("sub/dir/notes.txt", why), "nested .txt allowed");
  check(!protected_path("data.bin", why), "data.bin allowed");
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

  check(is_mutating_tool("write_file") && is_mutating_tool("mqtt_publish"), "mutating tools recognised");
  check(!is_mutating_tool("read_file") && !is_mutating_tool("rag_search"), "read-only tools not mutating");
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

int main() {
  test_suffix_helper();
  test_list_text_files_short_names();
  test_jail_path();
  test_protected_path();
  test_approval();
  test_json_helpers();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  if (g_failures) {
    std::cout << g_failures << " FAILED\n";
    return 1;
  }
  std::cout << "all good\n";
  return 0;
}
