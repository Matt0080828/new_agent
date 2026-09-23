#include "tools.hpp"

#include "http.hpp"
#include "util.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>

namespace {

void audit_tool(const std::string& tool, const std::string& detail, bool allowed,
                const std::string& why) {
  std::cerr << "[slim] tool " << tool << " (" << detail << ") -> " << (allowed ? "allow" : "deny")
            << ": " << why << "\n";
}

std::string int_str(long n) {
  std::ostringstream o;
  o << n;
  return o.str();
}

std::string number(size_t n) {
  std::ostringstream o;
  o << n;
  return o.str();
}

}  // namespace

std::string ToolBudget::add(const std::string& text) {
  if (used >= max_total_bytes) {
    elided += text.size();
    return "[tool output omitted: " + number(max_total_bytes) + "-byte budget exhausted]";
  }
  size_t left = max_total_bytes - used;
  if (text.size() <= left) {
    used += text.size();
    return text;
  }
  elided += text.size() - left;
  used += left;
  return text.substr(0, left) + "\n[truncated " + number(text.size() - left) +
         " bytes: tool output budget reached]";
}

std::string ToolBudget::summary() const {
  std::string s = "tool output " + number(used) + "/" + number(max_total_bytes) + " bytes";
  if (elided)
    s += ", " + number(elided) + " bytes elided";
  return s;
}

void ToolBudget::reset() {
  used = 0;
  elided = 0;
}

bool ChangeQueue::stage(const std::string& rel, const std::string& full, const std::string& content,
                        std::string& why) {
  if (content.size() > kMaxWrite) {
    why = "refusing to stage " + rel + ": " + number(content.size()) + " bytes is larger than the " +
          number(kMaxWrite) + "-byte limit for one file";
    return false;
  }
  size_t total = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].full == full) {
      // Last write wins, but say so: a model that changes its mind about a file is worth
      // seeing in the manifest.
      entries[i].content = content;
      ++replaced;
      return true;
    }
    total += entries[i].content.size();
  }
  if (entries.size() >= kMaxQueuedFiles) {
    why = "refusing to stage " + rel + ": the change queue already holds " +
          number(kMaxQueuedFiles) + " files";
    return false;
  }
  if (total + content.size() > kMaxQueuedBytes) {
    why = "refusing to stage " + rel + ": the change queue would exceed " +
          number(kMaxQueuedBytes) + " bytes";
    return false;
  }
  Entry e;
  e.rel = rel;
  e.full = full;
  e.content = content;
  entries.push_back(e);
  return true;
}

std::vector<std::string> ChangeQueue::apply(std::vector<std::string>& failures) {
  std::vector<std::string> wrote;
  for (size_t i = 0; i < entries.size(); ++i) {
    std::string err;
    if (!write_file_limited(entries[i].full, entries[i].content, kMaxWrite, err)) {
      failures.push_back(entries[i].rel + ": " + err);
      continue;
    }
    wrote.push_back("wrote " + entries[i].rel + " (" + number(entries[i].content.size()) + " bytes)");
  }
  return wrote;
}

std::string ChangeQueue::summary() const {
  size_t total = 0;
  for (size_t i = 0; i < entries.size(); ++i)
    total += entries[i].content.size();
  std::string s = number(entries.size()) + " file(s), " + number(total) + " bytes";
  if (replaced)
    s += ", " + number(replaced) + " overwrite(s) of an earlier staged change";
  return s;
}

void ChangeQueue::clear() {
  entries.clear();
  replaced = 0;
}

bool known_tool(const std::string& name) {
  return name == "rag_search" || name == "read_file" || name == "write_file" ||
         name == "mqtt_publish" || name == "run_command";
}

bool parse_tool_call(const std::string& text, std::string& name, std::string& json_obj) {
  size_t start = text.find('{');
  if (start == std::string::npos)
    return false;
  size_t end = text.rfind('}');
  if (end <= start)
    return false;
  json_obj = text.substr(start, end - start + 1);
  if (!json_extract_string(json_obj, "tool", name))
    return false;
  return known_tool(name);
}

std::string run_tool(const ToolEnv& env, const std::string& name, const std::string& obj,
                     bool human_initiated, ToolBudget* budget, ChangeQueue* queue) {
  std::string result;
  if (name == "rag_search") {
    if (!env.rag_fn)
      return "tool error: rag search is not available in this build";
    std::string q;
    json_get_string_field(obj, "query", q);
    result = env.rag_fn(env.rag_ctx, q);
  } else if (name == "read_file") {
    std::string rel, full, err;
    json_get_string_field(obj, "path", rel);
    if (!jail_path(env.data_dir, rel, full, err))
      return "tool error: " + err;
    result = read_file_limited(full, 16 * 1024);
    if (result.empty())
      return "tool error: empty or missing";
  } else if (name == "write_file") {
    std::string rel, content, full, err, why;
    json_get_string_field(obj, "path", rel);
    json_get_string_field(obj, "content", content);
    if (!jail_path(env.data_dir, rel, full, err))
      return "tool error: " + err;
    if (protected_path(rel, why)) {
      audit_tool(name, rel, false, why);
      return "tool error: " + why;
    }
    if (!approve_mutation(env.policy, name, rel, human_initiated, why)) {
      audit_tool(name, rel, false, why);
      return "tool error: " + why;
    }
    if (queue) {
      // Staged, not written: the queue is applied once when the turn ends, and the
      // manifest is what the operator audits.
      if (!queue->stage(rel, full, content, why)) {
        audit_tool(name, rel, false, why);
        return "tool error: " + why;
      }
      audit_tool(name, rel, true, why + " (staged until the end of the turn)");
      return "staged " + rel + " (" + number(content.size()) +
             " bytes); it is written when this turn ends";
    }
    audit_tool(name, rel, true, why);
    if (!write_file_limited(full, content, kMaxWrite, err))
      return "tool error: " + err;
    result = "wrote " + rel;
  } else if (name == "mqtt_publish") {
    if (env.mqtt_http.empty())
      return "tool error: mqtt disabled; set SLIM_MQTT_HTTP";
    std::string topic, payload, why;
    json_get_string_field(obj, "topic", topic);
    json_get_string_field(obj, "payload", payload);
    if (!approve_mutation(env.policy, name, topic, human_initiated, why)) {
      audit_tool(name, topic, false, why);
      return "tool error: " + why;
    }
    audit_tool(name, topic, true, why);
    std::ostringstream js;
    js << "{\"topic\":\"" << json_escape(topic) << "\",\"payload\":\"" << json_escape(payload)
       << "\"}";
    // Deliberately single-shot: a publish is a mutation, and a retry after an ambiguous
    // failure can duplicate the effect. The C++ test counts requests on a local server.
    HttpResult r = http_post_json(env.mqtt_http, js.str(), "", 10);
    if (!r.error.empty() && r.status == 0)
      return "tool error: " + r.error;
    result = "mqtt http " + r.body.substr(0, 200);
  } else if (name == "run_command") {
    // Execution is the one tool that changes the machine, so it has three gates, in order:
    // a bare command name (no paths - a model must not point this at a binary it wrote),
    // membership in the allowlist (a missing or empty file permits nothing), and the usual
    // model-initiated approval. No shell is involved at any point in this path.
    std::string cmd, argstr, why;
    json_get_string_field(obj, "command", cmd);
    json_get_string_field(obj, "args", argstr);
    if (!bare_command_name(cmd)) {
      audit_tool(name, cmd, false, "refusing a command that is not a bare name");
      return "tool error: refusing to run '" + cmd +
             "': a bare command name is required (no path, no directory part)";
    }
    if (!string_in_list(env.policy.exec_allow, cmd)) {
      audit_tool(name, cmd, false, "not in the command allowlist");
      return "tool error: refusing to run " + cmd +
             ": it is not in the command allowlist (<data-dir>/commands.allow)";
    }
    if (!approve_mutation(env.policy, name, cmd, human_initiated, why)) {
      audit_tool(name, cmd, false, why);
      return "tool error: " + why;
    }
    // Whitespace-split arguments: no quoting, no globbing, no shell. Each token is one
    // argv entry, so `;`, `|` or `$(...)` inside a token is just a character.
    std::vector<std::string> argv;
    argv.push_back(cmd);
    {
      std::string cur;
      for (size_t i = 0; i <= argstr.size(); ++i) {
        if (i == argstr.size() || argstr[i] == ' ' || argstr[i] == '\t') {
          if (!cur.empty()) {
            argv.push_back(cur);
            cur.clear();
          }
        } else {
          cur.push_back(argstr[i]);
        }
      }
    }
    audit_tool(name, cmd + (argstr.empty() ? std::string() : " " + argstr), true, why);
    std::string out, err;
    int status = 0;
    if (!run_argv_capture(argv, env.exec_path, kExecTimeoutSec, kMaxExecOutput, out, status, err)) {
      audit_tool(name, cmd, false, err);
      return "tool error: " + err;
    }
    result = "exit " + int_str(status) + "\n" + out;
  } else {
    // Unknown tool: refused rather than ignored, so a typo or a new tool name cannot
    // silently pass through.
    audit_tool(name, "", false, "not a classified tool (fail-closed)");
    return "tool error: unknown tool";
  }
  if (budget)
    return budget->add(result);
  return result;
}
