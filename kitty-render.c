#include "kitty-graphics.h"

#include <stdlib.h>
#include <string.h>

#include "kitty-placement.h"

#define LOG_MODULE "kitty-render"
#define LOG_ENABLE_DBG 0
#include "debug.h"
#include "grid.h"
#include "log.h"
#include "xmalloc.h"

/*
 * Screen row of the placement's first visible row. Returns false when
 * no part of the placement is inside the viewport. The result may be
 * negative: the placement then starts above the viewport and is
 * clipped.
 */
static bool
placement_visible_top(
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

void
kitty_render_prepass(struct terminal *term)
{
    if (term->kitty == NULL || overlay_hidden(term))
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    if (tll_length(store->placements) == 0)
        return;

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (p->virtual_placement || p->z < 0)
            continue;

        int top;
        if (!placement_visible_top(term, p, &top))
            continue;

        placement_ensure_cache(term, store, p);
    }
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
        if (!placement_visible_top(term, p, &top))
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
        if (!placement_visible_top(term, p, &top))
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

void
kitty_render_underlay(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
    /* z < 0 placements are added by a later task (hook A). */
    (void)term; (void)pix; (void)row; (void)row_no; (void)col; (void)x; (void)y;
}

bool
kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
    /* Unicode placeholders are added by a later task (hook B). */
    (void)term; (void)pix; (void)row; (void)row_no; (void)col; (void)x; (void)y;
    return false;
}
