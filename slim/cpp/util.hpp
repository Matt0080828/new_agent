#ifndef SLIM_UTIL_HPP
#define SLIM_UTIL_HPP

#include <string>
#include <vector>

std::string json_escape(const std::string& s);
bool json_extract_string(const std::string& json, const std::string& key, std::string& out);
bool json_extract_object(const std::string& text, std::string& obj);
bool json_get_string_field(const std::string& obj, const std::string& key, std::string& out);
std::string join_url(const std::string& base, const std::string& suffix);
// Suffix test that is safe for names shorter than the suffix (the previous
// inline name.substr(name.size() - 4) underflowed and aborted the process).
bool has_suffix(const std::string& s, const std::string& suffix);
// Truncate to max_chars and mark it, so a huge prompt cannot blow the context
// window of a 2048-token local model.
std::string cap_prompt(const std::string& s, size_t max_chars);
bool jail_path(const std::string& root, const std::string& rel, std::string& full, std::string& err);
std::string read_file_limited(const std::string& path, size_t maxn);
bool write_file_limited(const std::string& path, const std::string& data, size_t maxn, std::string& err);
void list_text_files(const std::string& root, const std::string& prefix,
                     std::vector<std::pair<std::string, std::string> >& out);
bool string_in_list(const std::vector<std::string>& list, const std::string& s);
// Runs argv directly - there is no shell involved anywhere in this path, so a `;`, `|`, `$()`
// or backtick inside an argument is just a character that reaches the program as one argument.
// stdout and stderr are captured together, capped at max_bytes; the child is killed after
// timeout_sec. Returns false with err set when the command cannot be started or had to be
// killed for running too long.
bool run_argv_capture(const std::vector<std::string>& argv, const std::string& search_path,
                      int timeout_sec, size_t max_bytes, std::string& out, int& status,
                      std::string& err);
// One name per line, blanks and `#` comments ignored; a missing file is an empty list, because
// "no allowlist" must mean "nothing is allowed", never "everything is allowed".
std::vector<std::string> load_name_list(const std::string& path);
// A bare command name: no slash, no `.`/`..`, nothing empty. Paths are refused so a model
// cannot point the runner at a binary it just wrote into the data directory.
bool bare_command_name(const std::string& name);
// Absolute path of `name`, searched in search_path (colon separated) and then the usual
// system directories. Empty when it is not found.
std::string find_command(const std::string& name, const std::string& search_path);

#endif
