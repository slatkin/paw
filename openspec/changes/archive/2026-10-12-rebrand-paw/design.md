# Design

## Context

See proposal.md for the reason. The fork has no commits ahead of `upstream/master` (codeberg dnkl/foot). Every edit to an upstream file can conflict on a later merge, so this design counts edits by line and prefers edits in files that upstream seldom changes.

The `foot` name is hard-coded in these places:

| Item | Location |
|---|---|
| Binary names | `meson.build` `executable('foot', ...)`, `executable('footclient', ...)` |
| Project name | `meson.build:1` |
| Config path | `config.c` (3 literals in the config search) |
| Default title and app-id | `config.c` (`.title`, `.app_id`) |
| Icon name from app-id | `terminal.c` (`streq(app_id, "footclient") ? "foot"`), `wayland.c` (same) |
| Spawn app-id argument | `main.c` (`"foot"`), `server.c` (`"footclient"`) |
| Fallback window title | `render.c` (`: "foot"`) |
| Server socket | `config.c` `get_server_socket_path()`, `client.c` (fallback chain, a second copy of the same logic) |
| XTVERSION | `csi.c` (`"\033P>|foot(%u.%u.%u%s%s)\033\\"`) |
| Help and version text | `main.c`, `client.c` |
| Install rules | `meson.build` (desktop files, systemd, example config, docs, themes), `completions/meson.build`, `icons/meson.build`, `doc/meson.build` |
| PGO scripts | `pgo/full-inner.sh`, `pgo/partial.sh` (call `${blddir}/foot`, `${blddir}/footclient`) |

## Goals / Non-Goals

**Goals:**
- paw and foot install to the same prefix without file collisions, and run in the same session.
- The edit to each upstream file is as small as possible.

**Non-Goals:**
- Renaming internal identifiers: `LOG_MODULE` strings, `foot-features.h`, `FOOT_*` macros, `foot_exit_failure`, memfd and shm pool names.
- Renaming source files in git.
- Rewriting man page and README text. The installed man pages keep the foot text for now.
- A new terminfo entry.
- Reading an old `foot.ini` as a fallback.

## Decisions

### D1: Rename installed files with `install_data(rename:)`, not `git mv`

Example: `install_data('foot.desktop', rename: 'paw.desktop', ...)`. The source files keep their upstream names, so an upstream edit to `foot.desktop` merges with no rename tracking. The project requires meson >= 0.59, and `rename:` is available since 0.46.

The *content* of the desktop files, the systemd units, and the completions still says `foot` (for example `Exec=foot`). These files need a content edit too. Each one is a small file that upstream rarely changes, so the edit is in place.

- Alternative: `git mv` to `paw.desktop`. Rejected. Git rename detection on merge works, but it fails when upstream edits the file heavily. It also shows up in every diff against upstream.
- Alternative: meson `configure_file()` to put `@name@` into the files. Rejected. It changes more upstream lines than a direct edit.

### D2: Systemd units

Install `foot-server.service.in` as `paw-server.service` and `foot-server.socket` as `paw-server.socket`. The service uses `Requires=%N.socket`, so the two unit names must match. Change `ExecStart` to `@bindir@/paw --server=3`, `ListenStream` to `%t/paw.sock`, and `Description` and `Documentation` to paw.

### D3: Server socket in two places

`config.c` `get_server_socket_path()` and the fallback chain in `client.c` both build the socket path. Change both, to `paw-%s.sock`, `paw.sock`, and `/tmp/paw.sock`. Do not merge the two copies into one helper. That is a refactor of upstream code, and it adds merge risk with no user benefit.

### D4: Keep `TERM=foot`; change XTVERSION to `paw`

- **`TERM`.** The `foot` terminfo is in ncurses, so SSH hosts already know it. Many apps (for example neovim) turn on features from it. A new `TERM` would break remote shells.
- **XTVERSION.** This reply exists to identify the terminal program, so it says `paw`. Apps can then add paw support later.

Known effect: yazi selects its image protocol from `TERM=foot` and from an XTVERSION reply that contains `foot`. With `TERM=foot`, yazi uses sixel in paw, not kitty graphics. Sixel works in paw, so images still show. To fix this, a later upstream request to yazi can add a paw entry. This change does not depend on it.

### D5: Icon

`icons/meson.build` installs the whole `hicolor` tree, so the icon file is installed as `foot.png` / `foot.svg`. Replace `install_subdir` with `install_data(..., rename:)` calls that install the same artwork as `paw.png` / `paw.svg`. The toplevel icon name is `paw` (see the spec).

### D6: Man pages

`doc/meson.build` builds the man pages in a `foreach` over `{'name': 'foot', ...}`. Change only the *output* names to `paw.1`, `paw.ini.5`, `pawclient.1`, `paw-ctlseqs.7`. Keep the source `.scd` file names. Do not change the man page text in this change.

## Risks / Trade-offs

- [Upstream adds a new `"foot"` string in a user-visible path, for example a new fallback path] → After each merge, run `git diff upstream/master -- '*.c' | grep -n '"foot'` and also grep the merged tree for new `"foot` literals. Record this check in the merge checklist in `README.md`.
- [The man page text still says foot] → This is accepted for now. It is a docs-only follow-up.
- [A user of this fork loses their config on upgrade] → Breaking change. Document `mv ~/.config/foot/foot.ini ~/.config/paw/paw.ini` in `CHANGELOG.md`.
- [yazi uses sixel, not kitty graphics] → See D4. Images still work.

## Migration Plan

1. Build and install to a test prefix. Check that no installed path also exists in a foot install to the same prefix.
2. Users copy or move their config to `~/.config/paw/paw.ini`.
3. Rollback: revert the commit. No data format changes.
