# Kitty graphics

paw implements a part of the [kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/). paw stores an
image per screen buffer. The main and the alternate screen have separate
stores.

A graphics command is an APC string: `ESC _ G <control data> [; <base64
payload>] ESC \`. The control data is a comma-separated list of `key=value`
pairs. paw ignores an APC string that does not start with `G`. paw also
ignores SOS and PM strings. The control data is limited to 1 KiB.

### Supported keys

| Key | Meaning |
| --- | --- |
| `a` | Action: `t` transmit, `T` transmit and display, `q` query, `p` put, `d` delete. The default is `t`. |
| `f` | Pixel format: `24` (RGB), `32` (RGBA, the default), `100` (PNG). |
| `t` | Medium: `d` direct (the default), `f` file, `t` temp file, `s` shared memory. |
| `o` | Compression: `z` (zlib). No other value is accepted. |
| `s`, `v` | Image width and height, in pixels. |
| `i`, `I` | Image id, image number. A command cannot use both. |
| `p` | Placement id. |
| `c`, `r` | Placement size, in columns and rows. |
| `x`, `y` | Source rectangle position. For `a=d`, the delete cell (1-based). |
| `w`, `h` | Source rectangle size. |
| `X`, `Y` | Pixel offset from the top-left of the cell. paw limits it to the cell size minus 1. |
| `z` | Z-index. |
| `C` | Cursor movement: `0` move (the default), `1` do not move. |
| `q` | Quiet mode: `1` suppresses `OK` replies, `2` suppresses all replies. |
| `m` | More chunks: `1` more data follows, `0` last chunk. |
| `d` | Delete selector. See below. |
| `O`, `S` | File or shared memory byte offset and byte count. |
| `U` | `1` creates a Unicode placeholder placement. |

paw ignores unknown keys. An example is the usage hint `N=1`.

The delete selectors are `a` (all visible, the default), `i` (image id), `n`
(the newest image with number `I`), `r` (image ids `x` to `y`), `c` (at the
cursor), `p` (at cell `x,y`), `q` (at cell `x,y` with z-index `z`), `x`
(column `x`), `y` (row `y`), and `z` (z-index `z`). Every selector has an
uppercase form: `A`, `I`, `N`, `R`, `C`, `P`, `Q`, `X`, `Y`, `Z`. An
uppercase selector also frees the image data of every image that is left
with no placement. An image that never had a placement is freed only by `I`,
`N`, and `R` (without `p`). The selectors `i`/`I`, `n`/`N`, and `r`/`R` also
delete virtual placements. The selectors `a`, `c`, `p`, `q`, `x`, `y`, and `z`
do not affect virtual placements. The animation selector `d=f` changes
nothing.

paw replies to a command that has `i` or `I`. The reply is `ESC _ G
i=<id>[,I=<number>][,p=<placement id>] ; <message> ESC \`. The message is
`OK` on success. On failure the message is an error code and text, for
example `ENOENT:no such image`. paw sends no reply when the command has
neither `i` nor `I`.

`a=q` loads and checks the image data. It stores nothing and replaces no
image.

### Not supported

paw rejects these with an error reply (when a reply is due), and changes no
state:

* animation: `a=f` (frame), `a=a` (animate), `a=c` (compose);
* relative placements: the `P` and `Q` keys;
* a value of `a`, `f`, `o`, or `t` that is not in the table above.

### Transmission

* **Direct** (`t=d`, the default). The base64 payload carries the pixel data.
  A command with `m=1` starts a chunked transfer. paw joins the payloads of
  the following `m=1` commands and of the last `m=0` command into one image.
  Only the first chunk carries the full control data. An `a=d` command
  cancels the transfer, and then runs as a normal delete command.
* **Compressed** (`o=z`). paw inflates the payload with zlib before it reads
  the pixel format.
* **PNG** (`f=100`). paw reads the size from the PNG data. For `f=24` and
  `f=32`, `s` and `v` are required, and the data length must be exactly
  `s * v * 3` or `s * v * 4` bytes.
* **File** (`t=f`). The payload is a base64 file path.
* **Temp file** (`t=t`). paw reads the file and then deletes it, under the
  conditions below.
* **Shared memory** (`t=s`). The payload is a base64 POSIX shared memory
  name.

### File-transfer security

paw resolves symlinks first. Then it applies these rules to `t=f` and `t=t`:

* paw reads regular files only.
* paw refuses paths under `/proc`, `/sys`, and `/dev`, except paths under
  `/dev/shm`.
* paw checks the path before the open, opens it without blocking, and checks
  again that the open file is a regular file. A named pipe cannot block paw.
* paw deletes a `t=t` file only when the resolved path is inside `/tmp`,
  `/dev/shm`, or `$TMPDIR`, and the path contains `tty-graphics-protocol`.
  In every other case paw reads the file when the rules allow it, and keeps
  it.

For `t=s`, the name must start with `/`, contain no other `/`, and be no
longer than `NAME_MAX`. paw unlinks the object after a valid name, also when
the read or the decode fails. paw does not unlink the object when the name
is invalid.

Every file or shared memory failure gives the same reply:
`EBADF:Failed to read image file`. paw writes the real reason to its log
only.

### Storage quota

Each screen buffer stores at most 320 MB of decoded pixel data. When a new
image would exceed the limit, paw deletes images that have no placement
first, oldest first. Then paw deletes the oldest images that have a
placement. An image larger than the limit fails, and paw deletes no other
image.

### Placement limits

Each screen buffer holds at most 4096 placements. When a new placement would
exceed this count, paw deletes the oldest placement of that screen buffer
first, and writes a warning to its log.

paw keeps a scaled copy of each placed image, for fast drawing. Each screen
buffer uses at most 128 MB for these copies. When a copy would exceed the
limit, paw does not keep it. It scales the image from the source pixels on
each frame instead, which is slower but shows the same picture.

### Unicode placeholders

With `U=1`, `a=p` and `a=T` create a virtual placement. A virtual placement
draws nothing by itself and does not move the cursor. A cell whose base
character is `U+10EEEE` then shows part of the image:

* the foreground color gives the image id. It is the 8-bit index in 256-color
  mode, and the 24-bit value in true-color mode;
* a third diacritic gives the most significant byte of the id;
* the underline color, when set, gives the placement id;
* the first and second diacritics give the row and the column, through the
  kitty `rowcolumn-diacritics` table.

A cell with missing diacritics inherits the values from the placeholder cell
to its left. This applies only when that cell has the same foreground and
underline colors. paw scales the image to fit the `c` by `r` cell area,
keeps the aspect ratio, and centers the image. Transparent pixels show the
cell background. Placeholder cells scroll, reflow, enter the scrollback, and
are erased like other text cells.

### Screen state

* Clear screen (`CSI 2 J`) deletes every placement that is visible on the
  screen. `CSI 3 J` also deletes the placements in the scrollback. In both
  cases paw then frees every image that no placement refers to any more. A
  later `a=p` for a freed image replies `ENOENT`.
* Leaving the alternate screen deletes all placements and images of the
  alternate screen. The main screen keeps its images.
* A terminal reset (RIS) deletes every placement and image on both screens.
* Scrolling moves placements with the text. paw deletes a placement whose top
  row leaves the scrollback. In a scroll region, only placements fully inside
  the region move. Parts that leave the region are clipped.
* A font size change scales each placement, so that it covers the same
  columns and rows. A window resize moves placements by the same number of
  lines as the text near the cursor.

This feature has no configuration. The `foot.ini` options and the
**foot.ini**(5) man page are unchanged.
