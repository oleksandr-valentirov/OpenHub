#!/usr/bin/env python3
"""The corpus check_docs.py must agree with, so a divergence is not an occasion.

The hub and the device each keep a copy of this checker, and today each copy's
arbitrary choice covered a hole in the other's: one keyed on owned prefixes and
had precision inline, one keyed on case and had recall inside fences. Neither was
chosen for the reason it mattered, and both holes were found by running the
other's cases rather than by reading the other's diff.

That works while both copies are exercised against the same cases and decays into
two half-checkers the first week nobody cross-runs them - and nothing forces a
cross-run. This is the corpus that does: every case either session found, with
what must be reported and what must stay silent. Run it against either copy.
"""

import os
import subprocess
import sys
import tempfile

PAGE = """# Fixture

A macro alone in ticks: `RADIO_GHOST_ONE_US`.
A macro with arguments: `RADIO_GHOST_TWO_US(28u)`.
A project call inline: `radio_ghost_inline_us()`.
Field widths, not calls: `superframe(4) || device_id(4) || direction(1)`.
A symbol that exists: `RADIO_SLOT_COUNT`.
A path that does not resolve: `CM4/Core/Src/ghost_nowhere.c`.

```c
    uint32_t t = radio_ghost_fenced_us(len);
    if (begin_ghost_unprefixed(n) != 0) return;
```

```bash
$P -c port=SWD ghost_shell_thing=1
```
"""

OTHER = """# Other fixture

Exempted on the fixture page, not on this one: `RADIO_GHOST_THREE_US`.
"""

ALLOW = """# fixture exemptions
scope/page.md:RADIO_GHOST_THREE_US  # exempt on page.md, which does not name it
scope/page.md:RADIO_GHOST_ONE_US    # a deliberate quotation
"""

# Reported, and why each one is in the corpus.
MUST_REPORT = {
    "scope/page.md: RADIO_GHOST_TWO_US":
        "a macro with arguments fell between both original regexes",
    "scope/page.md: radio_ghost_inline_us":
        "a project call named in prose",
    "scope/page.md: radio_ghost_fenced_us":
        "live arithmetic goes in fences",
    "scope/page.md: begin_ghost_unprefixed":
        "an owned-prefix rule alone cannot reach hub_ipc_call or begin_quiesce",
    "scope/page.md: CM4/Core/Src/ghost_nowhere.c":
        "a path is the one claim in prose a machine can settle",
}

# Silent, and why. Each of these was somebody's false positive first.
MUST_BE_SILENT = {
    "device_id": "a field width in a nonce layout, not a call",
    "superframe": "likewise",
    "ghost_shell_thing": "a shell fence is commands",
    "RADIO_SLOT_COUNT": "it exists",
    "RADIO_GHOST_ONE_US": "exempted on its own page",
}

# Exempted on another page: the allow file must not be evidence of its own names.
MUST_REPORT_CROSS = "scope/other.md: RADIO_GHOST_THREE_US"


def run(tmp, allow_text):
    root = os.path.join(tmp, "docs")
    os.makedirs(os.path.join(root, "scope"), exist_ok=True)
    open(os.path.join(root, "scope", "page.md"), "w").write(PAGE)
    open(os.path.join(root, "scope", "other.md"), "w").write(OTHER)
    allow = os.path.join(tmp, "allow.txt")
    open(allow, "w").write(allow_text)
    here = os.path.dirname(os.path.abspath(__file__))
    return subprocess.run(
        [sys.executable, os.path.join(here, "check_docs.py"),
         "--docs", root, "--scopes", "scope", "--allow", allow],
        capture_output=True, text=True,
        cwd=os.path.dirname(here)).stdout


def main():
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        out = run(tmp, ALLOW)
        for want, why in MUST_REPORT.items():
            if want not in out:
                fails.append("not reported: %s  (%s)" % (want, why))
        if MUST_REPORT_CROSS not in out:
            fails.append("not reported: %s  (an exemption on another page must "
                         "not silence this one, and the allow file must not be "
                         "read as source)" % MUST_REPORT_CROSS)
        for bad, why in MUST_BE_SILENT.items():
            for line in out.splitlines():
                if line.strip().startswith(("scope/",)) and bad in line:
                    fails.append("wrongly reported: %s  (%s)" % (bad, why))

        # The allow file's own defects: match the line, never the heading.
        out2 = run(tmp, "scope/page.md:RADIO_GHOST_TWO_US\n")
        if "-- no reason given" not in out2:
            fails.append("an exemption without a reason must be reported")
        out3 = run(tmp, "RADIO_GHOST_TWO_US  # bare name\n")
        if "-- not page:name" not in out3:
            fails.append("a bare-name exemption must be reported")

    # Not reachable through the CLI: a corpus allow file is not tracked.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import check_docs
    src = check_docs.source_text()

    # Read as source, this file's ghosts exist and every case passes vacuously.
    if "RADIO_GHOST_TWO_US" in src:
        fails.insert(0, "this corpus is read as source, so its ghost names "
                        "exist and every case above passes vacuously")

    marker = "This file is excluded from the source scan"
    if os.path.exists(check_docs.ALLOW):
        if marker not in open(check_docs.ALLOW, encoding="utf-8").read():
            fails.append("docs_allow.txt lost the marker this check reads")
        elif marker in src:
            fails.append("the allow file is read as source, so every exemption "
                         "is evidence that its own symbol exists")

    for f in fails:
        print("FAIL " + f)
    if not fails:
        print("check_docs: ok (%d reported cases, %d silent cases, 2 allow-file "
              "defects)" % (len(MUST_REPORT) + 1, len(MUST_BE_SILENT)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
