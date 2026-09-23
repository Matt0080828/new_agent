#include "http.hpp"
#include "policy.hpp"
#include "session.hpp"
#include "sse.hpp"
#include "tools.hpp"
#include "util.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static const size_t kMaxPrompt = 6000;

struct Cfg {
  std::string base_url;
  std::string fallback_url;
  std::string model;
  std::string fallback_model;
  std::string api_key;
  std::string data_dir;
  std::string skills_dir;
  std::string docs_dir;
  std::string mqtt_http;
  int max_tokens;
  int timeout;
  Policy policy;
  RetryPolicy retry;
  bool verbose_http;
  bool stream;
  std::string session;       // --session NAME / SLIM_SESSION
  std::string session_file;  // <data_dir>/sessions/<name>.jsonl
  int history;               // --history N: how many turns to replay into the prompt
  bool dry_run_writes;       // --dry-run-writes: report planned writes, write nothing
};

static void die(const std::string& m, int c = 2) {
  std::cerr << m << "\n";
  std::exit(c);
}

static std::string env_or(const char* k, const std::string& d) {
  const char* v = std::getenv(k);
  return v && v[0] ? std::string(v) : d;
}

static void print_delta(void* ctx, const std::string& delta) {
  (void)ctx;
  std::cout << delta << std::flush;  // live output: the point of streaming on a slow CPE
}

static std::string openai_chat(const Cfg& cfg, const std::string& url, const std::string& model,
                               const std::vector<std::pair<std::string, std::string> >& msgs) {
  std::ostringstream js;
  js << "{\"model\":\"" << json_escape(model) << "\",\"stream\":"
     << (cfg.stream ? "true" : "false") << ",\"temperature\":0"
     << ",\"max_tokens\":" << cfg.max_tokens
     << ",\"stop\":[\"\\nuser\",\"\\nassistant\",\"\\nmodel\",\"<start_of_turn>\",\"\\n- No\"]"
     << ",\"messages\":[";
  for (size_t i = 0; i < msgs.size(); ++i) {
    if (i)
      js << ",";
    js << "{\"role\":\"" << json_escape(msgs[i].first) << "\",\"content\":\"" << json_escape(msgs[i].second)
       << "\"}";
  }
  js << "]}";
  std::string endpoint = join_url(url, "/chat/completions");
  HttpResult r;
  if (cfg.stream) {
    // Deltas go straight to stdout as they arrive; the assembled text comes back
    // in r.body. Retry applies only before the first delta (see http.hpp).
    r = http_post_json_stream(endpoint, js.str(), cfg.api_key, cfg.timeout, cfg.retry,
                              cfg.verbose_http, print_delta, 0);
    if (!r.error.empty())
      std::cerr << "\n[slim] stream: " << r.error << "\n";
  } else {
    // Retried: a completion request has no lasting server-side effect, so repeating
    // it after a transport error or a 5xx is safe.
    r = http_post_json_retry(endpoint, js.str(), cfg.api_key, cfg.timeout, cfg.retry,
                             cfg.verbose_http);
  }
  // A request that failed is never content. Checked before touching r.body: the
  // streaming path used to fall through with the raw response in the body, which the
  // caller then printed and recorded as the model's answer.
  const bool bad_status = r.status && (r.status < 200 || r.status >= 300);
  if (!r.error.empty() || bad_status)
    throw std::runtime_error(r.error.empty() ? "http error" : r.error);
  std::string content;
  if (cfg.stream) {
    content = r.body;
    if (content.empty())
      throw std::runtime_error("empty stream");
  } else {
    // Some servers answer with an event stream even when stream was not requested;
    // a plain "first content" extraction would then return only the first delta,
    // so prefer assembling the data lines when the body carries any.
    std::string assembled;
    if (r.body.find("data:") != std::string::npos && sse_assemble(r.body, assembled))
      content = assembled;
    else if (!json_extract_string(r.body, "content", content))
      throw std::runtime_error(r.error.empty() ? "no content in response" : r.error + " " + r.body.substr(0, 200));
  }
  return content;
}

static std::string chat_fallback(const Cfg& cfg, const std::vector<std::pair<std::string, std::string> >& msgs) {
  try {
    return openai_chat(cfg, cfg.base_url, cfg.model, msgs);
  } catch (const std::exception& e) {
    if (cfg.fallback_url.empty())
      throw;
    return openai_chat(cfg, cfg.fallback_url, cfg.fallback_model.empty() ? cfg.model : cfg.fallback_model, msgs);
  }
}

// Model tool calls exist again, but a turn still runs at most one tool round, so there is
// nothing to trim between rounds; the size guard lives in cap_prompt() (util.cpp), which
// has a unit test.
struct Hit {
  std::string path;
  std::string snippet;
  int score;
};

static std::vector<Hit> rag_search(const Cfg& cfg, const std::string& q, int limit) {
  std::vector<std::pair<std::string, std::string> > files;
  list_text_files(cfg.skills_dir, "skill", files);
  list_text_files(cfg.docs_dir, "doc", files);
  list_text_files(cfg.data_dir, "data", files);
  std::vector<Hit> hits;
  if (q.empty())
    return hits;
  for (size_t i = 0; i < files.size(); ++i) {
    int score = 0;
    std::string::size_type p = 0;
    while ((p = files[i].second.find(q, p)) != std::string::npos) {
      ++score;
      p += q.size();
    }
    if (score == 0)
      continue;
    Hit h;
    h.path = files[i].first;
    h.score = score;
    std::string::size_type at = files[i].second.find(q);
    std::string::size_type a = at > 80 ? at - 80 : 0;
    h.snippet = files[i].second.substr(a, 240);
    hits.push_back(h);
  }
  for (size_t i = 0; i < hits.size(); ++i) {
    for (size_t j = i + 1; j < hits.size(); ++j) {
      if (hits[j].score > hits[i].score) {
        Hit t = hits[i];
        hits[i] = hits[j];
        hits[j] = t;
      }
    }
  }
  if ((int)hits.size() > limit)
    hits.resize((size_t)limit);
  return hits;
}

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r'))
    s.erase(s.begin());
  while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t' || s[s.size() - 1] == '\r'))
    s.resize(s.size() - 1);
  return s;
}

static bool is_role_line(const std::string& line) {
  std::string t = trim_copy(line);
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] >= 'A' && t[i] <= 'Z')
      t[i] = (char)(t[i] - 'A' + 'a');
  }
  return t == "user" || t == "assistant" || t == "system" || t == "model" || t == "who is the user?" ||
         t == "who is the assistant?";
}

static std::string sanitize_reply(std::string s) {
  std::string out;
  std::string prev;
  int same = 0;
  std::string line;
  int kept = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      std::string t = trim_copy(line);
      if (is_role_line(t)) {
        if (kept > 0)
          break;
        line.clear();
        continue;
      }
      if (t == prev && !t.empty()) {
        ++same;
        if (same >= 1)
          break;
      } else {
        same = 0;
        prev = t;
      }
      if (!out.empty())
        out.push_back('\n');
      out += t;
      ++kept;
      if (kept >= 2)
        break;
      line.clear();
    } else {
      line.push_back(s[i]);
    }
  }
  while (!out.empty() && (out[out.size() - 1] == '\n' || out[out.size() - 1] == ' '))
    out.resize(out.size() - 1);
  return out;
}

// The RAG index lives here (the agent owns the directories); the tool layer takes it as
// an injected function so it stays testable without argv.
static std::string agent_rag(void* ctx, const std::string& q) {
  const Cfg* cfg = static_cast<const Cfg*>(ctx);
  std::vector<Hit> hits = rag_search(*cfg, q, 3);
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < hits.size(); ++i) {
    if (i)
      o << ",";
    o << "{\"path\":\"" << json_escape(hits[i].path) << "\",\"snippet\":\""
      << json_escape(hits[i].snippet) << "\"}";
  }
  o << "]";
  return o.str();
}

static ToolEnv tool_env_from(const Cfg& cfg) {
  ToolEnv env;
  env.data_dir = cfg.data_dir;
  env.mqtt_http = cfg.mqtt_http;
  env.policy = cfg.policy;
  env.rag_fn = &agent_rag;
  env.rag_ctx = const_cast<Cfg*>(&cfg);
  return env;
}

static std::string first_word(const std::string& s, std::string& rest) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  size_t j = i;
  while (j < s.size() && s[j] != ' ' && s[j] != '\t')
    ++j;
  rest = trim_copy(s.substr(j));
  return s.substr(i, j - i);
}

static std::string run_slash(const Cfg& cfg, const std::string& line) {
  std::string rest;
  std::string cmd = first_word(line, rest);
  if (cmd == "/help" || cmd == "/h")
    return "/rag QUERY\n/read PATH\n/write PATH TEXT\n/mqtt TOPIC PAYLOAD\n/run CMD [ARGS]\n/history\nplain text goes to the LLM";
  if (cmd == "/history" || cmd == "/hist") {
    std::vector<Turn> turns = session_load(cfg.session_file, cfg.history);
    if (turns.empty())
      return "no turns recorded yet in " + cfg.session_file;
    std::ostringstream out;
    for (size_t i = 0; i < turns.size(); ++i)
      out << turns[i].ts << "  " << turns[i].role << ": " << turns[i].text << "\n";
    out << "(" << turns.size() << " turn(s) from " << cfg.session_file << ")";
    return out.str();
  }
  if (cmd == "/rag") {
    std::ostringstream js;
    js << "{\"query\":\"" << json_escape(rest) << "\"}";
    return run_tool(tool_env_from(cfg), "rag_search", js.str(), true, 0, 0);
  }
  if (cmd == "/read") {
    std::ostringstream js;
    js << "{\"path\":\"" << json_escape(rest) << "\"}";
    return run_tool(tool_env_from(cfg), "read_file", js.str(), true, 0, 0);
  }
  if (cmd == "/write") {
    std::string path, content;
    path = first_word(rest, content);
    std::ostringstream js;
    js << "{\"path\":\"" << json_escape(path) << "\",\"content\":\"" << json_escape(content) << "\"}";
    if (cfg.dry_run_writes) {
      // Stage instead of writing and report what would have changed. Nothing is written
      // in this mode, which is the point of having it on a device you cannot easily
      // recover.
      std::string full, err, why;
      if (!jail_path(cfg.data_dir, path, full, err))
        return "tool error: " + err;
      if (protected_path(path, why))
        return "tool error: " + why;
      ChangeQueue planned;
      if (!planned.stage(path, full, content, why))
        return "tool error: " + why;
      std::ostringstream msg;
      msg << "dry run: " << planned.summary() << "\nwould write " << path << " ("
          << content.size() << " bytes)";
      return msg.str();
    }
    return run_tool(tool_env_from(cfg), "write_file", js.str(), true, 0, 0);
  }
  if (cmd == "/mqtt") {
    std::string topic, payload;
    topic = first_word(rest, payload);
    std::ostringstream js;
    js << "{\"topic\":\"" << json_escape(topic) << "\",\"payload\":\"" << json_escape(payload) << "\"}";
    return run_tool(tool_env_from(cfg), "mqtt_publish", js.str(), true, 0, 0);
  }
  if (cmd == "/run") {
    // The operator (or a script) asked for this, so the gate is the allowlist; the
    // model's tool path goes through the same run_tool() gates with human=false.
    std::string name, rest_args;
    name = first_word(rest, rest_args);
    std::ostringstream js;
    js << "{\"command\":\"" << json_escape(name) << "\",\"args\":\""
       << json_escape(rest_args) << "\"}";
    return run_tool(tool_env_from(cfg), "run_command", js.str(), true, 0, 0);
  }
  return "unknown command; /help";
}

// A failing session store must be visible, but it must not kill the turn: losing
// history is bad, losing the answer the operator asked for is worse.
static void record_turn(const Cfg& cfg, const std::string& role, const std::string& text) {
  if (cfg.session_file.empty())
    return;
  std::string why;
  if (!session_append(cfg.session_file, role, text, why))
    std::cerr << "[slim] session: " << why << "\n";
}

static std::string run_turn(const Cfg& cfg, const std::string& user) {
  std::string u = trim_copy(user);
  if (!u.empty() && u[0] == '/')
    return run_slash(cfg, u);
  if (cfg.base_url.empty() || cfg.model.empty())
    throw std::runtime_error("set --base-url and --model, or use /rag /read /write");
  std::vector<std::pair<std::string, std::string> > msgs;
  // Replay the session first: that is what makes a session resumable after a reboot,
  // rather than a log nobody reads.
  std::vector<Turn> history = session_load(cfg.session_file, cfg.history);
  for (size_t i = 0; i < history.size(); ++i)
    msgs.push_back(std::make_pair(history[i].role == "assistant" ? std::string("assistant")
                                                                : std::string("user"),
                                  history[i].text));
  // Tell the model the tool contract up front; a weak model needs the format in the
  // prompt to emit it. Same JSON examples as the Python client's TOOLS block. Slash
  // commands keep working exactly as before.
  const std::string tool_hint =
      "Tools (reply with ONE JSON object, nothing else, only when you need one): "
      "{\"tool\":\"rag_search\",\"query\":\"keywords\"} "
      "{\"tool\":\"read_file\",\"path\":\"relative.md\"} "
      "{\"tool\":\"write_file\",\"path\":\"note.txt\",\"content\":\"...\"} "
      "{\"tool\":\"mqtt_publish\",\"topic\":\"a/b\",\"payload\":\"hi\"} "
      "{\"tool\":\"run_command\",\"command\":\"uptime\",\"args\":\"-s\"} "
      "(run_command: the name must be in commands.allow; never a shell)\n"
      "After a tool result, answer in plain text.\n";
  msgs.push_back(std::make_pair(std::string("user"),
                                tool_hint + "Q: " + cap_prompt(u, kMaxPrompt) + "\nA:"));
  std::string reply = chat_fallback(cfg, msgs);
  // One bounded tool round: if the model answered with a tool call, run it as a
  // model-initiated call - every gate applies (policy flags, the allowlist, the
  // protected paths, the write jail) - then let the model finish the answer.
  // A tool call in the final reply is never executed, same as the Python client.
  std::string tool_name, tool_json;
  if (parse_tool_call(reply, tool_name, tool_json)) {
    ToolEnv env = tool_env_from(cfg);
    ToolBudget budget;
    ChangeQueue queue;
    std::string result = run_tool(env, tool_name, tool_json, /*human_initiated=*/false,
                                  &budget, &queue);
    std::string applied;
    if (!queue.entries.empty()) {
      if (cfg.dry_run_writes) {
        applied = "dry run: " + queue.summary() + "; nothing written";
      } else {
        std::vector<std::string> failures;
        std::vector<std::string> wrote = queue.apply(failures);
        applied = queue.summary() + " (" + std::to_string(wrote.size()) + " written)";
        for (size_t i = 0; i < failures.size(); ++i)
          applied += "\n" + failures[i];
      }
    }
    std::ostringstream follow;
    follow << "tool " << tool_name << " result:\n" << result;
    if (!applied.empty())
      follow << "\n" << applied;
    msgs.push_back(std::make_pair(std::string("assistant"), reply));
    msgs.push_back(std::make_pair(std::string("user"),
                                  follow.str() + "\nAnswer in plain text. A:"));
    reply = chat_fallback(cfg, msgs);
  }
  record_turn(cfg, "user", u);
  record_turn(cfg, "assistant", reply);
  if (cfg.stream)
    return std::string();  // the deltas were already printed live
  return sanitize_reply(reply);
}

static void print_reply(const Cfg& cfg, const std::string& out, bool slash_command) {
  // With --stream the deltas have already been written, so only the trailing
  // newline is left. Slash commands never stream and print as usual.
  if (cfg.stream && !slash_command)
    std::cout << "\n";
  else
    std::cout << out << "\n";
}

static void usage() {
  std::cerr << "slim-agent (C++ t830-slim). Model tool calls are gated; slash commands still work.\n"
            << "  slim-agent --base-url URL --model ID [--once TEXT]\n"
            << "  /rag QUERY  /read PATH  /write PATH TEXT  /mqtt TOPIC PAYLOAD\n"
            << "  /run CMD [ARGS] runs an allowlisted command (no shell; see below)\n"
            << "  stream: --stream prints tokens as they arrive (SLIM_STREAM=1)\n"
            << "  session: --session NAME --history N (default 6; 0 replays nothing, -1 all)\n"
            << "  retry: --attempts N --retry-base-ms MS (default 3 x 500ms, capped 4000ms)\n"
            << "         retries transport errors, 429 and 5xx; mutations are never retried\n"
            << "  policy: model-initiated writes/publishes are denied unless enabled\n"
            << "  writes: --dry-run-writes reports what /write would change and writes nothing\n"
            << "          --allow-write / --allow-mqtt (or SLIM_ALLOW_WRITE=1 / SLIM_ALLOW_MQTT=1)\n"
            << "          state files (*.sqlite, *.db, dotfiles) are never writable\n"
            << "          --non-interactive turns off the approval prompt (deny by default)\n"
            << "  exec: model-initiated commands are on by default, but only the bare names in\n"
            << "        <data-dir>/commands.allow may run (no shell - args go as-is; 10 s,\n"
            << "        16 KiB cap). --no-allow-exec or SLIM_ALLOW_EXEC=0 disables it; that file\n"
            << "        is not writable through a tool, so the model cannot widen its own list\n"
            << "env: SLIM_BASE_URL SLIM_FALLBACK_URL SLIM_MODEL SLIM_FALLBACK_MODEL\n"
            << "     SLIM_API_KEY SLIM_DATA_DIR SLIM_SKILLS_DIR SLIM_DOCS_DIR SLIM_MQTT_HTTP\n"
            << "     SLIM_ALLOW_WRITE SLIM_ALLOW_MQTT SLIM_ALLOW_EXEC\n";
}

int main(int argc, char** argv) {
  Cfg cfg;
  cfg.max_tokens = 64;
  {
    std::string mt = env_or("SLIM_MAX_TOKENS", "");
    if (!mt.empty())
      cfg.max_tokens = atoi(mt.c_str());
  }
  cfg.timeout = 120;
  cfg.base_url = env_or("SLIM_BASE_URL", "");
  cfg.fallback_url = env_or("SLIM_FALLBACK_URL", "");
  cfg.model = env_or("SLIM_MODEL", "");
  cfg.fallback_model = env_or("SLIM_FALLBACK_MODEL", "");
  cfg.api_key = env_or("SLIM_API_KEY", "");
  cfg.data_dir = env_or("SLIM_DATA_DIR", "./slim/data");
  cfg.skills_dir = env_or("SLIM_SKILLS_DIR", "./slim/skills");
  cfg.docs_dir = env_or("SLIM_DOCS_DIR", "./slim/docs");
  cfg.mqtt_http = env_or("SLIM_MQTT_HTTP", "");
  cfg.policy.allow_write = env_or("SLIM_ALLOW_WRITE", "") == "1";
  cfg.policy.allow_mqtt = env_or("SLIM_ALLOW_MQTT", "") == "1";
  // Model-initiated execution is on by default; the scope is still only the bare names in
  // <data-dir>/commands.allow (no file, nothing runs). SLIM_ALLOW_EXEC=0 or --no-allow-exec
  // turns it off.
  cfg.policy.allow_exec = env_or("SLIM_ALLOW_EXEC", "1") != "0";
  cfg.policy.interactive = isatty(0) != 0;
  cfg.stream = env_or("SLIM_STREAM", "") == "1";
  cfg.dry_run_writes = env_or("SLIM_DRY_RUN_WRITES", "") == "1";
  cfg.verbose_http = env_or("SLIM_HTTP_VERBOSE", "") == "1";
  cfg.session = env_or("SLIM_SESSION", "default");
  cfg.history = 6;
  {
    std::string h = env_or("SLIM_HISTORY", "");
    if (!h.empty())
      cfg.history = atoi(h.c_str());
  }
  {
    std::string a = env_or("SLIM_ATTEMPTS", "");
    if (!a.empty())
      cfg.retry.attempts = atoi(a.c_str());
    std::string b = env_or("SLIM_RETRY_BASE_MS", "");
    if (!b.empty())
      cfg.retry.base_delay_ms = atoi(b.c_str());
  }
  std::string once;
  bool ingest_only = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](std::string& dst) {
      if (i + 1 >= argc)
        die("missing value for " + a);
      dst = argv[++i];
    };
    if (a == "--base-url")
      need(cfg.base_url);
    else if (a == "--fallback-url")
      need(cfg.fallback_url);
    else if (a == "--model")
      need(cfg.model);
    else if (a == "--fallback-model")
      need(cfg.fallback_model);
    else if (a == "--api-key")
      need(cfg.api_key);
    else if (a == "--data-dir")
      need(cfg.data_dir);
    else if (a == "--skills-dir")
      need(cfg.skills_dir);
    else if (a == "--docs-dir")
      need(cfg.docs_dir);
    else if (a == "--mqtt-http")
      need(cfg.mqtt_http);
    else if (a == "--max-tokens") {
      std::string v;
      need(v);
      cfg.max_tokens = atoi(v.c_str());
    } else if (a == "--attempts") {
      std::string v;
      need(v);
      cfg.retry.attempts = atoi(v.c_str());
    } else if (a == "--retry-base-ms") {
      std::string v;
      need(v);
      cfg.retry.base_delay_ms = atoi(v.c_str());
    } else if (a == "--http-verbose")
      cfg.verbose_http = true;
    else if (a == "--stream")
      cfg.stream = true;
    else if (a == "--dry-run-writes")
      cfg.dry_run_writes = true;
    else if (a == "--session")
      need(cfg.session);
    else if (a == "--history") {
      std::string v;
      need(v);
      cfg.history = atoi(v.c_str());
    } else if (a == "--allow-write")
      cfg.policy.allow_write = true;
    else if (a == "--allow-mqtt")
      cfg.policy.allow_mqtt = true;
      else if (a == "--allow-exec")
        cfg.policy.allow_exec = true;
    else if (a == "--no-allow-exec")
      cfg.policy.allow_exec = false;
    else if (a == "--non-interactive")
      cfg.policy.interactive = false;
    else if (a == "--interactive")
      cfg.policy.interactive = true;
    else if (a == "--once")
      need(once);
    else if (a == "--ingest-only")
      ingest_only = true;
    else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else
      die("unknown arg " + a);
  }
  mkdir(cfg.data_dir.c_str(), 0755);
  {
    std::string why;
    if (!valid_session_name(cfg.session, why))
      die("bad --session: " + why, 2);
    // The allowlist is read once, after every flag is known. It lives in the data
    // directory, but protected_path() keeps the model from writing it, so it cannot
    // grant itself a command. No file (or an empty one) permits nothing at all.
    {
      std::string dir = cfg.data_dir;
      if (!dir.empty() && dir[dir.size() - 1] != '/')
        dir += "/";
      cfg.policy.exec_allow = load_name_list(dir + "commands.allow");
    }
    cfg.session_file = session_path(cfg.data_dir, cfg.session);
  }
  if (ingest_only) {
    std::vector<std::pair<std::string, std::string> > files;
    list_text_files(cfg.skills_dir, "skill", files);
    list_text_files(cfg.docs_dir, "doc", files);
    list_text_files(cfg.data_dir, "data", files);
    std::cout << "indexed " << files.size() << " files\n";
    return 0;
  }
  bool once_slash = !once.empty() && once[0] == '/';
  if (!once_slash && !ingest_only && (cfg.base_url.empty() || cfg.model.empty())) {
    usage();
    die("set --base-url and --model (not needed for /rag /read /write --once)");
  }
  try {
    if (!once.empty()) {
      print_reply(cfg, run_turn(cfg, once), once_slash);
      return 0;
    }
    std::cerr << "slash: /rag /read /write /help. empty line or Ctrl-D to quit.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        break;
      print_reply(cfg, run_turn(cfg, line), !line.empty() && line[0] == '/');
    }
  } catch (const std::exception& e) {
    die(e.what(), 1);
  }
  return 0;
}
