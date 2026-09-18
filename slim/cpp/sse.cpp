#include "sse.hpp"

#include "util.hpp"

SseParser::SseParser() : done_(false) {}

void SseParser::reset() {
  buf_.clear();
  pending_.clear();
  ready_.clear();
  done_ = false;
}

void SseParser::feed(const std::string& chunk) {
  buf_ += chunk;
  std::string::size_type pos;
  while ((pos = buf_.find('\n')) != std::string::npos) {
    std::string line = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    if (!line.empty() && line[line.size() - 1] == '\r')
      line.resize(line.size() - 1);  // tolerate CRLF
    handle_line(line);
  }
}

void SseParser::flush() {
  // A server may end the stream without a trailing newline or blank line. Process
  // whatever is still buffered as the final line first, otherwise that leftover
  // would sit in the buffer and corrupt whatever is parsed next.
  if (!buf_.empty()) {
    std::string line = buf_;
    buf_.clear();
    if (!line.empty() && line[line.size() - 1] == '\r')
      line.resize(line.size() - 1);
    handle_line(line);
  }
  if (!pending_.empty()) {
    ready_.push_back(pending_);
    pending_.clear();
  }
}

void SseParser::handle_line(const std::string& line) {
  if (line.empty()) {
    // Blank line terminates an event: whatever data lines were collected become
    // one payload (multi-line data joins with '\n', per the SSE spec).
    if (!pending_.empty()) {
      ready_.push_back(pending_);
      pending_.clear();
    }
    return;
  }
  if (line.compare(0, 5, "data:") != 0)
    return;  // comment (":"), event:, id:, retry:, or a line we do not speak
  std::string value = line.substr(5);
  if (!value.empty() && value[0] == ' ')
    value.erase(0, 1);
  if (value == "[DONE]") {
    done_ = true;
    return;
  }
  if (!pending_.empty())
    pending_ += "\n";
  pending_ += value;
}

bool SseParser::next(std::string* payload) {
  if (ready_.empty())
    return false;
  *payload = ready_.front();
  ready_.erase(ready_.begin());
  return true;
}

bool SseParser::done() const { return done_; }

bool sse_delta_content(const std::string& payload, std::string& out) {
  if (payload.empty())
    return false;
  if (json_extract_string(payload, "content", out))
    return !out.empty();
  return false;
}

bool sse_assemble(const std::string& raw_body, std::string& out) {
  out.clear();
  if (raw_body.empty())
    return false;
  SseParser parser;
  parser.feed(raw_body);
  parser.flush();
  std::string payload, delta;
  while (parser.next(&payload)) {
    if (sse_delta_content(payload, delta) && !delta.empty())
      out += delta;
  }
  return !out.empty();
}
