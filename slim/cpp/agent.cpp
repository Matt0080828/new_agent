#include "http.hpp"
#include "util.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

static const size_t kMaxWrite = 8 * 1024;
static const size_t kMaxPrompt = 6000;
static const int kMaxHistory = 6;

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
  js << "{\"model\":\"" << json_escape(model) << "\",\"stream\":false,\"max_tokens\":" << cfg.max_tokens
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

static void trim(std::vector<std::pair<std::string, std::string> >& msgs) {
  while (!msgs.empty()) {
    size_t n = 0;
    for (size_t i = 0; i < msgs.size(); ++i)
      n += msgs[i].second.size();
    if (n <= kMaxPrompt)
      break;
    if (msgs.size() > 2 && msgs[0].first == "system")
      msgs.erase(msgs.begin() + 1);
    else if (msgs.size() > 1)
      msgs.erase(msgs.begin());
    else
      break;
  }
}

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

static std::string skills_blob(const Cfg& cfg) {
  std::vector<std::pair<std::string, std::string> > files;
  list_text_files(cfg.skills_dir, "", files);
  if (files.empty())
    return "";
  std::ostringstream o;
  o << "Available skills (markdown only, do not execute):\n";
  for (size_t i = 0; i < files.size() && i < 8; ++i)
    o << "### " << files[i].first << "\n" << files[i].second.substr(0, 1200) << "\n";
  return o.str();
}

static std::string system_prompt(const Cfg& cfg) {
  std::ostringstream o;
  o << "You are a small CPE agent. Answer briefly.\n"
    << "Context window is about 2048 tokens. Do not claim Hermes compatibility.\n"
    << "Tools (emit ONE JSON object, nothing else, if you need a tool):\n"
    << "- rag_search: {\"tool\":\"rag_search\",\"query\":\"keywords\"}\n"
    << "- read_file: {\"tool\":\"read_file\",\"path\":\"relative.md\"}\n"
    << "- write_file: {\"tool\":\"write_file\",\"path\":\"note.txt\",\"content\":\"...\"}\n"
    << "- mqtt_publish: {\"tool\":\"mqtt_publish\",\"topic\":\"a/b\",\"payload\":\"hi\"} (needs mqtt http)\n"
    << "After a tool result, answer in plain text. Never execute shell.\n"
    << skills_blob(cfg);
  return o.str();
}

static std::string run_tool(const Cfg& cfg, const std::string& name, const std::string& obj) {
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
    std::string rel, content, full, err;
    json_get_string_field(obj, "path", rel);
    json_get_string_field(obj, "content", content);
    if (!jail_path(cfg.data_dir, rel, full, err))
      return "tool error: " + err;
    if (!write_file_limited(full, content, kMaxWrite, err))
      return "tool error: " + err;
    return "wrote " + rel;
  }
  if (name == "mqtt_publish") {
    if (cfg.mqtt_http.empty())
      return "tool error: mqtt disabled; set SLIM_MQTT_HTTP";
    std::string topic, payload;
    json_get_string_field(obj, "topic", topic);
    json_get_string_field(obj, "payload", payload);
    std::ostringstream js;
    js << "{\"topic\":\"" << json_escape(topic) << "\",\"payload\":\"" << json_escape(payload) << "\"}";
    HttpResult r = http_post_json(cfg.mqtt_http, js.str(), "", 10);
    if (!r.error.empty() && r.status == 0)
      return "tool error: " + r.error;
    return "mqtt http " + r.body.substr(0, 200);
  }
  return "tool error: unknown tool";
}

static std::string run_turn(const Cfg& cfg, const std::string& user) {
  std::vector<std::pair<std::string, std::string> > msgs;
  msgs.push_back(std::make_pair(std::string("system"), system_prompt(cfg)));
  std::vector<Hit> hits = rag_search(cfg, user, 3);
  if (!hits.empty()) {
    std::ostringstream o;
    o << "RAG hits: ";
    for (size_t i = 0; i < hits.size(); ++i)
      o << hits[i].path << " ";
    msgs.push_back(std::make_pair(std::string("system"), o.str()));
  }
  msgs.push_back(std::make_pair(std::string("user"), user));
  trim(msgs);
  std::string reply = chat_fallback(cfg, msgs);
  std::string obj;
  if (json_extract_object(reply, obj)) {
    std::string tool;
    if (json_get_string_field(obj, "tool", tool) && !tool.empty()) {
      std::string result = run_tool(cfg, tool, obj);
      msgs.push_back(std::make_pair(std::string("assistant"), reply));
      msgs.push_back(std::make_pair(std::string("user"), "tool " + tool + " result:\n" + result));
      trim(msgs);
      reply = chat_fallback(cfg, msgs);
    }
  }
  return reply;
}

static void usage() {
  std::cerr << "slim-agent (C++ t830-slim). Not hardware-verified on T830.\n"
            << "  slim-agent --base-url URL --model ID [--once TEXT]\n"
            << "env: SLIM_BASE_URL SLIM_FALLBACK_URL SLIM_MODEL SLIM_FALLBACK_MODEL\n"
            << "     SLIM_API_KEY SLIM_DATA_DIR SLIM_SKILLS_DIR SLIM_DOCS_DIR SLIM_MQTT_HTTP\n";
}

int main(int argc, char** argv) {
  Cfg cfg;
  cfg.max_tokens = 256;
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
  if (cfg.base_url.empty() || cfg.model.empty()) {
    usage();
    die("set --base-url and --model");
  }
  try {
    if (!once.empty()) {
      std::cout << run_turn(cfg, once) << "\n";
      return 0;
    }
    std::cerr << "slim-agent C++. empty line or Ctrl-D to quit.\n";
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
