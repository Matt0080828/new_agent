#ifndef SLIM_SESSION_HPP
#define SLIM_SESSION_HPP

#include <string>
#include <vector>

// Append-only session store: one JSON object per line,
//   {"ts":"2026-09-18T12:00:00Z","role":"user","text":"..."}
//
// JSONL on purpose. No database, no library to link into a musl binary, and the same
// file can be read by the stdlib-Python client (or by grep, or by eye) on the CPE, so
// a session is not trapped in one client.
//
// Reading is tolerant: a truncated or hand-edited line is skipped instead of failing
// the whole session, because a broken tail must not lock the operator out of their
// own history.
struct Turn {
  std::string ts;
  std::string role;
  std::string text;
};

// A session name becomes a file name, so it is validated before it is used.
bool valid_session_name(const std::string& name, std::string& why);

// <data_dir>/sessions/<name>.jsonl
std::string session_path(const std::string& data_dir, const std::string& name);

// Append one turn, creating the directory when needed. False + why when it fails.
bool session_append(const std::string& path, const std::string& role, const std::string& text,
                    std::string& why);

// The last `limit` turns, oldest first. limit < 0 means every turn, 0 means none.
// A missing or unreadable file yields no turns (that is "a new session", not an error).
std::vector<Turn> session_load(const std::string& path, int limit);

#endif
