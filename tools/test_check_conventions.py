#!/usr/bin/env python3
"""Cases for check_conventions.py, each one a defect it let through at some point.

Every arm names what it pins, and the suite grades the **exit code** as well as
the text: a checker that prints a violation and exits zero is the shape that let a
148-character comment reach a flashed binary on 2026-08-22.

The corpus writes Cyrillic and CJK on purpose, to be caught. Its own literals are
built from code points so the file stays pure ASCII and the checker does not flag
the tests that test it - an exemption here would be a hole in the rule.
"""
import os, subprocess, sys, tempfile

CYR = "".join(chr(c) for c in (0x442, 0x435, 0x441, 0x442))          # a Cyrillic word
HAN = chr(0x8A08)                                                     # one CJK ideograph
MICRO = chr(0xB5)                                                     # a letter by category
CHECKER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_conventions.py")

# Over the limit on purpose: a short banner cannot fail.
LD_BANNER = "/*\n" + "".join("**  vendor line %d, padding padding padding\n" % i
                             for i in range(6)) + "*/\n"
LONG = "x" * 130


def run(root):
    """Runs the checker over a throwaway repository and returns (exit, text)."""
    p = subprocess.run([sys.executable, CHECKER], cwd=root,
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def build(files):
    root = tempfile.mkdtemp(prefix="cconv-")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    for name, body in files.items():
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True) if os.path.dirname(name) else None
        with open(path, "w") as f:
            f.write(body)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    return root


CLEAN_C = "/* a short comment */\nint f(void) { return 0; }\n"

CASES = [
    ("clean tree is silent", {"a.c": CLEAN_C}, 0, None),
    ("Cyrillic in .c", {"a.c": "/* " + CYR + " */\n"}, 1, "non-English"),
    ("Cyrillic in .md", {"a.md": CYR + "\n"}, 1, "non-English"),
    ("Cyrillic in .ld", {"a.ld": "/* " + CYR + " */\n"}, 1, "non-English"),
    ("CJK in .md", {"a.md": "be" + HAN + " computed\n"}, 1, "non-English"),
    ("micro sign is not foreign", {"a.c": "/* 700 " + MICRO + "s */\n"}, 0, None),
    ("CLAUDE.md is exempt", {"CLAUDE.md": CYR + "\n", "a.c": CLEAN_C}, 0, None),
    ("long block in .c", {"a.c": "/* " + LONG + " */\n"}, 1, "over 100"),
    # Both halves under the limit and their join over it, or it cannot fail.
    ("leading star is a dereference",
     {"a.c": "void f(uint8_t *detail, int rc) {\n"
             "    *detail = (uint8_t)((rc < 0) ? 0xFFu : rc);\n"
             "    /* reported rather than rolled back, because the write "
             "already happened */\n}\n"},
     0, None),
    ("long trailing comment", {"a.c": "int x = 1;  /* " + LONG + " */\n"}, 1, "over 100"),
    ("long block in .ld", {"a.ld": "/* " + LONG + " */\n"}, 1, "over 100"),
    # The body must span lines, or the case cannot fail.
    ("multi-line /* */ body in .ld",
     {"a.ld": "/* the body continues\n   " + LONG + "\n   and closes here */\n"}, 1, "over 100"),
    ("a doc path does not count",
     {"a.c": "/* short. radio_devices_docs/open_hub/" + "d" * 120 + ".md */\n"}, 0, None),
    # The harm is the wildcard absorbed into the comment above it.
    ("linker wildcard is not a continuation",
     {"a.ld": "SECTIONS {\n  /* the output section */\n    *(.text .text.* "
              + " ".join(".rodata.section%d" % i for i in range(12)) + ")\n}\n"}, 0, None),
    ("vendor ** banner in .ld is not ours", {"a.ld": LD_BANNER}, 0, None),
    ("a NUL means not prose", {"a.c": CLEAN_C, "b.bin": "\x00" + CYR}, 0, None),
    ("struct field comment on its own line",
     {"a.h": "struct s {\n    /* what it is for */\n    int x;\n};\n"}, 1, "own line"),
    ("Python docstring first line",
     {"a.py": '"""' + LONG + '"""\n'}, 1, "docstring"),
]


# The link layer's ten files, with the includes check_portable pins.
def link_corpus(**edits):
    files = {
        "radio_stack/src/grid.c":       '#include "grid.h"\nint a;\n',
        "radio_stack/src/gridmaster.c": '#include "gridmaster.h"\nint b;\n',
        "radio_stack/src/superframe.c": '#include "superframe.h"\n#include "grid.h"\n'
                                   '#include "timebase.h"\nint c;\n',
        "radio_stack/src/beacon.c":     '#include "beacon.h"\n#include "radio_slots.h"\nint d;\n',
        "radio_stack/src/hop.c":        '#include "hop.h"\n#include "radio_phy.h"\nint e;\n',
        "radio_stack/inc/grid.h":       "#include <stdint.h>\n",
        "radio_stack/inc/gridmaster.h": '#include <stdint.h>\n#include "grid.h"\n',
        "radio_stack/inc/superframe.h": '#include <stdint.h>\n#include "grid.h"\n',
        "radio_stack/inc/beacon.h":     '#include <stdint.h>\n#include "superframe.h"\n',
        "radio_stack/inc/hop.h":        "#include <stdint.h>\n",
        "radio_stack/src/exchange.c":   '#include "exchange.h"\nint f;\n',
        "radio_stack/inc/exchange.h":   '#include <stdint.h>\n#include "kdf.h"\n',
        "radio_stack/inc/kdf.h":        "#include <stdint.h>\n",
        "radio_stack/inc/radio_phy.h":   '#include <stdint.h>\n#include "profile.h"\n'
                                         '#include "radio_slots.h"\n',
        "radio_stack/inc/radio_slots.h": '#include <stdint.h>\n#include "profile.h"\n'
                                         '#include "radio_layout.h"\n',
        "radio_stack/inc/radio_layout.h": "#include <stdint.h>\n",
        "radio_stack/profiles/profile.h": '#include "profile_ids.h"\n'
                                          '#include "profile_asbuilt.h"\n',
        "radio_stack/profiles/profile_ids.h":      "\n",
        "radio_stack/profiles/profile_asbuilt.h":  "\n",
        "radio_stack/profiles/profile_hosttest.h": "\n",
    }
    for key, body in edits.items():
        path = key.replace("__", "/").replace("_c", ".c").replace("_h", ".h")
        if body is None:
            del files[path]
        else:
            files[path] = body
    return files


LINK_CASES = [
    ("link corpus clean", link_corpus(), 0, None),
    ("no corpus: another tree", {"a.c": CLEAN_C}, 0, None),
    ("hal include in grid.c",
     link_corpus(**{"radio_stack__src__grid_c":
                    '#include "grid.h"\n#include "stm32h7xx_hal.h"\nint a;\n'}),
     1, "not on this file"),
    ("stdio in beacon.c",
     link_corpus(**{"radio_stack__src__beacon_c":
                    '#include "beacon.h"\n#include <stdio.h>\nint d;\n'}),
     1, "not freestanding"),
    ("a listed file renamed away",
     link_corpus(**{"radio_stack__src__grid_c": None}), 1, "MISSING"),
    # The rule is passed the instant; it must not read a clock of its own.
    # radio_devices_docs/specs/03-roadmap.md
    ("timebase back in grid.c",
     link_corpus(**{"radio_stack__src__grid_c":
                    '#include "grid.h"\n#include "timebase.h"\nint a;\n'}),
     1, "not on this file"),
    # The include a listing cannot see, because it is reached through a header.
    # radio_devices_docs/specs/03-roadmap.md
    ("sha256.h back in exchange.c",
     link_corpus(**{"radio_stack__src__exchange_c":
                    '#include "exchange.h"\n#include "sha256.h"\nint f;\n'}),
     1, "not on this file"),
    ("sha256.h back in exchange.h",
     link_corpus(**{"radio_stack__inc__exchange_h":
                    '#include <stdint.h>\n#include "kdf.h"\n#include "sha256.h"\n'}),
     1, "not on this file"),
]
CASES = CASES + LINK_CASES


def main():
    bad = 0
    for name, files, want_exit, want_text in CASES:
        root = build(files)
        code, out = run(root)
        ok = code == want_exit and (want_text is None or want_text in out)
        if not ok:
            bad += 1
            print("FAIL %-42s exit %d (want %d)%s"
                  % (name, code, want_exit,
                     "" if want_text is None else "  looking for %r" % want_text))
            # Whole output: a summary filter cropped the line naming the file.
            for l in out.splitlines():
                print("     " + l)
        else:
            print("ok   %s" % name)
    print("\n%d case(s), %d failed" % (len(CASES), bad))
    return 1 if bad else 0


sys.exit(main())
