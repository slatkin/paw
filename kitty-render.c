#include "kitty-graphics.h"

#include <stdlib.h>
#include <string.h>

#include "kitty-diacritics.h"
#include "kitty-placement.h"
#include "kitty-render-internal.h"

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
 * nothing if the cache is valid for the current cell size. If the cache
 * would exceed the per-screen cache budget, the "cache" is only a scaled
 * view of the source image.
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

    /* An unscaled placement keeps its pixel size until the cell size
     * changes; then it covers the same cells, scaled like c and r. */
    if (p->scaled || p->cell_width != term->cell_width ||
        p->cell_height != term->cell_height)
    {
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

    /* A scratch view of the source rectangle, inside the image data. */
    uint32_t *sub_data =
        p->image->data + (size_t)p->src_y * p->image->width + p->src_x;
    pixman_image_t *sub = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, p->src_w, p->src_h,
        sub_data, p->image->width * 4);
    if (sub == NULL)
        return;

    struct pixman_f_transform fscale;
    pixman_f_transform_init_scale(
        &fscale, (double)p->src_w / display_w, (double)p->src_h / display_h);

    struct pixman_transform scale;
    pixman_transform_from_pixman_f_transform(&scale, &fscale);

    pixman_image_set_transform(sub, &scale);
    pixman_image_set_filter(sub, PIXMAN_FILTER_GOOD, NULL, 0);

    size_t bytes = 0;
    uint32_t *data = NULL;
    pixman_image_t *scaled = NULL;

    if (kitty_checked_size(
            (uint32_t)display_w, (uint32_t)display_h, 4, SIZE_MAX, &bytes) &&
        bytes <= KITTY_PLACEMENT_CACHE_QUOTA &&
        store->cache_bytes <= KITTY_PLACEMENT_CACHE_QUOTA - bytes)
    {
        data = xmalloc(bytes);
        memset(data, 0, bytes);

        scaled = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, display_w, display_h, data, display_w * 4);
        if (scaled == NULL) {
            free(data);
            data = NULL;
        }
    }

    if (scaled != NULL) {
        pixman_image_composite32(
            PIXMAN_OP_SRC, sub, NULL, scaled, 0, 0, 0, 0, 0, 0,
            display_w, display_h);
        pixman_image_unref(sub);
    } else {
        /* Over budget: draw straight from the source image. The
         * transform scales it on every frame, and the cache uses no
         * memory. */
        LOG_DBG("scaled image cache over budget, drawing without it");
        scaled = sub;
        bytes = 0;
    }

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

/* Overlays are only drawn while the terminal is on the main screen. */
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
    if (a->image_id.v != b->image_id.v)
        return a->image_id.v < b->image_id.v ? -1 : 1;
    if (a->placement_id.v != b->placement_id.v)
        return a->placement_id.v < b->placement_id.v ? -1 : 1;
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
         * scaled cache must be ready before the workers run.
         */
        if (p->virtual_placement) {
            placement_ensure_cache(term, store, p);
            continue;
        }

        int top;
        if (!kitty_placement_visible_top(term, p, &top))
            continue;

        /* The workers never allocate: build every visible cache here. */
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
    struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage)
{
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
         * add up.
         */
        kitty_placement_dirty(term, p);
    }

    free(order);
}

/*
 * True when the cell's effective background is the default one. This
 * is the same test render_cell() uses for transparency.
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

/*
 * Composite the part of the placement's cache under one cell. src_x
 * and src_y may be negative, when the cell starts left of or above the
 * cache. The clipped part is drawn at (dst_x, dst_y).
 */
static void
blit_cell(
    pixman_image_t *pix, const struct kitty_placement *p,
    int src_x, int src_y, int dst_x, int dst_y, int w, int h)
{
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
        return;

    pixman_image_composite32(
        PIXMAN_OP_OVER, p->cache_pix, NULL, pix,
        src_x, src_y, 0, 0, dst_x, dst_y, w, h);
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

        blit_cell(pix, p, x - px, y - py, x, y, cell_w, cell_h);
    }
}

bool
kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int col, int x, int y)
{
    uint32_t id, placement_id;
    int p_row, p_col;

    if (!kitty_placeholder_decode(
            term, row, col, &id, &placement_id, &p_row, &p_col))
    {
        return false;
    }

    /* A placeholder never shows a glyph; draw nothing when hidden or
     * when no image matches. */
    if (term->kitty == NULL || overlay_hidden(term))
        return true;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    struct kitty_placement *p = kitty_placeholder_find_virtual(
        store, KITTY_IMAGE_ID(id), KITTY_PLACEMENT_ID(placement_id));
    if (p == NULL || p->cache_pix == NULL)
        return true;

    /* Bounds: a row or column outside the virtual placement shows the
     * cell background. */
    if (p_row < 0 || p_row >= p->rows ||
        p_col < 0 || p_col >= p->cols)
    {
        return true;
    }

    const int cell_w = term->cell_width;
    const int cell_h = term->cell_height;

    /* The cache is the fitted, centered image; the box cell (p_row,
     * p_col) selects the matching slice. */
    blit_cell(
        pix, p, p_col * cell_w - p->cache_x, p_row * cell_h - p->cache_y,
        x, y, cell_w, cell_h);

    return true;
}

#ifndef NDEBUG
#include "kitty-test.h"

/*
 * Hook B: a row or column index outside the virtual placement shows
 * nothing; an in-range index draws the matching image slice.
 */
/* The stub terminal of the tests below: 2 x 2 cells of 4 x 4 pixels. */
#define PLACEHOLDER_TERM(t) KITTY_TEST_TERM(t, 2, 2, 4, 4, 4)

/* Store an opaque red 4x4 image, with its pixman image. */
static void
test_add_red_image(struct terminal *term, uint32_t id)
{
    uint32_t *src = xcalloc(16, sizeof(src[0]));
    for (int i = 0; i < 16; i++)
        src[i] = 0xffff0000u;

    struct kitty_image image = {
        .id = KITTY_IMAGE_ID(id), .width = 4, .height = 4,
        .data = src, .size = 16 * sizeof(src[0]),
        .pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, 4, 4, src, 16),
    };
    xassert(kitty_image_store_add(kitty_image_store_for(term), &image));
}

UNITTEST
{
    struct terminal term;
    PLACEHOLDER_TERM(term);

    test_add_red_image(&term, 42);

    /* A 2x2 cell virtual placement of image 42. */
    struct kitty_command put;
    kitty_command_init(&put);
    put.action = KITTY_ACTION_PUT;
    put.id = kitty_some_u32(42);
    put.cols = kitty_some_u32(1);
    put.rows = kitty_some_u32(1);
    put.unicode_placeholder = kitty_some_u32(1);
    kitty_test_put(&term, &put);

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
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[0]}, 1), 42);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0));
    xassert(dst[0] == 0xffff0000u);

    /* Row 1 is outside the 1-row placement: nothing is drawn. */
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[1]}, 1), 42);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0));
    xassert(dst[0] == 0xff0000ffu);

    /* Column 1 is outside the 1-column placement: nothing is drawn. */
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){
                                  kitty_diacritics[0], kitty_diacritics[1]}, 2),
                          42);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0));
    xassert(dst[0] == 0xff0000ffu);

    /* Unknown image id: nothing is drawn. */
    for (int i = 0; i < 16; i++)
        dst[i] = 0xff0000ffu;
    kitty_test_placeholder_cell(&cells[0], kitty_test_placeholder_wc(
                              &term, (char32_t[]){kitty_diacritics[0]}, 1), 43);
    xassert(kitty_render_placeholder(&term, dst_pix, &row, 0, 0, 0));
    xassert(dst[0] == 0xff0000ffu);

    pixman_image_unref(dst_pix);
    free(dst);

    composed_free(term.composed);
    KITTY_TEST_FINI(term);
}

UNITTEST
{
    /* An unscaled placement keeps its cell span on a cell size change. */
    struct terminal term;
    PLACEHOLDER_TERM(term);

    test_add_red_image(&term, 42);

    struct kitty_command put;
    kitty_command_init(&put);
    put.action = KITTY_ACTION_PUT;
    put.id = kitty_some_u32(42);
    kitty_test_put(&term, &put);

    const struct kitty_placement *p =
        &tll_front(kitty_placement_store_for(&term)->placements);
    kitty_render_prepass(&term);
    xassert(p->cache_w == 4 && p->cache_h == 4);

    term.cell_width = term.cell_height = 8;
    kitty_cell_size_changed(&term);
    kitty_render_prepass(&term);
    xassert(p->cols == 1 && p->rows == 1);
    xassert(p->cache_w == 8 && p->cache_h == 8);

    KITTY_TEST_FINI(term);
}

#endif /* !NDEBUG */
