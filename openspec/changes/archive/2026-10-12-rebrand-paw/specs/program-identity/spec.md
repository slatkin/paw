# Spec Delta

## Purpose

Defines the names by which users, the desktop, and other programs find and identify paw, so that paw can be installed and run next to an unmodified foot.

## ADDED Requirements

### Requirement: Binary names
The build SHALL install the terminal as `paw` and the client as `pawclient`. It SHALL NOT install binaries named `foot` or `footclient`.

#### Scenario: Installed binaries
- **WHEN** a user runs `meson install` for paw
- **THEN** the bin directory contains `paw` and `pawclient`
- **AND** the bin directory contains no `foot` or `footclient` from this build

#### Scenario: Version output
- **WHEN** a user runs `paw --version` or `pawclient --version`
- **THEN** the output starts with `paw ` or `pawclient ` respectively

### Requirement: Config file location
When no `--config` option is given, paw SHALL search for `$XDG_CONFIG_HOME/paw/paw.ini` (or `~/.config/paw/paw.ini` when `XDG_CONFIG_HOME` is not set), then for `paw/paw.ini` in each `$XDG_CONFIG_DIRS` directory (default `/etc/xdg`). paw SHALL NOT read `foot/foot.ini`.

#### Scenario: User config in XDG_CONFIG_HOME
- **WHEN** `$XDG_CONFIG_HOME/paw/paw.ini` exists and the user starts paw without `--config`
- **THEN** paw loads that file

#### Scenario: Only a foot config exists
- **WHEN** only `$XDG_CONFIG_HOME/foot/foot.ini` exists
- **THEN** paw does not load the foot file

### Requirement: Default title and app-id
paw SHALL use `paw` as the default window title and app-id. In server mode the default app-id SHALL be `pawclient`. The toplevel icon name SHALL be `paw` when the app-id is `pawclient`.

#### Scenario: Standalone window
- **WHEN** a user starts `paw` with no `--title` or `--app-id`
- **THEN** the compositor sees the app-id `paw` and the title `paw`

#### Scenario: Server window icon
- **WHEN** `pawclient` opens a window with the default app-id
- **THEN** the app-id is `pawclient` and the icon name is `paw`

### Requirement: Server socket path
The default server socket SHALL be `$XDG_RUNTIME_DIR/paw-$WAYLAND_DISPLAY.sock`. When `WAYLAND_DISPLAY` is not set, the default SHALL be `$XDG_RUNTIME_DIR/paw.sock`. When `XDG_RUNTIME_DIR` is not set, the default SHALL be `/tmp/paw.sock`. `pawclient` SHALL try these same paths.

#### Scenario: paw and foot servers run together
- **WHEN** `foot --server` and `paw --server` run in the same session
- **THEN** both servers start, each on its own socket
- **AND** `pawclient` opens a window from the paw server

### Requirement: Terminal identification
paw SHALL reply to XTVERSION (`CSI > 0 q`) with `DCS > | paw(<version>) ST`. The default `TERM` value SHALL stay `foot`.

#### Scenario: XTVERSION query
- **WHEN** an application sends `CSI > 0 q`
- **THEN** paw replies with `ESC P > | paw(` followed by the version and `) ESC \`

#### Scenario: Default TERM
- **WHEN** a shell starts in paw with no `term` config option
- **THEN** `TERM` is `foot`

### Requirement: Installed support files use paw names
The build SHALL install the desktop files, systemd user units, shell completions, icon, man pages, example config, themes, and docs under paw names and paw directories. No installed path SHALL be the same as a path that foot installs, except terminfo when the user enables it.

#### Scenario: Install next to foot
- **WHEN** foot is installed and a user installs paw to the same prefix with `-Dterminfo=disabled`
- **THEN** no file from the foot install is overwritten

#### Scenario: Systemd socket activation
- **WHEN** a user enables `paw-server.socket`
- **THEN** systemd listens on `%t/paw.sock` and starts `paw --server=3` on the first connection
