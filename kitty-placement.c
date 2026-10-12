#include "kitty-placement.h"

#include <stdlib.h>
#include <string.h>

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
    store->cache_bytes = 0;
}

void
kitty_placement_on_image_removed(void *user, uint32_t image_id)
{
    struct kitty_placement_store *store = user;

    tll_foreach(store->placements, it) {
        if (it->item.image_id != image_id)
            continue;

        kitty_placement_cache_free(store, &it->item);
        tll_remove(store->placements, it);
    }
}

void
kitty_placement_cell_span(
    int src_w, int src_h,
    bool has_cols, uint32_t cols, bool has_rows, uint32_t rows,
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

    if (has_cols)
        c = (int)(cols > max_cells ? max_cells : (cols == 0 ? 1 : cols));
    if (has_rows)
        r = (int)(rows > max_cells ? max_cells : (rows == 0 ? 1 : rows));

    if (has_cols && has_rows) {
        /* Use the given span as-is. */
    } else if (has_cols) {
        /* rows = ceil(c * cell_width * src_h / (src_w * cell_height)) */
        int64_t num = (int64_t)c * cell_width * src_h;
        int64_t den = (int64_t)src_w * cell_height;
        int64_t value = (num + den - 1) / den;
        if (value > max_cells)
            value = max_cells;
        r = (int)(value < 1 ? 1 : value);
    } else if (has_rows) {
        /* cols = ceil(r * cell_height * src_w / (src_h * cell_width)) */
        int64_t num = (int64_t)r * cell_height * src_w;
        int64_t den = (int64_t)src_h * cell_width;
        int64_t value = (num + den - 1) / den;
        if (value > max_cells)
            value = max_cells;
        c = (int)(value < 1 ? 1 : value);
    } else {
        c = (src_w + cell_width - 1) / cell_width;
        r = (src_h + cell_height - 1) / cell_height;
        if (c > (int)max_cells)
            c = (int)max_cells;
        if (r > (int)max_cells)
            r = (int)max_cells;
        if (c < 1)
            c = 1;
        if (r < 1)
            r = 1;
    }

    *out_cols = c;
    *out_rows = r;
}

void
kitty_placement_dirty(struct terminal *term, struct kitty_placement *p)
{
    const int num_rows = term->grid->num_rows;
    const int view = term->grid->view;
    const int rows = term->rows;

    /* The visible part of the placement starts at this screen row. It
     * may be negative when the placement starts above the viewport. */
    int d = (p->row - view + num_rows) & (num_rows - 1);
    int top;

    if (d < rows)
        top = d;
    else if (num_rows - d < p->rows)
        top = -(num_rows - d);
    else
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

    if ((size_t)tll_length(store->placements) >= KITTY_MAX_PLACEMENTS)
        return "EINVAL:too many placements";

    /* Source rectangle: x, y, w, h with defaults, intersected with the
     * image. All arithmetic is in 64-bit to stay wrap-safe. */
    const int64_t image_w = image->width;
    const int64_t image_h = image->height;

    const int64_t x0 = cmd->has_x ? cmd->x : 0;
    const int64_t y0 = cmd->has_y ? cmd->y : 0;
    const int64_t w = cmd->has_w ? cmd->w : image_w;
    const int64_t h = cmd->has_h ? cmd->h : image_h;

    const int64_t sx0 = max((int64_t)0, x0);
    const int64_t sy0 = max((int64_t)0, y0);
    const int64_t sx1 = min(image_w, x0 + w);
    const int64_t sy1 = min(image_h, y0 + h);

    const int64_t sw = sx1 - sx0;
    const int64_t sh = sy1 - sy0;

    if (sw <= 0 || sh <= 0)
        return "EINVAL:invalid source rectangle";

    const bool scaled = cmd->has_cols || cmd->has_rows;

    int cols, rows;
    kitty_placement_cell_span(
        (int)sw, (int)sh,
        cmd->has_cols, cmd->cols, cmd->has_rows, cmd->rows,
        term->cell_width, term->cell_height, &cols, &rows);

    /* A placement taller than the whole ring cannot be represented. */
    if (rows > term->grid->num_rows)
        rows = term->grid->num_rows;

    const uint32_t placement_id =
        cmd->has_placement_id ? cmd->placement_id : 0;
    const bool virtual_placement =
        cmd->has_unicode_placeholder && cmd->unicode_placeholder != 0;

    struct kitty_placement p = {
        .image_id = image->id,
        .placement_id = placement_id,
        .row = grid_row_absolute(term->grid, term->grid->cursor.point.row),
        .col = term->grid->cursor.point.col,
        .X = cmd->has_X ? cmd->X : 0,
        .Y = cmd->has_Y ? cmd->Y : 0,
        .src_x = (int)sx0,
        .src_y = (int)sy0,
        .src_w = (int)sw,
        .src_h = (int)sh,
        .cols = cols,
        .rows = rows,
        .scaled = scaled,
        .virtual_placement = virtual_placement,
        .z = cmd->has_z ? cmd->z : 0,
        .image = image,
    };

    /* Replace an existing placement with the same (image id, placement
     * id) pair. */
    tll_foreach(store->placements, it) {
        if (it->item.image_id != p.image_id ||
            it->item.placement_id != p.placement_id)
        {
            continue;
        }

        kitty_placement_cache_free(store, &it->item);
        tll_remove(store->placements, it);
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
        if (!cmd->has_cursor || cmd->cursor == 0) {
            if (added->cols > 0)
                term_cursor_right(term, added->cols);

            if (added->rows > 1) {
                const int start = term->scroll_region.start;
                const int end = term->scroll_region.end;
                const int cur_row = term->grid->cursor.point.row;
                int target = cur_row + added->rows - 1;

                /* Stop at the bottom of the scroll region when the cursor
                 * is inside it (spec: Cursor movement after placement). */
                const int bottom =
                    (cur_row >= start && cur_row < end) ? end : term->rows;
                if (target >= bottom)
                    target = bottom - 1;

                if (target > cur_row)
                    term_cursor_to(term, target, term->grid->cursor.point.col);
            }
        }
    }

    return NULL;
}

UNITTEST
{
    /* c and r given: the span is exactly c x r. */
    int cols, rows;
    kitty_placement_cell_span(
        100, 50, true, 10, true, 5, 8, 16, &cols, &rows);
    xassert(cols == 10 && rows == 5);
}

UNITTEST
{
    /* Only c: rows from the aspect ratio. 100x50 image, cells 10x10:
     * c=5 -> 50px wide -> 25px high -> 3 rows (ceil). */
    int cols, rows;
    kitty_placement_cell_span(
        100, 50, true, 5, false, 0, 10, 10, &cols, &rows);
    xassert(cols == 5 && rows == 3);
}

UNITTEST
{
    /* Only r: cols from the aspect ratio. 100x50 image, cells 10x10:
     * r=3 -> 30px high -> 60px wide -> 6 columns. */
    int cols, rows;
    kitty_placement_cell_span(
        100, 50, false, 0, true, 3, 10, 10, &cols, &rows);
    xassert(cols == 6 && rows == 3);
}

UNITTEST
{
    /* Neither: the pixel size decides, rounded up. */
    int cols, rows;
    kitty_placement_cell_span(
        100, 50, false, 0, false, 0, 32, 16, &cols, &rows);
    xassert(cols == 4 && rows == 4);

    kitty_placement_cell_span(
        1, 1, false, 0, false, 0, 8, 16, &cols, &rows);
    xassert(cols == 1 && rows == 1);
}

UNITTEST
{
    /* Hostile c is clamped, and never wraps the arithmetic. */
    int cols, rows;
    kitty_placement_cell_span(
        1, 1, true, UINT32_MAX, false, 0, 8, 16, &cols, &rows);
    xassert(cols == (int)KITTY_MAX_CELLS);
    xassert(rows >= 1 && rows <= (int)KITTY_MAX_CELLS);
}
