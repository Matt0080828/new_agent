#ifndef SLIM_TOOLS_HPP
#define SLIM_TOOLS_HPP

#include "policy.hpp"

#include <string>
#include <vector>

// Per-file write cap, applied when a change is staged and again when it is applied.
const size_t kMaxWrite = 8 * 1024;

// A change queue longer than this, or bigger than this, is refused: a 2048-token model
// in a loop must not be able to fill the device in one turn.
const size_t kMaxQueuedFiles = 16;
const size_t kMaxQueuedBytes = 64 * 1024;

// Wall-clock budget and output cap for run_command. A 0.5B model in a loop must not be able
// to keep the device busy or flood the conversation with one command.
const int kExecTimeoutSec = 10;
const size_t kMaxExecOutput = 16 * 1024;

// Everything the tool layer needs. A struct (rather than the agent's argv-derived Cfg)
// so the tool layer can be driven straight from tests.
struct ToolEnv {
  std::string data_dir;
  std::string mqtt_http;
  // Colon-separated directories searched for an allowlisted command; empty means the
  // usual /bin:/usr/bin:/sbin:/usr/sbin:/usr/local/bin.
  std::string exec_path;
  Policy policy;
  // Read-only RAG is injected instead of reimplemented here: the agent owns the index,
  // and a test can stub it.
  std::string (*rag_fn)(void* ctx, const std::string& query);
  void* rag_ctx;
  ToolEnv() : rag_fn(0), rag_ctx(0) {}
};

// Accumulates what tools put into the conversation. Without a bound, a couple of tool
// rounds can push the prompt far past the context window of the local model.
struct ToolBudget {
  size_t max_total_bytes;
  size_t used;
  size_t elided;
  explicit ToolBudget(size_t max_bytes = 8 * 1024) : max_total_bytes(max_bytes), used(0), elided(0) {}
  // Returns the text, trimmed to whatever is left of the budget, with a marker when it
  // had to trim. Once the budget is gone every later result is an elision marker, so the
  // model is told why its tool output is missing rather than silently getting nothing.
  std::string add(const std::string& text);
  std::string summary() const;
  void reset();
};

// A turn's planned file changes. Staging instead of writing gives the operator one
// manifest for the whole turn (and a single flash write per file) instead of a stream of
// writes nobody sees; last write to the same path wins, and that is reported.
struct ChangeQueue {
  struct Entry {
    std::string rel;
    std::string full;
    std::string content;
  };
  std::vector<Entry> entries;
  size_t replaced;

  ChangeQueue() : replaced(0) {}

  bool stage(const std::string& rel, const std::string& full, const std::string& content,
             std::string& why);
  // Writes every staged file. Returns one "wrote <rel> (N bytes)" line per success and a
  // "<rel>: <reason>" line per failure; a failure never discards the others.
  std::vector<std::string> apply(std::vector<std::string>& failures);
  std::string summary() const;
  void clear();
};

// Run one tool call. `human_initiated` is the operator, not the model - a call site that
// forgets to say who asked gets the fail-closed answer.
std::string run_tool(const ToolEnv& env, const std::string& name, const std::string& json_args,
                     bool human_initiated, ToolBudget* budget, ChangeQueue* queue);

// True when the tool name is one the tool layer implements.
bool known_tool(const std::string& name);

#endif
