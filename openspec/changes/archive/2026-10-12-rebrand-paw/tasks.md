# Tasks

## 1. Binaries and build scripts

- [x] 1.1 In `meson.build`, change `project('foot', ...)` to `project('paw', ...)` and the two `executable()` names to `paw` and `pawclient`. Verify: `meson setup build && ninja -C build` produces `build/paw` and `build/pawclient`.
- [x] 1.2 In `pgo/full-inner.sh` and `pgo/partial.sh`, call `${blddir}/paw` and `${blddir}/pawclient`. Verify: `grep -n 'blddir}"/foot' pgo/*.sh` returns nothing.
- [x] 1.3 Change the `print_version_and_features()` prefixes in `main.c` and `client.c` to `"paw "` and `"pawclient "`, and change the help text in `main.c` and `client.c` (binary names, config path, socket path, default title and app-id). Verify: `build/paw --version` starts with `paw ` and `build/pawclient --help` shows `paw-$WAYLAND_DISPLAY.sock`.

## 2. Runtime identity

- [x] 2.1 In `config.c`, change the three config search paths to `/paw/paw.ini` and `/.config/paw/paw.ini`. Verify: with `XDG_CONFIG_HOME=$(mktemp -d)` holding `paw/paw.ini` with `title=from-paw`, `build/paw` opens with that title. With only `foot/foot.ini` present, the title is `paw`.
- [x] 2.2 In `config.c`, change the default `.title` to `"paw"` and `.app_id` to `"pawclient"` / `"paw"`. In `main.c` and `server.c`, change the spawn app-id arguments. In `render.c`, change the fallback title. In `terminal.c` and `wayland.c`, change the icon mapping to `streq(app_id, "pawclient") ? "paw"`. Verify: the existing `tests/test-config.c` passes (`meson test -C build`), and a running `build/paw` reports app-id `paw` (for example in the compositor's window list).
- [x] 2.3 In `config.c` `get_server_socket_path()` and in the `client.c` fallback chain, change the socket names to `paw-%s.sock`, `paw.sock`, and `/tmp/paw.sock`. Change the `client.c` error text to `is 'paw --server' running?`. Verify: start `foot --server` and `build/paw --server` in the same session. Both start, and `build/pawclient` opens a paw window.
- [x] 2.4 In `csi.c`, change the XTVERSION reply to `"\033P>|paw(%u.%u.%u%s%s)\033\\"`. Verify: inside `build/paw`, `printf '\e[>0q'; read -rsd '\' r; echo "$r" | cat -v` shows `^[P>|paw(`.

## 3. Installed files

- [x] 3.1 In `meson.build`, install the desktop files with `rename:` as `paw.desktop`, `paw-server.desktop`, `pawclient.desktop`. Edit `Exec=` and `Icon=` (and `Name=` if it says foot) in the three source files. Verify: `desktop-file-validate` passes on the installed files.
- [x] 3.2 Install the systemd units as `paw-server.service` and `paw-server.socket` (set `output:` in `configure_file()`, and `rename:` in `install_data()`). Edit `ExecStart`, `ListenStream`, `Description`, and `Documentation` as design D2 says. Verify: `systemd-analyze verify --user` on the installed units shows no errors.
- [x] 3.3 In `completions/meson.build`, install with `rename:` as `_paw`, `_pawclient`, `paw.fish`, `pawclient.fish`, `paw`, `pawclient`. In the six completion files, change the command names they complete to `paw` and `pawclient`. Verify: `fish -c 'complete -C "paw --"'` lists paw options after install to a test prefix.
- [x] 3.4 In `icons/meson.build`, install the 48x48 PNG and the scalable SVG as `paw.png` and `paw.svg` (design D5). Verify: the install tree has `share/icons/hicolor/scalable/apps/paw.svg` and no `foot.*`.
- [x] 3.5 In `meson.build`, install the example config to `sysconfdir/xdg/paw/paw.ini`, and the docs and themes under `datadir/doc/paw` and `datadir/paw`. In `doc/meson.build`, change the man page output names to `paw.1`, `paw.ini.5`, `pawclient.1`, `paw-ctlseqs.7` (design D6). Verify: `meson install -C build --destdir $(mktemp -d)`, then `find` in that directory shows no path that contains `foot`, except terminfo.

## 4. Integration check and docs

- [x] 4.1 Install foot and paw to one prefix (`--destdir`, the same `--prefix`) with paw built as `-Dterminfo=disabled`. Verify: the two file lists have no path in common.
- [x] 4.2 Add a "Merging upstream foot" section to `README.md`. It describes the merge steps and the check for new `"foot` string literals (design, Risks). Add a `CHANGELOG.md` entry that tells users to move `~/.config/foot/foot.ini` to `~/.config/paw/paw.ini`. Verify: the README command runs as written on the current tree and prints nothing.
