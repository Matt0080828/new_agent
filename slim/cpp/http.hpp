#ifndef SLIM_HTTP_HPP
#define SLIM_HTTP_HPP

#include <string>

struct HttpResult {
  int status;
  std::string body;
  std::string error;
};

HttpResult http_post_json(const std::string& url, const std::string& json,
                          const std::string& bearer, int timeout_sec);

// Bounded retry for the transport layer.
//
// Only read-ish calls are retried: the completion request is safe to repeat
// (worst case it costs another completion), while mqtt_publish is a mutation and
// is deliberately sent once. Retrying a publish could duplicate the effect.
struct RetryPolicy {
  int attempts;       // total tries, >= 1
  int base_delay_ms;  // delay before the second try
  int max_delay_ms;   // per-sleep cap
  RetryPolicy() : attempts(3), base_delay_ms(500), max_delay_ms(4000) {}
};

// status 0 (transport error), 429 and 5xx are worth another try; other 4xx and
// negative statuses (permanent/config errors) are not.
bool is_retryable_status(int status);

// attempt_index is 1-based for the sleep *after* that attempt: 1 -> base,
// 2 -> 2*base, ... capped at max_delay_ms. No jitter, so it is testable.
int retry_delay_ms(int attempt_index, const RetryPolicy& p);

typedef bool (*RetryAttemptFn)(void* ctx, HttpResult* out);
typedef void (*SleepFn)(int ms);

// Calls fn up to policy.attempts times, sleeping between tries. sleeper == 0 uses
// nanosleep. Appends one audit line per attempt to *log when log != 0.
// Returns true when the final attempt was not retryable (success or permanent).
bool run_with_retry(const RetryPolicy& policy, RetryAttemptFn fn, void* ctx, HttpResult* out,
                    SleepFn sleeper, std::string* log);

// http_post_json with the retry policy applied.
HttpResult http_post_json_retry(const std::string& url, const std::string& json,
                                const std::string& bearer, int timeout_sec,
                                const RetryPolicy& policy, bool verbose);

#endif
