#!/usr/bin/env python3
"""Checks that what the documentation names still exists in the code.

Every stale figure found this week was found by a grep aimed at something else:
a comment sweep, a constant sweep, a rename sweep. Nothing looked at prose on
purpose, and the pages are where the reasoning lives.

This does not check that a claim is true - no tool can. It checks the weaker
thing that is mechanical: an identifier or a path a page names must still be
findable, because a page describing symbols that no longer exist is a page
nobody can act on.
"""

import os
import argparse
import re
import subprocess
import sys

DOCS = "../radio_devices_docs"
# Pages naming this side's symbols. wl55_device/ is the other session's half.
SCOPES = ("radio", "open_hub")
ALLOW = "tools/docs_allow.txt"
# Inside an expression or a fence, only these read as claims about this tree.
OWNED = ("RADIO_", "IPC_", "KV_", "RFM_", "rfm69_", "radio_", "pairing_",
         "kv_", "ks_", "aead_", "hop_", "store_", "timebase_", "cli_")
# Symbols owned by somebody else's build; naming them is not a claim about this tree.
FOREIGN = ("mbedtls_", "altcp_", "lwip", "sys_arch", "HAL_", "os", "xQueue", "pd",
           "ExternalProject_", "VP_", "FREERTOS_", "tcp_", "netif_", "MX_")

IDENT = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)(?:\(\)|\(|`)")
MACRO = re.compile(r"`(RADIO_[A-Z0-9_]+|IPC_[A-Z0-9_]+|KV_[A-Z0-9_]+)`")
# Only a path is a claim about where something lives; a bare file name is prose.
PATH = re.compile(r"`([A-Za-z0-9_][A-Za-z0-9_./-]*/[A-Za-z0-9_.-]*"
                  r"\.(?:c|h|py|sh|md|txt|ld))(?::(\d+))?`")


def load_allow():
    """Exemptions are page:name and each must carry its reason.

    A bare name silences a symbol everywhere, and the same string can be a correct
    quotation on one page and a stale claim on another. A reason nobody is forced
    to write is a convention rather than a check.
    """
    seen, bad = {}, []
    if not os.path.exists(ALLOW):
        return seen, bad
    for n, line in enumerate(open(ALLOW, encoding="utf-8"), 1):
        raw = line.rstrip("\n")
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        entry, _, reason = raw.partition("#")
        entry = entry.strip()
        if not reason.strip():
            bad.append("%s:%d  %s -- no reason given" % (ALLOW, n, entry))
            continue
        if ":" not in entry:
            bad.append("%s:%d  %s -- not page:name" % (ALLOW, n, entry))
            continue
        seen[entry] = True
    return seen, bad


def source_text():
    """Every tracked source byte, plus the device tree when it is checked out."""
    # A submodule's files are not in the top-level ls-files, so each is a root.
    roots = [".", "CM4/rfm69_lib", "radio_stack"]
    if os.path.isdir("../wl55_device"):
        roots.append("../wl55_device")
    blob = []
    for root in roots:
        # --others: a new module is a definition before anyone stages it.
        files = subprocess.run(["git", "-C", root, "ls-files",
                                "--cached", "--others", "--exclude-standard",
                                "*.c", "*.h", "*.py", "*.sh", "*.txt", "*.ld",
                                "CMakeLists.txt"],
                               capture_output=True, text=True).stdout.split()
        for f in files:
            # rfm69_lib is this project's own driver, not a vendored tree.
            if any(s in f for s in ("third_party", "/Drivers/", "/Middlewares/")):
                continue
            # Both name symbols in order to say they must not exist.
            if f.endswith(("docs_allow.txt", "test_check_docs.py")):
                continue
            p = os.path.join(root, f)
            try:
                blob.append(open(p, encoding="utf-8", errors="replace").read())
            except OSError:
                pass
    return "\n".join(blob)


def doc_pages():
    for scope in SCOPES:
        base = os.path.join(DOCS, scope)
        for dirpath, _, names in os.walk(base):
            for n in sorted(names):
                if n.endswith(".md"):
                    yield os.path.join(dirpath, n)


def owned_names(text, fenced):
    """Names inside an expression or a fence, with the two read differently.

    A fence is code, so every snake_case name in it is a claim and the scan is
    wide. Inline backticks are prose mentioning code, where `device_id(4)` is a
    field width and not a call, so only project-owned prefixes are read there.
    Live arithmetic goes in fences, which is where recall has to be paid for.
    """
    out = set()
    for m in re.finditer(r"[A-Za-z_][A-Za-z0-9_]*", text):
        n = m.group(0)
        if n.startswith(OWNED) and len(n) > 5:
            out.add(n)
        elif fenced and len(n) >= 6 and "_" in n and not n.isupper() \
                and not n.startswith("_") and not n.startswith(FOREIGN):
            out.add(n)
    return out


def main():
    global DOCS, SCOPES, ALLOW
    ap = argparse.ArgumentParser()
    ap.add_argument("--docs", help="documentation root to check")
    ap.add_argument("--scopes", help="comma-separated subdirectories")
    ap.add_argument("--allow", help="exemption list")
    args = ap.parse_args()
    if args.docs:
        DOCS = args.docs
    if args.scopes:
        SCOPES = tuple(args.scopes.split(","))
    if args.allow:
        ALLOW = args.allow
    if not os.path.isdir(DOCS):
        sys.stderr.write("no %s; nothing to check\n" % DOCS)
        return 0
    allow, bad_allow = load_allow()
    src = source_text()
    words = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", src))

    missing_ident, missing_path, pages = [], [], 0
    for page in doc_pages():
        pages += 1
        text = open(page, encoding="utf-8", errors="replace").read()
        rel = os.path.relpath(page, DOCS)

        names = set()
        for m in MACRO.finditer(text):
            names.add(m.group(1))
        for m in IDENT.finditer(text):
            n = m.group(1)
            if len(n) >= 4 and "_" in n and not n.isupper() \
                    and not n.startswith(FOREIGN):
                names.add(n)
        # Backticked expressions and fenced blocks, project-owned names only.
        for chunk in re.findall(r"`([^`\n]*)`", text):
            names |= owned_names(chunk, False)
        # A shell fence is commands, not this project's symbols.
        for lang, chunk in re.findall(r"```([a-z]*)\n(.*?)```", text, re.S):
            names |= owned_names(chunk, lang not in ("bash", "sh", "console",
                                                     "text", "ini"))

        for n in sorted(names):
            if n in words:
                continue
            if "%s:%s" % (rel, n) in allow:
                continue
            missing_ident.append("%s: %s" % (rel, n))

        for m in PATH.finditer(text):
            p = m.group(1)
            if p.endswith(".md") or "%s:%s" % (rel, p) in allow:
                continue
            if p.startswith("../"):
                continue
            if not any(os.path.exists(os.path.join(r, p))
                       for r in (".", "..", "../wl55_device")):
                missing_path.append("%s: %s" % (rel, p))

    missing_ident = sorted(set(missing_ident))
    missing_path = sorted(set(missing_path))
    print("scope: %d pages under %s" % (pages, "/, ".join(SCOPES) + "/"))
    print()
    for title, items in (("named in the docs, absent from the code", missing_ident),
                         ("file paths in the docs that do not resolve", missing_path),
                         ("exemptions that are not page:name with a reason", bad_allow)):
        print("== %s: %d ==" % (title, len(items)))
        for i in items:
            print("   " + i)
    return 1 if (missing_ident or missing_path or bad_allow) else 0


if __name__ == "__main__":
    sys.exit(main())
