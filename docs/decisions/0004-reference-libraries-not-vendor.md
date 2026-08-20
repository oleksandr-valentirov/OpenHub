# ADR-0004 — Reference HAL and middleware, do not vendor them

**Status:** Accepted
**Date:** 2026-08-19

## Context

CubeMX offers to copy HAL, CMSIS and Middlewares into the project
(`ProjectManager.LibraryCopy`), or to reference them in place. Referencing bakes
**absolute paths** into `mx-generated.cmake` — paths containing the developer's
home directory and the exact FW package version, which makes the committed file
unbuildable by anyone else.

## Decision

Reference, not copy (`LibraryCopy=2`). `CM4/CMakeLists.txt` and
`CM7/CMakeLists.txt` **rewrite the absolute prefix to `${CUBE_FW_PATH}` at configure
time** with a regex independent of user name and FW version, and include the
processed copy from the build directory.

Override with `-DCUBE_FW_PATH=<path>` or the `CUBE_FW_PATH` environment variable.

There is **no post-generation hook**, and one must not be reintroduced: the rewrite
happens at configure time so that generating and building stay independent steps.

## Consequences

- The repository stays small — no megabytes of vendor code, and diffs show project
  changes rather than HAL churn.
- **A committed `mx-generated.cmake` carrying someone else's absolute paths still
  builds**, which is the property that makes referencing viable at all.
- The Cube FW package becomes a build prerequisite. `CMakeLists.txt` checks for it
  and fails with a clear message rather than a wall of missing headers.
- The exact HAL version is **not pinned by the repository**. Two developers on
  different FW packages get different binaries from the same commit. Accepted for a
  single-developer project; it would need addressing otherwise.
- HAL cannot be patched. Nothing currently needs it.

## Alternatives rejected

**Copy into the project.** Reproducible and version-pinned, at the cost of tens of
megabytes of vendored code and a merge burden on every FW upgrade.

**A post-generation hook.** Would fix the paths at generate time, and couples the
two steps: a fresh clone could not build without first running CubeMX.

## See also

[architecture/build-and-generation.md](../architecture/build-and-generation.md)
