# Proposal

## Why

foot shows images only through sixel. Many terminal programs (kitty `icat`, ranger, image.nvim, timg, chafa, and anything that uses tmux image passthrough) use the kitty graphics protocol. paw adds this protocol. paw must also keep merging upstream foot often, so the feature must be an addition next to foot's code, not a rewrite of it.

## What Changes

- paw accepts kitty graphics commands (`ESC _ G <keys> ; <payload> ESC \`). Today foot reads APC strings and discards them.
- Image transmission:
  - Direct, in the escape code, including chunked transfer (`m=1`).
  - From a file (`t=f`), a temp file (`t=t`), or POSIX shared memory (`t=s`), with the security rules of the kitty spec.
- Pixel formats: 24-bit RGB (`f=24`), 32-bit RGBA (`f=32`), and PNG (`f=100`). zlib compression (`o=z`).
- Actions: transmit (`a=t`), transmit and display (`a=T`), place (`a=p`), delete (`a=d`), and query (`a=q`). Replies follow the spec, including the quiet modes (`q=1`, `q=2`).
- Placement:
  - Source rectangle (`x,y,w,h`), cell size (`c,r`), and pixel offset (`X,Y`).
  - Cursor movement policy (`C`).
  - Placement ids (`p`).
  - Z-index layers: over text, under text, and under non-default backgrounds.
- Images scroll with the text. Screen clear, alternate screen entry, and terminal reset delete images, as the spec requires. Other text edits do not change images.
- Unicode placeholders (`U=1`, U+10EEEE with row/column diacritics), so images work inside tmux.
- A per-terminal image memory quota with eviction of the oldest images.
- New build dependencies: libpng and zlib.
- Out of scope: animation (`a=f`, `a=a`, `a=c`), relative placements (`P`, `Q`, `H`, `V`), and config options.

## Capabilities

### New Capabilities

- `kitty-graphics-transmission`: the command format, direct image transfer, pixel formats, compression, image storage and quota, query, and replies.
- `kitty-graphics-file-transfer`: loading image data from files, temp files, and shared memory, and the security rules for these sources.
- `kitty-graphics-placement`: putting images on screen, z-index layers, cursor movement, placement deletion, and how placements react to scroll, clear, alternate screen, reset, and resize.
- `kitty-graphics-placeholders`: images shown through Unicode placeholder characters in the text.

### Modified Capabilities

None.

## Impact

- New files: `kitty-graphics*.c` / `.h` (parser, image store, placements, file loading, rendering).
- Upstream files that get one-line hooks: `vt.c` (APC collection, about 15 lines), `terminal.h` (one pointer), `terminal.c` (scroll, reset, cell size, destroy), `csi.c` (screen clear, alternate screen), `render.c` (three render hooks, reflow), `meson.build` (sources and dependencies).
- `grid.c` does not change.
- Dependencies: libpng and zlib. Both are already loaded at runtime through fcft and freetype.
- Sixel does not change. Sixel and kitty images can be on screen together.
- Depends on nothing in `rebrand-paw`. The two changes can land in either order.
