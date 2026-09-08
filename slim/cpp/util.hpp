#ifndef SLIM_UTIL_HPP
#define SLIM_UTIL_HPP

#include <string>
#include <vector>

std::string json_escape(const std::string& s);
bool json_extract_string(const std::string& json, const std::string& key, std::string& out);
bool json_extract_object(const std::string& text, std::string& obj);
bool json_get_string_field(const std::string& obj, const std::string& key, std::string& out);
std::string join_url(const std::string& base, const std::string& suffix);
bool jail_path(const std::string& root, const std::string& rel, std::string& full, std::string& err);
std::string read_file_limited(const std::string& path, size_t maxn);
bool write_file_limited(const std::string& path, const std::string& data, size_t maxn, std::string& err);
void list_text_files(const std::string& root, const std::string& prefix,
                     std::vector<std::pair<std::string, std::string> >& out);

#endif
