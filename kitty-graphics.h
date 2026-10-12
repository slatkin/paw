#pragma once

#include "terminal.h"

/*
 * Kitty graphics protocol.
 *
 * This header is the public hook API. Upstream files (vt.c, csi.c,
 * render.c, terminal.c) include it and call the hooks below. All
 * protocol logic lives in the kitty-*.c files.
 *
 * When no kitty image exists, each hook is a single pointer test.
 */

void kitty_init(struct terminal *term);
void kitty_fini(struct terminal *term);

void kitty_scroll_up(
    struct terminal *term, struct scroll_region region, int rows);
void kitty_scroll_down(
    struct terminal *term, struct scroll_region region, int rows);
void kitty_clear_visible(struct terminal *term);
void kitty_alt_screen_clear(struct terminal *term);
void kitty_reset(struct terminal *term);
void kitty_cell_size_changed(struct terminal *term);
void kitty_reflow_begin(struct terminal *term);
void kitty_reflow_end(struct terminal *term);

void kitty_render_overlay(
    struct terminal *term, pixman_image_t *pix,
    pixman_region32_t *damage, const struct coord *cursor);

void kitty_render_underlay(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y);

bool kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y);
