# Design

## Context

See proposal.md for the reason. The specs give the behavior. This design covers how the feature attaches to foot with the smallest merge cost.

Facts about the current code:

- **APC.** `vt.c` sends APC, SOS, and PM (`ESC X`, `ESC ^`, `ESC _`, and C1 `0x9f`) to one state, `STATE_SOS_PM_APC_STRING`. It ignores every byte there. OSC uses a separate state with a growable buffer (`term->vt.osc`, `action_osc_start/put/end`).
- **Sixel as the model.** Sixel is foot's model for images over the grid. It keeps `tll(struct sixel) sixel_images` in `struct grid`, with absolute ring-buffer rows. The core calls short hooks: `sixel_scroll_up/down` in `term_scroll_partial` / `term_scroll_reverse_partial`, `sixel_cell_size_changed` in `term_set_fonts`, `sixel_reflow*` in `render.c`, alt-screen cleanup in `csi.c`, and reset and destroy in `terminal.c`.
- **Partial scrolls.** `term_scroll_partial` always moves `grid->offset` by `rows`, then swaps the rows of the fixed top and bottom margins back into place. A row inside the scroll region keeps its absolute index. A row in a fixed margin gets a new absolute index.
- **Rendering.** `render_cell()` paints one cell: it picks the glyph, fills the background (`PIXMAN_OP_SRC`, after the `/* Background */` comment), draws the cursor, and then draws the glyphs. Worker threads (`render_worker_thread`) call it in parallel, one row at a time. `render_sixel_images()` composites sixels after all rows. It marks the cells under a non-opaque sixel dirty, so that the next frame repaints them first.
- **Interactive resize.** During an interactive resize, foot draws a temporary grid without sixels, and reflows the original grid when the resize ends (`render.c`, comment "throw away sixels ... restored when we do the final reflow").
- **Cell colors.** For `COLOR_BASE16` and `COLOR_BASE256`, `cell->attrs.fg` holds the palette index. For `COLOR_RGB`, it holds the 24-bit value. Underline colors are in `row->extra->underline_ranges`.
- **Tests.** foot's unit tests are `UNITTEST { ... }` blocks in `.c` files. They run as constructors at startup in debug builds (`macros.h`).
- **Libraries.** libpng 1.6 and zlib are on the system and already loaded through fcft and freetype.

## Goals / Non-Goals

**Goals:**
- All protocol logic is in new files. Each edit to an upstream file is a short hook, placed next to an existing sixel call or a stable statement.
- `grid.c` does not change.
- When no kitty image exists, the cost is one pointer test per hook.

**Non-Goals:**
- Animation, relative placements, config options (see proposal).
- Image decoding off the main thread.
- Exact per-line image tracking through reflow. Kitty does not do it either (see D7).

## Decisions

### D1: New files and one state pointer

```
kitty-graphics.h       public hook API, the only header upstream files include
kitty-graphics.c       APC buffer, control-data parser, dispatch, replies, chunk state
kitty-image.c/.h       image store per screen, ids/numbers, quota, PNG + zlib decode
kitty-file.c/.h        t=f / t=t / t=s loading and the path checks
kitty-placement.c/.h   placements, delete selectors, scroll/clear/reflow/cell-size
kitty-render.c/.h      hooks A/B/C, scaled-image cache, placeholder decoding
kitty-diacritics.h     the rowcolumn-diacritics table (generated, see D9)
```

`struct terminal` gets one member: `struct kitty_graphics *kitty;`. Main and alternate screen state is inside that struct, keyed by `&term->normal` and `&term->alt`. These addresses do not change: the interactive resize code replaces the *contents* of `term->normal` (`term->normal = *grid`), not the address. Keeping the state outside `struct grid` is the reason `grid.c` needs no change.

Rejected alternative: a placement list in `struct grid`, as sixel does. This puts kitty lines into `grid.c` clone, free, and reflow code, and into the interactive-resize grid setup in `render.c`. Those are the busiest merge areas.

### D2: APC gets its own parser state

In `vt.c`, split `ESC _` (`0x5f`) and C1 `0x9f` out of the shared SOS/PM/APC state into a new `STATE_APC_STRING`. Its switch function is a copy of `state_osc_string_switch`. Printable bytes go to `kitty_apc_put()`. `ESC` (or the C1 `ST`) calls `kitty_apc_end()`. Entry calls `kitty_apc_start()`. SOS and PM keep the old state and stay ignored.

`kitty-graphics.c` owns the buffer. The first byte decides the command: a string that does not start with `G` is dropped. The control data is limited to 1 KiB. The payload is limited to the quota (320 MB of base64), because a single non-chunked command can carry a full image. Over the limit, the command is dropped. This is about 20 lines in `vt.c`, all in the state table.

### D3: Image store and quota

Each screen has an id map, a list ordered by age, and a byte counter for decoded RGBA. Pixel data is stored as pixman `a8r8g8b8` premultiplied, so that compositing is direct (`f=24` gets an opaque alpha). Ids from `I=` are allocated from a counter that skips ids in use. Eviction follows the spec order: images without placements first, oldest first. Virtual placements (U=1) count as placements here.

Decoding uses the libpng simplified API (`png_image_begin_read_from_memory`, `png_image_finish_read` with `PNG_FORMAT_BGRA`, then premultiply). Before the pixel buffer is allocated, the size `w*h*4` is checked against the quota, so that a small PNG with large dimensions cannot cause a large allocation. Inflate uses zlib `inflate` with an output cap, for the same reason.

### D4: File loading (`kitty-file.c`)

The sequence for `t=f` and `t=t`:

```
base64 path --> realpath()                  ELOOP etc. --> fail
            --> deny /proc, /sys, /dev       except /dev/shm
            --> stat(): S_ISREG?             before open (spec: open can have side effects)
            --> open(O_RDONLY|O_NONBLOCK|O_NOCTTY|O_CLOEXEC)
            --> fstat(): S_ISREG?            closes the swap-to-FIFO race
            --> pread(O, S)                  short read --> fail
t=t only    --> unlink() if the real path is under /tmp, /dev/shm or $TMPDIR
                and contains "tty-graphics-protocol"
any failure --> reply "EBADF:Failed to read image file", LOG_DBG the reason
```

`t=s`: validate the name (leading `/`, no other `/`, length <= `NAME_MAX`), then `shm_open(O_RDONLY)`, `fstat`, `mmap`, copy the `O,S` window, `munmap`, `shm_unlink`, `close`. After a valid name, `shm_unlink` runs on every path. All reads are synchronous on the main thread. This is the same as how foot handles all other input.

### D5: Placements

A placement stores: image id, placement id, absolute grid row and column of the anchor, `X,Y`, source rectangle, the cell span (columns and rows), z, and a flag for virtual placements. The cell span is computed once, when the placement is created, from `c,r` or from the pixel size and the cell size at that time. The cell span is fixed after that (spec: font zoom keeps the cell footprint).

Hooks (each next to the matching sixel call):

| Site | Hook | Effect |
|---|---|---|
| `term_scroll_partial` | `kitty_scroll_up(term, region, rows)` | Delete placements whose top row leaves the scrollback. In a partial region: anchors in the fixed margins get the same row swap as the grid rows. Placements that cross a region edge are clipped. |
| `term_scroll_reverse_partial` | `kitty_scroll_down(term, region, rows)` | Mirror of the above |
| `csi.c` ED 2 | `kitty_clear_visible(term)` | Delete visible placements (not virtual ones), then free every image of the screen that has no placement left, including one that never had a placement. |
| `csi.c` ED 3 | `kitty_clear_visible(term)`, then `term_erase_scrollback` → `kitty_scrollback_clear(term)` | As ED 2, and also delete the placements that are not on screen (not virtual ones). |
| `csi.c` `decset_decrst` alt-screen entry and exit, next to "Delete all sixel images on the alt screen" (exit) | `kitty_alt_screen_clear(term)` | Delete the alternate screen images and placements. foot clears alt sixels on exit, so the alt screen is always empty on the next entry. This gives the result the spec requires for entry. |
| `term_reset` | `kitty_reset(term)` | Delete all |
| `term_set_fonts`, next to `sixel_cell_size_changed` | `kitty_cell_size_changed(term)` | Drop the scaled caches. Spans stay. |
| `term_destroy`, next to `sixel_fini` | `kitty_fini(term)` | Free all |
| The two reflow sites in `render.c` | `kitty_reflow_begin/end(term)` | D7 |

Writing text, EL, ECH, ICH, and DCH get **no** hook (spec: text edits do not change images). Sixel needs five overwrite hooks there, kitty needs none. IL and DL already go through `term_scroll_partial` with a sub-region, so they are covered.

Each change to a placement (add, delete, move) sets `row->dirty` for the rows it covers, when those rows exist in the grid. foot then repaints those rows.

### D6: Rendering hooks

```
render_cell()
  |-- pick glyph            HOOK B  kitty_render_placeholder()  if base == U+10EEEE
  |-- clip, damage
  |-- /* Background */ fill
  |                         HOOK A  kitty_render_underlay()      z < 0 placements
  |-- cursor, glyphs
grid_render()
  |-- render_sixel_images()
  |                         HOOK C  kitty_render_overlay()       z >= 0 placements
```

- **Hook C** follows `render_sixel_images()`. It composites each visible placement with `PIXMAN_OP_OVER`, in z order then id order. It also marks the cells under each placement dirty for the next frame, as sixel does for non-opaque images. This stops alpha from adding up on a reused buffer.
- **Hook A** is in `render_cell` after the background fill. It composites, for this one cell rectangle, the slices of all `z < 0` placements that cover the cell. For `z < -1073741824`, it skips cells whose background is not the default. The test is the same one `render_cell` uses for alpha (`bg_src == COLOR_DEFAULT`, or a palette color equal to `colors.bg`). The hook gets the row's absolute index. A per-frame list of underlay placements, built on the main thread before the workers start, makes the per-cell test a quick bounds check.
- **Hook B** is before glyph lookup. When the base character is U+10EEEE, it decodes the image id, placement id, row, and column (D8). It draws the image slice after the background, and it sets the glyph count to 0 so that fcft never tries to find a font for U+10EEEE.
- **Thread safety.** Hooks A and B run in worker threads. They only read kitty state. The parser changes the state only on the main thread between frames. The scaled-image cache is filled on the main thread, in a pre-pass before the workers start, for every placement that is visible in the frame. So the workers never allocate.
- **Scaling.** Each placement has a cached pixman image, scaled to its cell span at the current cell size, built with a pixman transform and `PIXMAN_FILTER_GOOD`. The cache is cleared on cell-size change.
- **Interactive resize.** When `term->interactive_resizing.grid != NULL`, the hooks skip the main screen. This matches how sixels are hidden during an interactive resize.

### D7: Reflow

`kitty_reflow_begin` stores the cursor's absolute row. `kitty_reflow_end` moves every main-screen anchor by `new cursor row - old cursor row`, then deletes the placements that are now outside the scrollback. Kitty does the same: it shifts by the change in content lines and does not track each line. This needs two lines at each of the two reflow sites, and no change in `grid.c`. A per-line mapping would need hook points inside `grid_resize_and_reflow`.

The alternate screen does not reflow (`grid_resize_without_reflow`). Its anchors stay, and placements are clipped at draw time.

### D8: Placeholder decoding

- **Image id.** It comes from the cell foreground: the palette index for `COLOR_BASE16` / `COLOR_BASE256`, or the 24-bit value for `COLOR_RGB`. The high byte comes from the third diacritic.
- **Placement id.** It comes from the underline color in `row->extra->underline_ranges` (0 when the cell has none).
- **Row and column.** They come from the first two combining characters of the cell. Cells with combining characters are stored as composed entries (`composed.c`). Decoding reads the composed entry, which the cell already references. The hook needs no new storage.
- **Inheritance.** The inheritance rules read the cell to the left. Because a worker draws one whole row, the hook can keep the previous placeholder state for the row in a stack variable. Columns are rendered left to right inside `render_row`, so this is safe.

The fit (aspect, centered in `c x r`) is computed once for each virtual placement at the current cell size. It is cached together with the scaled image.

### D9: Diacritics table

Generate `kitty-diacritics.h` from kitty's `rowcolumn-diacritics.txt` (297 code points) with a small script in `scripts/`. Commit the generated header, so that the build has no network step. Lookup is a binary search over the sorted code points, and the index is the value.

### D10: Build

`meson.build`: add `dependency('libpng')` and `dependency('zlib')`, and add the `kitty-*.c` files to the `paw` executable. This is about 4 lines, next to the existing dependency lines and the source list.

## Risks / Trade-offs

- **[Upstream edits `render_cell` near hooks A and B]** → Each hook is one line, next to a stable anchor (the `/* Background */` comment, the glyph-selection block). A conflict is quick to fix by hand. The merge checklist names these two lines.
- **[Upstream changes the sixel hook sites]** → The kitty hooks sit beside them, so the same merge shows both. This is a reminder to check the kitty line too.
- **[Large image decode blocks input on the main thread]** → This is accepted for this change. foot handles sixel the same way. A decode thread is a later change if it is needed.
- **[Hook C marks covered cells dirty every frame, so a large image costs a repaint of those cells each frame]** → Sixel has the same cost. A later optimization can skip this for opaque images, as sixel does.
- **[Reflow shift puts an image next to the wrong line after a heavy rewrap]** → This matches kitty's behavior. The image is not lost.
- **[yazi picks sixel because `TERM=foot`]** → This is outside the scope of this change. See rebrand-paw design D4.
- **[Untrusted PNG or zlib data]** → libpng and zlib with size caps applied before allocation (D3). Decode errors are replies, not crashes.

## Migration Plan

This is an additive feature. Programs that do not send kitty commands see no change. Rollback: revert the commits. No stored state outlives the process.
