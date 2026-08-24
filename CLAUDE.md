# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

This file is the operational brief and nothing else: conventions, how to build,
how to flash, and where the rest lives. **Reasoning does not belong here** — when
a change alters *why* something is done rather than how, it goes to the
documentation repository or to a skill.

## Conventions

- **Спілкування з розробником — українською.** Відповіді в чаті, пояснення,
  плани, повідомлення про помилки.
- **Усе, що лягає в репозиторій, — англійською.** Код, коментарі, повідомлення
  комітів, документація в `../radio_devices_docs`, скіли. Мова спілкування і мова
  репозиторію — різні речі: перша для розробника, друга для двох прошивок, які
  читають одну специфікацію.
- **Це поширюється на КОЖЕН файл репозиторію**, не лише на `.c`/`.h`:
  `.gitignore`, `CMakeLists.txt`, `*.cmake`, `*.py`, `*.sh`, `*.ld`, `*.md`.
  Конфіг і білд-файли — теж артефакти. **Якщо є сумнів, чи файл підпадає під
  правило, — підпадає.** Застосовувати, а не питати: двічі саме це запитання
  закінчилось відхиленням від конвенції.
- **Doxygen is the comment format.** `/** ... */` for anything a caller reads —
  a function, a type, a file — with `@brief`, `@param`, `@return`, `@retval`.
  Implementation comments inside a function body stay plain `/* ... */`.
- **A comment on a struct field goes on the same line as the field, and is one
  short line naming what the field is for** — and only when that is not obvious
  from the field's name. It takes Doxygen's trailing member form,
  `uint8_t slot; /**< which device this is addressed to */`, which is the
  same-line rule and not a second one. No line above the field, no paragraph.
- **A Doxygen block is measured per `@brief`, not per block.** `@param`,
  `@retval` and `@return` lines do not count towards any limit — a realistic
  block with them runs to about 170 characters and could never pass the rule
  below, which is why this tree had no Doxygen at all until 2026-08-21. The
  `@brief` itself is one line inside 100 characters; if the explanation needs a
  paragraph it is a documentation page, and the comment carries the path.
- **Every other comment is measured whole**, and that is the rule below.
- **A plain block comment is at most 100 characters in total** — a block being a
  run of contiguous comment lines, counted whole rather than per line, and a
  `/* */` body counted to its close however its continuation lines are indented.
  Every file type above, section separators included.
- **Виняток: шлях до документації не рахується в ці 100 символів.** Його можна
  писати повністю — `radio_devices_docs/radio/pairing.md`, — бо посилання, яке
  доводиться вгадувати, не веде нікуди. Скорочувати шлях заради ліміту не треба.
- Comments state what, not why. No restating the code, no filler.
- **Причина не йде в коментар — вона йде в `../radio_devices_docs`.** Саме тому
  коментарю не потрібен другий рядок: якщо думка не вміщається в один, це не
  коментар, а сторінка документації і посилання на неї.
- Committing is fine. Do **not** add `Co-Authored-By` or any co-authorship
  trailer.

Конвенція без перевірки — декоративна, тож перевіряти після правок
(`verification` skill називає цей клас):

```bash
tools/check_conventions.sh
```

## Build

```bash
cmake --preset Debug          # configures both cores
cmake --build --preset Debug  # -> CM4/build/*.elf, CM7/build/*.elf
```

Presets: `Debug`, `RelWithDebInfo`, `Release`, `MinSizeRel`. The top-level project
drives each core through `ExternalProject_Add`, so a single core is built by
configuring its directory:

```bash
cmake -S CM7 -B CM7/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/gcc-arm-none-eabi.cmake && cmake --build CM7/build
```

Toolchain comes from STM32CubeCLT (`arm-none-eabi-gcc`, `cmake`, `ninja`). Host
tests: `make -C Common/test check`.

## Flash and inspect

CM7 lives at `0x08000000`, CM4 at `0x08100000`; writing the ELF picks the address
up automatically.

```bash
set -e                                 # gate the flash on the build's exit code
cmake --build --preset Debug
P=/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI
HUB=sn=0049004A3234510637333934        # the H755 probe, never a bare -c port=SWD
$P -c port=SWD $HUB mode=UR -w CM7/build/testHubFreeRTOS_CM7.elf -v
$P -c port=SWD $HUB mode=UR -w CM4/build/testHubFreeRTOS_CM4.elf -v
$P -c port=SWD $HUB mode=HOTPLUG --rst
```

Console: `/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0049004A3234510637333934-if02`
at 115200 8N1 — not `/dev/ttyACM<N>`, whose numbering shifts when other boards are
plugged in.

Three traps, with the reasoning in
[`on-target.md`](../radio_devices_docs/open_hub/testing/on-target.md): gate the
flash on the build's **exit code** rather than on grepping its output; always pin
the probe, because WL55 device boards share the bus; and **never erase bank 1 from
CM7** — it has bricked this board twice and needs an external programmer to
recover ([`keystore.md`](../radio_devices_docs/open_hub/arch/keystore.md)).

## Cross-check the air, every time

Host tests and firmware counters both sit above the packet engine, so "the
device stopped sending" and "the hub stopped hearing" produce the same zero.
One capture separates them, and it belongs to the post-flash routine rather than
to debugging:

```bash
cd tools/sdr
../../.venv/bin/python capture.py air.iq -f 866.56e6 -s 2.4e6 -t 120 -g 30 --label hub
../../.venv/bin/python airgrid.py air.iq        # non-zero on any failed check
```

Six checks over the grid in `Common/inc`, read through the compiler rather than
restated. The `sdr` skill carries what each one costs when it is missing, and
why 2.4 Msps is the rate.

## Skills

Deep knowledge lives in skills, not in this file. Load the one that matches the
task before starting.

| Skill | Lives in | Use when |
|---|---|---|
| `rfm69` | this repo | the radio will not transmit, will not receive, corrupts frames, or you are touching `CM4/rfm69_lib` or the PHY |
| `config-migration` | this repo | before editing `cfg_config_t`, `cfg_device_t`, `cfg_snapshot_t` or `CFG_VERSION`; before anything that erases or rewrites the store's flash; when two stores must share sectors |
| `sdr` | `~/.claude/skills` | a radio claim needs evidence from the air rather than from a counter |
| `cubemx` | `~/.claude/skills` | peripherals, pins, clocks, FreeRTOS tasks or middleware change — or a change is about to be hand-written into a generated file |
| `verification` | `~/.claude/skills` | adding or reading a check, a self-test, a counter, a test vector or a probe; before quoting a measurement; whenever a first success is imminent |
| `regression` | `~/.claude/skills` | opening, conducting, aborting or grading a regression run — the protocol, not the checks |
| `telemetry` | `~/.claude/skills` | reading the hub's live state or commanding it through `openhub-server`, over REST or the websocket, instead of the console |

Keep them current. A new surprise about the radio goes in `rfm69`, a new way the
configuration store bit back goes in `config-migration`, a new way a green check
turned out to be worthless goes in `verification` — not here.

**`sdr`, `cubemx`, `verification`, `telemetry` and `regression` are shared with
the WL55 device session and no longer live in this tree.** `rfm69` and
`config-migration` are still committed here, because the RFM69 is the hub's radio
and the configuration store is the hub's flash — the device has an SX126x and a
store of its own design. The shared ones are loaded by name exactly as before;
what changed is that an edit to any of them is not a commit here. Make the edit in `~/.claude/skills` and **tell the device session**,
so it picks the change up rather than finding it by accident.

They were moved out because both repositories were told to keep them current
while only one of them held the files, and two sessions committing into one
working tree is a hazard no amount of care removes. `verification` moved last, on
2026-08-22, and its case was the plainest of the three: the device session had a
finished entry to add, could not commit it, and had to send the text across for
this side to land — which is the shared-file hazard arriving as a delay rather
than as a conflict.

## Where the open work lives

[`ROADMAP.md`](ROADMAP.md), in this repository, and nowhere else. Every debt,
defect and agreed-but-unbuilt design is one entry there, pointing at the page
that holds the why. **Do not start a second list**: a defect written into a
documentation page as well is a defect that gets fixed once and closed nowhere.

## Where the reasoning lives

`../radio_devices_docs`, a separate repository shared with the WL55 device
project. It moved out of this tree because half of it was never the hub's alone:
the hop sequence, the slot grid and the wire crypto are a contract two firmwares
implement, and a document owned by one of them is a contract one side can revise
unilaterally.

- [`radio/`](../radio_devices_docs/radio/) — the air interface: PHY, TDMA,
  hopping, joining, pairing, wire crypto. **Changing anything there binds the
  device too**; agree it with the device session first and re-measure on air.
- [`open_hub/`](../radio_devices_docs/open_hub/) — this firmware's half: the
  dual-core split, IPC, memory map, flash stores, the RFM69 driver, the timebase,
  lwIP, the console, the SDR bench, and the
  [defects worth remembering](../radio_devices_docs/open_hub/known-defects.md),
  which are the **closed** ones.
- [`wl55_device/`](../radio_devices_docs/wl55_device/) — the device's half.
- Decision records are one global sequence split by scope between
  [`radio/decisions/`](../radio_devices_docs/radio/decisions/) and
  [`open_hub/decisions/`](../radio_devices_docs/open_hub/decisions/). Numbers were
  **not** reassigned when the docs moved, so `ADR-0021` in a source comment still
  resolves. **The next free number lives in the workspace `CLAUDE.md` and nowhere
  else** — this line used to carry a copy of it, and three indexes once said
  0022, 0027 and 0027 for one sequence.

Source comments cite a page by its full path, `radio_devices_docs/radio/pairing.md`,
and a decision as `ADR-0021` — the numbers are stable, so both still resolve.

**Start here when the task is unfamiliar:**
[`open_hub/README.md`](../radio_devices_docs/open_hub/README.md) for this
firmware, [`radio/README.md`](../radio_devices_docs/radio/README.md) for anything
that reaches the antenna.
