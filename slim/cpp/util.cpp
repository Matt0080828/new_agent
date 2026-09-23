#include "util.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>

bool has_suffix(const std::string& s, const std::string& suffix) {
  if (suffix.empty())
    return true;
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string cap_prompt(const std::string& s, size_t max_chars) {
  if (s.size() <= max_chars)
    return s;
  return s.substr(0, max_chars) + "\n[truncated]";
}

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

static bool hex4(const std::string& src, size_t i, unsigned& out) {
  if (i + 4 > src.size())
    return false;
  unsigned v = 0;
  for (int k = 0; k < 4; ++k) {
    char h = src[i + k];
    unsigned d;
    if (h >= '0' && h <= '9')
      d = (unsigned)(h - '0');
    else if (h >= 'a' && h <= 'f')
      d = (unsigned)(h - 'a' + 10);
    else if (h >= 'A' && h <= 'F')
      d = (unsigned)(h - 'A' + 10);
    else
      return false;
    v = v * 16 + d;
  }
  out = v;
  return true;
}

static void append_utf8(std::string& out, unsigned code) {
  if (code < 0x80) {
    out.push_back((char)code);
  } else if (code < 0x800) {
    out.push_back((char)(0xC0 | (code >> 6)));
    out.push_back((char)(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back((char)(0xE0 | (code >> 12)));
    out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (code & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | (code >> 18)));
    out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (code & 0x3F)));
  }
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
      else if (e == 'u') {
        // \uXXXX is one character. Servers that escape non-ASCII (Python's json.dumps
        // does by default) would otherwise arrive as the literal text "u6eab" instead
        // of the character, because the backslash is what makes it an escape.
        unsigned code = 0;
        if (hex4(src, i, code)) {
          i += 4;
          if (code >= 0xD800 && code <= 0xDBFF && i + 1 < src.size() && src[i] == '\\' &&
              src[i + 1] == 'u') {
            unsigned low = 0;
            if (hex4(src, i + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
              i += 6;
            }
          }
          append_utf8(out, code);
        }
      } else {
        out.push_back(e);
      }
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
      // Regression: this used to be an inline substr(name.size() - 4) behind a
      // size() >= 3 guard, so a 3-character file name underflowed size_t and
      // killed the process with std::out_of_range on ingest and on every search.
      if (has_suffix(name, ".md") || has_suffix(name, ".txt"))
        out.push_back(std::make_pair(rel, read_file_limited(full, 64 * 1024)));
    }
  }
  closedir(d);
}

void list_text_files(const std::string& root, const std::string& prefix,
                     std::vector<std::pair<std::string, std::string> >& out) {
  walk(root, prefix, out);
}

std::string format_skills(const std::string& root, int limit, int max_chars) {
  DIR* d = opendir(root.c_str());
  if (!d)
    return "";
  std::vector<std::string> names;
  struct dirent* de;
  while ((de = readdir(d)) != 0) {
    if (has_suffix(de->d_name, ".md"))
      names.push_back(de->d_name);
  }
  closedir(d);
  if (names.empty())
    return "";
  std::sort(names.begin(), names.end());
  if ((int)names.size() > limit)
    names.resize((size_t)limit);
  std::ostringstream o;
  o << "Available skills (markdown only, do not execute):";
  for (size_t i = 0; i < names.size(); ++i)
    o << "\n### " << names[i] << "\n" << read_file_limited(root + "/" + names[i], (size_t)max_chars);
  return o.str();
}

// ---- added: running a command without a shell (see run_argv_capture) ----

namespace {

std::vector<std::string> split_search_path(const std::string& p) {
  std::vector<std::string> dirs;
  std::string cur;
  for (size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == ':') {
      if (!cur.empty())
        dirs.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(p[i]);
    }
  }
  return dirs;
}

bool is_executable_file(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  if (!S_ISREG(st.st_mode))
    return false;
  return access(path.c_str(), X_OK) == 0;
}

std::string num_str(long n) {
  std::ostringstream o;
  o << n;
  return o.str();
}

std::string strip_trailing_newlines(const std::string& s) {
  std::string out = s;
  while (!out.empty() && (out[out.size() - 1] == '\n' || out[out.size() - 1] == '\r'))
    out.resize(out.size() - 1);
  return out;
}

}  // namespace

bool string_in_list(const std::vector<std::string>& list, const std::string& s) {
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i] == s)
      return true;
  }
  return false;
}

bool bare_command_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..")
    return false;
  if (name.find('/') != std::string::npos)
    return false;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '\n' || name[i] == '\r')
      return false;
  }
  return true;
}

std::string find_command(const std::string& name, const std::string& search_path) {
  if (!bare_command_name(name))
    return std::string();
  std::string all = search_path.empty()
                        ? std::string("/bin:/usr/bin:/sbin:/usr/sbin:/usr/local/bin")
                        : search_path;
  std::vector<std::string> dirs = split_search_path(all);
  for (size_t i = 0; i < dirs.size(); ++i) {
    std::string cand = dirs[i] + "/" + name;
    if (is_executable_file(cand))
      return cand;
  }
  return std::string();
}

std::vector<std::string> load_name_list(const std::string& path) {
  std::vector<std::string> names;
  std::ifstream in(path.c_str());
  if (!in)
    return names;  // no file means nothing is permitted, never everything
  std::string line;
  while (std::getline(in, line)) {
    std::string s = line;
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r'))
      s.erase(0, 1);
    while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t' ||
                          s[s.size() - 1] == '\r'))
      s.resize(s.size() - 1);
    if (s.empty() || s[0] == '#')
      continue;
    names.push_back(s);
  }
  return names;
}

bool run_argv_capture(const std::vector<std::string>& argv, const std::string& search_path,
                      int timeout_sec, size_t max_bytes, std::string& out, int& status,
                      std::string& err) {
  out.clear();
  status = -1;
  err.clear();
  if (argv.empty()) {
    err = "no command";
    return false;
  }
  std::string path = find_command(argv[0], search_path);
  if (path.empty()) {
    err = "command not found: " + argv[0];
    return false;
  }

  std::vector<char*> c_argv;
  c_argv.push_back(const_cast<char*>(path.c_str()));
  for (size_t i = 1; i < argv.size(); ++i)
    c_argv.push_back(const_cast<char*>(argv[i].c_str()));
  c_argv.push_back(0);

  int fds[2];
  if (pipe(fds) != 0) {
    err = std::string("pipe failed: ") + std::strerror(errno);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    err = std::string("fork failed: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    // Child: both output streams into the pipe, stdin from /dev/null so a command that reads
    // stdin cannot eat the agent's own input.
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, 0);
      close(devnull);
    }
    close(fds[0]);
    close(fds[1]);
    execv(path.c_str(), &c_argv[0]);
    _exit(127);  // reached only when execv failed
  }

  close(fds[1]);
  bool truncated = false;
  bool timed_out = false;
  struct timeval start;
  gettimeofday(&start, 0);
  for (;;) {
    struct timeval now;
    gettimeofday(&now, 0);
    double elapsed =
        (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_usec - start.tv_usec) / 1000000.0;
    if (timeout_sec > 0 && elapsed >= (double)timeout_sec) {
      timed_out = true;
      break;
    }
    double left = timeout_sec > 0 ? ((double)timeout_sec - elapsed) : 1.0;
    struct timeval tv;
    tv.tv_sec = (time_t)left;
    tv.tv_usec = (suseconds_t)((left - (double)tv.tv_sec) * 1000000.0);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);
    int ready = select(fds[0] + 1, &rfds, 0, 0, &tv);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ready == 0)
      continue;  // no data yet: the loop re-checks the deadline
    char buf[4096];
    ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (n == 0)
      break;  // EOF: the child closed the pipe
    if (out.size() < max_bytes) {
      size_t room = max_bytes - out.size();
      size_t take = ((size_t)n < room) ? (size_t)n : room;
      out.append(buf, take);
      if (take < (size_t)n)
        truncated = true;
    } else {
      truncated = true;
    }
  }
  close(fds[0]);

  if (timed_out)
    kill(pid, SIGKILL);
  int wait_status = 0;
  while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(wait_status))
    status = WEXITSTATUS(wait_status);
  else if (WIFSIGNALED(wait_status))
    status = 128 + WTERMSIG(wait_status);

  if (timed_out) {
    err = "command timed out after " + num_str((long)timeout_sec) + "s";
    return false;
  }
  out = strip_trailing_newlines(out);
  if (truncated)
    out += "\n[output truncated at " + num_str((long)max_bytes) + " bytes]";
  return true;
}
