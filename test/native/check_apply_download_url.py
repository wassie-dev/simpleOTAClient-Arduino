#!/usr/bin/env python3
"""Fail if apply() builds the firmware GET from the configured server base.

check() and status posts use that base. The download must keep using the
pre-signed offer URL stored in _url.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "SimpleOTAClient.cpp"
SIGNATURE = "OTAResult SimpleOTAClient::apply()"
FORBIDDEN = ("setServerUrl", "_serverUrl", "sotaBuildApiUrl")


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

    begins = re.findall(r"http\.begin\s*\(\s*client\s*,\s*_url\s*\)", code)
    if len(begins) != 1:
        print("apply() must GET the offer URL via http.begin(client, _url)",
              file=sys.stderr)
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
