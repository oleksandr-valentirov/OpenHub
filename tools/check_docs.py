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
import re
import subprocess
import sys

DOCS = "../radio_devices_docs"
# Pages naming this side's symbols. wl55_device/ is the other session's half.
SCOPES = ("radio", "open_hub")
ALLOW = "tools/docs_allow.txt"
# Symbols owned by somebody else's build; naming them is not a claim about this tree.
FOREIGN = ("mbedtls_", "altcp_", "lwip", "sys_arch", "HAL_", "os", "xQueue", "pd",
           "ExternalProject_", "VP_", "FREERTOS_", "tcp_", "netif_", "MX_")

IDENT = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)(?:\(\)|\(|`)")
MACRO = re.compile(r"`(RADIO_[A-Z0-9_]+|IPC_[A-Z0-9_]+|KV_[A-Z0-9_]+)`")
# Only a path is a claim about where something lives; a bare file name is prose.
PATH = re.compile(r"`([A-Za-z0-9_][A-Za-z0-9_./-]*/[A-Za-z0-9_.-]*"
                  r"\.(?:c|h|py|sh|md|txt|ld))(?::(\d+))?`")


def load_allow():
    seen = {}
    if not os.path.exists(ALLOW):
        return seen
    for line in open(ALLOW, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line:
            seen[line] = True
    return seen


def source_text():
    """Every tracked source byte, plus the device tree when it is checked out."""
    # rfm69_lib is a submodule, so the top-level ls-files does not reach into it.
    roots = [".", "CM4/rfm69_lib"]
    if os.path.isdir("../wl55_device"):
        roots.append("../wl55_device")
    blob = []
    for root in roots:
        files = subprocess.run(["git", "-C", root, "ls-files",
                                "*.c", "*.h", "*.py", "*.sh", "*.txt", "*.ld",
                                "CMakeLists.txt"],
                               capture_output=True, text=True).stdout.split()
        for f in files:
            # rfm69_lib is this project's own driver, not a vendored tree.
            if any(s in f for s in ("third_party", "/Drivers/", "/Middlewares/")):
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


def main():
    if not os.path.isdir(DOCS):
        sys.stderr.write("no %s; nothing to check\n" % DOCS)
        return 0
    allow = load_allow()
    src = source_text()
    words = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", src))

    missing_ident, missing_path, pages = [], [], 0
    for page in doc_pages():
        pages += 1
        text = open(page, encoding="utf-8", errors="replace").read()
        rel = os.path.relpath(page, DOCS)
        for m in MACRO.finditer(text):
            name = m.group(1)
            if name not in words and name not in allow:
                missing_ident.append("%s: %s" % (rel, name))
        for m in IDENT.finditer(text):
            name = m.group(1)
            if len(name) < 4 or "_" not in name:
                continue
            if name.isupper() or name.startswith(FOREIGN):
                continue
            if name not in words and name not in allow:
                missing_ident.append("%s: %s()" % (rel, name))
        for m in PATH.finditer(text):
            p = m.group(1)
            if p.endswith(".md") or p in allow:
                continue
            if p.startswith("../"):
                continue
            found = any(os.path.exists(os.path.join(r, p))
                        for r in (".", "..", "../wl55_device"))
            if not found:
                missing_path.append("%s: %s" % (rel, p))

    missing_ident = sorted(set(missing_ident))
    missing_path = sorted(set(missing_path))
    print("scope: %d pages under %s" % (pages, "/, ".join(SCOPES) + "/"))
    print()
    for title, items in (("named in the docs, absent from the code", missing_ident),
                         ("file paths in the docs that do not resolve", missing_path)):
        print("== %s: %d ==" % (title, len(items)))
        for i in items:
            print("   " + i)
    return 1 if (missing_ident or missing_path) else 0


if __name__ == "__main__":
    sys.exit(main())
