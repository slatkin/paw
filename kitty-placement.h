#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pixman.h>
#include <tllist.h>

#include "kitty-graphics.h"
#include "kitty-image.h"

/*
 * Placement geometry and lifetime (design D5).
 *
 * All limits below exist because the input is untrusted. They are
 * deliberately larger than any real client needs.
 */
enum {
    /* Maximum number of placements per screen (main or alternate). */
    KITTY_MAX_PLACEMENTS = 4096,

    /*
     * Maximum number of cells a placement may span on one axis.
     * Keeps the span arithmetic and the row/column loops bounded.
     */
    KITTY_MAX_CELLS = 1u << 20,

    /*
     * Total budget for all scaled-image caches of one screen. A cache
     * that would exceed the remaining budget is not built; the
     * placement is then simply not drawn.
     */
    KITTY_PLACEMENT_CACHE_QUOTA = 128u * 1024 * 1024,
};

struct kitty_placement {
    uint32_t image_id;
    uint32_t placement_id;

    /* Absolute ring-buffer row and column of the anchor cell. */
    int row;
    int col;

    /* Pixel offset from the anchor cell's top-left corner (X, Y). */
    int X, Y;

    /* Source rectangle, in image pixels (x, y, w, h, after clipping). */
    int src_x, src_y;
    int src_w, src_h;

    /* Cell span. Fixed at creation; not recomputed on font zoom. */
    int cols, rows;

    /* True if c and/or r was given: the image is scaled to the cell
     * box instead of keeping its pixel size. */
    bool scaled;

    bool virtual_placement; /* U=1: drawn by the placeholder hook */
    int32_t z;

    /* Borrowed; valid for as long as the image store holds the image. */
    struct kitty_image *image;

    /* Scaled cache, keyed on the cell size it was built for. */
    uint32_t *cache_data;
    pixman_image_t *cache_pix;
    int cache_w, cache_h;      /* Size of the scaled image */
    int cache_x, cache_y;      /* Offset inside the cell box */
    int cache_bytes;
    int cache_cell_width, cache_cell_height;
};

struct kitty_placement_store {
    tll(struct kitty_placement) placements;

    /* Sum of kitty_placement.cache_bytes for this screen. */
    size_t cache_bytes;
};

struct kitty_placement_store *kitty_placement_store_for(struct terminal *term);

void kitty_placement_store_init(struct kitty_placement_store *store);
void kitty_placement_store_fini(struct kitty_placement_store *store);

/* Image store callback: drop placements that refer to 'image_id'. */
void kitty_placement_on_image_removed(void *user, uint32_t image_id);

/*
 * Compute the cell span of a placement from the source rectangle and
 * the optional c/r keys. Pure function, so it is easy to unit test.
 */
void kitty_placement_cell_span(
    int src_w, int src_h,
    bool has_cols, uint32_t cols, bool has_rows, uint32_t rows,
    int cell_width, int cell_height,
    int *out_cols, int *out_rows);

/*
 * Create (or replace) a placement for 'image'. The image must already
 * be in the store of the current screen. Returns NULL on success, or a
 * printable error code and text.
 */
const char *kitty_placement_put(
    struct terminal *term, const struct kitty_command *cmd,
    struct kitty_image *image);

/* Drop one placement's scaled cache and update the store's budget. */
void kitty_placement_cache_free(
    struct kitty_placement_store *store, struct kitty_placement *p);

/* Mark the cells covered by a placement dirty. */
void kitty_placement_dirty(struct terminal *term, struct kitty_placement *p);
