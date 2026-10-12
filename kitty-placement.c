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

/*
 * Remove a placement from the store. The per-frame underlay list holds
 * pointers to placement nodes, so any removal invalidates it. Hook A
 * also runs from render_sixel_images(), before the next
 * kitty_render_prepass() rebuilds the list.
 */
#define kitty_placement_remove(store, it)       \
    do {                                        \
        (store)->underlay_count = 0;            \
        tll_remove((store)->placements, (it));  \
    } while (0)

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

    /* With both c and r given, the span is used as-is. */
    if (!has_cols && !has_rows) {
        c = cells_for(src_w, cell_width, max_cells);
        r = cells_for(src_h, cell_height, max_cells);
    } else if (!has_rows) {
        /* rows = ceil(c * cell_width * src_h / (src_w * cell_height)) */
        r = cells_for((int64_t)c * cell_width * src_h,
                      (int64_t)src_w * cell_height, max_cells);
    } else if (!has_cols) {
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

static void
placement_unlink(
    struct kitty_placement_store *store, struct kitty_placement *p)
{
    if (p->image != NULL && p->image->placements > 0)
        p->image->placements--;
    kitty_placement_cache_free(store, p);
}

/*
 * True when the placement overlaps the real screen rows (the ones
 * term_erase() and term_erase_scrollback() act on), regardless of the
 * current scroll view. Screen rows are the last term->rows rows of the
 * ring: their scrollback-relative index is >= history.
 */
static bool
placement_overlaps_screen(
    struct terminal *term, const struct kitty_placement *p)
{
    const struct grid *grid = term->grid;
    const int num_rows = grid->num_rows;

    if (num_rows <= 0 || p->rows <= 0 || p->rows > num_rows)
        return false;

    const int history = num_rows - term->rows;
    const int sb = grid_row_abs_to_sb(grid, term->rows, p->row);

    if (sb >= history)
        return true;

    /* Anchor in the scrollback: the body reaches the screen when it
     * spans past the first screen row without wrapping the ring. */
    return p->rows > history - sb;
}

/*
 * Clear screen (CSI 2 J and CSI 3 J): delete every placement that is
 * visible on the current screen, then free the images that no
 * placement refers to any more (kitty does this; spec: Clear,
 * alternate screen, and reset).
 */
void
kitty_clear_visible(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    struct kitty_image_store *images = kitty_image_store_for(term);

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement)
            continue;

        if (!placement_overlaps_screen(term, p))
            continue;

        kitty_placement_dirty(term, p);
        placement_unlink(store, p);
        kitty_placement_remove(store, it);
    }

    kitty_image_store_delete_unreferenced(images);
}

/*
 * Scrollback erase (CSI 3 J): delete every placement that is not on
 * screen, since its rows are about to be wiped. Then free the images
 * that no placement refers to any more.
 */
void
kitty_scrollback_clear(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    struct kitty_image_store *images = kitty_image_store_for(term);

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement)
            continue;

        if (placement_overlaps_screen(term, p))
            continue;

        placement_unlink(store, p);
        kitty_placement_remove(store, it);
    }

    kitty_image_store_delete_unreferenced(images);
}

/*
 * Reflow (design D7): move every placement of the main screen by the
 * same number of lines as the cursor, then drop the placements that
 * fall outside the new ring. The placement store is passed explicitly,
 * because the alternate screen may be active while the normal grid
 * reflows.
 *
 * Rows are compared in scrollback-relative coordinates: after a reflow
 * the ring is rebuilt and absolute rows have no meaning. Any consistent
 * screen height can be used for the old coordinates; only the
difference with the new height matters, and the constant cancels.
 */
void
kitty_placement_reflow(
    struct terminal *term, struct kitty_placement_store *store,
    int old_offset, int old_num_rows, int old_screen_rows, int delta)
{
    const struct grid *grid = &term->normal;
    const int new_rows = grid->num_rows;
    const int screen_rows = term->rows;

    if (new_rows <= 0 || old_num_rows <= 0 || screen_rows <= 0)
        return;

    const bool main_active = term->grid == &term->normal;

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement)
            continue;

        const int old_sb =
            (p->row - (old_offset + old_screen_rows) + old_num_rows) &
            (old_num_rows - 1);
        const int64_t sb = (int64_t)old_sb + delta;

        if (sb < 0 || sb >= new_rows) {
            if (main_active)
                kitty_placement_dirty(term, p);
            placement_unlink(store, p);
            kitty_placement_remove(store, it);
            continue;
        }

        p->row = ((int)sb + grid->offset + screen_rows) & (new_rows - 1);

        if (main_active)
            kitty_placement_dirty(term, p);
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

        /* placement_unlink() also keeps the image's placement count in
         * sync. */
        placement_unlink(store, &it->item);
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
        if (!cmd->has_cursor || cmd->cursor == 0) {
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
                     * region's bottom row (spec: Cursor movement after
                     * placement).
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
    const struct kitty_placement *p, const struct kitty_image *number_image)
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
        /* Only the newest image with this number (spec: Deletion). */
        if (number_image == NULL || p->image_id != number_image->id)
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

    /*
     * d=n/N matches the newest image with number I only (spec:
     * Deletion). Resolve it once, before any placement is removed.
     */
    struct kitty_image *number_image =
        (sel == 'n' || sel == 'N') && cmd->has_number
        ? kitty_image_store_find_number(images, cmd->number)
        : NULL;

    tll(uint32_t) touched = tll_init();

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (!delete_matches(term, cmd, p, number_image))
            continue;

        /* Repaint the cells the placement covered. */
        kitty_placement_dirty(term, p);
        tll_push_back(touched, p->image_id);
        placement_unlink(store, p);
        kitty_placement_remove(store, it);
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
                if (number_image != NULL)
                    delete_image_if_unused(images, number_image->id);
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

/*
 * Drop a placement during a scroll: release its scaled cache, give
 * the image's placement count back and mark the cells it covered for
 * a repaint. Must run while the grid offset is still the pre-scroll
 * one, so that the dirty rows are the ones the placement covered.
 */
#define scroll_drop(term, store, it)                                \
    do {                                                            \
        placement_unlink((store), &(it)->item);                     \
        kitty_placement_remove((store), (it));                      \
    } while (0)

/*
 * Scroll support (design D5, spec: Scrolling).
 *
 * The hooks run from term_scroll_partial() and
 * term_scroll_reverse_partial(), before the grid offset is updated.
 * All arithmetic below uses the pre-scroll offset.
 *
 * The grid is a ring of num_rows rows; the screen occupies the ring
 * rows [offset, offset + screen_rows) and the scrollback the ring
 * rows before them. A placement occupies consecutive ring rows from
 * its anchor and is drawn as one rigid rectangle over the text, so
 * only its anchor row has to follow the text line it was placed on.
 * A scroll changes ring rows in three places:
 *
 *  - scroll up destroys the `rows` oldest scrollback rows (they are
 *    erased as the scrolled-in lines) and, with a top margin, rotates
 *    the fixed-margin content `rows` ring rows forward while the top
 *    rows of the region rotate into the scrollback;
 *  - scroll down destroys the `rows` newest scrollback rows and
 *    rotates the fixed-margin content `rows` ring rows back, while
 *    the bottom rows of the region are pushed below the screen;
 *  - the erased scrolled-in lines themselves.
 *
 * Policy (kitty's scroll_filter_* functions, adapted to foot's ring):
 *  - an anchor in the scrollback keeps its ring row; it is deleted
 *    when its content line is destroyed (spec: a placement whose top
 *    row leaves the scrollback is deleted);
 *  - a placement fully inside the scroll region moves with the region
 *    (its ring row is unchanged), and follows the rotation when its
 *    anchor line enters the scrollback, hanging from the top of the
 *    screen like in kitty;
 *  - any other placement keeps the same screen position, like kitty
 *    freezes refs that are not fully inside the region: an anchor in
 *    a fixed margin follows the margin row swap, and a bottom-edge
 *    crossing anchor moves by the same amount;
 *  - a fully-inside placement whose anchor line is pushed below the
 *    region bottom is deleted (kitty removes refs that move out of
 *    their region).
 */
void
kitty_scroll_up(struct terminal *term, struct scroll_region region, int rows)
{
    if (term->kitty == NULL || rows <= 0)
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    if (tll_length(store->placements) == 0)
        return;

    const struct grid *grid = term->grid;
    const int num_rows = grid->num_rows;
    const int screen_rows = term->rows;

    if (num_rows <= 0 || screen_rows <= 0 || rows > screen_rows)
        return;
    if (region.start < 0 || region.end > screen_rows ||
        region.start >= region.end)
    {
        return;
    }

    const int offset = grid->offset; /* pre-scroll */
    const int history_rows = num_rows - screen_rows;
    const bool has_margins =
        region.start != 0 || region.end != screen_rows;

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement)
            continue;

        const int sb_top =
            (p->row - (offset + screen_rows) + num_rows) & (num_rows - 1);
        const int s = sb_top - history_rows; /* < 0: in the scrollback */

        /*
         * The anchor is in the scrollback and keeps its ring row,
         * unless it is one of the oldest `rows` scrollback rows,
         * whose content the scroll destroys.
         */
        if (s < 0 || sb_top < rows) {
            if (sb_top < rows) {
                kitty_placement_dirty(term, p);
                scroll_drop(term, store, it);
            }
            continue;
        }

        if (!has_margins) {
            /* Nothing is swapped: every content line keeps its ring
             * row, even the lines that enter the scrollback. */
            continue;
        }

        const bool fully_inside =
            s >= region.start && s + p->rows - 1 < region.end;

        if (!fully_inside) {
            /*
             * Keep the same screen position: the anchor follows the
             * margin row swap (or, for a bottom-edge crossing anchor,
             * moves by the same amount), which freezes the image like
             * kitty freezes refs that are not fully inside the
             * region.
             */
            p->row = (p->row + rows) & (num_rows - 1);
            continue;
        }

        if (s - rows >= region.start) {
            /* Moves up with the region; the ring row does not
             * change. */
            continue;
        }

        /* The anchor line rotates into the scrollback, to the ring
         * row the top-margin swaps leave it in. The image hangs from
         * the top of the screen, as in kitty. */
        p->row = (offset + (s % rows)) & (num_rows - 1);
    }
}

void
kitty_scroll_down(struct terminal *term, struct scroll_region region, int rows)
{
    if (term->kitty == NULL || rows <= 0)
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    if (tll_length(store->placements) == 0)
        return;

    const struct grid *grid = term->grid;
    const int num_rows = grid->num_rows;
    const int screen_rows = term->rows;

    if (num_rows <= 0 || screen_rows <= 0 || rows > screen_rows)
        return;
    if (region.start < 0 || region.end > screen_rows ||
        region.start >= region.end)
    {
        return;
    }

    const int offset = grid->offset; /* pre-scroll */
    const int history_rows = num_rows - screen_rows;
    const bool has_margins =
        region.start != 0 || region.end != screen_rows;

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement)
            continue;

        const int sb_top =
            (p->row - (offset + screen_rows) + num_rows) & (num_rows - 1);
        const int s = sb_top - history_rows; /* < 0: in the scrollback */

        /*
         * The anchor is in the scrollback and keeps its ring row,
         * unless it is one of the `rows` newest scrollback rows,
         * which the scroll destroys (they become the blank rows
         * inserted at the region top).
         */
        if (s < 0) {
            if (sb_top >= history_rows - rows) {
                kitty_placement_dirty(term, p);
                scroll_drop(term, store, it);
            }
            continue;
        }

        if (!has_margins) {
            /* Nothing is swapped. The content moves down with the
             * screen; a line pushed below the screen bottom is lost,
             * and kitty removes the images pushed out with it. */
            if (s + rows >= screen_rows) {
                kitty_placement_dirty(term, p);
                scroll_drop(term, store, it);
            }
            continue;
        }

        const bool fully_inside =
            s >= region.start && s + p->rows - 1 < region.end;

        if (fully_inside) {
            if (s + rows >= region.end) {
                /* The whole placement is pushed below the region
                 * bottom; the ring moves it behind every earlier
                 * scrollback line, out of order. Kitty removes refs
                 * that move out of their region. */
                kitty_placement_dirty(term, p);
                scroll_drop(term, store, it);
            }
            /* else: moves down with the region; ring row unchanged. */
            continue;
        }

        /* Keep the same screen position: the anchor follows the
         * margin row swap (backwards), or moves by the same amount
         * for a bottom-edge crossing anchor. */
        p->row = (p->row - rows + num_rows) & (num_rows - 1);
    }
}

/*
 * Scroll test helpers. They mirror what term_scroll_partial() and
 * term_scroll_reverse_partial() do around the hook: the hook runs
 * first, then the offset (and, when the view follows, the view) is
 * updated.
 */
#define SCROLL_TERM(t) do {                                             \
        memset(&(t), 0, sizeof(t));                                     \
        (t).ptmx = -1;                                                  \
        (t).rows = 4;                                                   \
        (t).cols = 8;                                                   \
        (t).cell_width = 8;                                             \
        (t).cell_height = 16;                                           \
        (t).normal.num_rows = 8;                                        \
        (t).normal.rows = xcalloc(8, sizeof((t).normal.rows[0]));       \
        (t).grid = &(t).normal;                                         \
        (t).selection.coords.end.row = -1;                              \
        kitty_init(&(t));                                               \
    } while (0)

/* Allocate every ring row, for tests that run term_scroll(). */
#define SCROLL_ALLOC_ROWS(t) do {                                       \
        for (int _i = 0; _i < (t).normal.num_rows; _i++)                \
            (t).normal.rows[_i] = grid_row_alloc((t).cols, true);       \
    } while (0)

#define SCROLL_PLACE(t, img_id, at_row, pcols, prows, pw, ph) do {      \
        const size_t npix = (size_t)(pw) * (size_t)(ph);                \
        uint32_t *px = xcalloc(npix, sizeof(px[0]));                    \
        for (size_t _i = 0; _i < npix; _i++)                            \
            px[_i] = 0xff000000u;                                       \
        struct kitty_image proto = {                                    \
            .id = (img_id), .width = (pw), .height = (ph),              \
            .data = px, .size = npix * sizeof(px[0]),                   \
        };                                                              \
        xassert(kitty_image_store_add(                                  \
                    kitty_image_store_for(&(t)), &proto));              \
        (t).normal.cursor.point.row = (at_row);                         \
        (t).normal.cursor.point.col = 0;                                \
        struct kitty_command pc;                                        \
        kitty_command_init(&pc);                                        \
        pc.action = KITTY_ACTION_PUT;                                   \
        pc.has_id = true; pc.id = (img_id);                             \
        pc.has_cols = true; pc.cols = (pcols);                          \
        pc.has_rows = true; pc.rows = (prows);                          \
        pc.has_cursor = true; pc.cursor = 1;                            \
        xassert(kitty_placement_put(                                    \
                    &(t), &pc,                                          \
                    kitty_image_store_find_id(                          \
                        kitty_image_store_for(&(t)),                    \
                        (img_id))) == NULL);                            \
    } while (0)

#define SCROLL_COUNT(t) \
    tll_length(kitty_placement_store_for(&(t))->placements)

/* The one placement of image 'id', or NULL. */
static inline struct kitty_placement *
scroll_test_find(struct terminal *t, uint32_t id)
{
    tll_foreach(kitty_placement_store_for(t)->placements, it) {
        if (it->item.image_id == id)
            return &it->item;
    }
    return NULL;
}

#define SCROLL_DO_UP(t, reg_start, reg_end, amount) do {                \
        kitty_scroll_up(&(t), (struct scroll_region){                   \
                            .start = (reg_start), .end = (reg_end)},    \
                        (amount));                                      \
        (t).grid->offset =                                              \
            ((t).grid->offset + (amount)) & ((t).grid->num_rows - 1);   \
        (t).grid->view = (t).grid->offset;                              \
    } while (0)

#define SCROLL_DO_DOWN(t, reg_start, reg_end, amount) do {              \
        kitty_scroll_down(&(t), (struct scroll_region){                 \
                              .start = (reg_start), .end = (reg_end)},  \
                          (amount));                                    \
        (t).grid->offset =                                              \
            ((t).grid->offset - (amount) + (t).grid->num_rows) &        \
            ((t).grid->num_rows - 1);                                   \
        (t).grid->view = (t).grid->offset;                              \
    } while (0)

UNITTEST
{
    /*
     * Full-screen scroll up: ring rows do not change. The placement
     * scrolls into the scrollback, is visible again when the user
     * scrolls back, and is deleted when its top row reaches the
     * oldest scrollback row.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 1, 1, 1, 1, 1);

    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    const int abs_row = p->row;

    int top;
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 1);

    SCROLL_DO_UP(term, 0, 4, 1);
    p = scroll_test_find(&term, 9);
    xassert(p != NULL && p->row == abs_row);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 0);

    /* The user scrolls back one line: the image is next to its text
     * again, at its original screen row. */
    term.grid->view = (term.grid->view - 1 + term.grid->num_rows) &
        (term.grid->num_rows - 1);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 1);
    term.grid->view = term.grid->offset;

    /* Four more scrolls keep it (scrollback holds 4 rows); the sixth
     * scroll in total destroys the anchor row. */
    for (int i = 0; i < 4; i++) {
        SCROLL_DO_UP(term, 0, 4, 1);
        xassert(SCROLL_COUNT(term) == 1);
    }
    SCROLL_DO_UP(term, 0, 4, 1);
    xassert(SCROLL_COUNT(term) == 0);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Scroll up with a top and a bottom margin: anchors in the fixed
     * margins follow the row swap and stay visually in place; anchors
     * inside the region move up one row.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 0, 1, 1, 1, 1);  /* top margin */
    SCROLL_PLACE(term, 10, 2, 1, 1, 1, 1); /* inside the region */
    SCROLL_PLACE(term, 11, 3, 1, 1, 1, 1); /* bottom margin */
    xassert(SCROLL_COUNT(term) == 3);

    SCROLL_DO_UP(term, 1, 3, 1);

    struct kitty_placement *p;
    int top;

    p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 0);

    p = scroll_test_find(&term, 10);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 1);

    p = scroll_test_find(&term, 11);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 3);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Scroll up with a top margin, amount 1: the region's top row
     * rotates into the scrollback (ring row offset + (s % rows)); a
     * row that stays inside the region keeps its ring row.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 2, 1, 1, 1, 1); /* top row of the region */
    SCROLL_PLACE(term, 10, 3, 1, 1, 1, 1);

    SCROLL_DO_UP(term, 2, 4, 1);

    /* Image 9 rotated into the scrollback, just above the screen. */
    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(p->row == 0);
    int top;
    xassert(!kitty_placement_visible_top(&term, p, &top));

    /* Image 10 moved up one row inside the region. */
    p = scroll_test_find(&term, 10);
    xassert(p != NULL);
    xassert(p->row == 3);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 2);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * No top margin: even rows that enter the scrollback keep their
     * ring rows, so a multi-row placement follows as a whole.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 0, 1, 3, 1, 1); /* rows 0-2, bottom margin is row 3 */

    SCROLL_DO_UP(term, 0, 3, 1);

    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(p->rows == 3);
    int top;
    /* The top row is one line above the viewport; two rows are still
     * visible. */
    xassert(kitty_placement_visible_top(&term, p, &top) && top == -1);

    /* Scrolling back shows the whole image again. */
    term.grid->view = (term.grid->view - 1 + term.grid->num_rows) &
        (term.grid->num_rows - 1);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 0);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Scroll up with a top margin and a multi-row placement that
     * rotates into the scrollback: it follows when the rotation keeps
     * its rows contiguous, is clipped when only part of it is pushed,
     * and is deleted when the rotation would tear it.
     */
    struct terminal term;
    SCROLL_TERM(term);
    term.normal.num_rows = 16;
    free(term.normal.rows);
    term.normal.rows = xcalloc(16, sizeof(term.normal.rows[0]));
    term.rows = 8;
    term.selection.coords.end.row = -1;

    SCROLL_PLACE(term, 9, 2, 1, 2, 1, 1); /* rows 2-3: fully in the pushed band */
    SCROLL_PLACE(term, 10, 3, 1, 3, 1, 3); /* rows 3-5: partly pushed */
    SCROLL_PLACE(term, 11, 2, 1, 3, 1, 3); /* rows 2-4: partly pushed */

    SCROLL_DO_UP(term, 2, 8, 2);

    /* Image 9 follows into the scrollback, contiguous. */
    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(p->row == 0 && p->rows == 2);

    /* Image 10 follows its pushed anchor row (3 -> ring row 1), as a
     * rigid rectangle. */
    p = scroll_test_find(&term, 10);
    xassert(p != NULL);
    xassert(p->rows == 3);
    xassert(p->src_h == 3);
    xassert(p->row == 1);

    /* Image 11 follows its pushed anchor row (2 -> ring row 0). */
    p = scroll_test_find(&term, 11);
    xassert(p != NULL);
    xassert(p->rows == 3 && p->src_h == 3);
    xassert(p->row == 0);
    xassert(SCROLL_COUNT(term) == 3);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Delete line (CSI M): term_scroll_partial() runs with the
     * sub-region below the cursor. A placement inside the sub-region
     * moves up; one above it stays.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 3, 1, 1, 1, 1);
    SCROLL_PLACE(term, 10, 0, 1, 1, 1, 1);

    SCROLL_DO_UP(term, 2, 4, 1);

    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    int top;
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 2);

    p = scroll_test_find(&term, 10);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 0);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Scroll down (reverse index / insert line): anchors inside the
     * region move down and keep their ring row; anchors in the fixed
     * margins follow the backwards row swap; an anchor pushed below
     * the region bottom is dropped; the newest scrollback rows are
     * destroyed.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 1, 1, 1, 1, 1); /* inside the region */
    SCROLL_PLACE(term, 10, 2, 1, 1, 1, 1); /* pushed below the region */
    SCROLL_PLACE(term, 11, 0, 1, 1, 1, 1); /* top margin */
    SCROLL_PLACE(term, 12, 3, 1, 1, 1, 1); /* bottom margin */

    SCROLL_DO_DOWN(term, 1, 3, 1);

    struct kitty_placement *p;
    int top;

    p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 2);

    xassert(scroll_test_find(&term, 10) == NULL);

    p = scroll_test_find(&term, 11);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 0);

    p = scroll_test_find(&term, 12);
    xassert(p != NULL);
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 3);
    xassert(SCROLL_COUNT(term) == 3);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Scroll down destroys the newest scrollback rows. A placement
     * anchored there is deleted; one whose body only reaches into the
     * destroyed rows is clipped.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_PLACE(term, 9, 1, 1, 1, 1, 1);
    SCROLL_PLACE(term, 10, 0, 1, 2, 1, 2);

    /* Two scroll ups put image 9's anchor at scrollback row 3, and
     * image 10's anchor at row 2 with its body reaching into row 3. */
    SCROLL_DO_UP(term, 0, 4, 1);
    SCROLL_DO_UP(term, 0, 4, 1);
    xassert(SCROLL_COUNT(term) == 2);

    SCROLL_DO_DOWN(term, 0, 4, 1);

    /* The newest scrollback row (row 3) was destroyed: image 9 is
     * gone. Image 10 is a rigid overlay and keeps its rows; its
     * second row now draws over the row that reused the destroyed
     * ring row. */
    xassert(scroll_test_find(&term, 9) == NULL);
    struct kitty_placement *p = scroll_test_find(&term, 10);
    xassert(p != NULL && p->rows == 2);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Cursor movement policy at the scroll region bottom: the move
     * scrolls the region (text and images) and leaves the cursor on
     * the region's bottom row.
     */
    struct terminal term;
    SCROLL_TERM(term);
    SCROLL_ALLOC_ROWS(term);
    term.scroll_region.start = 0;
    term.scroll_region.end = 4;

    uint32_t *px = xcalloc(4, sizeof(px[0]));
    for (int i = 0; i < 4; i++)
        px[i] = 0xff000000u;
    struct kitty_image proto = {
        .id = 9, .width = 1, .height = 4,
        .data = px, .size = 4 * sizeof(px[0]),
    };
    xassert(kitty_image_store_add(kitty_image_store_for(&term), &proto));
    term.normal.cursor.point.row = 2;
    term.normal.cursor.point.col = 0;
    struct kitty_command pc;
    kitty_command_init(&pc);
    pc.action = KITTY_ACTION_PUT;
    pc.has_id = true; pc.id = 9;
    pc.has_cols = true; pc.cols = 1;
    pc.has_rows = true; pc.rows = 4;
    /* C=0: the cursor move runs, and scrolls at the region bottom. */
    xassert(kitty_placement_put(
                &term, &pc,
                kitty_image_store_find_id(kitty_image_store_for(&term), 9))
            == NULL);

    xassert(term.grid->cursor.point.row == 3);
    xassert(term.grid->offset == 2);

    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    int top;
    xassert(kitty_placement_visible_top(&term, p, &top) && top == 0);
    xassert(p->rows == 4);

    kitty_fini(&term);
    tll_free(term.grid->scroll_damage);
    for (int i = 0; i < term.normal.num_rows; i++)
        grid_row_free(term.normal.rows[i]);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Clear screen (CSI 2 J): only the placements visible on screen
     * are dropped. A placement in the scrollback stays. Images left
     * with no placement are freed (kitty does this).
     */
    struct terminal term;
    SCROLL_TERM(term);

    SCROLL_PLACE(term, 9, 0, 1, 1, 1, 1);  /* will scroll into history */
    SCROLL_DO_UP(term, 0, 4, 1);           /* offset = 1; image 9 off screen */

    SCROLL_PLACE(term, 10, 1, 1, 1, 1, 1); /* visible at screen row 1 */

    xassert(SCROLL_COUNT(term) == 2);
    xassert(kitty_image_store_find_id(kitty_image_store_for(&term), 10) != NULL);

    kitty_clear_visible(&term);

    xassert(SCROLL_COUNT(term) == 1);
    xassert(scroll_test_find(&term, 9) != NULL);
    xassert(scroll_test_find(&term, 10) == NULL);

    xassert(kitty_image_store_find_id(kitty_image_store_for(&term), 9) != NULL);
    xassert(kitty_image_store_find_id(kitty_image_store_for(&term), 10) == NULL);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Scrollback erase (CSI 3 J): only the placements in the
     * scrollback are dropped; the visible one stays.
     */
    struct terminal term;
    SCROLL_TERM(term);

    SCROLL_PLACE(term, 9, 0, 1, 1, 1, 1);
    SCROLL_DO_UP(term, 0, 4, 1);           /* image 9 in history */

    SCROLL_PLACE(term, 10, 1, 1, 1, 1, 1); /* visible */

    xassert(SCROLL_COUNT(term) == 2);

    kitty_scrollback_clear(&term);

    xassert(SCROLL_COUNT(term) == 1);
    xassert(scroll_test_find(&term, 9) == NULL);
    xassert(scroll_test_find(&term, 10) != NULL);
    xassert(kitty_image_store_find_id(kitty_image_store_for(&term), 9) == NULL);
    xassert(kitty_image_store_find_id(kitty_image_store_for(&term), 10) != NULL);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Reflow: every main-screen anchor moves by the cursor delta in
     * scrollback-relative coordinates. An anchor pushed above the
     * oldest line is deleted.
     */
    struct terminal term;
    SCROLL_TERM(term);

    SCROLL_PLACE(term, 9, 1, 1, 1, 1, 1);
    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    const int abs_row = p->row;

    kitty_placement_reflow(
        &term, kitty_placement_store_for(&term),
        term.normal.offset, term.normal.num_rows, term.rows, 1);

    p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(p->row == ((abs_row + 1) & (term.normal.num_rows - 1)));

    /* A large negative delta pushes the anchor out of the ring. */
    kitty_placement_reflow(
        &term, kitty_placement_store_for(&term),
        term.normal.offset, term.normal.num_rows, term.rows,
        -term.normal.num_rows);

    xassert(scroll_test_find(&term, 9) == NULL);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * A cell size change keeps the cell span and the anchor, and drops
     * only the scaled cache (design D5).
     */
    struct terminal term;
    SCROLL_TERM(term);

    SCROLL_PLACE(term, 9, 1, 2, 3, 1, 1);
    struct kitty_placement *p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(p->cols == 2 && p->rows == 3);
    const int row = p->row, col = p->col;

    term.cell_width = 16;
    term.cell_height = 32;
    kitty_cell_size_changed(&term);

    p = scroll_test_find(&term, 9);
    xassert(p != NULL);
    xassert(p->cols == 2 && p->rows == 3);
    xassert(p->row == row && p->col == col);
    xassert(p->cache_pix == NULL && p->cache_bytes == 0);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * Virtual placement (U=1): a=p creates it for an image, without
     * drawing anything by itself and without moving the cursor. It is
     * not visible as a cell, so d=a (all visible) keeps it. d=i
     * deletes it (spec kitty-graphics-placeholders: Virtual
     * placements).
     */
    struct terminal term;
    SCROLL_TERM(term);

    uint32_t *px = xcalloc(4, sizeof(px[0]));
    for (int i = 0; i < 4; i++)
        px[i] = 0xff000000u;
    struct kitty_image proto = {
        .id = 7, .width = 2, .height = 2,
        .data = px, .size = 4 * sizeof(px[0]),
    };
    xassert(kitty_image_store_add(kitty_image_store_for(&term), &proto));

    struct kitty_command put;
    kitty_command_init(&put);
    put.action = KITTY_ACTION_PUT;
    put.has_id = true;
    put.id = 7;
    put.has_cols = true;
    put.cols = 2;
    put.has_rows = true;
    put.rows = 2;
    put.has_unicode_placeholder = true;
    put.unicode_placeholder = 1;

    term.normal.cursor.point.row = 2;
    term.normal.cursor.point.col = 3;

    xassert(kitty_placement_put(
                &term, &put,
                kitty_image_store_find_id(
                    kitty_image_store_for(&term), 7)) == NULL);

    struct kitty_placement *p = scroll_test_find(&term, 7);
    xassert(p != NULL);
    xassert(p->virtual_placement);
    xassert(p->cols == 2 && p->rows == 2);

    /* U=1 leaves the cursor where it was. */
    xassert(term.normal.cursor.point.row == 2);
    xassert(term.normal.cursor.point.col == 3);

    /* A normal placement, for contrast: d=a deletes it... */
    SCROLL_PLACE(term, 8, 1, 1, 1, 1, 1);

    struct kitty_command del;
    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.has_delete = true;
    del.delete_spec = 'a';
    kitty_placement_delete(&term, &del);

    xassert(scroll_test_find(&term, 8) == NULL);
    /* ...but keeps the virtual one. */
    xassert(scroll_test_find(&term, 7) != NULL);

    /* d=i deletes the virtual placement. */
    del.delete_spec = 'i';
    del.has_id = true;
    del.id = 7;
    kitty_placement_delete(&term, &del);
    xassert(scroll_test_find(&term, 7) == NULL);

    kitty_fini(&term);
    free(term.normal.rows);
}

UNITTEST
{
    /*
     * The remaining virtual-placement delete rules (spec:
     * Virtual placements): d=r/R and d=n/N delete a virtual
     * placement; d=c, d=p, d=x, d=y and d=z leave it alone.
     */
    struct terminal term;
    SCROLL_TERM(term);

    uint32_t *px = xcalloc(1, sizeof(px[0]));
    px[0] = 0xff000000u;
    struct kitty_image proto = {
        .id = 7, .width = 1, .height = 1, .has_number = true, .number = 5,
        .data = px, .size = sizeof(px[0]),
    };
    xassert(kitty_image_store_add(kitty_image_store_for(&term), &proto));

    struct kitty_command put;
    kitty_command_init(&put);
    put.action = KITTY_ACTION_PUT;
    put.has_id = true;
    put.id = 7;
    put.has_unicode_placeholder = true;
    put.unicode_placeholder = 1;

    struct kitty_command del;
    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.has_delete = true;

    /* Selectors that do not match virtual placements. */
    static const char *const ignores[] = {"c", "p", "x", "y", "z"};
    for (size_t i = 0; i < sizeof(ignores) / sizeof(ignores[0]); i++) {
        term.normal.cursor.point.row = 2;
        term.normal.cursor.point.col = 3;
        xassert(kitty_placement_put(
                    &term, &put,
                    kitty_image_store_find_id(
                        kitty_image_store_for(&term), 7)) == NULL);

        kitty_command_init(&del);
        del.action = KITTY_ACTION_DELETE;
        del.has_delete = true;
        del.delete_spec = ignores[i][0];
        del.has_x = true;
        del.x = 3;
        del.has_y = true;
        del.y = 2;
        del.has_z = true;
        del.z = 0;
        kitty_placement_delete(&term, &del);

        xassert(scroll_test_find(&term, 7) != NULL);
    }

    /* d=r/R matches the image id range and deletes it. */
    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.has_delete = true;
    del.delete_spec = 'r';
    del.has_x = true;
    del.x = 1;
    del.has_y = true;
    del.y = 10;
    kitty_placement_delete(&term, &del);
    xassert(scroll_test_find(&term, 7) == NULL);

    /* d=n/N matches the newest image with number I. */
    term.normal.cursor.point.row = 1;
    term.normal.cursor.point.col = 1;
    xassert(kitty_placement_put(
                &term, &put,
                kitty_image_store_find_id(
                    kitty_image_store_for(&term), 7)) == NULL);

    kitty_command_init(&del);
    del.action = KITTY_ACTION_DELETE;
    del.has_delete = true;
    del.delete_spec = 'n';
    del.has_number = true;
    del.number = 5;
    kitty_placement_delete(&term, &del);
    xassert(scroll_test_find(&term, 7) == NULL);

    kitty_fini(&term);
    free(term.normal.rows);
}
