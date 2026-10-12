# Tasks

Test convention: foot runs `UNITTEST { ... }` blocks as constructors in debug builds. "Unit check" below means: add `UNITTEST` blocks in the new file, then run `meson setup build-dbg --buildtype=debug && ninja -C build-dbg && build-dbg/paw --version` (it aborts on a failed `xassert`). "Live check" means: run the command inside a `build-dbg/paw` window.

## 1. Skeleton and build

- [x] 1.1 Add `kitty-graphics.h` with the hook API from design D1, D5, and D6 as empty functions in `kitty-graphics.c`. Add `struct kitty_graphics *kitty` to `struct terminal`. Create and free it in `term_init` / `term_destroy` (`kitty_fini` next to `sixel_fini`). Verify: `ninja -C build-dbg` builds and `build-dbg/paw` opens a window.
- [x] 1.2 Add `dependency('libpng')`, `dependency('zlib')`, and the `kitty-*.c` sources to the `paw` executable in `meson.build` (design D10). Verify: `ldd build-dbg/paw | grep -E 'png|libz'` shows both.
- [x] 1.3 Add `scripts/kitty-gfx-check.py`. It writes one graphics command (given as an argument) to the tty, then `CSI c`, and prints every reply it reads before the DA1 reply. Verify: in stock foot it prints only the DA1 reply.

## 2. APC parsing and replies

- [ ] 2.1 In `vt.c`, add `STATE_APC_STRING` for `ESC _` and C1 `0x9f`, which calls `kitty_apc_start/put/end` (design D2). SOS and PM keep the old ignored state. Verify: live check `printf '\e_hello\e\\ok\n'` prints `ok`. Unit check: a vt-level test feeds `ESC _ G ... ESC \` and sees `kitty_apc_end` called with the right bytes.
- [ ] 2.2 Implement the control-data parser and the reply writer in `kitty-graphics.c` (spec kitty-graphics-transmission: recognition, replies, `q` modes, `i`+`I` EINVAL, unknown keys ignored, length limits). Verify: unit checks for parsing `a=T,f=100,i=5,q=1`, a bad number, an unknown key, and 1 KiB overflow.
- [ ] 2.3 Implement `a=q` for direct `f=24`/`f=32` data. Verify: `scripts/kitty-gfx-check.py 'a=q,i=31,s=1,v=1,f=24;AAAA'` prints `_Gi=31;OK` before the DA1 reply.

## 3. Image store and decoding

- [ ] 3.1 Implement `kitty-image.c`: per-screen store, ids, `I=` numbers, replacement of an id, RGB/RGBA conversion to premultiplied pixman `a8r8g8b8`, and the data-length check (design D3). Verify: unit checks for an RGB image, an RGBA image, a wrong length, and re-transmission of an id.
- [ ] 3.2 Add PNG decoding through the libpng simplified API, and `o=z` inflate with an output cap. Check the size before allocation. Verify: unit checks with an embedded 2x2 PNG, an embedded zlib stream, a truncated PNG, and a PNG header that claims 100000x100000 (it must fail without a large allocation).
- [ ] 3.3 Add chunked transfer (`m=1` ... `m=0`), and cancel it on `a=d`. Verify: unit check with three chunks. Live check: `kitten icat --transfer-mode=stream <large.png>` returns without an error.
- [ ] 3.4 Add the 320 MB per-screen quota and the eviction order (spec: Storage quota). Verify: unit check with a lowered quota constant (a test-only parameter) that shows the eviction order and the too-large failure.
- [ ] 3.5 Reject `a=f`, `a=a`, `a=c`, `P`, and `Q` with an error reply. Verify: `scripts/kitty-gfx-check.py 'a=f,i=1;AAAA'` prints an error reply.

## 4. File, temp file, and shared memory transfer

- [ ] 4.1 Implement `t=f` with the path checks and the single error reply (design D4, spec kitty-graphics-file-transfer). Verify: unit checks for a regular file, `/dev/zero`, a symlink to `/proc/self/environ`, a FIFO (must not block), a symlink loop, a missing file, and a short file (`O`+`S`). All failures give `EBADF:Failed to read image file`. Live check: `scripts/kitty-gfx-check.py "a=T,t=f,f=100,i=1;$(printf %s /path/to/img.png | base64 -w0)"` shows the image and prints `_Gi=1;OK`.
- [ ] 4.2 Implement `t=t` with the delete rule. Verify: unit checks that `/tmp/tty-graphics-protocol-x` is deleted, `/tmp/photo` is kept, and `$HOME/tty-graphics-protocol` is kept. Live check: `kitten icat --transfer-mode=file <img>` (kitten's `file` mode sends `t=t`) shows the image, and `kitten icat --detect-support` prints a transfer mode.
- [ ] 4.3 Implement `t=s` with the name check and unlink on every path after a valid name. Verify: unit checks for a valid object (unlinked afterwards), short data (error, and unlinked), and the name `/a/b` (error, nothing unlinked). Live check: `kitten icat --transfer-mode=memory <img>` shows the image.

## 5. Placements and the overlay render

- [ ] 5.1 Implement `kitty-placement.c`: create a placement at the cursor with `X,Y`, `x,y,w,h`, `c,r` fit, and the cell span. Replace a placement with the same `(i,p)`. Apply the cursor movement policy `C`. Mark covered rows dirty (design D5). Verify: unit checks for the span calculation with `c` only, `r` only, both, and neither, and for the cursor move.
- [ ] 5.2 Implement hook C (`kitty_render_overlay` after `render_sixel_images`), the scaled-image cache pre-pass, and dirty marking of covered cells (design D6). Verify: live check `kitten icat <png with alpha>` shows the image with correct alpha, and repeated redraws (type in the shell under it) do not make it darker.
- [ ] 5.3 Implement hook A (`kitty_render_underlay` after the background fill in `render_cell`), including the default-background rule for `z < -1073741824`. Verify: live check `kitten icat --z-index=-1 <img>`, then type text over it: the text is visible on the image. Repeat with `--z-index=--1` (kitten's syntax for -1073741825) on a line with a colored background (for example `printf '\e[41m     \e[0m'`): the red cells stay red.
- [ ] 5.4 Implement `a=d` with all selectors in spec kitty-graphics-placement: Deletion, lowercase and uppercase. Verify: unit checks for each `d` value, and that `d=f` changes nothing. Live check: `kitten icat --clear` removes the images.

## 6. Placements follow the terminal

- [ ] 6.1 Add `kitty_scroll_up/down` next to `sixel_scroll_up/down`. Handle scrollback overflow, fixed-margin row swaps, and edge clipping (design D5). Verify: live check `kitten icat <img>; seq 200`, then scroll back with the mouse: the image is next to the same text. Live check with a scroll region: `printf '\e[5;15r'`, an image inside the region, then `seq 30` with the cursor in the region: only the image inside moves. Live check `CSI M` moves an image below the cursor up one row.
- [ ] 6.2 Add `kitty_clear_visible` for ED 2 and ED 3, `kitty_alt_screen_clear` next to the alt sixel cleanup, and `kitty_reset` in `term_reset`. Verify: live check `kitten icat <img>; clear` removes the image. Run `less` on a file and quit: the main-screen image is still there. `printf '\ec'` removes all images. `printf '\e[2K'` on an image row does not remove it.
- [ ] 6.3 Add `kitty_cell_size_changed` next to `sixel_cell_size_changed` and `kitty_reflow_begin/end` at the two reflow sites in `render.c`. Hide main-screen placements while `interactive_resizing.grid != NULL` (design D6, D7). Verify: live check, show an image and press ctrl+plus: the image covers the same cells. Resize the window narrower and wider: the image stays visible and is not deleted. During a drag resize, there is no crash and no image artifact.

## 7. Unicode placeholders

- [ ] 7.1 Add `scripts/generate-kitty-diacritics.py` and the generated `kitty-diacritics.h` (design D9). Verify: unit check that U+0305 is 0, U+030D is 1, U+030E is 2, and that the table has 297 entries.
- [ ] 7.2 Implement virtual placements (`U=1`) and their delete rules. Verify: unit check that `d=a` keeps a virtual placement and `d=i` deletes it.
- [ ] 7.3 Implement hook B (`kitty_render_placeholder` before glyph lookup in `render_cell`): decode the id, the high byte, the placement id, the row and column, the inheritance rules, and the fit and centering (design D8, spec kitty-graphics-placeholders). Verify: unit checks for decoding a cell with 0, 1, 2, and 3 diacritics and for the three inheritance rules. Live check: the two `printf` examples from the kitty spec for image 42 (after `a=T,U=1,i=42,c=2,r=2,q=2`) show the image. `kitten icat --unicode-placeholder <img>` shows the image.
- [ ] 7.4 Live check inside tmux (`set -g allow-passthrough on`): `kitten icat <img>` shows the image in the pane, and the image scrolls with the pane.

## 8. Integration and docs

- [ ] 8.1 Run the existing tests and a cross-tool check. Verify: `meson test -C build-dbg` passes. `kitten icat --detect-support` reports support. `timg -pk <img>` and `chafa -f kitty <img>` show images. `kitten icat` with sixel images also on screen shows both.
- [ ] 8.2 Document the feature: add a "Kitty graphics" section to `README.md` (supported keys, what is not supported, the file-transfer security rules). Add the hook list from design D5/D6 to the "Merging upstream foot" checklist in `README.md` (create the section if `rebrand-paw` has not landed). Verify: the README hook list matches `grep -n 'kitty_' *.c` outside the `kitty-*.c` files.
