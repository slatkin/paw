#include "kitty-graphics-internal.h"

#include "debug.h"
#include "grid.h"
#include "vt.h"

void
kitty_alt_screen_clear(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    /* The alternate screen always starts empty. */
    kitty_screen_fini(term->kitty, 1);
    kitty_screen_init(term->kitty, 1);
}

void
kitty_reset(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    for (int i = 0; i < 2; i++) {
        kitty_screen_fini(term->kitty, i);
        kitty_screen_init(term->kitty, i);
    }

    kitty_chunk_reset(term->kitty);
    term->kitty->reflow.active = false;
}

void
kitty_cell_size_changed(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    /*
     * The cell span of a placement stays; only the scaled cache is
     * rebuilt at the new cell size, on the next frame.
     */
    for (int i = 0; i < 2; i++) {
        struct kitty_placement_store *store =
            &term->kitty->screens[i].placement_store;
        tll_foreach(store->placements, it)
            kitty_placement_cache_free(store, &it->item);
    }
}

void
kitty_reflow_begin(struct terminal *term)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL)
        return;

    /*
     * During an interactive resize the reflow source is the stashed
     * original grid; otherwise it is the normal grid.
     */
    const struct grid *grid =
        term->interactive_resizing.grid != NULL
        ? term->interactive_resizing.grid
        : &term->normal;

    if (grid->num_rows <= 0 || term->rows <= 0)
        return;

    const int abs_row =
        (grid->offset + grid->cursor.point.row) & (grid->num_rows - 1);

    kitty->reflow.old_offset = grid->offset;
    kitty->reflow.old_num_rows = grid->num_rows;
    kitty->reflow.old_screen_rows = term->rows;
    kitty->reflow.cursor_sb =
        grid_row_abs_to_sb(grid, term->rows, abs_row);
    kitty->reflow.active = true;
}

void
kitty_reflow_end(struct terminal *term)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL || !kitty->reflow.active)
        return;

    kitty->reflow.active = false;

    const struct grid *grid = &term->normal;
    if (grid->num_rows <= 0 || term->rows <= 0)
        return;

    const int abs_row =
        (grid->offset + grid->cursor.point.row) & (grid->num_rows - 1);
    const int new_sb = grid_row_abs_to_sb(grid, term->rows, abs_row);
    const int delta = new_sb - kitty->reflow.cursor_sb;

    kitty_placement_reflow(
        term, &kitty->screens[0].placement_store,
        kitty->reflow.old_offset, kitty->reflow.old_num_rows,
        kitty->reflow.old_screen_rows, delta);
}

UNITTEST
{
    /*
     * Leaving the alternate screen drops all of its images and
     * placements, and leaves the main screen alone.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char img[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AAAA\x1b\\";
    static const char img2[] =
        "\x1b_Ga=t,i=2,f=24,s=1,v=1;AAAA\x1b\\";

    /* Image 1 and image 2 on the main screen. */
    vt_from_slave(&term, (const uint8_t *)img, sizeof(img) - 1);
    vt_from_slave(&term, (const uint8_t *)img2, sizeof(img2) - 1);
    xassert(kitty_image_store_find_id(&term.kitty->screens[0].image_store, KITTY_IMAGE_ID(1)) != NULL);
    xassert(kitty_image_store_find_id(&term.kitty->screens[0].image_store, KITTY_IMAGE_ID(2)) != NULL);

    /* Image 1 on the alternate screen too (same id, separate store). */
    term.grid = &term.alt;
    vt_from_slave(&term, (const uint8_t *)img, sizeof(img) - 1);
    xassert(kitty_image_store_find_id(&term.kitty->screens[1].image_store, KITTY_IMAGE_ID(1)) != NULL);

    kitty_alt_screen_clear(&term);

    xassert(tll_length(term.kitty->screens[1].image_store.images) == 0);
    xassert(kitty_image_store_find_id(&term.kitty->screens[0].image_store, KITTY_IMAGE_ID(1)) != NULL);
    xassert(kitty_image_store_find_id(&term.kitty->screens[0].image_store, KITTY_IMAGE_ID(2)) != NULL);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * RIS deletes all placements and images on both screens, and
     * cancels an in-progress chunked transfer.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char main_img[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AAAA\x1b\\";
    static const char alt_img[] =
        "\x1b_Ga=t,i=2,f=24,s=1,v=1;AAAA\x1b\\";
    static const char chunk[] =
        "\x1b_Ga=t,i=3,f=24,s=1,v=1,m=1;AAAA\x1b\\";

    vt_from_slave(&term, (const uint8_t *)main_img, sizeof(main_img) - 1);
    term.grid = &term.alt;
    vt_from_slave(&term, (const uint8_t *)alt_img, sizeof(alt_img) - 1);
    term.grid = &term.normal;
    vt_from_slave(&term, (const uint8_t *)chunk, sizeof(chunk) - 1);

    xassert(tll_length(term.kitty->screens[0].image_store.images) == 1);
    xassert(tll_length(term.kitty->screens[1].image_store.images) == 1);
    xassert(term.kitty->chunk.active);

    kitty_reset(&term);

    xassert(tll_length(term.kitty->screens[0].image_store.images) == 0);
    xassert(tll_length(term.kitty->screens[1].image_store.images) == 0);
    xassert(!term.kitty->chunk.active);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * DECSTR (soft reset) while on the alternate screen leaves the
     * alternate grid. Its images and placements must not survive, or
     * the next CSI ?1049h, which relies on the exit path having
     * cleared them, would show them again.
     */
    static struct config conf;
    static struct wayland wl;

    struct terminal term = {
        .ptmx = -1,
        .wl = &wl,
        .rows = 0,
        .cols = 0,
        .grid = &term.alt,
        .conf = &conf,
        .selection = {
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .auto_scroll = {
                .fd = -1,
            },
        },
    };
    conf.locked_title = true;
    term.window_title_has_been_set = true;

    kitty_init(&term);

    static const char img[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AAAA\x1b\\";
    vt_from_slave(&term, (const uint8_t *)img, sizeof(img) - 1);
    xassert(kitty_image_store_find_id(&term.kitty->screens[1].image_store, KITTY_IMAGE_ID(1)) != NULL);

    term_reset(&term, false);

    xassert(term.grid == &term.normal);
    xassert(tll_length(term.kitty->screens[1].image_store.images) == 0);

    kitty_fini(&term);
}
