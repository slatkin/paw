#include "kitty-placement.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "kitty-placement"
#define LOG_ENABLE_DBG 0
#include "debug.h"
#include "grid.h"
#include "log.h"
#include "util.h"
#include "xmalloc.h"

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
        /* The replaced placement refers to the same image; keep the
         * image's placement count in sync. */
        if (it->item.image != NULL && it->item.image->placements > 0)
            it->item.image->placements--;
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

static bool
placement_covers_abs_row(
    struct terminal *term, const struct kitty_placement *p, int64_t abs_row)
{
    const int num_rows = term->grid->num_rows;
    if (num_rows <= 0 || p->rows <= 0 || p->rows > num_rows)
        return false;

    int64_t delta = (abs_row - p->row) % num_rows;
    if (delta < 0)
        delta += num_rows;
    return delta < p->rows;
}

static bool
placement_covers_screen_row(
    struct terminal *term, const struct kitty_placement *p, int screen_row)
{
    const int num_rows = term->grid->num_rows;
    if (screen_row < 0 || screen_row >= num_rows)
        return false;

    const int64_t abs_row =
        ((int64_t)term->grid->view + screen_row) % num_rows;
    return placement_covers_abs_row(term, p, abs_row);
}

static bool
placement_covers_screen_cell(
    struct terminal *term, const struct kitty_placement *p,
    int col, int screen_row)
{
    if (col < p->col || col >= p->col + p->cols)
        return false;
    return placement_covers_screen_row(term, p, screen_row);
}

/*
 * Test one placement against the command's `d` selector. Coordinates
 * are 1-based, as in kitty. Placements that are not visible as cells
 * (virtual placements) are only matched by the selectors that kitty
 * applies to them (i, r, n).
 */
static bool
delete_matches(
    struct terminal *term, const struct kitty_command *cmd,
    const struct kitty_placement *p)
{
    const char sel = cmd->has_delete ? cmd->delete_spec : 'a';
    const int num_rows = term->grid->num_rows;

    switch (sel) {
    case 'a':
    case 'A': {
        if (p->virtual_placement)
            return false;
        int top;
        return kitty_placement_visible_top(term, p, &top);
    }

    case 'i':
    case 'I':
        if (!cmd->has_id || p->image_id != cmd->id)
            return false;
        return !cmd->has_placement_id || p->placement_id == cmd->placement_id;

    case 'n':
    case 'N':
        if (!cmd->has_number || p->image == NULL)
            return false;
        if (!p->image->has_number || p->image->number != cmd->number)
            return false;
        return !cmd->has_placement_id || p->placement_id == cmd->placement_id;

    case 'r':
    case 'R':
        if (!cmd->has_x || !cmd->has_y)
            return false;
        return (int64_t)cmd->x <= (int64_t)p->image_id &&
            (int64_t)p->image_id <= (int64_t)cmd->y;

    case 'c':
    case 'C': {
        if (p->virtual_placement)
            return false;
        const int col = term->grid->cursor.point.col;
        if (col < p->col || col >= p->col + p->cols)
            return false;
        const int64_t abs_row = grid_row_absolute(
            term->grid, term->grid->cursor.point.row);
        return placement_covers_abs_row(term, p, abs_row);
    }

    case 'p':
    case 'P': {
        if (p->virtual_placement || !cmd->has_x || !cmd->has_y)
            return false;
        const int64_t sy = (int64_t)cmd->y - 1;
        if (cmd->x < 1 || sy < 0 || sy >= num_rows)
            return false;
        return placement_covers_screen_cell(term, p, (int)cmd->x - 1, (int)sy);
    }

    case 'q':
    case 'Q': {
        if (p->virtual_placement || !cmd->has_x || !cmd->has_y || !cmd->has_z)
            return false;
        if (p->z != cmd->z)
            return false;
        const int64_t sy = (int64_t)cmd->y - 1;
        if (cmd->x < 1 || sy < 0 || sy >= num_rows)
            return false;
        return placement_covers_screen_cell(term, p, (int)cmd->x - 1, (int)sy);
    }

    case 'x':
    case 'X':
        if (p->virtual_placement || !cmd->has_x || cmd->x < 1)
            return false;
        return (int64_t)cmd->x - 1 >= p->col &&
            (int64_t)cmd->x - 1 < (int64_t)p->col + p->cols;

    case 'y':
    case 'Y': {
        if (p->virtual_placement || !cmd->has_y)
            return false;
        const int64_t sy = (int64_t)cmd->y - 1;
        return placement_covers_screen_row(term, p, (int)sy);
    }

    case 'z':
    case 'Z':
        if (p->virtual_placement || !cmd->has_z)
            return false;
        return p->z == cmd->z;

    default:
        /* Unknown selectors, and 'f' (animation frames), change
         * nothing. */
        return false;
    }
}

static void
delete_image_if_unused(struct kitty_image_store *images, uint32_t id)
{
    struct kitty_image *image = kitty_image_store_find_id(images, id);
    if (image != NULL && image->placements == 0)
        kitty_image_store_delete_id(images, id);
}

void
kitty_placement_delete(struct terminal *term, const struct kitty_command *cmd)
{
    const char sel = cmd->has_delete ? cmd->delete_spec : 'a';

    /* Animation frames are not supported: d=f changes nothing. */
    if (sel == 'f' || sel == 'F')
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    struct kitty_image_store *images = kitty_image_store_for(term);
    const bool uppercase = sel >= 'A' && sel <= 'Z';

    tll(uint32_t) touched = tll_init();

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (!delete_matches(term, cmd, p))
            continue;

        if (p->image != NULL && p->image->placements > 0)
            p->image->placements--;

        /* Repaint the cells the placement covered. */
        kitty_placement_dirty(term, p);
        kitty_placement_cache_free(store, p);

        const uint32_t id = p->image_id;
        tll_remove(store->placements, it);

        bool seen = false;
        tll_foreach(touched, t) {
            if (t->item == id) {
                seen = true;
                break;
            }
        }
        if (!seen)
            tll_push_back(touched, id);
    }

    if (uppercase) {
        /*
         * Kitty frees an image with no placement left for I, N and R
         * even when no placement matched.
         */
        if (!cmd->has_placement_id) {
            switch (sel) {
            case 'I':
                if (cmd->has_id)
                    delete_image_if_unused(images, cmd->id);
                break;

            case 'N':
                if (cmd->has_number) {
                    struct kitty_image *image =
                        kitty_image_store_find_number(images, cmd->number);
                    if (image != NULL)
                        delete_image_if_unused(images, image->id);
                }
                break;

            case 'R':
                if (cmd->has_x && cmd->has_y) {
                    tll(uint32_t) range = tll_init();
                    tll_foreach(images->images, img_it) {
                        const uint32_t id = img_it->item.id;
                        if ((int64_t)cmd->x <= (int64_t)id &&
                            (int64_t)id <= (int64_t)cmd->y)
                        {
                            tll_push_back(range, id);
                        }
                    }
                    tll_foreach(range, r)
                        delete_image_if_unused(images, r->item);
                    tll_free(range);
                }
                break;

            default:
                break;
            }
        }

        tll_foreach(touched, t)
            delete_image_if_unused(images, t->item);
    }

    tll_free(touched);
}

UNITTEST
{
    /*
     * Replacing a placement with the same (image id, placement id)
     * must not leak the image's placement count: it stays one.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    term.rows = 4;
    term.cols = 4;
    term.cell_width = 8;
    term.cell_height = 16;
    term.normal.num_rows = 8;
    term.normal.rows = xcalloc(8, sizeof(term.normal.rows[0]));
    term.grid = &term.normal;

    kitty_init(&term);

    struct kitty_image_store *images = kitty_image_store_for(&term);

    uint32_t *pixels = xcalloc(1, sizeof(pixels[0]));
    pixels[0] = 0xff000000u;
    struct kitty_image proto = {
        .id = 5,
        .width = 1,
        .height = 1,
        .data = pixels,
        .size = sizeof(pixels[0]),
    };
    xassert(kitty_image_store_add(images, &proto));
    struct kitty_image *img = kitty_image_store_find_id(images, 5);
    xassert(img != NULL);

    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.action = KITTY_ACTION_PUT;
    cmd.has_id = true;
    cmd.id = 5;
    cmd.has_placement_id = true;
    cmd.placement_id = 0;
    cmd.has_cursor = true;
    cmd.cursor = 1;

    for (int i = 0; i < 100; i++)
        xassert(kitty_placement_put(&term, &cmd, img) == NULL);

    xassert(img->placements == 1);
    xassert(tll_length(kitty_placement_store_for(&term)->placements) == 1);

    kitty_fini(&term);
    free(term.normal.rows);
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

UNITTEST
{
    /*
     * Delete selectors. Every case adds image 4 (number 7), places it
     * as a 2x2 placement at the cursor, runs the selector and checks
     * that the placement is gone. Lowercase selectors keep the image
     * data; uppercase selectors free it once no placement is left.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    term.rows = 4;
    term.cols = 8;
    term.cell_width = 8;
    term.cell_height = 16;
    term.normal.num_rows = 8;
    term.normal.rows = xcalloc(8, sizeof(term.normal.rows[0]));
    term.grid = &term.normal;

    kitty_init(&term);

    struct kitty_image_store *images = kitty_image_store_for(&term);
    struct kitty_placement_store *store = kitty_placement_store_for(&term);

#define DELETE_SETUP() do {                                             \
        uint32_t *px = xcalloc(1, sizeof(px[0]));                       \
        px[0] = 0xff000000u;                                            \
        struct kitty_image proto = {                                    \
            .id = 4, .has_number = true, .number = 7,                   \
            .width = 1, .height = 1,                                    \
            .data = px, .size = sizeof(px[0]),                          \
        };                                                              \
        xassert(kitty_image_store_add(images, &proto));                 \
        struct kitty_image *im = kitty_image_store_find_id(images, 4);  \
        xassert(im != NULL);                                            \
        struct kitty_command pc;                                        \
        kitty_command_init(&pc);                                        \
        pc.action = KITTY_ACTION_PUT;                                   \
        pc.has_id = true; pc.id = 4;                                    \
        pc.has_cols = true; pc.cols = 2;                                \
        pc.has_rows = true; pc.rows = 2;                                \
        pc.has_cursor = true; pc.cursor = 1;                            \
        xassert(kitty_placement_put(&term, &pc, im) == NULL);           \
        xassert(tll_length(store->placements) == 1);                    \
        xassert(im->placements == 1);                                   \
    } while (0)

#define DELETE_RUN(selchar, expect_image, keys) do {                    \
        DELETE_SETUP();                                                 \
        struct kitty_command dc;                                        \
        kitty_command_init(&dc);                                        \
        dc.action = KITTY_ACTION_DELETE;                                \
        dc.has_delete = true; dc.delete_spec = (selchar);               \
        keys;                                                           \
        kitty_placement_delete(&term, &dc);                             \
        xassert(tll_length(store->placements) == 0);                    \
        if (expect_image) {                                             \
            struct kitty_image *img =                                  \
                kitty_image_store_find_id(images, 4);                   \
            xassert(img != NULL && img->placements == 0);               \
        } else {                                                        \
            xassert(kitty_image_store_find_id(images, 4) == NULL);      \
        }                                                               \
    } while (0)

    DELETE_RUN('a', true, (void)0);
    DELETE_RUN('A', false, (void)0);
    DELETE_RUN('i', true, (dc.has_id = true, dc.id = 4));
    DELETE_RUN('I', false, (dc.has_id = true, dc.id = 4));
    DELETE_RUN('n', true, (dc.has_number = true, dc.number = 7));
    DELETE_RUN('N', false, (dc.has_number = true, dc.number = 7));
    DELETE_RUN('c', true, (void)0);
    DELETE_RUN('C', false, (void)0);
    DELETE_RUN('p', true, (dc.has_x = true, dc.x = 1, dc.has_y = true, dc.y = 1));
    DELETE_RUN('P', false, (dc.has_x = true, dc.x = 1, dc.has_y = true, dc.y = 1));
    DELETE_RUN('q', true,
               (dc.has_x = true, dc.x = 2, dc.has_y = true, dc.y = 2,
                dc.has_z = true, dc.z = 0));
    DELETE_RUN('Q', false,
               (dc.has_x = true, dc.x = 2, dc.has_y = true, dc.y = 2,
                dc.has_z = true, dc.z = 0));
    DELETE_RUN('r', true, (dc.has_x = true, dc.x = 1, dc.has_y = true, dc.y = 10));
    DELETE_RUN('R', false, (dc.has_x = true, dc.x = 1, dc.has_y = true, dc.y = 10));
    DELETE_RUN('x', true, (dc.has_x = true, dc.x = 2));
    DELETE_RUN('X', false, (dc.has_x = true, dc.x = 2));
    DELETE_RUN('y', true, (dc.has_y = true, dc.y = 2));
    DELETE_RUN('Y', false, (dc.has_y = true, dc.y = 2));
    DELETE_RUN('z', true, (dc.has_z = true, dc.z = 0));
    DELETE_RUN('Z', false, (dc.has_z = true, dc.z = 0));

    /* A selector with no match changes nothing. */
    DELETE_SETUP();
    {
        struct kitty_command dc;
        kitty_command_init(&dc);
        dc.action = KITTY_ACTION_DELETE;
        dc.has_delete = true;
        dc.delete_spec = 'X';
        dc.has_x = true;
        dc.x = 40;
        kitty_placement_delete(&term, &dc);
        xassert(tll_length(store->placements) == 1);
        xassert(kitty_image_store_find_id(images, 4) != NULL);
    }

    /* Animation frames are not supported: d=f changes nothing. */
    {
        struct kitty_command dc;
        kitty_command_init(&dc);
        dc.action = KITTY_ACTION_DELETE;
        dc.has_delete = true;
        dc.delete_spec = 'f';
        kitty_placement_delete(&term, &dc);
        xassert(tll_length(store->placements) == 1);
        xassert(kitty_image_store_find_id(images, 4) != NULL);
    }

#undef DELETE_RUN
#undef DELETE_SETUP

    kitty_fini(&term);
    free(term.normal.rows);
}
