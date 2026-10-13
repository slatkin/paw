#include "kitty-render-internal.h"

#include <string.h>

#include "composed.h"
#include "kitty-diacritics.h"

/*
 * Resolve a cell's base character and its combining diacritics.
 * Returns the number of diacritics (0..3). Cells with combining
 * characters are stored as composed entries.
 */
static int
placeholder_diacritics(
    const struct terminal *term, const struct cell *cell,
    char32_t *out_base, uint32_t out_diacritics[3])
{
    const char32_t wc = cell->wc;
    const struct composed *composed = NULL;

    if (wc >= CELL_COMB_CHARS_LO && wc <= CELL_COMB_CHARS_HI) {
        composed = composed_lookup(
            term->composed, wc - CELL_COMB_CHARS_LO);
    }

    *out_base = composed != NULL ? composed->chars[0] : wc;

    int count = 0;
    if (composed != NULL) {
        for (unsigned i = 1; i < composed->count && count < 3; i++) {
            if (composed->chars[i] == 0)
                break;
            out_diacritics[count++] = composed->chars[i];
        }
    }

    return count;
}

/*
 * The low 24 bits of the image id: the palette index in index color
 * mode, the 24-bit RGB value in true color mode.
 */
static uint32_t
placeholder_image_id(const struct terminal *term, const struct cell *cell)
{
    switch (cell->attrs.fg_src) {
    case COLOR_BASE16:
    case COLOR_BASE256:
    case COLOR_RGB:
        return cell->attrs.fg;

    case COLOR_DEFAULT:
        return term->reverse ? term->colors.bg : term->colors.fg;
    }

    return 0;
}

/* The placement id, from the cell's underline color (0 when unset). */
static uint32_t
placeholder_placement_id(const struct row *row, int col)
{
    if (row->extra == NULL)
        return 0;

    const struct row_ranges *ranges = &row->extra->underline_ranges;
    for (int i = 0; i < ranges->count; i++) {
        const struct row_range *range = &ranges->v[i];

        if (range->start > col)
            break;
        if (range->start <= col && col <= range->end) {
            switch (range->underline.color_src) {
            case COLOR_BASE256:
            case COLOR_RGB:
                return range->underline.color;
            case COLOR_DEFAULT:
            case COLOR_BASE16:
                return 0;
            }
        }
    }

    return 0;
}

/* kitty's diacritic_to_num(): index + 1, and 0 for an unknown char. */
static uint32_t
placeholder_diacritic_value(uint32_t cp)
{
    const int idx = kitty_diacritic_index(cp);
    return idx < 0 ? 0 : (uint32_t)idx + 1;
}

/*
 * Decode the placeholder cell at 'col'. Returns false when the base
 * character is not U+10EEEE. On success, '*out_id' is the image id,
 * '*out_placement_id' the placement id, and '*out_row' / '*out_col'
 * are the 0-based row and column inside the virtual placement.
 *
 * The inheritance rules are kitty's run-based algorithm:
 * a row, column and third diacritic may be omitted, and are then
 * inherited from the placeholder cell to the left, when that cell
 * continues the same run (same fg and underline color, and compatible
 * explicit values).
 */
bool
kitty_placeholder_decode(
    struct terminal *term, const struct row *row, int col,
    uint32_t *out_id, uint32_t *out_placement_id,
    int *out_row, int *out_col)
{
    /* All values are 1-based; 0 means "not set", as in kitty. */
    uint32_t prev_id_low = 0, prev_placement = 0;
    uint32_t prev_id_high = 0, prev_row = 0, prev_col = 0;
    bool placeholder = false;

    for (int i = 0; i <= col; i++) {
        const struct cell *cell = &row->cells[i];

        char32_t base;
        uint32_t diacritics[3];
        const int count = placeholder_diacritics(
            term, cell, &base, diacritics);

        const bool is_placeholder =
            base == KITTY_IMAGE_PLACEHOLDER_CHAR;

        uint32_t id_low = 0, placement = 0;
        uint32_t id_high = 0, r = 0, c = 0;

        if (is_placeholder) {
            id_low = placeholder_image_id(term, cell);
            placement = placeholder_placement_id(row, i);
            if (count > 0)
                r = placeholder_diacritic_value(diacritics[0]);
            if (count > 1)
                c = placeholder_diacritic_value(diacritics[1]);
            if (count > 2)
                id_high = placeholder_diacritic_value(diacritics[2]);
        }

        if (placeholder && is_placeholder &&
            id_low == prev_id_low &&
            placement == prev_placement &&
            (r == 0 || r == prev_row) &&
            (c == 0 || c == prev_col + 1) &&
            (id_high == 0 || id_high == prev_id_high))
        {
            /* Continue the run: inherit the missing values. */
            prev_col = prev_col + 1;
        } else if (is_placeholder) {
            /* Start a new run; unknown values default to 1. */
            placeholder = true;
            prev_id_low = id_low;
            prev_placement = placement;
            prev_id_high = id_high > 0 ? id_high : 1;
            prev_row = r > 0 ? r : 1;
            prev_col = c > 0 ? c : 1;
        } else {
            placeholder = false;
            prev_id_low = prev_placement = 0;
            prev_id_high = prev_row = prev_col = 0;
        }
    }

    if (!placeholder)
        return false;

    *out_id = prev_id_low |
        ((uint32_t)((prev_id_high - 1) & 0xffu) << 24);
    *out_placement_id = prev_placement;
    *out_row = (int)prev_row - 1;
    *out_col = (int)prev_col - 1;
    return true;
}

/* The first virtual placement for 'image_id' (and 'placement_id', when
 * non-zero). A placement id of 0 means "any" (kitty). */
struct kitty_placement *
kitty_placeholder_find_virtual(
    struct kitty_placement_store *store, struct kitty_image_id image_id,
    struct kitty_placement_id placement_id)
{
    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;

        if (!p->virtual_placement || p->image_id.v != image_id.v)
            continue;
        if (placement_id.v != 0 && p->placement_id.v != placement_id.v)
            continue;

        return p;
    }

    return NULL;
}

bool
kitty_cell_is_placeholder(
    const struct terminal *term, const struct cell *cell)
{
    char32_t base;
    uint32_t diacritics[3];
    placeholder_diacritics(term, cell, &base, diacritics);
    return base == KITTY_IMAGE_PLACEHOLDER_CHAR;
}

#ifndef NDEBUG
#include "kitty-test.h"

/*
 * Decode a placeholder cell with 0, 1, 2 and 3 diacritics. With no
 * cell to the left, a missing value defaults to 0 (kitty).
 */
UNITTEST
{
    struct terminal term;
    memset(&term, 0, sizeof(term));

    enum { COLS = 4 };
    struct cell cells[COLS] = {0};
    const struct row row = {.cells = (struct cell *)cells};

    uint32_t id, placement_id;
    int r, c;

    /* No diacritics. */
    kitty_test_placeholder_cell(&cells[0], KITTY_IMAGE_PLACEHOLDER_CHAR, 42);
    xassert(kitty_placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42 && placement_id == 0);
    xassert(r == 0 && c == 0);

    /* One diacritic: the row. */
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[5]}, 1), 42);
    xassert(kitty_placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42);
    xassert(r == 5 && c == 0);

    /* Two diacritics: row and column. */
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[5], kitty_diacritics[7]}, 2),
                          42);
    xassert(kitty_placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42);
    xassert(r == 5 && c == 7);

    /* Three diacritics: row, column and the high byte of the id. */
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[5], kitty_diacritics[7],
                                  kitty_diacritics[2]}, 3),
                          42);
    xassert(kitty_placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == (42u | (2u << 24)));
    xassert(r == 5 && c == 7);

    composed_free(term.composed);
}

/*
 * The three inheritance rules: a missing row, column and high byte are
 * taken from the placeholder cell to the left (kitty's algorithm).
 */
UNITTEST
{
    struct terminal term;
    memset(&term, 0, sizeof(term));

    enum { COLS = 4 };
    struct cell cells[COLS] = {0};
    const struct row row = {.cells = (struct cell *)cells};

    uint32_t id, placement_id;
    int r, c;

    /* cell 0: explicit (row 1, col 2, high byte 2). */
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[2],
                                  kitty_diacritics[2]}, 3),
                          42);

    /* cell 1: no diacritics: row and high byte inherited, column +1. */
    kitty_test_placeholder_cell(&cells[1], kitty_test_placeholder_wc(&term, NULL, 0), 42);
    xassert(kitty_placeholder_decode(
                &term, &row, 1, &id, &placement_id, &r, &c));
    xassert(id == (42u | (2u << 24)));
    xassert(r == 1 && c == 3);

    /* cell 2: row diacritic only: column +1, high byte inherited. */
    kitty_test_placeholder_cell(&cells[2], kitty_test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[1]}, 1), 42);
    xassert(kitty_placeholder_decode(
                &term, &row, 2, &id, &placement_id, &r, &c));
    xassert(id == (42u | (2u << 24)));
    xassert(r == 1 && c == 4);

    /* cell 3: row and column diacritics: high byte inherited. */
    kitty_test_placeholder_cell(&cells[3], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[5]}, 2),
                          42);
    xassert(kitty_placeholder_decode(
                &term, &row, 3, &id, &placement_id, &r, &c));
    xassert(id == (42u | (2u << 24)));
    xassert(r == 1 && c == 5);

    composed_free(term.composed);
}

/*
 * Inheritance needs the same foreground and underline colors. A cell
 * with a different fg starts a new run, so a missing value defaults to
 * 0.
 */
UNITTEST
{
    struct terminal term;
    memset(&term, 0, sizeof(term));

    enum { COLS = 2 };
    struct cell cells[COLS] = {0};
    const struct row row = {.cells = (struct cell *)cells};

    uint32_t id, placement_id;
    int r, c;

    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[2]}, 2),
                          42);
    kitty_test_placeholder_cell(&cells[1], kitty_test_placeholder_wc(&term, NULL, 0), 43);

    /* Different fg color: no inheritance. */
    xassert(kitty_placeholder_decode(
                &term, &row, 1, &id, &placement_id, &r, &c));
    xassert(id == 43);
    xassert(r == 0 && c == 0);

    composed_free(term.composed);
}

/*
 * The placement id comes from the cell's underline color; two cells
 * with different underline colors do not inherit from each other.
 */
UNITTEST
{
    struct terminal term;
    memset(&term, 0, sizeof(term));

    enum { COLS = 2 };
    struct cell cells[COLS] = {0};
    struct row_data extra = {0};
    struct row_range range = {
        .start = 0,
        .end = COLS - 1,
        .underline = {
            .style = UNDERLINE_SINGLE,
            .color_src = COLOR_BASE256,
            .color = 7,
        },
    };
    extra.underline_ranges.v = &range;
    extra.underline_ranges.count = 1;

    const struct row row = {.cells = (struct cell *)cells, .extra = &extra};

    uint32_t id, placement_id;
    int r, c;

    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[2]}, 2),
                          42);
    xassert(kitty_placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42 && placement_id == 7);
    xassert(r == 1 && c == 2);

    composed_free(term.composed);
}

#endif /* !NDEBUG */
