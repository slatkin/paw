#include "kitty-placement-internal.h"

#define LOG_MODULE "kitty-scroll"
#define LOG_ENABLE_DBG 0
#include "debug.h"
#include "grid.h"

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
 * placement refers to any more (kitty does this).
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
        kitty_placement_unlink(store, p);
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

        kitty_placement_unlink(store, p);
        kitty_placement_remove(store, it);
    }

    kitty_image_store_delete_unreferenced(images);
}

/*
 * Reflow: move every placement of the main screen by the
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
            kitty_placement_unlink(store, p);
            kitty_placement_remove(store, it);
            continue;
        }

        p->row = ((int)sb + grid->offset + screen_rows) & (new_rows - 1);

        if (main_active)
            kitty_placement_dirty(term, p);
    }
}

/*
 * Drop a placement during a scroll: release its scaled cache, give
 * the image's placement count back and mark the cells it covered for
 * a repaint. Must run while the grid offset is still the pre-scroll
 * one, so that the dirty rows are the ones the placement covered.
 */
#define scroll_drop(term, store, it)                                \
    do {                                                            \
        kitty_placement_unlink((store), &(it)->item);               \
        kitty_placement_remove((store), (it));                      \
    } while (0)

/*
 * Scroll support.
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
 *    when its content line is destroyed;
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

#ifndef NDEBUG
#include "kitty-test.h"

/* The common stub terminal of the tests below: 4 rows of 8 columns. */
#define PLACEMENT_TEST_TERM(t) KITTY_TEST_TERM(t, 4, 8, 8, 16, 8)

UNITTEST
{
    /*
     * Full-screen scroll up: ring rows do not change. The placement
     * scrolls into the scrollback, is visible again when the user
     * scrolls back, and is deleted when its top row reaches the
     * oldest scrollback row.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);
    kitty_test_place(&term, 9, 1, 1, 1, 1, 1);

    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    const int abs_row = p->row;

    KITTY_TEST_EXPECT_TOP(term, 9, 1);

    KITTY_TEST_SCROLL_UP(term, 0, 4, 1);
    p = kitty_test_placement(&term, 9);
    xassert(p != NULL && p->row == abs_row);
    KITTY_TEST_EXPECT_TOP(term, 9, 0);

    /* The user scrolls back one line: the image is next to its text
     * again, at its original screen row. */
    term.grid->view = (term.grid->view - 1 + term.grid->num_rows) &
        (term.grid->num_rows - 1);
    KITTY_TEST_EXPECT_TOP(term, 9, 1);
    term.grid->view = term.grid->offset;

    /* Four more scrolls keep it (scrollback holds 4 rows); the sixth
     * scroll in total destroys the anchor row. */
    for (int i = 0; i < 4; i++) {
        KITTY_TEST_SCROLL_UP(term, 0, 4, 1);
        xassert(kitty_test_count(&term) == 1);
    }
    KITTY_TEST_SCROLL_UP(term, 0, 4, 1);
    xassert(kitty_test_count(&term) == 0);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Scroll up in a region (also what delete line does): anchors in
     * the fixed margins follow the row swap and stay visually in
     * place; anchors inside the region move up one row.
     */
    static const struct {
        int start, end;
        int count;
        int at[3];  /* screen row of each placement, before */
        int top[3]; /* screen row of each placement, after */
    } cases[] = {
        {1, 3, 3, {0, 2, 3}, {0, 1, 3}}, /* top and bottom margin */
        {2, 4, 2, {3, 0}, {2, 0}},       /* bottom of the screen */
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        struct terminal term;
        PLACEMENT_TEST_TERM(term);

        for (int i = 0; i < cases[c].count; i++)
            kitty_test_place(&term, 9 + i, cases[c].at[i], 1, 1, 1, 1);
        xassert(kitty_test_count(&term) == (size_t)cases[c].count);

        KITTY_TEST_SCROLL_UP(term, cases[c].start, cases[c].end, 1);

        for (int i = 0; i < cases[c].count; i++)
            KITTY_TEST_EXPECT_TOP(term, 9 + i, cases[c].top[i]);

        KITTY_TEST_FINI(term);
    }
}

UNITTEST
{
    /*
     * Scroll up with a top margin, amount 1: the region's top row
     * rotates into the scrollback (ring row offset + (s % rows)); a
     * row that stays inside the region keeps its ring row.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);
    kitty_test_place(&term, 9, 2, 1, 1, 1, 1); /* top row of the region */
    kitty_test_place(&term, 10, 3, 1, 1, 1, 1);

    KITTY_TEST_SCROLL_UP(term, 2, 4, 1);

    /* Image 9 rotated into the scrollback, just above the screen. */
    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    xassert(p->row == 0);
    int top;
    xassert(!kitty_placement_visible_top(&term, p, &top));

    /* Image 10 moved up one row inside the region. */
    p = kitty_test_placement(&term, 10);
    xassert(p != NULL);
    xassert(p->row == 3);
    KITTY_TEST_EXPECT_TOP(term, 10, 2);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * No top margin: even rows that enter the scrollback keep their
     * ring rows, so a multi-row placement follows as a whole.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);
    /* Rows 0-2, the bottom margin is row 3. */
    kitty_test_place(&term, 9, 0, 1, 3, 1, 1);

    KITTY_TEST_SCROLL_UP(term, 0, 3, 1);

    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    xassert(p->rows == 3);

    /* The top row is one line above the viewport; two rows are still
     * visible. */
    KITTY_TEST_EXPECT_TOP(term, 9, -1);

    /* Scrolling back shows the whole image again. */
    term.grid->view = (term.grid->view - 1 + term.grid->num_rows) &
        (term.grid->num_rows - 1);
    KITTY_TEST_EXPECT_TOP(term, 9, 0);

    KITTY_TEST_FINI(term);
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
    KITTY_TEST_TERM(term, 8, 8, 8, 16, 16);

    kitty_test_place(&term, 9, 2, 1, 2, 1, 1);  /* rows 2-3: fully pushed */
    kitty_test_place(&term, 10, 3, 1, 3, 1, 3); /* rows 3-5: partly pushed */
    kitty_test_place(&term, 11, 2, 1, 3, 1, 3); /* rows 2-4: partly pushed */

    KITTY_TEST_SCROLL_UP(term, 2, 8, 2);

    /* Image 9 follows into the scrollback, contiguous. */
    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    xassert(p->row == 0 && p->rows == 2);

    /* Image 10 follows its pushed anchor row (3 -> ring row 1), as a
     * rigid rectangle. */
    p = kitty_test_placement(&term, 10);
    xassert(p != NULL);
    xassert(p->rows == 3);
    xassert(p->src_h == 3);
    xassert(p->row == 1);

    /* Image 11 follows its pushed anchor row (2 -> ring row 0). */
    p = kitty_test_placement(&term, 11);
    xassert(p != NULL);
    xassert(p->rows == 3 && p->src_h == 3);
    xassert(p->row == 0);
    xassert(kitty_test_count(&term) == 3);

    KITTY_TEST_FINI(term);
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
    PLACEMENT_TEST_TERM(term);
    kitty_test_place(&term, 9, 1, 1, 1, 1, 1);  /* inside the region */
    kitty_test_place(&term, 10, 2, 1, 1, 1, 1); /* pushed below the region */
    kitty_test_place(&term, 11, 0, 1, 1, 1, 1); /* top margin */
    kitty_test_place(&term, 12, 3, 1, 1, 1, 1); /* bottom margin */

    KITTY_TEST_SCROLL_DOWN(term, 1, 3, 1);

    KITTY_TEST_EXPECT_TOP(term, 9, 2);
    xassert(kitty_test_placement(&term, 10) == NULL);
    KITTY_TEST_EXPECT_TOP(term, 11, 0);
    KITTY_TEST_EXPECT_TOP(term, 12, 3);
    xassert(kitty_test_count(&term) == 3);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Scroll down destroys the newest scrollback rows. A placement
     * anchored there is deleted; one whose body only reaches into the
     * destroyed rows is clipped.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);
    kitty_test_place(&term, 9, 1, 1, 1, 1, 1);
    kitty_test_place(&term, 10, 0, 1, 2, 1, 2);

    /* Two scroll ups put image 9's anchor at scrollback row 3, and
     * image 10's anchor at row 2 with its body reaching into row 3. */
    KITTY_TEST_SCROLL_UP(term, 0, 4, 1);
    KITTY_TEST_SCROLL_UP(term, 0, 4, 1);
    xassert(kitty_test_count(&term) == 2);

    KITTY_TEST_SCROLL_DOWN(term, 0, 4, 1);

    /* The newest scrollback row (row 3) was destroyed: image 9 is
     * gone. Image 10 is a rigid overlay and keeps its rows; its
     * second row now draws over the row that reused the destroyed
     * ring row. */
    xassert(kitty_test_placement(&term, 9) == NULL);
    struct kitty_placement *p = kitty_test_placement(&term, 10);
    xassert(p != NULL && p->rows == 2);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Cursor movement policy at the scroll region bottom: the move
     * scrolls the region (text and images) and leaves the cursor on
     * the region's bottom row.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);
    KITTY_TEST_ALLOC_ROWS(term);
    term.scroll_region.start = 0;
    term.scroll_region.end = 4;

    kitty_test_add_image(&term, 9, 1, 4);
    term.normal.cursor.point.row = 2;
    term.normal.cursor.point.col = 0;

    /* C=0: the cursor move runs, and scrolls at the region bottom. */
    struct kitty_command put = kitty_test_put_command(9);
    put.cols = kitty_some_u32(1);
    put.rows = kitty_some_u32(4);
    kitty_test_put(&term, &put);

    xassert(term.grid->cursor.point.row == 3);
    xassert(term.grid->offset == 2);

    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    KITTY_TEST_EXPECT_TOP(term, 9, 0);
    xassert(p->rows == 4);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Clear screen (CSI 2 J): only the placements visible on screen
     * are dropped. A placement in the scrollback stays. Images left
     * with no placement are freed (kitty does this).
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_place(&term, 9, 0, 1, 1, 1, 1);  /* will scroll into history */
    KITTY_TEST_SCROLL_UP(term, 0, 4, 1);        /* image 9 is off screen */
    kitty_test_place(&term, 10, 1, 1, 1, 1, 1); /* visible at screen row 1 */

    xassert(kitty_test_count(&term) == 2);
    xassert(kitty_test_image(&term, 10) != NULL);

    kitty_clear_visible(&term);

    xassert(kitty_test_count(&term) == 1);
    xassert(kitty_test_placement(&term, 9) != NULL);
    xassert(kitty_test_placement(&term, 10) == NULL);
    xassert(kitty_test_image(&term, 9) != NULL);
    xassert(kitty_test_image(&term, 10) == NULL);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Scrollback erase (CSI 3 J): only the placements in the
     * scrollback are dropped; the visible one stays.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_place(&term, 9, 0, 1, 1, 1, 1);  /* will be in history */
    KITTY_TEST_SCROLL_UP(term, 0, 4, 1);
    kitty_test_place(&term, 10, 1, 1, 1, 1, 1); /* visible */

    xassert(kitty_test_count(&term) == 2);

    kitty_scrollback_clear(&term);

    xassert(kitty_test_count(&term) == 1);
    xassert(kitty_test_placement(&term, 9) == NULL);
    xassert(kitty_test_placement(&term, 10) != NULL);
    xassert(kitty_test_image(&term, 9) == NULL);
    xassert(kitty_test_image(&term, 10) != NULL);

    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /*
     * Reflow: every main-screen anchor moves by the cursor delta in
     * scrollback-relative coordinates. An anchor pushed above the
     * oldest line is deleted.
     */
    struct terminal term;
    PLACEMENT_TEST_TERM(term);

    kitty_test_place(&term, 9, 1, 1, 1, 1, 1);
    struct kitty_placement *p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    const int abs_row = p->row;

    kitty_placement_reflow(
        &term, kitty_placement_store_for(&term),
        term.normal.offset, term.normal.num_rows, term.rows, 1);

    p = kitty_test_placement(&term, 9);
    xassert(p != NULL);
    xassert(p->row == ((abs_row + 1) & (term.normal.num_rows - 1)));

    /* A large negative delta pushes the anchor out of the ring. */
    kitty_placement_reflow(
        &term, kitty_placement_store_for(&term),
        term.normal.offset, term.normal.num_rows, term.rows,
        -term.normal.num_rows);

    xassert(kitty_test_placement(&term, 9) == NULL);

    KITTY_TEST_FINI(term);
}


#endif /* !NDEBUG */
