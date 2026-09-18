"""Bounded retry for the HTTP transport. Stdlib only.

Only read-ish calls are retried. The completion request is safe to repeat (worst
case it costs another completion); a mutation such as mqtt_publish is sent once,
because a retry after an ambiguous failure can duplicate the effect.

Rules:
  * retry transport errors (no response), 429 and 5xx
  * never retry a permanent/config error (bad URL, 4xx other than 429)
  * never retry an exception type this module does not recognise (fail-closed)
  * bound total attempts, per-sleep backoff, and the overall wall-clock budget
"""
import time
import urllib.error

DEFAULT_ATTEMPTS = 3
DEFAULT_BASE_DELAY = 0.5      # seconds before the second try
DEFAULT_MAX_DELAY = 4.0       # per-sleep cap
DEFAULT_MAX_TOTAL = 20.0      # overall budget for one logical request


def is_retryable_status(status):
    """429 and 5xx are worth another try."""
    return status == 429 or 500 <= status <= 599


def is_retryable_exception(exc):
    """Classify an exception. Unknown types are not retried (fail-closed)."""
    if isinstance(exc, urllib.error.HTTPError):
        return is_retryable_status(exc.code)
    if isinstance(exc, urllib.error.URLError):
        reason = getattr(exc, "reason", None)
        if isinstance(reason, str) and "unknown url type" in reason:
            return False  # https:// or a malformed URL: retrying cannot help
        if isinstance(reason, (ValueError,)):
            return False
        return True
    if isinstance(exc, OSError):
        return True  # connection reset, timeout, broken pipe: often transient
    return False


def retry_delay_seconds(attempt_index, base_delay=DEFAULT_BASE_DELAY, max_delay=DEFAULT_MAX_DELAY):
    """Delay after attempt N (1-based): base, 2*base, ... capped. No jitter."""
    if attempt_index < 1:
        attempt_index = 1
    delay = base_delay * (2 ** (attempt_index - 1))
    if max_delay is not None and max_delay > 0:
        delay = min(delay, max_delay)
    return max(0.0, delay)


def run_with_retry(fn, attempts=DEFAULT_ATTEMPTS, base_delay=DEFAULT_BASE_DELAY,
                   max_delay=DEFAULT_MAX_DELAY, max_total=DEFAULT_MAX_TOTAL,
                   sleep=time.sleep, clock=time.monotonic, log=None, log_prefix="[slim] http"):
    """Call fn() until it succeeds or the policy gives up; re-raise the last error.

    sleep and clock are injectable so tests stay deterministic.
    """
    attempts = max(1, int(attempts))
    started = clock()
    last = None
    for i in range(1, attempts + 1):
        try:
            result = fn()
        except Exception as exc:  # noqa: BLE001 - classification decides
            last = exc
            retryable = is_retryable_exception(exc)
            if log:
                log("%s attempt %d/%d error=%s: %s %s"
                    % (log_prefix, i, attempts, type(exc).__name__, exc,
                       "retryable" if retryable else "final"))
            if not retryable or i == attempts:
                raise
            delay = retry_delay_seconds(i, base_delay, max_delay)
            if max_total is not None and max_total > 0 and (clock() - started) + delay > max_total:
                if log:
                    log("%s giving up: %.1fs budget would be exceeded" % (log_prefix, max_total))
                raise
            sleep(delay)
        else:
            if log:
                log("%s attempt %d/%d ok" % (log_prefix, i, attempts))
            return result
    raise last if last else RuntimeError("retry loop exited without result")
