// Tests for the tool layer (slim/cpp/tools.cpp): gating, limits, the output budget and
// the change queue. This is the layer the model reaches, so it is tested directly rather
// than only through the agent's CLI.
#include "../policy.hpp"
#include "../tools.hpp"
#include "../util.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static std::string g_dir;

static std::string read_file(const std::string& path) {
  std::string out;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return out;
  char buf[1024];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    out.append(buf, (size_t)n);
  close(fd);
  return out;
}

static bool exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string stub_rag(void*, const std::string& q) {
  return "[{\"path\":\"stub.md\",\"snippet\":\"answer for " + q + "\"}]";
}

static ToolEnv base_env() {
  ToolEnv env;
  env.data_dir = g_dir;
  env.rag_fn = &stub_rag;
  env.rag_ctx = 0;
  return env;
}

static std::string json_str(const std::string& k, const std::string& v) {
  return "{\"" + k + "\":\"" + v + "\"}";
}

// ---------------------------------------------------------------------------
// Unknown tools and read-only tools
// ---------------------------------------------------------------------------

static void test_unknown_tool_is_refused() {
  std::cout << "-- unknown tool\n";
  ToolEnv env = base_env();
  std::string r = run_tool(env, "shell_exec", "{}", false, 0, 0);
  check(r.find("unknown tool") != std::string::npos, "an unclassified tool is refused");
  check(r.find("tool error") == 0, "the refusal is reported as a tool error");
}

static void test_rag_uses_the_injected_index() {
  std::cout << "-- rag_search\n";
  ToolEnv env = base_env();
  std::string r = run_tool(env, "rag_search", json_str("query", "temp"), true, 0, 0);
  check(r.find("answer for temp") != std::string::npos, "the injected index is used");
  ToolEnv bare;
  bare.data_dir = g_dir;
  check(run_tool(bare, "rag_search", "{}", true, 0, 0).find("not available") != std::string::npos,
        "without an index the tool says so");
}

static void test_read_file_jail() {
  std::cout << "-- read_file\n";
  ToolEnv env = base_env();
  check(!exists(g_dir + "/notes.md"), "precondition: the file does not exist yet");
  check(run_tool(env, "read_file", json_str("path", "notes.md"), true, 0, 0).find("empty or missing") !=
            std::string::npos,
        "a missing file is reported, not a crash");
  check(run_tool(env, "read_file", json_str("path", "../../etc/passwd"), true, 0, 0)
                .find("tool error") == 0,
        "a path outside the jail is refused");
  std::string content(20000, 'x');
  std::string werr;
  check(write_file_limited(g_dir + "/big.txt", content, 64 * 1024, werr), "fixture written");
  std::string got = run_tool(env, "read_file", json_str("path", "big.txt"), true, 0, 0);
  check(got.size() <= 16 * 1024, "a large file is truncated to 16 KiB");
}

// ---------------------------------------------------------------------------
// write_file: policy, then the queue
// ---------------------------------------------------------------------------

static void test_write_file_gating() {
  std::cout << "-- write_file gating\n";
  ToolEnv env = base_env();
  std::string r = run_tool(env, "write_file", "{\"path\":\"a.md\",\"content\":\"hi\"}", false, 0, 0);
  check(r.find("not enabled") != std::string::npos, "model write denied without an opt-in");
  env.policy.allow_write = true;
  r = run_tool(env, "write_file", "{\"path\":\"slim.sqlite\",\"content\":\"x\"}", false, 0, 0);
  check(r.find("agent state/database file") != std::string::npos,
        "the database is refused even with --allow-write");
  r = run_tool(env, "write_file", "{\"path\":\"sessions/s.jsonl\",\"content\":\"x\"}", false, 0, 0);
  check(r.find("append-only agent state") != std::string::npos,
        "the session store is refused even with --allow-write");
  r = run_tool(env, "write_file", json_str("path", "../escape.md"), false, 0, 0);
  check(r.find("tool error") == 0, "a path outside the jail is refused");
}

static void test_change_queue() {
  std::cout << "-- change queue\n";
  ToolEnv env = base_env();
  env.policy.allow_write = true;
  ChangeQueue q;
  std::string r = run_tool(env, "write_file", "{\"path\":\"q1.md\",\"content\":\"one\"}", false, 0, &q);
  check(r.find("staged") == 0, "an allowed write is staged, not written");
  check(!exists(g_dir + "/q1.md"), "nothing is on disk while it is only staged");
  check(q.entries.size() == 1 && q.summary().find("1 file(s), 3 bytes") == 0, "summary counts the queue");

  run_tool(env, "write_file", "{\"path\":\"q1.md\",\"content\":\"two\"}", false, 0, &q);
  check(q.entries.size() == 1, "the same path is not queued twice");
  check(q.replaced == 1, "overwriting a staged change is counted");

  run_tool(env, "write_file", "{\"path\":\"q2.md\",\"content\":\"second\"}", false, 0, &q);
  std::vector<std::string> failures;
  std::vector<std::string> wrote = q.apply(failures);
  check(failures.empty() && wrote.size() == 2, "both files apply");
  check(read_file(g_dir + "/q2.md") == "second", "the second file landed");
  check(read_file(g_dir + "/q1.md") == "two", "the last staged content wins");
  check(wrote[0].find("wrote q1.md (3 bytes)") != std::string::npos, "the manifest names the size");
}

static void test_change_queue_caps() {
  std::cout << "-- change queue caps\n";
  ChangeQueue q;
  std::string why;
  std::string big(kMaxWrite + 1, 'x');
  check(!q.stage("too-big.md", g_dir + "/too-big.md", big, why), "a file over the per-file cap is refused");
  check(why.find("larger than") != std::string::npos, "and says why");

  for (size_t i = 0; i < kMaxQueuedFiles; ++i) {
    std::string name = "f" + std::string(1, (char)('a' + (i % 26))) + std::string(1, (char)('a' + (i / 26)));
    if (!q.stage(name + ".md", g_dir + "/" + name + ".md", "x", why)) {
      check(false, "staging within the file cap should succeed: " + why);
      break;
    }
  }
  check(q.entries.size() == kMaxQueuedFiles, "the queue filled to its file cap");
  check(!q.stage("one-more.md", g_dir + "/one-more.md", "x", why), "one more file is refused");
  check(why.find("already holds") != std::string::npos, "and says how many");

  ChangeQueue bytes;
  std::string chunk(kMaxWrite, 'y');
  size_t staged = 0;
  while (bytes.stage("b" + std::string(1, (char)('a' + staged)) + ".md",
                     g_dir + "/b" + std::string(1, (char)('a' + staged)) + ".md", chunk, why) &&
         staged < 20) {
    ++staged;
  }
  check(bytes.entries.size() * kMaxWrite <= kMaxQueuedBytes, "the byte cap holds");
  check(!bytes.stage("final.md", g_dir + "/final.md", chunk, why) || bytes.entries.size() * kMaxWrite <= kMaxQueuedBytes,
        "the queue never exceeds its byte budget");
}

// ---------------------------------------------------------------------------
// Tool output budget
// ---------------------------------------------------------------------------

static void test_tool_budget() {
  std::cout << "-- tool output budget\n";
  ToolBudget b(100);
  check(b.add("short") == "short", "text under the budget passes through");
  check(b.used == 5, "and is counted");
  std::string big(200, 'x');
  std::string trimmed = b.add(big);
  check(trimmed.size() < big.size(), "text over what is left is trimmed");
  check(trimmed.find("[truncated") != std::string::npos, "with an explicit marker");
  check(b.used == 100 && b.elided == 105, "used and elided bytes are tracked");
  std::string after = b.add("more");
  check(after.find("omitted") != std::string::npos, "once exhausted, later results are elisions");
  check(b.summary().find("100/100 bytes") != std::string::npos, "the summary reports the budget");
  check(b.summary().find("elided") != std::string::npos, "and the elided bytes");
  b.reset();
  check(b.used == 0 && b.add("again") == "again", "reset makes it usable again");
}

// ---------------------------------------------------------------------------
// mqtt_publish: disabled, denied, and sent exactly once
// ---------------------------------------------------------------------------

static pid_t start_counting_server(const std::string& response, const std::string& count_path,
                                   int* port_out) {
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
  if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(lsock, 8) != 0) {
    close(lsock);
    return -1;
  }
  socklen_t alen = sizeof(addr);
  getsockname(lsock, (struct sockaddr*)&addr, &alen);
  *port_out = ntohs(addr.sin_port);

  pid_t pid = fork();
  if (pid == 0) {
    for (int i = 0; i < 6; ++i) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(lsock, &set);
      struct timeval tv;
      tv.tv_sec = 2;
      tv.tv_usec = 0;
      if (select(lsock + 1, &set, 0, 0, &tv) <= 0)
        break;  // no further request: stop waiting
      int c = accept(lsock, 0, 0);
      if (c < 0)
        break;
      int cf = open(count_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
      if (cf >= 0) {
        ssize_t n = write(cf, "1", 1);
        (void)n;
        close(cf);
      }
      std::string req;
      char buf[1024];
      size_t body_len = 0;
      std::string::size_type hend = std::string::npos;
      for (;;) {
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0)
          break;
        req.append(buf, (size_t)n);
        if (hend == std::string::npos) {
          std::string::size_type p = req.find("\r\n\r\n");
          if (p != std::string::npos) {
            hend = p + 4;
            std::string::size_type cl = req.find("Content-Length:");
            if (cl != std::string::npos && cl < hend)
              body_len = (size_t)atoi(req.c_str() + cl + 15);
          }
        }
        if (hend != std::string::npos && req.size() >= hend + body_len)
          break;
      }
      size_t written = 0;
      while (written < response.size()) {
        ssize_t n = send(c, response.data() + written, response.size() - written, 0);
        if (n <= 0)
          break;
        written += (size_t)n;
      }
      close(c);
    }
    close(lsock);
    _exit(0);
  }
  close(lsock);
  return pid;
}

static void reap(pid_t pid) {
  for (int i = 0; i < 80; ++i) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid)
      return;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000;
    nanosleep(&ts, 0);
  }
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
}

static void test_mqtt() {
  std::cout << "-- mqtt_publish\n";
  ToolEnv env = base_env();
  check(run_tool(env, "mqtt_publish", "{\"topic\":\"a/b\",\"payload\":\"x\"}", false, 0, 0)
            .find("mqtt disabled") != std::string::npos,
        "disabled without SLIM_MQTT_HTTP");

  std::string count_path = g_dir + "/mqtt-count";
  int port = 0;
  std::string response = "HTTP/1.0 500 Server Error\r\nContent-Length: 4\r\n\r\nbusy";
  pid_t pid = start_counting_server(response, count_path, &port);
  check(pid > 0 && port > 0, "counting server started");
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/mqtt", port);
  env.mqtt_http = url;

  std::string denied = run_tool(env, "mqtt_publish", "{\"topic\":\"a/b\",\"payload\":\"x\"}", false, 0, 0);
  check(denied.find("not enabled") != std::string::npos, "denied without an opt-in");

  env.policy.allow_mqtt = true;
  std::string sent = run_tool(env, "mqtt_publish", "{\"topic\":\"a/b\",\"payload\":\"x\"}", false, 0, 0);
  check(sent.find("mqtt http") == 0, "an allowed publish reports the server answer");
  reap(pid);

  std::string counted = read_file(count_path);
  check(counted.size() == 1, "the server saw exactly one request, even though it answered 500");
  if (counted.size() != 1)
    std::cout << "      (server saw " << counted.size() << " requests)\n";
}

static void test_run_command() {
  std::cout << "-- run_command: allowlist, no shell\n";
  ToolEnv env;
  env.data_dir = g_dir;
  env.exec_path = "/bin:/usr/bin:/sbin:/usr/sbin";

  // No allowlist at all: nothing may run, whatever the flags say.
  std::string r = run_tool(env, "run_command", json_str("command", "echo"), true, 0, 0);
  check(r.find("allowlist") != std::string::npos, "with no allowlist nothing runs");

  env.policy.exec_allow.push_back("echo");
  // A path is refused even when the name is on the list.
  r = run_tool(env, "run_command", json_str("command", "/bin/echo"), true, 0, 0);
  check(r.find("bare command name") != std::string::npos, "a path is refused");

  // Allowlisted: it runs, and the status comes back.
  r = run_tool(env, "run_command", json_str("command", "echo"), true, 0, 0);
  check(r.find("exit 0") == 0, "an allowlisted command runs and reports its status");

  // Arguments reach the program verbatim: there is no shell, so ; | $() and backticks are
  // ordinary characters rather than syntax.
  std::string args = "a; echo b `id` $(whoami) | cat";
  std::string js = "{\"command\":\"echo\",\"args\":\"" + args + "\"}";
  r = run_tool(env, "run_command", js, true, 0, 0);
  check(r.find(args) != std::string::npos, "arguments arrive verbatim (no shell to reinterpret)");
  check(r.find("uid=") == std::string::npos, "the backtick did not run id");

  // Model-initiated execution needs its own opt-in on top of the allowlist.
  r = run_tool(env, "run_command", json_str("command", "echo"), false, 0, 0);
  check(r.find("not enabled") != std::string::npos, "the model needs --allow-exec");
  env.policy.allow_exec = true;
  r = run_tool(env, "run_command", json_str("command", "echo"), false, 0, 0);
  check(r.find("exit 0") == 0, "with --allow-exec plus the allowlist the model may run it");

  // Allowlisted but not installed: a clean error, not a crash.
  env.policy.exec_allow.push_back("definitely-not-a-command");
  r = run_tool(env, "run_command", json_str("command", "definitely-not-a-command"), true, 0, 0);
  check(r.find("command not found") != std::string::npos, "a missing command is reported");

  // The allowlist is a permission file: a tool must not be able to rewrite it.
  std::string why;
  check(protected_path("commands.allow", why), "commands.allow is refused by protected_path");
  check(why.find("permission file") != std::string::npos, "and the reason says why");
}

// --- parse_tool_call: the model's tool JSON, same shape as the Python client ---

static void test_parse_tool_call() {
  std::string name, obj;
  check(!parse_tool_call("just a plain answer, no tools", name, obj),
        "prose without braces is not a tool call");
  check(!parse_tool_call("an open brace only {", name, obj), "an unmatched brace is not a call");
  check(!parse_tool_call("{\"tool\":\"fork_bomb\"}", name, obj),
        "a tool the layer does not implement is refused");
  check(!parse_tool_call("{\"tool\":\"uptime\"}", name, obj),
        "a tool field naming a command, not a tool, is refused");
  check(!parse_tool_call("{\"query\":\"wifi\"}", name, obj), "an object without a tool field is not a call");
  check(parse_tool_call("{\"tool\":\"run_command\",\"command\":\"uptime\"}", name, obj),
        "a bare tool object is recognised");
  check(name == "run_command", "and the tool name comes back");
  check(obj == "{\"tool\":\"run_command\",\"command\":\"uptime\"}", "the object passes through intact");
  check(parse_tool_call("sure, here you go: {\"tool\":\"rag_search\",\"query\":\"wifi\"}", name, obj),
        "a tool object inside prose is recognised");
  check(name == "rag_search", "with the right name");
}

int main() {
  char tmpl[] = "/tmp/slim-tools-test-XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (!dir) {
    std::cout << "cannot create a temp directory\n";
    return 1;
  }
  g_dir = dir;
  test_unknown_tool_is_refused();
  test_rag_uses_the_injected_index();
  test_read_file_jail();
  test_write_file_gating();
  test_change_queue();
  test_change_queue_caps();
  test_tool_budget();
  test_mqtt();
  test_run_command();
  test_parse_tool_call();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  if (g_failures) {
    std::cout << g_failures << " FAILED\n";
    return 1;
  }
  std::cout << "all good\n";
  return 0;
}
