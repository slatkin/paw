#pragma once

#include <stdbool.h>
#include <stdint.h>

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

enum kitty_action {
    KITTY_ACTION_TRANSMIT = 0,         /* t */
    KITTY_ACTION_TRANSMIT_AND_DISPLAY, /* T */
    KITTY_ACTION_QUERY,                /* q */
    KITTY_ACTION_PUT,                  /* p */
    KITTY_ACTION_DELETE,               /* d */
    KITTY_ACTION_FRAME,                /* f, unsupported */
    KITTY_ACTION_ANIMATE,              /* a, unsupported */
    KITTY_ACTION_COMPOSE,              /* c, unsupported */
};

enum kitty_format {
    KITTY_FORMAT_RGB = 24,
    KITTY_FORMAT_RGBA = 32,
    KITTY_FORMAT_PNG = 100,
};

enum kitty_medium {
    KITTY_MEDIUM_DIRECT = 0, /* d */
    KITTY_MEDIUM_FILE,       /* f */
    KITTY_MEDIUM_TEMP_FILE,  /* t */
    KITTY_MEDIUM_SHM,        /* s */
};

enum kitty_compression {
    KITTY_COMPRESSION_NONE = 0,
    KITTY_COMPRESSION_ZLIB, /* o=z */
};

/* One parsed graphics command. Field names follow the protocol keys. */
struct kitty_command {
    enum kitty_action action;
    enum kitty_format format;
    enum kitty_medium medium;
    enum kitty_compression compression;

    bool has_width, has_height;
    uint32_t width, height; /* s, v */

    bool has_id, has_number;
    uint32_t id, number; /* i, I */

    bool has_placement_id;
    uint32_t placement_id; /* p */

    bool has_cols, has_rows;
    uint32_t cols, rows; /* c, r */

    bool has_x, has_y;
    int32_t x, y; /* x, y (source rectangle / delete cell) */

    bool has_w, has_h;
    uint32_t w, h; /* w, h (source rectangle size) */

    bool has_X, has_Y;
    int32_t X, Y; /* X, Y (cell offsets) */

    bool has_z;
    int32_t z;

    bool has_cursor;
    uint32_t cursor; /* C */

    bool has_quiet, has_more;
    uint32_t quiet, more; /* q, m */

    bool has_delete;
    char delete_spec; /* d */

    bool has_offset, has_size;
    uint32_t offset, size; /* O, S */

    bool has_unicode_placeholder;
    uint32_t unicode_placeholder; /* U */

    bool has_parent_id, has_parent_placement;
    uint32_t parent_id, parent_placement; /* P, Q */
};

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

/*
 * APC string hooks, called from vt.c. The buffer holds every byte
 * between the APC introducer and the string terminator.
 */
void kitty_apc_start(struct terminal *term);
void kitty_apc_put(struct terminal *term, uint8_t c);
void kitty_apc_end(struct terminal *term);

bool kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y);
