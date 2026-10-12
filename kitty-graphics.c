#include "kitty-graphics.h"

#include "xmalloc.h"

struct kitty_graphics {
    struct terminal *term;
};

void
kitty_init(struct terminal *term)
{
    xassert(term->kitty == NULL);
    term->kitty = xcalloc(1, sizeof(struct kitty_graphics));
    term->kitty->term = term;
}

void
kitty_fini(struct terminal *term)
{
    free(term->kitty);
    term->kitty = NULL;
}

void
kitty_scroll_up(struct terminal *term, struct scroll_region region, int rows)
{
}

void
kitty_scroll_down(struct terminal *term, struct scroll_region region, int rows)
{
}

void
kitty_clear_visible(struct terminal *term)
{
}

void
kitty_alt_screen_clear(struct terminal *term)
{
}

void
kitty_reset(struct terminal *term)
{
}

void
kitty_cell_size_changed(struct terminal *term)
{
}

void
kitty_reflow_begin(struct terminal *term)
{
}

void
kitty_reflow_end(struct terminal *term)
{
}

void
kitty_render_overlay(
    struct terminal *term, pixman_image_t *pix,
    pixman_region32_t *damage, const struct coord *cursor)
{
}

void
kitty_render_underlay(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
}

bool
kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
    return false;
}
