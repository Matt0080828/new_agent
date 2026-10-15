"""Network helpers shared by the slim clients: credential policy and redirect safety.

Stdlib only, like the rest of the Python client.

Two rules, applied wherever a bearer token is attached:

  * a token is never put on a plaintext ``http://`` URL unless the operator
    explicitly accepts that (``--allow-plaintext-key`` / ``SLIM_ALLOW_PLAINTEXT_KEY=1``);
  * a redirect must not carry the token to a different origin, or down from
    ``https://`` to ``http://``.
"""
import urllib.parse
import urllib.request


def is_tls(url):
    """True for an https:// URL (the only scheme that protects the token)."""
    return (url or "").lower().startswith("https://")


def auth_header(url, token, allow_plaintext=False, label="credential"):
    """Return an ``Authorization`` header for ``token``, or ``{}`` when there is none.

    Refuses to send a token over plaintext http:// unless the operator opted in: a
    key on the wire is worse than no key at all.
    """
    if not token:
        return {}
    if not is_tls(url) and not allow_plaintext:
        raise ValueError(
            "%s would go over plaintext http:// to %s; use https, or pass "
            "--allow-plaintext-key / SLIM_ALLOW_PLAINTEXT_KEY=1 to accept the risk"
            % (label, url))
    return {"Authorization": "Bearer " + token}


class _CredentialSafeRedirect(urllib.request.HTTPRedirectHandler):
    """Drop the Authorization header when a redirect leaves the origin or downgrades."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = urllib.request.HTTPRedirectHandler.redirect_request(
            self, req, fp, code, msg, headers, newurl)
        if new is None:
            return None
        old = urllib.parse.urlsplit(req.full_url)
        tgt = urllib.parse.urlsplit(newurl)
        if (old.scheme, old.hostname, old.port) != (tgt.scheme, tgt.hostname, tgt.port):
            new.headers.pop("Authorization", None)
        if old.scheme == "https" and tgt.scheme != "https":
            new.headers.pop("Authorization", None)
        return new


_opener = urllib.request.build_opener(_CredentialSafeRedirect)
#: Use this instead of urllib.request.urlopen so redirects stay credential-safe.
open_url = _opener.open
