#include "policy.hpp"

#include "util.hpp"

#include <cstdio>
#include <cstring>

namespace {

bool ends_with(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string basename_of(const std::string& rel) {
  std::string::size_type p = rel.rfind('/');
  return p == std::string::npos ? rel : rel.substr(p + 1);
}

// Suffixes that mean "this is agent state, not user content".
const char* const kStateSuffixes[] = {
    ".sqlite", ".sqlite3", ".db", ".sqlite-wal", ".sqlite-journal", ".sqlite-shm",
    ".db-wal", ".db-journal", ".db-shm",
};

const char* const kBinaryNames[] = {"slim-agent", "slim-agent-t830"};

// The command allowlist is a permission file: if a tool could rewrite it, the model could
// grant itself every command in the list.
const char* const kProtectedNames[] = {"commands.allow"};

// A session directory holds append-only conversation state. write_file is jailed to
// data_dir, which is also where sessions live, so without this rule the model could
// rewrite its own history (or a session name could be used to escape the jail).
bool has_path_component(const std::string& rel, const std::string& component) {
  size_t start = 0;
  while (start <= rel.size()) {
    size_t slash = rel.find('/', start);
    std::string part =
        rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part == component)
      return true;
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return false;
}

}  // namespace

bool protected_path(const std::string& rel, std::string& why) {
  std::string base = basename_of(rel);
  if (base.empty()) {
    why = "path has no file name";
    return true;
  }
  if (has_path_component(rel, "sessions")) {
    why = "refusing to write " + rel + ": the session store is append-only agent state";
    return true;
  }
  for (size_t i = 0; i < sizeof(kStateSuffixes) / sizeof(kStateSuffixes[0]); ++i) {
    if (ends_with(base, kStateSuffixes[i])) {
      why = "refusing to write " + base + ": looks like an agent state/database file (" +
            kStateSuffixes[i] + ")";
      return true;
    }
  }
  for (size_t i = 0; i < sizeof(kProtectedNames) / sizeof(kProtectedNames[0]); ++i) {
    if (base == kProtectedNames[i]) {
      why = "refusing to write " + base + ": it is a permission file (the command allowlist)";
      return true;
    }
  }
  for (size_t i = 0; i < sizeof(kBinaryNames) / sizeof(kBinaryNames[0]); ++i) {
    if (base == kBinaryNames[i]) {
      why = "refusing to overwrite the agent binary " + base;
      return true;
    }
  }
  if (base.size() > 1 && base[0] == '.') {
    why = "refusing to write dotfile " + base;
    return true;
  }
  return false;
}

bool exec_allowed(const Policy& policy, const std::string& name) {
  return policy.allow_exec && string_in_list(policy.exec_allow, name);
}

bool is_mutating_tool(const std::string& tool) {
  return tool == "write_file" || tool == "mqtt_publish" || tool == "run_command";
}

bool approve_mutation(const Policy& policy, const std::string& tool, const std::string& detail,
                      bool human_initiated, std::string& why) {
  if (human_initiated) {
    why = "human-initiated";
    return true;
  }
  bool allowed = (tool == "write_file")   ? policy.allow_write
                 : (tool == "mqtt_publish") ? policy.allow_mqtt
                 : (tool == "run_command")  ? policy.allow_exec
                                            : false;
  if (allowed) {
    why = "allowed by policy";
    return true;
  }
  if (!policy.interactive) {
    why = "blocked by policy: model-initiated " + tool +
          " is not enabled (use --allow-write / --allow-mqtt / --allow-exec, or "
          "SLIM_ALLOW_WRITE=1 / SLIM_ALLOW_MQTT=1 / SLIM_ALLOW_EXEC=1)";
    return false;
  }
  std::fprintf(stderr, "[slim] approve model tool %s (%s)? [y/N] ", tool.c_str(), detail.c_str());
  std::fflush(stderr);
  char buf[32];
  if (std::fgets(buf, sizeof(buf), stdin) == 0) {
    why = "no answer on stdin; denied";
    return false;
  }
  std::string answer(buf);
  if (answer == "y\n" || answer == "Y\n" || answer == "yes\n" || answer == "YES\n") {
    why = "approved at prompt";
    return true;
  }
  why = "denied at prompt";
  return false;
}
