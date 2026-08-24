#!/usr/bin/env python3
"""Checks the comment conventions from CLAUDE.md; see that file for the rules."""
import ast, io, os, re, subprocess, sys, tokenize, unicodedata

# CubeMX and vendor files: regeneration overwrites them, so the rules cannot hold there.
GENERATED = re.compile(
    r"(^|/)(LWIP/|third_party/|build[-A-Za-z0-9]*/|\.git/|\.venv/|__pycache__/|\.cache/"
    r"|system_stm32h7xx|stm32h7xx_(hal_conf|hal_msp|it|nucleo_conf|hal_timebase)"
    r"|syscalls\.c|sysmem\.c|freertos\.c|FreeRTOSConfig\.h|startup_"
    r"|starm-clang\.cmake|stm32h755xx_[A-Za-z0-9_]*\.ld)")
# Vector files are emitted by tools/; the generator is the hand-written artifact.
GENERATED_VEC = re.compile(r"Common/test/vectors/.*\.(txt|h)$")
PREFIX = {".c": "//", ".h": "//", ".py": "#", ".sh": "#", ".cmake": "#", ".ld": "/*"}

def touched_lines():
    """Line numbers added or changed against HEAD, per file. None = whole file."""
    diff = subprocess.run(["git", "diff", "-U0", "HEAD"],
                          capture_output=True, text=True).stdout
    hits, cur = {}, None
    for line in diff.split("\n"):
        if line.startswith("+++ b/"):
            cur = line[6:]
            hits.setdefault(cur, set())
        elif line.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if m:
                start, count = int(m.group(1)), int(m.group(2) or 1)
                hits[cur].update(range(start, start + count))
    for f in subprocess.run(["git", "ls-files", "--others", "--exclude-standard"],
                            capture_output=True, text=True).stdout.split():
        hits[f] = None
    return hits

def tracked(changed):
    if changed:
        return [f for f in touched_lines() if os.path.isfile(f)]
    # Untracked too: a new file is invisible to `git ls-files` until it is
    # committed, so the full-tree check was green on exactly the file that had
    # never been checked. The --changed path already did this.
    out = subprocess.run(["git", "ls-files"], capture_output=True,
                         text=True).stdout.split()
    out += subprocess.run(["git", "ls-files", "--others", "--exclude-standard"],
                          capture_output=True, text=True).stdout.split()
    return out

def foreign_letters(text):
    """Letters from a script the repository does not write in, with their names.

    The check was Cyrillic-only for months under a heading that said non-English,
    which is a name broader than its coverage - a CJK character slipped through it
    on 2026-08-22. Latin and Greek stay, because Greek carries units; so do the
    micro and ohm signs, which are letters by category and symbols by intent.
    """
    if "\x00" in text:
        return []
    keep = ("LATIN", "GREEK")
    allow = ("MICRO SIGN", "OHM SIGN", "ANGSTROM SIGN")
    out = []
    for ch in set(text):
        if ord(ch) < 0x80 or not unicodedata.category(ch).startswith("L"):
            continue
        try:
            name = unicodedata.name(ch)
        except ValueError:
            name = "UNNAMED U+%04X" % ord(ch)
        if name.startswith(keep) or name in allow:
            continue
        out.append(name)
    return sorted(set(out))


def kind(path):
    base = os.path.basename(path)
    if base == ".gitignore" or base == "CMakeLists.txt":
        return "#"
    return PREFIX.get(os.path.splitext(path)[1])

def hash_comment_lines(text):
    """Line numbers holding a real # comment. A # inside a string is not one."""
    try:
        toks = tokenize.generate_tokens(io.StringIO(text).readline)
        return {t.start[0] for t in toks if t.type == tokenize.COMMENT}
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return None

def vendor_banner_end(lines):
    """Last line of a leading ** banner, the shape CubeMX writes atop a linker script."""
    if not lines or lines[0].strip() != "/*" or not lines[1:2]:
        return None
    if not lines[1].strip().startswith("**"):
        return None
    for n, l in enumerate(lines[:80], 1):
        if n > 1 and "*/" in l:
            return n
    return None

def handwritten(text):
    """Line numbers a human owns. In a CubeMX file that is the USER CODE regions only."""
    lines = text.split("\n")
    if "USER CODE BEGIN" not in text:
        # A linker script has no markers; the banner on top is still the vendor's.
        end = vendor_banner_end(lines)
        return None if end is None else set(range(end + 1, len(lines) + 1))
    own, inside = set(), False
    for n, l in enumerate(lines, 1):
        if "USER CODE BEGIN" in l:
            # A "Header" region holds the banner CubeMX writes and rewrites.
            inside = "USER CODE BEGIN Header" not in l
        elif "USER CODE END" in l:
            inside = False
        elif inside:
            own.add(n)
    return own

def comment_cols(text):
    """Column of every real # comment, per line. A # inside a string is not one."""
    try:
        toks = tokenize.generate_tokens(io.StringIO(text).readline)
        return {t.start[0]: t.start[1] for t in toks if t.type == tokenize.COMMENT}
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return None

def marker_col(line, pfx):
    """Column where a comment opens, skipping any marker that sits inside a literal."""
    i, q, esc = 0, None, False
    while i < len(line):
        c = line[i]
        if q:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == q:
                q = None
        elif c in "\"'":
            q = c
        elif pfx == "//" and line[i:i + 2] in ("//", "/*"):
            return i
        elif pfx == "/*" and line[i:i + 2] == "/*":
            return i
        elif pfx == "#" and c == "#":
            return i
        i += 1
    return None

def trailing_blocks(path, lines, pfx, cols, mine, only):
    """Comments that follow code on the same line. Each is its own block."""
    out = []
    for n, line in enumerate(lines, 1):
        st = line.strip()
        # A leading * continues a block in C and opens a wildcard in a linker
        # script, so only C may skip it.
        if not st or st.startswith(pfx) or (pfx == "//" and st.startswith(("/*", "*"))):
            continue
        col = cols.get(n) if cols is not None else marker_col(line, pfx)
        if col is None or not line[:col].strip():
            continue
        body = line[col:]
        # The required member form is Doxygen and is measured with the rest of it.
        if body.startswith("/**<"):
            continue
        j = n
        while pfx in ("//", "/*") and body.lstrip().startswith("/*") and "*/" not in body[2:]:
            if j >= len(lines):
                break
            body += " " + lines[j].strip()
            j += 1
        if mine is not None and n not in mine:
            continue
        if only is not None and only.get(path) is not None and n not in only[path]:
            continue
        body = re.sub(r"radio_devices_docs/[A-Za-z0-9_./-]+", "", body).strip()
        if len(body) > 100:
            out.append("%s:%d (%d chars)" % (path, n, len(body)))
    return out

def long_docstrings(path, text, only):
    """A docstring is the Doxygen block of a Python file: its first line is the @brief."""
    out = []
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return out
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.ClassDef,
                                 ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        doc = ast.get_docstring(node, clean=False)
        if not doc:
            continue
        first = re.sub(r"radio_devices_docs/[A-Za-z0-9_./-]+", "", doc.strip().split("\n")[0])
        n = getattr(node, "lineno", 1)
        if len(first) > 100 and (only is None or only.get(path) is None or n in only[path]):
            out.append("%s:%d (%d chars)" % (path, n, len(first)))
    return out

def check(files, only=None):
    long_blocks, own_line, cyrillic, long_brief = [], [], [], []
    long_doc = []
    for p in files:
        if GENERATED.search(p) or GENERATED_VEC.search(p) or not os.path.isfile(p):
            continue
        try:
            text = open(p, errors="replace").read()
        except OSError:
            continue
        # Before kind(): the language rule covers every file, and .md and .ld have
        # no comment prefix, so gating on one skipped them for the rule's whole life.
        if p != "CLAUDE.md":
            foreign = foreign_letters(text)
            if foreign:
                cyrillic.append("%s (%s)" % (p, ", ".join(foreign[:3])))
        pfx = kind(p)
        if pfx is None:
            continue
        if p.endswith(".py"):
            long_doc.extend(long_docstrings(p, text, only))
        mine = handwritten(text)
        # Shell and CMake are not tokenizable; only Python gets the string check.
        hashes = hash_comment_lines(text) if p.endswith(".py") else None
        cols = comment_cols(text) if p.endswith(".py") else None
        long_blocks.extend(trailing_blocks(p, text.split("\n"), pfx, cols, mine, only))
        run, start, incomment = [], 0, False
        for n, line in enumerate(text.split("\n") + [""], 1):
            st = line.strip()
            # A USER CODE marker is structure, not a comment: it must not open a block.
            marker = "USER CODE BEGIN" in st or "USER CODE END" in st
            # A star continues something or it is a dereference.
            iscomment = not marker and (st.startswith(pfx) or (pfx == "//" and
                                        (st.startswith("/*") or
                                         (incomment and st.startswith("*")))))
            # A /* */ body counts to its close, or a continuation dodges the limit
            # by not opening with a star.
            if pfx in ("//", "/*"):
                if incomment:
                    iscomment = True
                if iscomment and "/*" in st and "*/" not in st.split("/*", 1)[1]:
                    incomment = True
                elif incomment and "*/" in st:
                    incomment = False
            if iscomment and hashes is not None and n not in hashes:
                iscomment = False
            if iscomment:
                if not run:
                    start = n
                run.append(st)
                continue
            # A documentation path is exempt: CLAUDE.md allows it in full.
            body = re.sub(r"radio_devices_docs/[A-Za-z0-9_./-]+", "", " ".join(run))
            span = set(range(start, n))
            mineok = run and (mine is None or start in mine)
            onlyok = only is None or only.get(p) is None or (span & only[p])
            # A Doxygen block's limit is per @brief, not per block.
            if mineok and run[0].startswith("/**") and not run[0].startswith("/**<"):
                for off, line in enumerate(run):
                    m = re.search(r"@brief\s+(.*)", line)
                    if m and len(m.group(1).rstrip("*/ ")) > 100 and onlyok:
                        long_brief.append("%s:%d (%d chars)"
                                          % (p, start + off, len(m.group(1).rstrip("*/ "))))
            elif mineok and len(body) > 100 and onlyok:
                long_blocks.append("%s:%d (%d chars)" % (p, start, len(body)))
            run = []
        if p.endswith((".c", ".h")):
            inside = 0
            for n, l in enumerate(text.split("\n"), 1):
                if re.match(r"\s*(typedef\s+)?struct\b.*\{", l):
                    inside = 1
                elif inside and re.match(r"\s*\}", l):
                    inside = 0
                elif inside and re.match(r"\s*(/\*|//|\*)", l):
                    if mine is not None and n not in mine:
                        continue
                    if only is None or only.get(p) is None or n in only[p]:
                        own_line.append("%s:%d" % (p, n))
    return long_blocks, own_line, cyrillic, long_brief, long_doc

# The library, pinned per file, reaching into the submodule. Its item 3.
PORTABLE = {
    "radio_stack/src/grid.c":        {"grid.h"},
    "radio_stack/src/gridmaster.c":  {"gridmaster.h"},
    "radio_stack/src/superframe.c":  {"superframe.h", "grid.h", "timebase.h"},
    "radio_stack/src/beacon.c":      {"beacon.h", "radio_phy.h", "radio_protocol.h",
                                      "radio_slots.h"},
    "radio_stack/src/hop.c":         {"hop.h", "radio_phy.h"},
    # No sha256.h: a hash arrives through kdf.h, never as an implementation.
    "radio_stack/src/exchange.c":    {"exchange.h"},
    "radio_stack/inc/grid.h":        set(),
    "radio_stack/inc/gridmaster.h":  {"grid.h"},
    "radio_stack/inc/superframe.h":  {"grid.h", "radio_slots.h"},
    "radio_stack/inc/beacon.h":      {"superframe.h", "radio_slots.h"},
    "radio_stack/inc/hop.h":         set(),
    "radio_stack/inc/exchange.h":    {"kdf.h", "radio_protocol.h"},
    "radio_stack/inc/kdf.h":         set(),
    # The profile is chosen here, so radio_phy.h and radio_slots.h reach it.
    "radio_stack/inc/radio_phy.h":   {"profile.h", "radio_slots.h"},
    "radio_stack/inc/radio_slots.h": {"profile.h"},
    "radio_stack/profiles/profile.h":          {"profile_ids.h", "profile_asbuilt.h",
                                                "profile_hosttest.h"},
    "radio_stack/profiles/profile_ids.h":      set(),
    "radio_stack/profiles/profile_asbuilt.h":  set(),
    "radio_stack/profiles/profile_hosttest.h": set(),
}
# Freestanding only; <stdio.h> pulls newlib into a file with no part under it.
PORTABLE_ANGLE = {"stddef.h", "stdint.h", "stdbool.h", "string.h", "limits.h"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def check_portable():
    """The include list of the library-to-be, per file rather than as a union.

    A union would permit grid.c a timebase.h it must not have, and a permission
    nobody needs is a permission that gets used.

    A missing file is a failure and not a skip: a file renamed out from under
    this list is otherwise indistinguishable from a file that passes. None
    present at all is another tree, and returns nothing."""
    present = [p for p in PORTABLE if os.path.isfile(p)]
    if not present:
        return []
    bad = []
    for path, allowed in sorted(PORTABLE.items()):
        if not os.path.isfile(path):
            bad.append("%s: MISSING - the list is stale, not the file portable" % path)
            continue
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        own = os.path.basename(path).replace(".c", ".h")
        for bracket, name in INCLUDE_RE.findall(text):
            if bracket == "<":
                if name not in PORTABLE_ANGLE:
                    bad.append("%s: <%s> is not freestanding" % (path, name))
            elif name not in allowed and name != own:
                bad.append("%s: \"%s\" is not on this file's list" % (path, name))
    return bad


def main():
    changed = "--changed" in sys.argv
    files = tracked(changed)
    longb, own, cyr, brief, docs = check(files, touched_lines() if changed else None)
    port = check_portable()
    scope = "lines changed against HEAD" if changed else "every file a human owns"
    print("scope: %s (%d), generated and vendored excluded\n" % (scope, len(files)))
    for title, items in (("non-English outside CLAUDE.md", cyr),
                         ("comment blocks over 100 characters", longb),
                         ("struct-field comments on their own line", own),
                         ("Doxygen @brief over 100 characters", brief),
                         ("Python docstring first line over 100 characters", docs),
                         ("includes outside the link layer's list", port)):
        print("== %s: %d ==" % (title, len(items)))
        for i in items[:15]:
            print("   " + i)
        if len(items) > 15:
            print("   ... and %d more" % (len(items) - 15))
    return 1 if (longb or own or cyr or brief or docs or port) else 0

sys.exit(main())
