#include "http.hpp"
#include "policy.hpp"
#include "util.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static const size_t kMaxWrite = 8 * 1024;
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
};

static void die(const std::string& m, int c = 2) {
  std::cerr << m << "\n";
  std::exit(c);
}

static std::string env_or(const char* k, const std::string& d) {
  const char* v = std::getenv(k);
  return v && v[0] ? std::string(v) : d;
}

static std::string openai_chat(const Cfg& cfg, const std::string& url, const std::string& model,
                               const std::vector<std::pair<std::string, std::string> >& msgs) {
  std::ostringstream js;
  js << "{\"model\":\"" << json_escape(model) << "\",\"stream\":false,\"temperature\":0"
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
  HttpResult r = http_post_json(endpoint, js.str(), cfg.api_key, cfg.timeout);
  if (!r.error.empty() && r.body.empty())
    throw std::runtime_error(r.error);
  if (r.status && (r.status < 200 || r.status >= 300) && r.body.empty())
    throw std::runtime_error(r.error.empty() ? "http error" : r.error);
  std::string content;
  if (!json_extract_string(r.body, "content", content))
    throw std::runtime_error(r.error.empty() ? "no content in response" : r.error + " " + r.body.substr(0, 200));
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

// History trimming used to live here. It became unreachable when model tool
// calls were dropped (every turn is now a single user message), so the size
// guard moved to cap_prompt() in util.cpp, where it has a unit test.
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

static void audit_tool(const std::string& tool, const std::string& detail, bool allowed,
                        const std::string& why) {
  std::cerr << "[slim] tool " << tool << " (" << detail << ") -> "
            << (allowed ? "allow" : "deny") << ": " << why << "\\n";
}

// human_initiated defaults to false on purpose: a call site that forgets to say
// who asked gets the fail-closed answer.
static std::string run_tool(const Cfg& cfg, const std::string& name, const std::string& obj,
                            bool human_initiated = false) {
  if (name == "rag_search") {
    std::string q;
    json_get_string_field(obj, "query", q);
    std::vector<Hit> hits = rag_search(cfg, q, 3);
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i)
        o << ",";
      o << "{\"path\":\"" << json_escape(hits[i].path) << "\",\"snippet\":\"" << json_escape(hits[i].snippet)
        << "\"}";
    }
    o << "]";
    return o.str();
  }
  if (name == "read_file") {
    std::string rel, full, err;
    json_get_string_field(obj, "path", rel);
    if (!jail_path(cfg.data_dir, rel, full, err))
      return "tool error: " + err;
    std::string body = read_file_limited(full, 16 * 1024);
    if (body.empty())
      return "tool error: empty or missing";
    return body;
  }
  if (name == "write_file") {
    std::string rel, content, full, err, why;
    json_get_string_field(obj, "path", rel);
    json_get_string_field(obj, "content", content);
    if (!jail_path(cfg.data_dir, rel, full, err))
      return "tool error: " + err;
    if (protected_path(rel, why)) {
      audit_tool(name, rel, false, why);
      return "tool error: " + why;
    }
    if (!approve_mutation(cfg.policy, name, rel, human_initiated, why)) {
      audit_tool(name, rel, false, why);
      return "tool error: " + why;
    }
    audit_tool(name, rel, true, why);
    if (!write_file_limited(full, content, kMaxWrite, err))
      return "tool error: " + err;
    return "wrote " + rel;
  }
  if (name == "mqtt_publish") {
    if (cfg.mqtt_http.empty())
      return "tool error: mqtt disabled; set SLIM_MQTT_HTTP";
    std::string topic, payload, why;
    json_get_string_field(obj, "topic", topic);
    json_get_string_field(obj, "payload", payload);
    if (!approve_mutation(cfg.policy, name, topic, human_initiated, why)) {
      audit_tool(name, topic, false, why);
      return "tool error: " + why;
    }
    audit_tool(name, topic, true, why);
    std::ostringstream js;
    js << "{\"topic\":\"" << json_escape(topic) << "\",\"payload\":\"" << json_escape(payload) << "\"}";
    HttpResult r = http_post_json(cfg.mqtt_http, js.str(), "", 10);
    if (!r.error.empty() && r.status == 0)
      return "tool error: " + r.error;
    return "mqtt http " + r.body.substr(0, 200);
  }
  return "tool error: unknown tool";
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
    return "/rag QUERY\n/read PATH\n/write PATH TEXT\n/mqtt TOPIC PAYLOAD\nplain text goes to the LLM";
  if (cmd == "/rag") {
    std::ostringstream js;
    js << "{\"query\":\"" << json_escape(rest) << "\"}";
    return run_tool(cfg, "rag_search", js.str(), true);
  }
  if (cmd == "/read") {
    std::ostringstream js;
    js << "{\"path\":\"" << json_escape(rest) << "\"}";
    return run_tool(cfg, "read_file", js.str(), true);
  }
  if (cmd == "/write") {
    std::string path, content;
    path = first_word(rest, content);
    std::ostringstream js;
    js << "{\"path\":\"" << json_escape(path) << "\",\"content\":\"" << json_escape(content) << "\"}";
    return run_tool(cfg, "write_file", js.str(), true);
  }
  if (cmd == "/mqtt") {
    std::string topic, payload;
    topic = first_word(rest, payload);
    std::ostringstream js;
    js << "{\"topic\":\"" << json_escape(topic) << "\",\"payload\":\"" << json_escape(payload) << "\"}";
    return run_tool(cfg, "mqtt_publish", js.str(), true);
  }
  return "unknown command; /help";
}

static std::string run_turn(const Cfg& cfg, const std::string& user) {
  std::string u = trim_copy(user);
  if (!u.empty() && u[0] == '/')
    return run_slash(cfg, u);
  if (cfg.base_url.empty() || cfg.model.empty())
    throw std::runtime_error("set --base-url and --model, or use /rag /read /write");
  std::vector<std::pair<std::string, std::string> > msgs;
  msgs.push_back(std::make_pair(std::string("user"),
                                std::string("Q: ") + cap_prompt(u, kMaxPrompt) + "\nA:"));
  return sanitize_reply(chat_fallback(cfg, msgs));
}

static void usage() {
  std::cerr << "slim-agent (C++ t830-slim). 270M cannot do tool JSON; use slash commands.\n"
            << "  slim-agent --base-url URL --model ID [--once TEXT]\n"
            << "  /rag QUERY  /read PATH  /write PATH TEXT  /mqtt TOPIC PAYLOAD\n"
            << "  policy: model-initiated writes/publishes are denied unless enabled\n"
            << "          --allow-write / --allow-mqtt (or SLIM_ALLOW_WRITE=1 / SLIM_ALLOW_MQTT=1)\n"
            << "          state files (*.sqlite, *.db, dotfiles) are never writable\n"
            << "          --non-interactive turns off the approval prompt (deny by default)\n"
            << "env: SLIM_BASE_URL SLIM_FALLBACK_URL SLIM_MODEL SLIM_FALLBACK_MODEL\n"
            << "     SLIM_API_KEY SLIM_DATA_DIR SLIM_SKILLS_DIR SLIM_DOCS_DIR SLIM_MQTT_HTTP\n";
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
  cfg.policy.interactive = isatty(0) != 0;
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
    } else if (a == "--allow-write")
      cfg.policy.allow_write = true;
    else if (a == "--allow-mqtt")
      cfg.policy.allow_mqtt = true;
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
      std::cout << run_turn(cfg, once) << "\n";
      return 0;
    }
    std::cerr << "slash: /rag /read /write /help. empty line or Ctrl-D to quit.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        break;
      std::cout << run_turn(cfg, line) << "\n";
    }
  } catch (const std::exception& e) {
    die(e.what(), 1);
  }
  return 0;
}
