#include "util.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      o.push_back('\\');
      o.push_back(c);
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\r') {
      o += "\\r";
    } else if (c == '\t') {
      o += "\\t";
    } else {
      o.push_back(c);
    }
  }
  return o;
}

static bool unescape_into(const std::string& src, size_t& i, std::string& out) {
  if (i >= src.size() || src[i] != '"')
    return false;
  ++i;
  while (i < src.size()) {
    char c = src[i++];
    if (c == '"')
      return true;
    if (c == '\\' && i < src.size()) {
      char e = src[i++];
      if (e == 'n')
        out.push_back('\n');
      else if (e == 'r')
        out.push_back('\r');
      else if (e == 't')
        out.push_back('\t');
      else
        out.push_back(e);
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool json_extract_string(const std::string& json, const std::string& key, std::string& out) {
  std::string pat = "\"" + key + "\"";
  std::string::size_type p = 0;
  while ((p = json.find(pat, p)) != std::string::npos) {
    size_t i = p + pat.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
      ++i;
    if (i < json.size() && json[i] == ':')
      ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
      ++i;
    out.clear();
    if (unescape_into(json, i, out))
      return true;
    p += pat.size();
  }
  return false;
}

bool json_extract_object(const std::string& text, std::string& obj) {
  std::string::size_type a = text.find('{');
  std::string::size_type b = text.rfind('}');
  if (a == std::string::npos || b == std::string::npos || b <= a)
    return false;
  obj = text.substr(a, b - a + 1);
  return true;
}

bool json_get_string_field(const std::string& obj, const std::string& key, std::string& out) {
  return json_extract_string(obj, key, out);
}

std::string join_url(const std::string& base, const std::string& suffix) {
  std::string b = base;
  while (!b.empty() && b[b.size() - 1] == '/')
    b.resize(b.size() - 1);
  if (b.size() >= 3 && b.substr(b.size() - 3) == "/v1")
    return b + suffix;
  return b + "/v1" + suffix;
}

bool jail_path(const std::string& root, const std::string& rel, std::string& full, std::string& err) {
  if (rel.empty() || rel[0] == '/') {
    err = "path must be relative and stay in data dir";
    return false;
  }
  std::string cur;
  for (size_t i = 0; i <= rel.size(); ++i) {
    if (i == rel.size() || rel[i] == '/') {
      if (cur == "..") {
        err = "path must be relative and stay in data dir";
        return false;
      }
      cur.clear();
    } else {
      cur.push_back(rel[i]);
    }
  }
  full = root;
  if (!full.empty() && full[full.size() - 1] != '/')
    full += '/';
  full += rel;
  return true;
}

std::string read_file_limited(const std::string& path, size_t maxn) {
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in)
    return "";
  std::string s;
  s.resize(maxn);
  in.read(&s[0], (std::streamsize)maxn);
  s.resize((size_t)in.gcount());
  return s;
}

bool write_file_limited(const std::string& path, const std::string& data, size_t maxn, std::string& err) {
  if (data.size() > maxn) {
    err = "write too large";
    return false;
  }
  std::string::size_type slash = path.rfind('/');
  if (slash != std::string::npos) {
    std::string dir = path.substr(0, slash);
    mkdir(dir.c_str(), 0755);
  }
  std::ofstream out(path.c_str(), std::ios::binary);
  if (!out) {
    err = "open for write failed";
    return false;
  }
  out.write(data.data(), (std::streamsize)data.size());
  return true;
}

static void walk(const std::string& dir, const std::string& prefix,
                 std::vector<std::pair<std::string, std::string> >& out) {
  DIR* d = opendir(dir.c_str());
  if (!d)
    return;
  struct dirent* de;
  while ((de = readdir(d)) != 0) {
    if (de->d_name[0] == '.')
      continue;
    std::string name = de->d_name;
    std::string full = dir + "/" + name;
    std::string rel = prefix.empty() ? name : prefix + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      walk(full, rel, out);
    else if (S_ISREG(st.st_mode)) {
      if (name.size() >= 3 && (name.substr(name.size() - 3) == ".md" || name.substr(name.size() - 4) == ".txt"))
        out.push_back(std::make_pair(rel, read_file_limited(full, 64 * 1024)));
    }
  }
  closedir(d);
}

void list_text_files(const std::string& root, const std::string& prefix,
                     std::vector<std::pair<std::string, std::string> >& out) {
  walk(root, prefix, out);
}
