#ifndef SLIM_SSE_HPP
#define SLIM_SSE_HPP

#include <string>
#include <vector>

// Incremental server-sent-events parser.
//
// A chunk boundary can fall anywhere - inside a line, inside an event, in the
// middle of "data:". The parser therefore buffers and only emits a payload once
// the terminating blank line has been seen, which is also the bug class that
// makes hand-rolled SSE readers lose or corrupt the first token.
class SseParser {
 public:
  SseParser();
  void feed(const std::string& chunk);
  // Emit a final unterminated event at end of stream (some servers omit the
  // trailing blank line before closing).
  void flush();
  bool next(std::string* payload);  // pop one complete data payload
  bool done() const;                // "data: [DONE]" seen
  void reset();

 private:
  void handle_line(const std::string& line);
  std::string buf_;
  std::string pending_;
  std::vector<std::string> ready_;
  bool done_;
};

// Extracts the assistant text from one payload: choices[0].delta.content for a
// streaming chunk, or message.content if a server answers in one shot.
bool sse_delta_content(const std::string& payload, std::string& out);

// Assembles a whole event-stream body (already read into memory) into the reply
// text. Used when a server answers with SSE even though stream was not requested,
// where a plain "first content" extraction would return only the first delta.
bool sse_assemble(const std::string& raw_body, std::string& out);

#endif
