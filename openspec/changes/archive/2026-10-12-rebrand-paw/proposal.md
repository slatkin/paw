# Proposal

## Why

paw is a fork of foot. It must install and run next to foot on the same system. Today both builds use the same binary names, config path, server socket, desktop files, and install directories, so one overwrites or blocks the other. The rename must stay small, because the fork merges upstream foot often.

## What Changes

- The binaries are `paw` and `pawclient`. They replace `foot` and `footclient`.
- The default config path is `$XDG_CONFIG_HOME/paw/paw.ini` (and the matching fallbacks).
- The default window title and app-id are `paw` (the server mode uses `pawclient`).
- The default server socket is `$XDG_RUNTIME_DIR/paw-$WAYLAND_DISPLAY.sock` (and the matching fallbacks).
- The XTVERSION reply is `paw(<version>)`.
- The help text, desktop files, systemd units, shell completions, icon, man pages, themes, and the example config install under paw names and directories.
- `TERM` stays `foot`. The `foot` terminfo entry is in ncurses, so remote hosts over SSH already know it, and apps keep their foot support. The terminfo build options do not change. When foot's terminfo is already installed, build paw with `-Dterminfo=disabled`.
- Internal names (log modules, `foot-features.h`, shm pool names, source file names) do not change.
- **BREAKING** for anyone who used this fork under the foot names: they must move the config file to the paw path.

## Capabilities

### New Capabilities

- `program-identity`: the names by which users, the desktop, and other programs find and identify paw (binaries, config path, socket, app-id, title, XTVERSION, installed files, TERM).

### Modified Capabilities

None.

## Impact

- C sources: `config.c`, `client.c`, `main.c`, `server.c`, `terminal.c`, `wayland.c`, `render.c`, `csi.c`. About 20 string literals.
- Build: `meson.build`, `completions/meson.build`, `icons/meson.build`, `doc/meson.build`, `pgo/*.sh`.
- Install files: desktop files, systemd units, completions, icon. The installed names change. The source file names stay the same where `install_data(rename:)` allows this.
- No new dependencies.
- Merge cost: each later upstream merge can conflict only on these lines. Upstream seldom edits them.
