# Intellectual property policy

This project improves Train Simulator Classic performance at runtime. It must
not publish anything belonging to the game's makers. The aim is to show that an
in-memory performance fix can be built and shared without redistributing or
disclosing the game's code.

## Never committed

- Game files, or any part of them (executables, DLLs, assets, shaders, scripts).
- Disassembly, decompiled code, or pseudo-code of game functions.
- Raw byte patterns or signatures copied from game code.
- Module addresses (`GameManagerVC64.dll+0x...`): code is located by fingerprint, never by address.
- Reverse-engineering notes beyond what the code needs (write-ups of how the engine works).
- Profiling captures (`.etl`, PresentMon CSVs, thread dumps). They carry symbols
  and offsets.

All of this lives in `research/` and `captures/`. Both folders are gitignored.
The `githooks/pre-commit` hook also blocks it.

## Allowed in the repository

- Our own code: the proxy DLL, hooks, replacement implementations, tools.
- Hooks at the Windows API boundary, such as import-table redirection of CRT
  and kernel32 functions. These need no knowledge of game code.
- One-way digests that locate a patch site. A digest identifies a site in the
  user's own installed copy of the game, but nobody can recover the game's
  bytes from it.
- Field offsets inside engine objects, and instruction offsets inside a function
  that is located by fingerprint (to reach an engine global it references). These
  say where our code reads or hooks, not what the game's code is.
- Aggregate measurements (fps, frame-time percentiles, % CPU per thread role).

- Unmodified open-source dependencies in `third_party/`, each with its license (e.g. MinHook, BSD-2). The hook
  still blocks binaries there, but skips the byte-pattern checks.

## How in-memory patches stay IP-clean

A patch site is described by the digest of a short code window, a window length
and a relative fix-up offset. It is not described by the bytes themselves. At
start-up the DLL scans the game's code section in the user's own process memory
and hashes each candidate window. Where a digest matches, it applies a patch.
The patch bytes are always our own code, typically a jump to a function in our
DLL. Nothing from the game is ever stored in, or distributed with, this project.

Setup after cloning: `git config core.hooksPath githooks`
