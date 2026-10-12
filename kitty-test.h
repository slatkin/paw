#pragma once

/*
 * Helpers for the UNITTEST blocks in the kitty-*.c files. Only the
 * tests use them.
 */

#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "grid.h"
#include "kitty-graphics.h"
#include "kitty-image.h"
#include "kitty-placement.h"
#include "terminal.h"
#include "xmalloc.h"

/*
 * A stub terminal of 'nrows' x 'ncols' cells of 'cw' x 'ch' pixels,
 * with a ring of 'ring' grid rows (a power of two). No grid row is
 * allocated, see KITTY_TEST_ALLOC_ROWS().
 */
#define KITTY_TEST_TERM(t, nrows, ncols, cw, ch, ring) do {             \
        memset(&(t), 0, sizeof(t));                                     \
        (t).ptmx = -1;                                                  \
        (t).rows = (nrows);                                             \
        (t).cols = (ncols);                                             \
        (t).cell_width = (cw);                                          \
        (t).cell_height = (ch);                                         \
        (t).width = (ncols) * (cw);                                     \
        (t).height = (nrows) * (ch);                                    \
        (t).normal.num_rows = (ring);                                   \
        (t).normal.rows = xcalloc((ring), sizeof((t).normal.rows[0]));  \
        (t).grid = &(t).normal;                                         \
        (t).selection.coords.end.row = -1;                              \
        kitty_init(&(t));                                               \
    } while (0)

/* Allocate every ring row, for tests that run term_scroll(). */
#define KITTY_TEST_ALLOC_ROWS(t) do {                                   \
        for (int _i = 0; _i < (t).normal.num_rows; _i++)                \
            (t).normal.rows[_i] = grid_row_alloc((t).cols, true);       \
    } while (0)

#define KITTY_TEST_FINI(t) do {                                         \
        kitty_fini(&(t));                                               \
        tll_free((t).normal.scroll_damage);                             \
        for (int _i = 0; _i < (t).normal.num_rows; _i++) {              \
            if ((t).normal.rows[_i] != NULL)                            \
                grid_row_free((t).normal.rows[_i]);                     \
        }                                                               \
        free((t).normal.rows);                                          \
    } while (0)

/*
 * Scroll the grid like term_scroll_partial() and
 * term_scroll_reverse_partial() do: the hook runs first, then the
 * offset and the view are updated.
 */
#define KITTY_TEST_SCROLL_UP(t, reg_start, reg_end, amount) do {        \
        kitty_scroll_up(&(t), (struct scroll_region){                   \
                            .start = (reg_start), .end = (reg_end)},    \
                        (amount));                                      \
        (t).grid->offset =                                              \
            ((t).grid->offset + (amount)) & ((t).grid->num_rows - 1);   \
        (t).grid->view = (t).grid->offset;                              \
    } while (0)

#define KITTY_TEST_SCROLL_DOWN(t, reg_start, reg_end, amount) do {      \
        kitty_scroll_down(&(t), (struct scroll_region){                 \
                              .start = (reg_start), .end = (reg_end)},  \
                          (amount));                                    \
        (t).grid->offset =                                              \
            ((t).grid->offset - (amount) + (t).grid->num_rows) &        \
            ((t).grid->num_rows - 1);                                   \
        (t).grid->view = (t).grid->offset;                              \
    } while (0)

static inline struct kitty_image *
kitty_test_image(struct terminal *term, uint32_t id)
{
    return kitty_image_store_find_id(
        kitty_image_store_for(term), KITTY_IMAGE_ID(id));
}

/* The one placement of image 'id', or NULL. */
static inline struct kitty_placement *
kitty_test_placement(struct terminal *term, uint32_t id)
{
    tll_foreach(kitty_placement_store_for(term)->placements, it) {
        if (it->item.image_id.v == id)
            return &it->item;
    }
    return NULL;
}

static inline size_t
kitty_test_count(struct terminal *term)
{
    return tll_length(kitty_placement_store_for(term)->placements);
}

/* Store an opaque black image of 'w' x 'h' pixels. */
static inline void
kitty_test_add_image(struct terminal *term, uint32_t id, int w, int h)
{
    const size_t count = (size_t)w * (size_t)h;
    uint32_t *px = (uint32_t *)xcalloc(count, sizeof(px[0]));
    for (size_t i = 0; i < count; i++)
        px[i] = 0xff000000u;

    struct kitty_image image = {
        .id = KITTY_IMAGE_ID(id), .width = w, .height = h,
        .data = px, .size = count * sizeof(px[0]),
    };
    xassert(kitty_image_store_add(kitty_image_store_for(term), &image));
}

/* An a=p command for image 'id'. */
static inline struct kitty_command
kitty_test_put_command(uint32_t id)
{
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.action = KITTY_ACTION_PUT;
    cmd.id = kitty_some_u32(id);
    return cmd;
}

/* Put the image of a command, which is in the store already. */
static inline void
kitty_test_put(struct terminal *term, const struct kitty_command *cmd)
{
    xassert(kitty_placement_put(
                term, cmd, kitty_test_image(term, cmd->id.value)) == NULL);
}

/*
 * Add image 'id' of 'w' x 'h' pixels and place it at screen row 'row'
 * of the first column, over 'cols' x 'rows' cells, with C=1.
 */
static inline void
kitty_test_place(
    struct terminal *term, uint32_t id, int row, int cols, int rows,
    int w, int h)
{
    kitty_test_add_image(term, id, w, h);

    term->normal.cursor.point.row = row;
    term->normal.cursor.point.col = 0;

    struct kitty_command put = kitty_test_put_command(id);
    put.cols = kitty_some_u32(cols);
    put.rows = kitty_some_u32(rows);
    put.cursor = kitty_some_u32(1);
    kitty_test_put(term, &put);
}

/* Assert that placement 'id' shows its first row at screen row 'want'. */
#define KITTY_TEST_EXPECT_TOP(t, id, want) do {                         \
        struct kitty_placement *_p = kitty_test_placement(&(t), (id));  \
        xassert(_p != NULL);                                            \
        int _top;                                                       \
        xassert(kitty_placement_visible_top(&(t), _p, &_top));          \
        xassert(_top == (want));                                        \
    } while (0)
