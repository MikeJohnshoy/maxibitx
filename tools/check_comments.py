#!/usr/bin/env python3
"""check_comments.py - flag source comments that break docs/code_comments.md.

The policy: code comments say what the code does now; history, bug stories
and measurements live in docs/. This script lists comment lines that look
like history, plus comment blocks long enough to be a derivation that
belongs in a doc. It's advisory - every hit is a suspect to look at, not
an error - and it only reads comments, never code or strings.

It also checks that a source file's header names the file itself. A file
whose header names a *different* source file has almost always been
overwritten by another file's contents (an upload under the wrong name),
and that is reported as an error.

Usage:
    tools/check_comments.py            files changed vs origin/main (committed
                                       or not); all of src/ if git can't tell
    tools/check_comments.py --all      every .c/.h under src/
    tools/check_comments.py FILE...    just these files

The header check always runs over every file under src/, whichever files
are being checked for comments.

Exit status: 1 if any header names the wrong file, else 0 (with --strict,
1 if anything at all was flagged).
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Each pattern is a history smell: (regex, short reason). Case-insensitive.
SUSPECTS = [
    (r"\b(used to|previously|formerly|no longer|any ?more)\b", "describes a past state"),
    (r"\b(earlier|older|original|old|previous|first) (version|scheme|design|code|value|build|revision|attempt|fix)\b",
     "refers to an earlier version"),
    (r"\bwas (changed|replaced|removed|added|moved|fixed|renamed|reverted|tried)\b", "narrates a change"),
    (r"\b(is|are) now\b|\bnow (uses|does|reads|runs|lives|sets|takes)\b|\bas of\b", "narrates a change ('now')"),
    (r"\b(i|we) (tried|found|noticed|discovered|measured|saw)\b", "tells a story"),
    (r"\bon[- ]air (report|test)\b|\bbench[- ](confirmed|verified|measured)\b", "test history"),
    (r"\b20\d\d-\d\d(-\d\d)?\b", "contains a date"),
    (r"\bbug\b", "mentions a bug"),
    (r"\bminibitx\b", "old project name"),
    (r"\bstep \d+\b", "build-order step without a doc pointer"),
]
SUSPECTS = [(re.compile(p, re.IGNORECASE), why) for p, why in SUSPECTS]

# A line that points at a doc section is the sanctioned way to reference
# history ("ARCHITECTURE.md §10 step 8"), so doc pointers aren't flagged.
DOC_POINTER = re.compile(r"\.md\b|§")

LONG_BLOCK_LINES = 30          # a comment block this long is probably a derivation
SOURCE_NAME = re.compile(r"\b[\w-]+\.(c|h|py)\b")
HEADER_LINES = 5               # how far into a file to look for its own name


def comment_lines(text):
    """Yields (line_number, comment_text, block_id) for every line of every
    comment in C source, skipping string and character literals."""
    i, n, line, block = 0, len(text), 1, 0
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in "\"'":
            q = c
            i += 1
            while i < n and text[i] != q and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            yield line, text[i + 2:j], ("line", line)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j
            block += 1
            for k, part in enumerate(text[i + 2:j].split("\n")):
                yield line + k, part, ("block", block)
            line += text.count("\n", i, j)
            i = j + 2
        else:
            i += 1


def blocks(comments):
    """Groups comment lines into blocks: one /* */ comment, or a run of //
    lines on consecutive source lines."""
    out, cur, last = [], [], None
    for ln, body, bid in comments:
        if bid[0] == "block":
            key = bid
        else:
            key = ("run",)
        if cur and (key != last or (key == ("run",) and ln != cur[-1][0] + 1)):
            out.append(cur)
            cur = []
        cur.append((ln, body))
        last = key
    if cur:
        out.append(cur)
    return out


def check_header(path, text):
    """Returns an error string if the file's header names another source file."""
    own = os.path.basename(path)
    for line in text.splitlines()[:HEADER_LINES]:
        names = [m.group(0) for m in SOURCE_NAME.finditer(line)]
        if own in names:
            return None
        prefixes = ("#",) if path.endswith(".py") else ("//", "/*", "*")
        if names and line.lstrip().startswith(prefixes):
            return "header names %s, not %s - overwritten by another file?" % (names[0], own)
    return None


def check_file(path):
    rel = os.path.relpath(path, ROOT)
    try:
        text = open(path, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
    except OSError as e:
        return [("ERROR", rel, 0, str(e))], 1

    errors = 0
    hits = []
    err = check_header(path, text)
    if err:
        hits.append(("ERROR", rel, 1, err))
        errors += 1

    if not path.endswith((".c", ".h")):
        return hits, errors

    # A generated file's comments aren't hand-written prose that can drift
    # from the code it sits next to, so the heuristics below don't apply: its
    # header describes its own provenance and any long block is a table its
    # generator emits alongside the values. The header check above still
    # runs - a generated file naming the wrong file is a real bug in
    # whatever generated it. The marker must appear near the top, so a
    # passing mention further down can't exempt a hand-written file.
    if "GENERATED FILE" in "\n".join(text.split("\n")[:10]):
        return hits, errors

    comments = list(comment_lines(text))
    for ln, body in ((c[0], c[1]) for c in comments):
        if DOC_POINTER.search(body):
            continue
        for rx, why in SUSPECTS:
            if rx.search(body):
                hits.append(("history", rel, ln, "%s: %s" % (why, body.strip())))
                break

    for blk in blocks(comments):
        if len(blk) >= LONG_BLOCK_LINES:
            hits.append(("long", rel, blk[0][0],
                         "%d-line comment - a derivation or history that belongs in docs?" % len(blk)))
    return hits, errors


def changed_files():
    """Source files that differ from origin/main, committed or not."""
    try:
        out = subprocess.run(["git", "diff", "--name-only", "origin/main", "--"],
                             cwd=ROOT, capture_output=True, text=True, check=True).stdout
        new = subprocess.run(["git", "ls-files", "--others", "--exclude-standard"],
                             cwd=ROOT, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    names = [n for n in (out + new).split() if n.endswith((".c", ".h", ".py"))]
    return [os.path.join(ROOT, n) for n in names if os.path.exists(os.path.join(ROOT, n))]


def all_sources():
    out = []
    for d, _, files in os.walk(os.path.join(ROOT, "src")):
        out += [os.path.join(d, f) for f in files if f.endswith((".c", ".h"))]
    return sorted(out)


def main(argv):
    strict = "--strict" in argv
    args = [a for a in argv if not a.startswith("--")]
    if args:
        files = [os.path.abspath(a) for a in args]
    elif "--all" in argv:
        files = all_sources()
    else:
        files = changed_files()
        if files is None:
            print("check_comments: git unavailable, checking all of src/")
            files = all_sources()
        elif not files:
            print("check_comments: no source files changed vs origin/main")

    total_errors, total_hits = 0, 0
    # The header check is cheap, so run it over every source file, not
    # just the ones being checked for comments.
    checked = set(files)
    for path in all_sources():
        if path not in checked:
            err = check_header(path, open(path, encoding="utf-8", errors="replace").read())
            if err:
                total_errors += 1
                print("%s:1: [ERROR] %s" % (os.path.relpath(path, ROOT), err))
    for path in files:
        hits, errors = check_file(path)
        total_errors += errors
        total_hits += len(hits)
        for kind, rel, ln, msg in hits:
            print("%s:%d: [%s] %s" % (rel, ln, kind, msg))

    print("check_comments: %d file(s), %d flagged, %d error(s)" % (len(files), total_hits, total_errors))
    if total_errors or (strict and total_hits):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
