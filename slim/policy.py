"""Fail-closed tool policy for the slim agent. Stdlib only.

Two independent ideas, matching the C++ side (slim/cpp/policy.cpp):

1. protected_path(): files a tool may never write, whoever asks. The interesting
   case is agent state: with --db defaulting to <data_dir>/slim.sqlite and
   write_file jailed to <data_dir>, one bad tool call could overwrite the agent's
   own database and lose its memory for good.

2. approve_mutation(): model-initiated writes and outbound publishes need an
   explicit opt-in. No opt-in and no terminal to ask means no.

Tools are classified explicitly: anything the policy does not recognise is
denied rather than allowed, so a future tool cannot become writable by accident.
"""
import os

MUTATING_TOOLS = ("write_file", "mqtt_publish")
READ_ONLY_TOOLS = ("rag_search", "read_file")

# Suffixes that mean "this is agent state, not user content".
_STATE_SUFFIXES = (
    ".sqlite", ".sqlite3", ".db",
    ".sqlite-wal", ".sqlite-journal", ".sqlite-shm",
    ".db-wal", ".db-journal", ".db-shm",
)
_BINARY_NAMES = ("slim-agent", "slim-agent-t830")


def protected_path(rel):
    """Return a reason string when rel must never be written, else None."""
    normalized = (rel or "").replace("\\", "/")
    base = os.path.basename(normalized)
    if not base:
        return "path has no file name"
    # write_file is jailed to data_dir, and sessions live under data_dir: without this
    # rule the model could rewrite its own conversation history.
    parts = [p for p in normalized.split("/") if p not in ("", ".")]
    if "sessions" in parts:
        return ("refusing to write %s: the session store is append-only agent state"
                % normalized)
    for suffix in _STATE_SUFFIXES:
        if base.endswith(suffix):
            return ("refusing to write %s: looks like an agent state/database file (%s)"
                    % (base, suffix))
    if base in _BINARY_NAMES:
        return "refusing to overwrite the agent binary %s" % base
    if len(base) > 1 and base.startswith("."):
        return "refusing to write dotfile %s" % base
    return None


def classify(tool):
    """'mutating', 'read-only' or 'unknown' (unknown is denied, never allowed)."""
    if tool in MUTATING_TOOLS:
        return "mutating"
    if tool in READ_ONLY_TOOLS:
        return "read-only"
    return "unknown"


def tool_allowed(policy, tool):
    if tool == "write_file":
        return bool((policy or {}).get("allow_write"))
    if tool == "mqtt_publish":
        return bool((policy or {}).get("allow_mqtt"))
    return False


def approve_tool(policy, tool, detail="", human=False, input_fn=None):
    """Fail-closed approval. Returns (ok, reason).

    human=True means the operator typed the action themselves.
    """
    kind = classify(tool)
    if kind == "unknown":
        return False, "blocked: tool %s is not classified by policy (fail-closed)" % tool
    if kind == "read-only":
        return True, "read-only tool"
    if human:
        return True, "human-initiated"
    if tool_allowed(policy, tool):
        return True, "allowed by policy"
    if not (policy or {}).get("interactive"):
        return False, ("blocked by policy: model-initiated %s is not enabled "
                       "(use --allow-write / --allow-mqtt, or SLIM_ALLOW_WRITE=1 / "
                       "SLIM_ALLOW_MQTT=1)" % tool)
    ask = input_fn or input
    try:
        answer = ask("approve model tool %s (%s)? [y/N] " % (tool, detail))
    except EOFError:
        return False, "no answer on stdin; denied"
    if answer.strip().lower() in ("y", "yes"):
        return True, "approved at prompt"
    return False, "denied at prompt"


# Backwards-friendly alias used by the C++ naming.
approve_mutation = approve_tool
