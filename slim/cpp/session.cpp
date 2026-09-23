#include "session.hpp"

#include "util.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

const size_t kMaxLineBytes = 16 * 1024;   // one turn of a 2048-token model
const size_t kMaxFileBytes = 1024 * 1024; // history is read back in full

std::string now_iso8601() {
  std::time_t t = std::time(0);
  struct tm g;
  gmtime_r(&t, &g);
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g) == 0)
    return std::string("1970-01-01T00:00:00Z");
  return std::string(buf);
}

// mkdir -p for the session directory. A single level is not enough: the caller only
// guarantees data_dir exists.
bool make_dirs(const std::string& dir, std::string& why) {
  if (dir.empty())
    return true;
  std::string path;
  size_t start = 0;
  if (dir[0] == '/') {
    path = "/";
    start = 1;
  }
  while (start <= dir.size()) {
    size_t slash = dir.find('/', start);
    std::string part = dir.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) {
      if (path.empty() || path[path.size() - 1] != '/')
        path += "/";
      path += part;
      if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        why = "cannot create " + path + ": " + std::strerror(errno);
        return false;
      }
    }
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return true;
}

bool read_whole_file(const std::string& path, std::string& out) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  out.clear();
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      return false;
    }
    if (n == 0)
      break;
    if (out.size() + (size_t)n > kMaxFileBytes)
      break;  // a session log this large is not worth loading into a 2048-token prompt
    out.append(buf, (size_t)n);
  }
  close(fd);
  return true;
}

}  // namespace

bool valid_session_name(const std::string& name, std::string& why) {
  if (name.empty()) {
    why = "session name is empty";
    return false;
  }
  if (name.size() > 64) {
    why = "session name is longer than 64 characters";
    return false;
  }
  if (name[0] == '.') {
    why = "session name must not start with a dot";
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.';
    if (!ok) {
      why = std::string("session name may only contain letters, digits, '-', '_' and '.', not '") +
            c + "'";
      return false;
    }
  }
  if (name.find("..") != std::string::npos) {
    why = "session name must not contain '..'";
    return false;
  }
  return true;
}

std::string session_path(const std::string& data_dir, const std::string& name) {
  std::string dir = data_dir;
  while (dir.size() > 1 && dir[dir.size() - 1] == '/')
    dir.resize(dir.size() - 1);
  return dir + "/sessions/" + name + ".jsonl";
}

bool session_append(const std::string& path, const std::string& role, const std::string& text,
                    std::string& why) {
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && !make_dirs(path.substr(0, slash), why))
    return false;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    why = "cannot append to " + path + ": " + std::strerror(errno);
    return false;
  }
  std::ostringstream line;
  line << "{\"ts\":\"" << json_escape(now_iso8601()) << "\",\"role\":\"" << json_escape(role)
       << "\",\"text\":\"" << json_escape(text) << "\"}\n";
  std::string bytes = line.str();
  size_t done = 0;
  while (done < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      why = "cannot write to " + path + ": " + std::strerror(errno);
      close(fd);
      return false;
    }
    done += (size_t)n;
  }
  close(fd);
  return true;
}

std::vector<Turn> session_load(const std::string& path, int limit) {
  std::vector<Turn> out;
  std::string raw;
  if (!read_whole_file(path, raw))
    return out;
  std::vector<Turn> all;
  size_t start = 0;
  while (start < raw.size()) {
    size_t nl = raw.find('\n', start);
    std::string line = raw.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    start = (nl == std::string::npos) ? raw.size() : nl + 1;
    if (line.empty())
      continue;
    if (line.size() > kMaxLineBytes)
      continue;  // a line this long is not a turn we wrote
    std::string role, text, ts;
    if (!json_extract_string(line, "role", role) || !json_extract_string(line, "text", text))
      continue;  // tolerate a truncated or hand-edited line
    json_extract_string(line, "ts", ts);
    Turn t;
    // json_extract_string already decodes the escapes, so nothing is unescaped twice.
    t.role = role;
    t.text = text;
    t.ts = ts;
    all.push_back(t);
  }
  size_t want = (limit < 0) ? all.size() : (size_t)limit;  // <0: everything, 0: nothing
  size_t from = all.size() > want ? all.size() - want : 0;
  for (size_t i = from; i < all.size(); ++i)
    out.push_back(all[i]);
  return out;
}

std::string json_unescape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\' || i + 1 >= in.size()) {
      out += in[i];
      continue;
    }
    char c = in[++i];
    switch (c) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'u':
        // We never write \u escapes (text is stored as raw UTF-8), but a hand-edited
        // file might: consume the four hex digits so they do not leak into the text.
        i += (i + 4 < in.size()) ? 4 : (in.size() - i - 1);
        break;
      default:
        // Anything else: keep the character rather than the escape, so text never
        // silently disappears.
        out += c;
        break;
    }
  }
  return out;
}

void list_session_files(const std::string& data_dir,
                        std::vector<std::pair<std::string, std::string> >& out) {
  const std::string dir = data_dir + "/sessions";
  DIR* d = opendir(dir.c_str());
  if (!d)
    return;
  struct dirent* de;
  while ((de = readdir(d)) != 0) {
    if (!has_suffix(de->d_name, ".jsonl"))
      continue;
    out.push_back(std::make_pair(std::string("session/") + de->d_name,
                                 read_file_limited(dir + "/" + de->d_name, 64 * 1024)));
  }
  closedir(d);
}

std::string memory_path(const std::string& data_dir) {
  return data_dir + "/memory.md";
}

bool memory_append(const std::string& data_dir, const std::string& text, std::string& why) {
  if (text.empty()) {
    why = "nothing to remember";
    return false;
  }
  if (!make_dirs(data_dir, why))
    return false;
  int fd = open(memory_path(data_dir).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    why = "cannot write to " + memory_path(data_dir) + ": " + std::strerror(errno);
    return false;
  }
  std::string bytes = text + "\n";
  size_t done = 0;
  while (done < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      why = "cannot write to " + memory_path(data_dir) + ": " + std::strerror(errno);
      close(fd);
      return false;
    }
    done += (size_t)n;
  }
  close(fd);
  return true;
}

std::string memory_load(const std::string& data_dir, size_t max_chars) {
  return read_file_limited(memory_path(data_dir), max_chars);
}
