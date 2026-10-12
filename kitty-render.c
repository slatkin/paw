#include "kitty-graphics.h"

#include <stdlib.h>
#include <string.h>

#include "kitty-diacritics.h"
#include "kitty-placement.h"

#define LOG_MODULE "kitty-render"
#define LOG_ENABLE_DBG 0
#include "composed.h"
#include "debug.h"
#include "grid.h"
#include "log.h"
#include "terminal.h"
#include "xmalloc.h"

/*
 * Build (or refresh) the scaled image cache of one placement. Does
 * nothing if the cache is valid for the current cell size. Skips the
 * placement if the cache would exceed the per-screen cache budget.
 */
static void
placement_ensure_cache(
    struct terminal *term, struct kitty_placement_store *store,
    struct kitty_placement *p)
{
    if (p->cache_pix != NULL &&
        p->cache_cell_width == term->cell_width &&
        p->cache_cell_height == term->cell_height)
    {
        return;
    }

    kitty_placement_cache_free(store, p);

    if (p->image == NULL || p->image->pix == NULL)
        return;

    const int64_t box_w = (int64_t)p->cols * term->cell_width;
    const int64_t box_h = (int64_t)p->rows * term->cell_height;

    int display_w, display_h;
    int off_x, off_y;

    if (p->scaled) {
        const double sx = (double)box_w / p->src_w;
        const double sy = (double)box_h / p->src_h;
        const double s = sx < sy ? sx : sy;

        display_w = (int)(p->src_w * s + 0.5);
        display_h = (int)(p->src_h * s + 0.5);
        if (display_w < 1)
            display_w = 1;
        if (display_h < 1)
            display_h = 1;

        off_x = (int)((box_w - display_w) / 2);
        off_y = (int)((box_h - display_h) / 2);
    } else {
        /* No scaling: keep the source pixel size. */
        display_w = p->src_w;
        display_h = p->src_h;
        off_x = off_y = 0;
    }

    size_t bytes;
    if (!kitty_checked_size(
            (uint32_t)display_w, (uint32_t)display_h, 4, SIZE_MAX, &bytes))
    {
        return;
    }

    if (bytes > KITTY_PLACEMENT_CACHE_QUOTA)
        return;
    if (store->cache_bytes > KITTY_PLACEMENT_CACHE_QUOTA - bytes)
        return;

    uint32_t *data = xmalloc(bytes);
    memset(data, 0, bytes);

    pixman_image_t *scaled = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, display_w, display_h, data, display_w * 4);
    if (scaled == NULL) {
        free(data);
        return;
    }

    /* A scratch view of the source rectangle, inside the image data. */
    uint32_t *sub_data =
        p->image->data + (size_t)p->src_y * p->image->width + p->src_x;
    pixman_image_t *sub = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, p->src_w, p->src_h,
        sub_data, p->image->width * 4);
    if (sub == NULL) {
        pixman_image_unref(scaled);
        free(data);
        return;
    }

    struct pixman_f_transform fscale;
    pixman_f_transform_init_scale(
        &fscale, (double)p->src_w / display_w, (double)p->src_h / display_h);

    struct pixman_transform scale;
    pixman_transform_from_pixman_f_transform(&scale, &fscale);

    pixman_image_set_transform(sub, &scale);
    pixman_image_set_filter(sub, PIXMAN_FILTER_GOOD, NULL, 0);

    pixman_image_composite32(
        PIXMAN_OP_SRC, sub, NULL, scaled, 0, 0, 0, 0, 0, 0,
        display_w, display_h);

    pixman_image_unref(sub);

    p->cache_data = data;
    p->cache_pix = scaled;
    p->cache_w = display_w;
    p->cache_h = display_h;
    p->cache_x = off_x;
    p->cache_y = off_y;
    p->cache_bytes = (int)bytes;
    p->cache_cell_width = term->cell_width;
    p->cache_cell_height = term->cell_height;
    store->cache_bytes += bytes;
}

/* Overlays are only drawn while the terminal is on the main screen
 * (design D6). */
static bool
overlay_hidden(struct terminal *term)
{
    return term->grid == &term->normal &&
        term->interactive_resizing.grid != NULL;
}

static int
placement_cmp(const void *_a, const void *_b)
{
    const struct kitty_placement *a = *(const struct kitty_placement **)_a;
    const struct kitty_placement *b = *(const struct kitty_placement **)_b;

    if (a->z != b->z)
        return a->z < b->z ? -1 : 1;
    if (a->image_id != b->image_id)
        return a->image_id < b->image_id ? -1 : 1;
    if (a->placement_id != b->placement_id)
        return a->placement_id < b->placement_id ? -1 : 1;
    return 0;
}

static void
underlay_push(struct kitty_placement_store *store, struct kitty_placement *p)
{
    if (store->underlay_count == store->underlay_size) {
        size_t new_size =
            store->underlay_size == 0 ? 8 : store->underlay_size * 2;
        store->underlay = xrealloc(
            store->underlay, new_size * sizeof(store->underlay[0]));
        store->underlay_size = new_size;
    }

    store->underlay[store->underlay_count++] = p;
}

void
kitty_render_prepass(struct terminal *term)
{
    if (term->kitty == NULL || overlay_hidden(term))
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    store->underlay_count = 0;

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;

        /*
         * Placeholder images are drawn per cell by hook B; their
         * scaled cache must be ready before the workers run (design
         * D6).
         */
        if (p->virtual_placement) {
            placement_ensure_cache(term, store, p);
            continue;
        }

        int top;
        if (!kitty_placement_visible_top(term, p, &top))
            continue;

        /* The workers never allocate: build every visible cache here
         * (design D6). */
        placement_ensure_cache(term, store, p);

        if (p->z < 0)
            underlay_push(store, p);
    }

    if (store->underlay_count > 1) {
        qsort(store->underlay, store->underlay_count,
              sizeof(store->underlay[0]), placement_cmp);
    }
}

void
kitty_render_overlay(
    struct terminal *term, pixman_image_t *pix,
    pixman_region32_t *damage, const struct coord *cursor)
{
    (void)cursor;

    if (term->kitty == NULL || overlay_hidden(term))
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    const size_t count = tll_length(store->placements);
    if (count == 0)
        return;

    struct kitty_placement **order = xmalloc(count * sizeof(*order));
    size_t n = 0;

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement || p->z < 0)
            continue;
        int top;
        if (!kitty_placement_visible_top(term, p, &top))
            continue;
        order[n++] = p;
    }

    qsort(order, n, sizeof(*order), placement_cmp);

    for (size_t i = 0; i < n; i++) {
        struct kitty_placement *p = order[i];

        placement_ensure_cache(term, store, p);
        if (p->cache_pix == NULL)
            continue;

        int top;
        if (!kitty_placement_visible_top(term, p, &top))
            continue;

        const int x =
            term->margins.left + p->col * term->cell_width + p->X + p->cache_x;
        const int y =
            term->margins.top + top * term->cell_height + p->Y + p->cache_y;

        int dst_x = x, dst_y = y;
        int w = p->cache_w, h = p->cache_h;
        int src_x = 0, src_y = 0;

        if (dst_x < 0) {
            src_x = -dst_x;
            w -= src_x;
            dst_x = 0;
        }
        if (dst_y < 0) {
            src_y = -dst_y;
            h -= src_y;
            dst_y = 0;
        }
        if (dst_x + w > term->width)
            w = term->width - dst_x;
        if (dst_y + h > term->height)
            h = term->height - dst_y;

        if (w <= 0 || h <= 0)
            continue;

        pixman_image_composite32(
            PIXMAN_OP_OVER, p->cache_pix, NULL, pix,
            src_x, src_y, 0, 0, dst_x, dst_y, w, h);

        if (damage != NULL)
            pixman_region32_union_rect(damage, damage, dst_x, dst_y, w, h);

        /*
         * Repaint the cells under the image on the next frame, so that
         * the image is composited once per frame and alpha does not
         * add up (design D6).
         */
        kitty_placement_dirty(term, p);
    }

    free(order);
}

/*
 * True when the cell's effective background is the default one. This
 * is the same test render_cell() uses for transparency (spec: Z-index
 * layers, the z < -1073741824 rule).
 */
static bool
cell_has_default_bg(const struct terminal *term, const struct cell *cell)
{
    switch (cell->attrs.bg_src) {
    case COLOR_DEFAULT:
        return true;

    case COLOR_BASE16:
    case COLOR_BASE256:
        return term->colors.table[cell->attrs.bg] == term->colors.bg;

    case COLOR_RGB:
        return cell->attrs.bg == term->colors.bg;
    }

    return false;
}

void
kitty_render_underlay(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
    if (term->kitty == NULL || overlay_hidden(term))
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    if (store->underlay_count == 0)
        return;

    const bool default_bg = cell_has_default_bg(term, &row->cells[col]);
    const int cell_w = term->cell_width;
    const int cell_h = term->cell_height;

    for (size_t i = 0; i < store->underlay_count; i++) {
        struct kitty_placement *p = store->underlay[i];

        if (p->cache_pix == NULL)
            continue;
        if (col < p->col || col >= p->col + p->cols)
            continue;
        if (p->z < KITTY_Z_DEFAULT_BG_CUTOFF && !default_bg)
            continue;

        int top;
        if (!kitty_placement_visible_top(term, p, &top))
            continue;
        if (row_no < top || row_no >= top + p->rows)
            continue;

        const int px =
            term->margins.left + p->col * cell_w + p->X + p->cache_x;
        const int py =
            term->margins.top + top * cell_h + p->Y + p->cache_y;

        int src_x = x - px;
        int src_y = y - py;
        int dst_x = x;
        int dst_y = y;
        int w = cell_w;
        int h = cell_h;

        if (src_x < 0) {
            dst_x -= src_x;
            w += src_x;
            src_x = 0;
        }
        if (src_y < 0) {
            dst_y -= src_y;
            h += src_y;
            src_y = 0;
        }
        if (src_x + w > p->cache_w)
            w = p->cache_w - src_x;
        if (src_y + h > p->cache_h)
            h = p->cache_h - src_y;

        if (w <= 0 || h <= 0)
            continue;

        pixman_image_composite32(
            PIXMAN_OP_OVER, p->cache_pix, NULL, pix,
            src_x, src_y, 0, 0, dst_x, dst_y, w, h);
    }
}

/* The kitty Unicode placeholder character. */
#define KITTY_IMAGE_PLACEHOLDER_CHAR 0x10EEEEu

/*
 * Resolve a cell's base character and its combining diacritics.
 * Returns the number of diacritics (0..3). Cells with combining
 * characters are stored as composed entries (design D8).
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
 * mode, the 24-bit RGB value in true color mode (spec: Placeholder
 * cells show image parts).
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
 * The inheritance rules are kitty's run-based algorithm (design D8):
 * a row, column and third diacritic may be omitted, and are then
 * inherited from the placeholder cell to the left, when that cell
 * continues the same run (same fg and underline color, and compatible
 * explicit values).
 */
static bool
placeholder_decode(
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
static struct kitty_placement *
placeholder_find_virtual(
    struct kitty_placement_store *store, uint32_t image_id,
    uint32_t placement_id)
{
    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;

        if (!p->virtual_placement || p->image_id != image_id)
            continue;
        if (placement_id != 0 && p->placement_id != placement_id)
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

bool
kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
    (void)row_no;

    uint32_t id, placement_id;
    int p_row, p_col;

    if (!placeholder_decode(
            term, row, col, &id, &placement_id, &p_row, &p_col))
    {
        return false;
    }

    /* A placeholder never shows a glyph; draw nothing when hidden or
     * when no image matches (spec: Placeholder cells show image
     * parts). */
    if (term->kitty == NULL || overlay_hidden(term))
        return true;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    struct kitty_placement *p = placeholder_find_virtual(
        store, id, placement_id);
    if (p == NULL || p->cache_pix == NULL)
        return true;

    /* Bounds: a row or column outside the virtual placement shows the
     * cell background (spec: Image fit inside the placeholder area). */
    if (p_row < 0 || p_row >= p->rows ||
        p_col < 0 || p_col >= p->cols)
    {
        return true;
    }

    const int cell_w = term->cell_width;
    const int cell_h = term->cell_height;

    /* The cache is the fitted, centered image; the box cell (p_row,
     * p_col) selects the matching slice. */
    int src_x = p_col * cell_w - p->cache_x;
    int src_y = p_row * cell_h - p->cache_y;
    int dst_x = x;
    int dst_y = y;
    int w = cell_w;
    int h = cell_h;

    if (src_x < 0) {
        dst_x -= src_x;
        w += src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        dst_y -= src_y;
        h += src_y;
        src_y = 0;
    }
    if (src_x + w > p->cache_w)
        w = p->cache_w - src_x;
    if (src_y + h > p->cache_h)
        h = p->cache_h - src_y;

    if (w > 0 && h > 0) {
        pixman_image_composite32(
            PIXMAN_OP_OVER, p->cache_pix, NULL, pix,
            src_x, src_y, 0, 0, dst_x, dst_y, w, h);
    }

    return true;
}

/*
 * Test helpers. A placeholder cell with combining characters is stored
 * as a composed entry (design D8), exactly as the VT parser builds it.
 */
static char32_t
test_placeholder_wc(
    struct terminal *term, const char32_t *diacritics, int count)
{
    if (count == 0)
        return KITTY_IMAGE_PLACEHOLDER_CHAR;

    char32_t chars[4] = {KITTY_IMAGE_PLACEHOLDER_CHAR};
    for (int i = 0; i < count; i++)
        chars[i + 1] = diacritics[i];

    const uint32_t key = composed_key_from_chars(
        (const uint32_t *)chars, (size_t)count + 1);

    if (composed_lookup(term->composed, key) == NULL) {
        struct composed *cc = xcalloc(1, sizeof(*cc));
        cc->chars = xcalloc((size_t)count + 1, sizeof(cc->chars[0]));
        for (int i = 0; i <= count; i++)
            cc->chars[i] = chars[i];
        cc->count = (uint8_t)(count + 1);
        cc->width = 1;
        cc->key = key;
        composed_insert(&term->composed, cc);
    }

    return CELL_COMB_CHARS_LO + key;
}

static void
test_placeholder_cell(struct cell *cell, char32_t wc, uint32_t fg_index)
{
    memset(cell, 0, sizeof(*cell));
    cell->wc = wc;
    cell->attrs.fg_src = COLOR_BASE256;
    cell->attrs.fg = fg_index;
}

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
    test_placeholder_cell(&cells[0], KITTY_IMAGE_PLACEHOLDER_CHAR, 42);
    xassert(placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42 && placement_id == 0);
    xassert(r == 0 && c == 0);

    /* One diacritic: the row. */
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[5]}, 1), 42);
    xassert(placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42);
    xassert(r == 5 && c == 0);

    /* Two diacritics: row and column. */
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[5], kitty_diacritics[7]}, 2),
                          42);
    xassert(placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42);
    xassert(r == 5 && c == 7);

    /* Three diacritics: row, column and the high byte of the id. */
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[5], kitty_diacritics[7],
                                  kitty_diacritics[2]}, 3),
                          42);
    xassert(placeholder_decode(
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
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[2],
                                  kitty_diacritics[2]}, 3),
                          42);

    /* cell 1: no diacritics: row and high byte inherited, column +1. */
    test_placeholder_cell(&cells[1], test_placeholder_wc(&term, NULL, 0), 42);
    xassert(placeholder_decode(
                &term, &row, 1, &id, &placement_id, &r, &c));
    xassert(id == (42u | (2u << 24)));
    xassert(r == 1 && c == 3);

    /* cell 2: row diacritic only: column +1, high byte inherited. */
    test_placeholder_cell(&cells[2], test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[1]}, 1), 42);
    xassert(placeholder_decode(
                &term, &row, 2, &id, &placement_id, &r, &c));
    xassert(id == (42u | (2u << 24)));
    xassert(r == 1 && c == 4);

    /* cell 3: row and column diacritics: high byte inherited. */
    test_placeholder_cell(&cells[3], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[5]}, 2),
                          42);
    xassert(placeholder_decode(
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

    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[2]}, 2),
                          42);
    test_placeholder_cell(&cells[1], test_placeholder_wc(&term, NULL, 0), 43);

    /* Different fg color: no inheritance. */
    xassert(placeholder_decode(
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

    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[1], kitty_diacritics[2]}, 2),
                          42);
    xassert(placeholder_decode(
                &term, &row, 0, &id, &placement_id, &r, &c));
    xassert(id == 42 && placement_id == 7);
    xassert(r == 1 && c == 2);

    composed_free(term.composed);
}

/*
 * Hook B: a row or column index outside the virtual placement shows
 * nothing; an in-range index draws the matching image slice.
 */
#define PLACEHOLDER_TERM(t) do {                                        \
        memset(&(t), 0, sizeof(t));                                     \
        (t).ptmx = -1;                                                  \
        (t).rows = 2;                                                   \
        (t).cols = 2;                                                   \
        (t).cell_width = 4;                                             \
        (t).cell_height = 4;                                            \
        (t).width = 8;                                                  \
        (t).height = 8;                                                 \
        (t).normal.num_rows = 4;                                        \
        (t).normal.rows = xcalloc(4, sizeof((t).normal.rows[0]));       \
        (t).grid = &(t).normal;                                         \
        (t).selection.coords.end.row = -1;                              \
        kitty_init(&(t));                                               \
    } while (0)

UNITTEST
{
    struct terminal term;
    PLACEHOLDER_TERM(term);

    /* A 4x4 opaque red image. */
    uint32_t *src = xcalloc(16, sizeof(src[0]));
    for (int i = 0; i < 16; i++)
        src[i] = 0xffff0000u;
    pixman_image_t *src_pix = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 4, 4, src, 16);

    struct kitty_image proto = {
        .id = 42,
        .width = 4,
        .height = 4,
        .data = src,
        .size = 16 * sizeof(src[0]),
        .pix = src_pix,
    };
    xassert(kitty_image_store_add(kitty_image_store_for(&term), &proto));

    /* A 2x2 cell virtual placement of image 42. */
    struct kitty_command put;
    kitty_command_init(&put);
    put.action = KITTY_ACTION_PUT;
    put.has_id = true;
    put.id = 42;
    put.has_cols = true;
    put.cols = 1;
    put.has_rows = true;
    put.rows = 1;
    put.has_unicode_placeholder = true;
    put.unicode_placeholder = 1;
    xassert(kitty_placement_put(
                &term, &put,
                kitty_image_store_find_id(
                    kitty_image_store_for(&term), 42)) == NULL);

    kitty_render_prepass(&term);

    /* Destination buffer: one cell, blue. */
    uint32_t *dst = xcalloc(16, sizeof(dst[0]));
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    pixman_image_t *dst_pix = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 4, 4, dst, 16);

    struct cell cells[1] = {0};
    const struct row row = {.cells = cells};

    /* Row 0, column 0 is inside the 1x1 placement: the image is drawn
     * unscaled (cell 4x4, image 4x4). */
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[0]}, 1), 42);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0, 0));
    xassert(dst[0] == 0xffff0000u);

    /* Row 1 is outside the 1-row placement: nothing is drawn. */
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[1]}, 1), 42);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0, 0));
    xassert(dst[0] == 0xff0000ffu);

    /* Column 1 is outside the 1-column placement: nothing is drawn. */
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[0], kitty_diacritics[1]}, 2),
                          42);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0, 0));
    xassert(dst[0] == 0xff0000ffu);

    /* Unknown image id: nothing is drawn. */
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    test_placeholder_cell(&cells[0], test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[0]}, 1), 43);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0, 0));
    xassert(dst[0] == 0xff0000ffu);

    pixman_image_unref(dst_pix);
    free(dst);

    composed_free(term.composed);
    kitty_fini(&term);
    free(term.normal.rows);
}
