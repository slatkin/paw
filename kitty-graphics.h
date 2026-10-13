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

/* A protocol value that may be absent. */
struct kitty_u32 {
    uint32_t value;
    bool set;
};

struct kitty_i32 {
    int32_t value;
    bool set;
};

static inline struct kitty_u32
kitty_some_u32(uint32_t value)
{
    return (struct kitty_u32){.value = value, .set = true};
}

static inline struct kitty_i32
kitty_some_i32(int32_t value)
{
    return (struct kitty_i32){.value = value, .set = true};
}

/* The value of an optional field, or 'fallback' when it is absent. */
#define kitty_or(opt, fallback) ((opt).set ? (opt).value : (fallback))

/* Distinct types, so that the three kinds of number cannot be mixed up. */
struct kitty_image_id { uint32_t v; };
struct kitty_image_number { uint32_t v; };
struct kitty_placement_id { uint32_t v; };

#define KITTY_IMAGE_ID(n) ((struct kitty_image_id){.v = (n)})
#define KITTY_IMAGE_NUMBER(n) ((struct kitty_image_number){.v = (n)})
#define KITTY_PLACEMENT_ID(n) ((struct kitty_placement_id){.v = (n)})

/* One parsed graphics command. Field names follow the protocol keys. */
struct kitty_command {
    enum kitty_action action;
    enum kitty_format format;
    enum kitty_medium medium;
    enum kitty_compression compression;

    struct kitty_u32 width, height; /* s, v */
    struct kitty_u32 id, number;    /* i, I */
    struct kitty_u32 placement_id;  /* p */
    struct kitty_u32 cols, rows;    /* c, r */

    /* x, y (source rectangle / delete cell) */
    struct kitty_i32 x, y;

    /* w, h (source rectangle size) */
    struct kitty_u32 w, h;

    /* X, Y (cell offsets) */
    struct kitty_i32 X, Y;

    struct kitty_i32 z;
    struct kitty_u32 cursor;               /* C */
    struct kitty_u32 quiet, more;          /* q, m */
    char delete_spec;                      /* d, 0 when absent */
    struct kitty_u32 offset, size;         /* O, S */
    struct kitty_u32 unicode_placeholder;  /* U */
    struct kitty_u32 parent_id, parent_placement; /* P, Q */
};

void kitty_init(struct terminal *term);
void kitty_fini(struct terminal *term);

void kitty_command_init(struct kitty_command *cmd);

void kitty_scroll_up(
    struct terminal *term, struct scroll_region region, int rows);
void kitty_scroll_down(
    struct terminal *term, struct scroll_region region, int rows);
void kitty_clear_visible(struct terminal *term);
void kitty_scrollback_clear(struct terminal *term);
void kitty_alt_screen_clear(struct terminal *term);
void kitty_reset(struct terminal *term);
void kitty_cell_size_changed(struct terminal *term);
void kitty_reflow_begin(struct terminal *term);
void kitty_reflow_end(struct terminal *term);

/*
 * Build the scaled-image cache for every placement visible in the next
 * frame. Called on the main thread, before the render workers start.
 */
void kitty_render_prepass(struct terminal *term);

void kitty_render_overlay(
    struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage);

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

/*
 * True when the cell's base character is the Unicode placeholder
 * U+10EEEE. Called before the glyph lookup, so that fcft never tries
 * to find a font for the placeholder character.
 */
bool kitty_cell_is_placeholder(
    const struct terminal *term, const struct cell *cell);

/*
 * Draw the image slice of a placeholder cell, after the background
 * fill. Returns true when the cell is a placeholder.
 */
bool kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int col, int x, int y);
