#ifndef SLIM_POLICY_HPP
#define SLIM_POLICY_HPP

#include <string>
#include <vector>

// Fail-closed tool policy for the slim agent.
//
// Two independent ideas:
//   1. protected_path(): files a tool may never write, whatever the caller wants.
//      State/database files are the interesting case: if the agent can overwrite
//      them, one bad tool call destroys its own memory with no way back.
//   2. approve_mutation(): model-initiated writes and outbound publishes need an
//      explicit opt-in. With no opt-in and no terminal to ask, the answer is no.
struct Policy {
  bool allow_write;   // --allow-write / SLIM_ALLOW_WRITE=1
  bool allow_mqtt;    // --allow-mqtt  / SLIM_ALLOW_MQTT=1
  bool allow_exec;    // --allow-exec  / SLIM_ALLOW_EXEC=1 (model-initiated run_command)
  bool interactive;   // a terminal is attached, so we may ask
  // Command names the operator permitted, one per line in <data-dir>/commands.allow.
  // Empty (or no file) means nothing may run, whatever the flags say.
  std::vector<std::string> exec_allow;
  Policy() : allow_write(false), allow_mqtt(false), allow_exec(false), interactive(false) {}
};

// Returns true (with a reason) when rel names something no tool may write.
bool protected_path(const std::string& rel, std::string& why);

// Fail-closed approval for mutating tools.
// human_initiated: the operator typed the action, rather than the model asking for it.
// Returns true when the mutation may proceed; otherwise false with a reason.
bool approve_mutation(const Policy& policy, const std::string& tool, const std::string& detail,
                      bool human_initiated, std::string& why);

// True when tool is one of the mutating tools this policy governs.
bool is_mutating_tool(const std::string& tool);

// True when name may be run at all: it has to be in the allowlist. The allowlist is the
// gate for everyone, operator included; allow_exec only decides the model's side.
bool exec_allowed(const Policy& policy, const std::string& name);

#endif
