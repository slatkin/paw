# AGENTS.md

This file provides guidance to AI coding agents when working with code in this repository.

## Project state

- The repository is a fork of foot, a terminal emulator for Wayland. The code uses C11 and the meson build system.
- Git remotes: `origin` is the fork (github.com/slatkin/paw). `upstream` is foot (codeberg.org/dnkl/foot).
- Master builds `paw` and `pawclient`. Commit `d390d73f` renamed the binaries. Later commits moved the config path, server socket, and XTVERSION reply to paw names. The rebrand change is archived in `openspec/changes/archive/2026-10-12-rebrand-paw/`. Run `git log` and `git status` before you assume a name.
- Master has fork commits on top of `upstream/master`, for example `d390d73f`.

## Build and test

Install the dependencies listed in INSTALL.md. Install `tllist` 1.1.0 or later as a system package. If pkg-config cannot find it, meson clones it from codeberg.org (see `subprojects/tllist.wrap`). If `subprojects/tllist` exists but is empty, meson stops with `Subproject exists but has no meson.build file`. The system package avoids this, because meson uses it first.

Run `just check` for the usual checks. It configures and builds `bld/`, runs the tests, builds and tests with sanitizers in `bld/asan/`, runs clang-tidy on changed C files, and runs codespell. Run `just` to list the other recipes. `just release` builds the release tree in `bld/release/`. Do not use valgrind on the CachyOS host used here. It fails at startup, because its glibc has no debug symbols that valgrind can read. Use the `asan` recipe for memory errors and leaks.

Build in `bld/`. This directory is git-ignored:

```sh
meson setup bld --buildtype=debug
ninja -C bld
bld/paw --version
```

The binary name comes from `executable()` in `meson.build`. On master, the terminal binary is `paw`.

Run all tests, or run one test by name. The only test is `config` (defined in `tests/meson.build`):

```sh
ninja -C bld test
meson test -C bld config
```

Warnings fail the build (`werror=true` in `meson.build`). Fix the cause of each warning.

To change the log output, run `bld/paw -d info`. The log levels are `info` (most output), `warning` (default), `error`, and `none`.

CI (`.woodpecker.yaml`) runs these checks from the repository root. `codespell` checks spelling in all files. `mypy` (strict on `scripts/`) and `ruff check` check Python:

```sh
codespell
mypy
ruff check
```

Release builds use profile-guided optimization (PGO). See INSTALL.md.

## Architecture

### Processes

In the bullets below, `foot` means the terminal binary and `footclient` means the client binary. On master, these binaries are `paw` and `pawclient`.

- `foot` (main.c) runs one terminal window. With `--server`, it runs as a server (server.c). The server keeps running without a window. It opens windows when a client asks.
- `footclient` (client.c) connects to the server socket. It sends one `struct client_data` message (client-protocol.h). The message holds the working directory, the arguments, and the environment. The client does not pass file descriptors. The server reads the message and opens a window.
- `main()` runs these steps in order: `config_load()` (config.c), `fdm_init()` (fdm.c), `reaper_init()` (reaper.c), `wayl_init()` (wayland.c), and `render_init()` (render.c). Then `term_init()` opens one window, or `server_init()` starts server mode. Last, `main()` calls `fdm_poll()` in a loop.
- The main loop runs on one thread. Render workers (`render_worker_thread` in render.c, started from terminal.c) and font loader threads (terminal.c) run in parallel.

### One window

- `struct terminal` (terminal.h) holds the state of one window. `ptmx` is the PTY (pseudo-terminal) master. The shell runs on the PTY slave.
- `slave_spawn()` (slave.c) starts the shell. It calls `setsid()` and `TIOCSCTTY`, then `exec`.
- Output: terminal.c reads `ptmx` in a loop and calls `vt_from_slave()` (vt.c). The VT parser sends CSI sequences to csi.c, OSC sequences to osc.c, and DCS sequences to dcs.c. Sixel images are handled in sixel.c. These functions change `struct terminal` and the grid.
- The grid (grid.c) is a ring of `struct row`. Each row holds `struct cell` values. `term->normal` is the main screen. `term->alt` is the alternate screen.
- Input: input.c handles Wayland keyboard and pointer events. It looks up key bindings from the config first (key-binding.c). Other input becomes bytes. `term_to_slave()` (terminal.c) queues the bytes for `ptmx`. The queue writes when the PTY can accept data.
- Drawing: state changes call `render_refresh()`. That function sets one flag (`term->render.refresh.grid`). render.c draws the frame later. Drawing buffers are shared memory (shm.c).

### Event loop

`fdm` (fdm.c) is an epoll loop. It owns all file descriptors and signals. The Wayland connection, each PTY, the server and client sockets, and timers (timerfd) register with `fdm_add()`.

### Module map

| Area | Files |
|---|---|
| Startup and modes | main.c, server.c, client.c, client-protocol.h |
| Event loop, processes, threads | fdm.c, reaper.c, async.c, spawn.c, slave.c |
| Wayland and input | wayland.c, input.c, ime.c, key-binding.c, shm.c |
| Rendering | render.c, box-drawing.c, sixel.c |
| Terminal state and VT parser | terminal.c, vt.c, csi.c, osc.c, dcs.c, grid.c, selection.c, search.c, url-mode.c |
| Config and commands | config.c, config.h, commands.c |
| Generated at build time | version.h, foot-terminfo.h, emoji-variation-sequences.h, srgb.c, srgb.h |

Constraints that are not obvious:

- `tests/test-config.c` includes `config.c` directly and stubs some functions. If `config.c` calls another module, the test can fail to link. Check the test after you change `config.c`.
- The default server socket path is in two places: `get_server_socket_path()` (config.c) and the fallback chain in client.c. Change both together.
- The default `TERM` is `foot`. It comes from the meson option `default-terminfo`. The terminfo source is `foot.info`.

## Fork rules

These rules come from `openspec/changes/archive/2026-10-12-rebrand-paw/design.md`:

- Keep edits to upstream files small. Each upstream merge can conflict with fork edits. Prefer files that upstream rarely changes.
- Keep internal names. The rebrand does not rename `LOG_MODULE` strings, `FOOT_*` macros, or source file names.
- Do not refactor upstream code only to remove duplicates.
- After an upstream merge, search new user-visible strings: `git diff upstream/master -- '*.c' | grep -n '"foot'`.

## OpenSpec

- Changes live in `openspec/changes/<name>/`: `proposal.md`, `design.md`, `tasks.md`, and `specs/<capability>/spec.md`. The config is `openspec/config.yaml` (schema `spec-driven`).
- Archived changes go to `openspec/changes/archive/`, for example `2026-10-12-rebrand-paw`. Final specs go to `openspec/specs/`, for example `program-identity`.
- Slash commands are in `.claude/commands/opsx/`, for example `/opsx:propose`, `/opsx:apply`, and `/opsx:archive`. The same prompts for pi are in `.pi/prompts/opsx-*.md`.
- `openspec/`, `.claude/`, and `.pi/` are committed.

## Conventions

- Use C11 with `-pedantic`. Use 4 spaces for indent, LF line endings, and no trailing whitespace (`.editorconfig`). Meson files use 2 spaces. `.scd` man page sources use tabs.
- Put the return type of a function on its own line. Start the function name in column 0. Put the opening brace on its own line.
- Use the project helpers. `xmalloc.h` provides `xcalloc()` and `xstrdup()`. Use `tll(...)` lists (tllist). Use the `LOG_*` macros (log.h). Use `BUG()` (debug.h) for states that must never happen.
- Write commit subjects as `area: lowercase summary`, for example `grid: reflow: fix two issues when truncating wide characters`.
- Add user-visible changes under `## Unreleased` in CHANGELOG.md.
- The config reference is `doc/foot.ini.5.scd`. The example config is `foot.ini`.

## Upstream AI policy

CONTRIBUTING.md and the README ("No AI / LLM policy") forbid generative AI and LLMs in every contribution to foot. That covers patches, bug reports, and comments on issue and pull request trackers. Before you write text for an upstream issue, pull request, or patch, ask the user whether this policy applies to that submission.
