#!/usr/bin/env python3
"""Fail if apply() builds the firmware GET from the configured server base.

check() and status posts use that base. The download must keep using the
pre-signed offer URL stored in _url. http.begin may pass that String or an
expression drawn from it, such as _url.c_str().
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "SimpleOTAClient.cpp"
SIGNATURE = "OTAResult SimpleOTAClient::apply()"
FORBIDDEN = (
    "setServerUrl",
    "_serverUrl",
    "sotaBuildApiUrl",
    "sotaEffectiveServerUrl",
    "kSotaDefaultServerUrl",
)


def strip_code(src, begin, end):
    """Code from begin to end, with comments and string literals removed."""
    i = begin
    out = []
    while i < end:
        c = src[i]
        nxt = src[i + 1] if i + 1 < end else ""
        if c == '"':
            i += 1
            while i < end and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
            i += 1
            out.append('""')
        elif c == "'":
            i += 1
            while i < end and src[i] != "'":
                i += 2 if src[i] == "\\" else 1
            i += 1
            out.append("''")
        elif c == "/" and nxt == "/":
            i = src.find("\n", i)
            if i < 0 or i >= end:
                break
        elif c == "/" and nxt == "*":
            i = src.find("*/", i + 2)
            if i < 0 or i + 2 > end:
                sys.exit("unterminated comment in apply()")
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def function_span(src):
    start = src.find(SIGNATURE)
    if start < 0:
        sys.exit("apply() not found")
    open_brace = src.find("{", start)
    if open_brace < 0:
        sys.exit("apply() has no body")
    depth = 0
    i = open_brace
    n = len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == '"':
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
            i += 1
        elif c == "'":
            i += 1
            while i < n and src[i] != "'":
                i += 2 if src[i] == "\\" else 1
            i += 1
        elif c == "/" and nxt == "/":
            i = src.find("\n", i)
            if i < 0:
                sys.exit("apply() unterminated")
        elif c == "/" and nxt == "*":
            i = src.find("*/", i + 2)
            if i < 0:
                sys.exit("unterminated comment in apply()")
            i += 2
        elif c == "{":
            depth += 1
            i += 1
        elif c == "}":
            depth -= 1
            i += 1
            if depth == 0:
                return open_brace, i
        else:
            i += 1
    sys.exit("apply() unterminated")


def split_args(arglist):
    """Split on commas that are not inside parentheses."""
    args = []
    depth = 0
    start = 0
    for i, c in enumerate(arglist):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            args.append(arglist[start:i].strip())
            start = i + 1
    args.append(arglist[start:].strip())
    if len(args) == 1 and args[0] == "":
        return []
    return args


def matching_paren(src, open_at):
    depth = 0
    i = open_at
    while i < len(src):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def begin_url_arguments(code):
    """URL argument of each http.begin: the sole argument, or the one after the client."""
    found = []
    for match in re.finditer(r"\bhttp\s*\.\s*begin\s*\(", code):
        open_at = match.end() - 1
        close_at = matching_paren(code, open_at)
        if close_at < 0:
            found.append(None)
            continue
        args = split_args(code[open_at + 1:close_at])
        if len(args) == 1:
            found.append(args[0])
        elif len(args) >= 2:
            found.append(args[1])
        else:
            found.append("")
    return found


def uses_offer_url(expr):
    return expr is not None and re.search(r"\b_url\b", expr) is not None


def main():
    src = SOURCE.read_text()
    begin, end = function_span(src)
    code = strip_code(src, begin, end)
    if len(code) < 500:
        sys.exit("apply() body looks truncated (%d chars)" % len(code))

    failed = False
    for name in FORBIDDEN:
        if re.search(r"\b%s\b" % re.escape(name), code):
            print("apply() references %s" % name, file=sys.stderr)
            failed = True

    urls = begin_url_arguments(code)
    if len(urls) != 1 or not uses_offer_url(urls[0] if urls else None):
        shown = ", ".join(repr(u) for u in urls) if urls else "none"
        print("apply() must GET the offer URL via http.begin from _url "
              "(got %s)" % shown, file=sys.stderr)
        failed = True
    if not re.search(r"http\.GET\s*\(", code):
        print("apply() does not GET the firmware", file=sys.stderr)
        failed = True

    assigns = re.findall(r"_url\s*=\s*([^;]*);", code)
    if assigns != ['""']:
        print("apply() must not replace _url (assignments: %s)" % assigns,
              file=sys.stderr)
        failed = True

    if failed:
        sys.exit(1)
    print("apply() downloads the offer URL, not the configured server base")


if __name__ == "__main__":
    main()
