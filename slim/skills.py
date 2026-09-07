"""Load markdown skills as prompt text. Does not execute scripts."""
import os


def load_skills(root, limit=8, max_chars=1200):
    out = []
    if not os.path.isdir(root):
        return out
    names = sorted(n for n in os.listdir(root) if n.endswith(".md"))
    for name in names[:limit]:
        path = os.path.join(root, name)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            body = fh.read(max_chars)
        out.append({"name": name, "body": body})
    return out


def format_skills(skills):
    if not skills:
        return ""
    parts = ["Available skills (markdown only, do not execute):"]
    for s in skills:
        parts.append("### %s\n%s" % (s["name"], s["body"]))
    return "\n\n".join(parts)
