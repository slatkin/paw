#include "kitty-placement.h"

#include <stdlib.h>
#include <string.h>

#include "kitty-placement-internal.h"

#define LOG_MODULE "kitty-placement"
#define LOG_ENABLE_DBG 0
#include "debug.h"
#include "grid.h"
#include "log.h"
#include "util.h"

void
kitty_placement_store_init(struct kitty_placement_store *store)
{
    /* A zeroed tll is a valid, empty list. */
    memset(store, 0, sizeof(*store));
}

void
kitty_placement_cache_free(
    struct kitty_placement_store *store, struct kitty_placement *p)
{
    const size_t bytes = p->cache_bytes;

    if (p->cache_pix != NULL)
        pixman_image_unref(p->cache_pix);
    free(p->cache_data);

    p->cache_pix = NULL;
    p->cache_data = NULL;
    p->cache_w = p->cache_h = 0;
    p->cache_bytes = 0;
    p->cache_cell_width = p->cache_cell_height = 0;

    /* The store pointer is NULL when the placement is being torn down
     * together with the store itself. */
    if (store != NULL)
        store->cache_bytes -= bytes;
}

void
kitty_placement_store_fini(struct kitty_placement_store *store)
{
    tll_foreach(store->placements, it)
        kitty_placement_cache_free(NULL, &it->item);
    tll_free(store->placements);
    free(store->underlay);
    store->underlay = NULL;
    store->underlay_count = store->underlay_size = 0;
    store->cache_bytes = 0;
}

bool
kitty_placement_visible_top(
    struct terminal *term, const struct kitty_placement *p, int *out_top)
{
    const int num_rows = term->grid->num_rows;
    const int view = term->grid->view;
    const int rows = term->rows;

    if (num_rows == 0)
        return false;

    const int d = (p->row - view + num_rows) & (num_rows - 1);

    if (d < rows) {
        *out_top = d;
        return true;
    }

    /* Anchor above the viewport, but the placement may reach into it. */
    if (num_rows - d < p->rows) {
        *out_top = -(num_rows - d);
        return true;
    }

    return false;
}

void
kitty_placement_on_image_removed(void *user, struct kitty_image_id image_id)
{
    struct kitty_placement_store *store = user;

    tll_foreach(store->placements, it) {
        if (it->item.image_id.v != image_id.v)
            continue;

        kitty_placement_cache_free(store, &it->item);
        kitty_placement_remove(store, it);
    }
}

/* ceil(num / den), clamped to the range [1, limit]. */
static int
cells_for(int64_t num, int64_t den, uint32_t limit)
{
    const int64_t value = (num + den - 1) / den;
    return (int)(value < 1 ? 1 : value > limit ? limit : value);
}

static void
kitty_placement_cell_span(
    int src_w, int src_h,
    struct kitty_u32 cols, struct kitty_u32 rows,
    int cell_width, int cell_height,
    int *out_cols, int *out_rows)
{
    if (cell_width <= 0)
        cell_width = 1;
    if (cell_height <= 0)
        cell_height = 1;

    /* Clamp the cell size. Anything larger than 4096 pixels per cell is
     * not a real terminal; the clamp keeps the products below in range
     * even with hostile source rectangle sizes. */
    if (cell_width > 4096)
        cell_width = 4096;
    if (cell_height > 4096)
        cell_height = 4096;

    if (src_w <= 0)
        src_w = 1;
    if (src_h <= 0)
        src_h = 1;

    const uint32_t max_cells = KITTY_MAX_CELLS;

    int c = 1;
    int r = 1;

    if (cols.set) {
        c = (int)(cols.value > max_cells ? max_cells
                  : (cols.value == 0 ? 1 : cols.value));
    }
    if (rows.set) {
        r = (int)(rows.value > max_cells ? max_cells
                  : (rows.value == 0 ? 1 : rows.value));
    }

    /* With both c and r given, the span is used as-is. */
    if (!cols.set && !rows.set) {
        c = cells_for(src_w, cell_width, max_cells);
        r = cells_for(src_h, cell_height, max_cells);
    } else if (!rows.set) {
        /* rows = ceil(c * cell_width * src_h / (src_w * cell_height)) */
        r = cells_for((int64_t)c * cell_width * src_h,
                      (int64_t)src_w * cell_height, max_cells);
    } else if (!cols.set) {
        /* cols = ceil(r * cell_height * src_w / (src_h * cell_width)) */
        c = cells_for((int64_t)r * cell_height * src_w,
                      (int64_t)src_h * cell_width, max_cells);
    }

    *out_cols = c;
    *out_rows = r;
}

void
kitty_placement_dirty(struct terminal *term, struct kitty_placement *p)
{
    const int num_rows = term->grid->num_rows;
    const int rows = term->rows;

    /* The visible part of the placement starts at this screen row. It
     * may be negative when the placement starts above the viewport. */
    int top;
    if (!kitty_placement_visible_top(term, p, &top))
        return;

    const int first_col = max(0, p->col);
    const int last_col = min(term->cols, p->col + p->cols);

    for (int i = 0; i < p->rows; i++) {
        const int screen_row = top + i;
        if (screen_row < 0 || screen_row >= rows)
            continue;

        const int abs_row = (p->row + i) & (num_rows - 1);
        struct row *row = term->grid->rows[abs_row];
        if (row == NULL)
            continue;

        row->dirty = true;

        for (int col = first_col; col < last_col; col++)
            row->cells[col].attrs.clean = 0;
    }
}

const char *
kitty_placement_put(
    struct terminal *term, const struct kitty_command *cmd,
    struct kitty_image *image)
{
    xassert(image != NULL);

    struct kitty_placement_store *store = kitty_placement_store_for(term);

    if ((size_t)tll_length(store->placements) >= KITTY_MAX_PLACEMENTS) {
        LOG_WARN("kitty graphics: placement limit (%d) reached",
                 KITTY_MAX_PLACEMENTS);
        return "EINVAL:too many placements";
    }

    /* Source rectangle: x, y, w, h with defaults, intersected with the
     * image. All arithmetic is in 64-bit to stay wrap-safe. */
    const int64_t image_w = image->width;
    const int64_t image_h = image->height;

    const int64_t x0 = kitty_or(cmd->x, 0);
    const int64_t y0 = kitty_or(cmd->y, 0);
    const int64_t w = kitty_or(cmd->w, image_w);
    const int64_t h = kitty_or(cmd->h, image_h);

    const int64_t sx0 = max((int64_t)0, x0);
    const int64_t sy0 = max((int64_t)0, y0);
    const int64_t sx1 = min(image_w, x0 + w);
    const int64_t sy1 = min(image_h, y0 + h);

    const int64_t sw = sx1 - sx0;
    const int64_t sh = sy1 - sy0;

    if (sw <= 0 || sh <= 0)
        return "EINVAL:invalid source rectangle";

    const bool scaled = cmd->cols.set || cmd->rows.set;

    int cols, rows;
    kitty_placement_cell_span(
        (int)sw, (int)sh,
        cmd->cols, cmd->rows,
        term->cell_width, term->cell_height, &cols, &rows);

    /* A placement taller than the whole ring cannot be represented. */
    if (rows > term->grid->num_rows)
        rows = term->grid->num_rows;

    const struct kitty_placement_id placement_id =
        KITTY_PLACEMENT_ID(kitty_or(cmd->placement_id, 0));
    const bool virtual_placement =
        cmd->unicode_placeholder.set && cmd->unicode_placeholder.value != 0;

    struct kitty_placement p = {
        .image_id = image->id,
        .placement_id = placement_id,
        .row = grid_row_absolute(term->grid, term->grid->cursor.point.row),
        .col = term->grid->cursor.point.col,
        /* The offset stays inside the anchor cell, as in kitty. */
        .X = cmd->X.set ? max(0, min(cmd->X.value, term->cell_width - 1)) : 0,
        .Y = cmd->Y.set ? max(0, min(cmd->Y.value, term->cell_height - 1)) : 0,
        .src_x = (int)sx0,
        .src_y = (int)sy0,
        .src_w = (int)sw,
        .src_h = (int)sh,
        .cols = cols,
        .rows = rows,
        .scaled = scaled,
        .cell_width = term->cell_width,
        .cell_height = term->cell_height,
        .virtual_placement = virtual_placement,
        .z = kitty_or(cmd->z, 0),
        .image = image,
    };

    /* Replace an existing placement with the same (image id, placement
     * id) pair. A placement id of 0 never replaces. */
    tll_foreach(store->placements, it) {
        if (p.placement_id.v == 0 ||
            it->item.image_id.v != p.image_id.v ||
            it->item.placement_id.v != p.placement_id.v)
        {
            continue;
        }

        /* kitty_placement_unlink() also keeps the image's placement count in
         * sync. */
        kitty_placement_unlink(store, &it->item);
        kitty_placement_remove(store, it);
        break;
    }

    tll_push_back(store->placements, p);
    image->placements++;

    struct kitty_placement *added = &tll_back(store->placements);

    if (!virtual_placement) {
        kitty_placement_dirty(term, added);

        /* Cursor movement policy: C=0 (default) moves the cursor to the
         * cell after the image's columns, on the image's last row. C=1
         * leaves the cursor alone. */
        if (!cmd->cursor.set || cmd->cursor.value == 0) {
            if (added->cols > 0)
                term_cursor_right(term, added->cols);

            if (added->rows > 1) {
                const int start = term->scroll_region.start;
                const int end = term->scroll_region.end;
                const int cur_row = term->grid->cursor.point.row;
                const int target = cur_row + added->rows - 1;

                if (cur_row >= start && cur_row < end && target >= end) {
                    /*
                     * The move goes past the bottom of the scroll
                     * region: scroll the region, so that the text and
                     * the images move, and leave the cursor on the
                     * region's bottom row.
                     */
                    int amount = target - (end - 1);
                    if (amount > end - start)
                        amount = end - start;
                    term_scroll(term, amount);
                    term_cursor_to(
                        term, end - 1, term->grid->cursor.point.col);
                } else {
                    /* Stop at the bottom of the scroll region when the
                     * cursor is inside it, and at the bottom of the
                     * screen otherwise. */
                    const int bottom =
                        (cur_row >= start && cur_row < end) ? end : term->rows;
                    int stop = target;
                    if (stop >= bottom)
                        stop = bottom - 1;

                    if (stop > cur_row) {
                        term_cursor_to(
                            term, stop, term->grid->cursor.point.col);
                    }
                }
            }
        }
    }

    return NULL;
}

#ifndef NDEBUG
#include "kitty-test.h"

/* The common stub terminal of the tests below: 4 rows of 8 columns. */
#define PLACEMENT_TEST_TERM(t) KITTY_TEST_TERM(t, 4, 8, 8, 16, 8)

UNITTEST
{
    /*
     * A cell size change keeps the cell span and the anchor, and drops
     * only the scaled cache.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_place(&term, 9, 1, 2, 3, 1, 1);
    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    xassert(p->cols == 2 && p->rows == 3);
    const int row = p->row, col = p->col;

    term.cell_width = 16;
    term.cell_height = 32;
    kitty_cell_size_changed(&term);

    p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    xassert(p->cols == 2 && p->rows == 3);
    xassert(p->row == row && p->col == col);
    xassert(p->cache_pix == NULL && p->cache_bytes == 0);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Virtual placement (U=1): a=p creates it for an image, without
     * drawing anything by itself and without moving the cursor. It is
     * not visible as a cell, so d=a (all visible) keeps it. d=i
     * deletes it.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_add_image(&term, 7, 2, 2);

    struct kitty_command put = kitty_test_put_command(7);
    put.cols = kitty_some_u32(2);
    put.rows = kitty_some_u32(2);
    put.unicode_placeholder = kitty_some_u32(1);

    term.normal.cursor.point.row = 2;
    term.normal.cursor.point.col = 3;
    kitty_test_put(&term, &put);

    struct kitty_placement *p = kitty_test_placement(&term, 7);
    xassert(p != NULL);
    xassert(p->virtual_placement);
    xassert(p->cols == 2 && p->rows == 2);

    /* U=1 leaves the cursor where it was. */
    xassert(term.normal.cursor.point.row == 2);
    xassert(term.normal.cursor.point.col == 3);

    /* A normal placement, for contrast: d=a deletes it... */
    kitty_test_place(&term, 8, 1, 1, 1, 1, 1);

    struct kitty_command del;
    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.delete_spec = 'a';
    kitty_placement_delete(&term, &del);

    xassert(kitty_test_placement(&term, 8) == NULL);
    /* ...but keeps the virtual one. */
    xassert(kitty_test_placement(&term, 7) != NULL);

    /* d=i deletes the virtual placement. */
    del.delete_spec = 'i';
    del.id = kitty_some_u32(7);
    kitty_placement_delete(&term, &del);
    xassert(kitty_test_placement(&term, 7) == NULL);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * The remaining virtual-placement delete rules: d=r/R and d=n/N
     * delete a virtual placement; d=c, d=p, d=x, d=y and d=z leave it
     * alone.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_add_image(&term, 7, 1, 1);
    kitty_test_image(&term, 7)->number = kitty_some_u32(5);

    struct kitty_command put = kitty_test_put_command(7);
    put.unicode_placeholder = kitty_some_u32(1);

    struct kitty_command del;

    /* Selectors that do not match virtual placements. */
    static const char *const ignores[] = {"c", "p", "x", "y", "z"};
    for (size_t i = 0; i < sizeof(ignores) / sizeof(ignores[0]); i++) {
        term.normal.cursor.point.row = 2;
        term.normal.cursor.point.col = 3;
        kitty_test_put(&term, &put);

        kitty_command_init(&del);
        del.action = KITTY_ACTION_DELETE;
        del.delete_spec = ignores[i][0];
        del.x = kitty_some_i32(3);
        del.y = kitty_some_i32(2);
        del.z = kitty_some_i32(0);
        kitty_placement_delete(&term, &del);

        xassert(kitty_test_placement(&term, 7) != NULL);
    }

    /* d=r/R matches the image id range and deletes it. */
    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.delete_spec = 'r';
    del.x = kitty_some_i32(1);
    del.y = kitty_some_i32(10);
    kitty_placement_delete(&term, &del);
    xassert(kitty_test_placement(&term, 7) == NULL);

    /* d=n/N matches the newest image with number I. */
    term.normal.cursor.point.row = 1;
    term.normal.cursor.point.col = 1;
    kitty_test_put(&term, &put);

    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.delete_spec = 'n';
    del.number = kitty_some_u32(5);
    kitty_placement_delete(&term, &del);
    xassert(kitty_test_placement(&term, 7) == NULL);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * A placement id of 0 never replaces another placement, and an
     * X or Y offset stays inside the cell.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_place(&term, 9, 0, 1, 1, 1, 1);

    struct kitty_command put = kitty_test_put_command(9);
    put.X = kitty_some_i32(100);
    put.Y = kitty_some_i32(-5);
    kitty_test_put(&term, &put);
    xassert(kitty_test_count(&term) == 2);

    const struct kitty_placement *p = &tll_back(
        kitty_placement_store_for(&term)->placements);
    xassert(p->X == term.cell_width - 1 && p->Y == 0);

    /* The same non-zero pair replaces. */
    put.placement_id = kitty_some_u32(3);
    kitty_test_put(&term, &put);
    kitty_test_put(&term, &put);
    xassert(kitty_test_count(&term) == 3);

    KITTY_TEST_FINI(term);
}

#endif /* !NDEBUG */
