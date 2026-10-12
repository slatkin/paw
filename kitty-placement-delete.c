#include "kitty-placement-internal.h"

#define LOG_MODULE "kitty-placement-delete"
#define LOG_ENABLE_DBG 0
#include "grid.h"

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
    const char sel = cmd->delete_spec ? cmd->delete_spec : 'a';
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
        if (!cmd->id.set || p->image_id.v != cmd->id.value)
            return false;
        return !cmd->placement_id.set ||
            p->placement_id.v == cmd->placement_id.value;

    case 'n':
    case 'N':
        /* Only the newest image with this number. */
        if (number_image == NULL || p->image_id.v != number_image->id.v)
            return false;
        return !cmd->placement_id.set ||
            p->placement_id.v == cmd->placement_id.value;

    case 'r':
    case 'R':
        if (!cmd->x.set || !cmd->y.set)
            return false;
        return (int64_t)cmd->x.value <= (int64_t)p->image_id.v &&
            (int64_t)p->image_id.v <= (int64_t)cmd->y.value;

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
        if (p->virtual_placement || !cmd->x.set || !cmd->y.set)
            return false;
        const int64_t sy = (int64_t)cmd->y.value - 1;
        if (cmd->x.value < 1 || sy < 0 || sy >= num_rows)
            return false;
        return placement_covers_screen_cell(
            term, p, (int)cmd->x.value - 1, (int)sy);
    }

    case 'q':
    case 'Q': {
        if (p->virtual_placement || !cmd->x.set || !cmd->y.set || !cmd->z.set)
            return false;
        if (p->z != cmd->z.value)
            return false;
        const int64_t sy = (int64_t)cmd->y.value - 1;
        if (cmd->x.value < 1 || sy < 0 || sy >= num_rows)
            return false;
        return placement_covers_screen_cell(
            term, p, (int)cmd->x.value - 1, (int)sy);
    }

    case 'x':
    case 'X':
        if (p->virtual_placement || !cmd->x.set || cmd->x.value < 1)
            return false;
        return (int64_t)cmd->x.value - 1 >= p->col &&
            (int64_t)cmd->x.value - 1 < (int64_t)p->col + p->cols;

    case 'y':
    case 'Y': {
        if (p->virtual_placement || !cmd->y.set)
            return false;
        const int64_t sy = (int64_t)cmd->y.value - 1;
        return placement_covers_screen_row(term, p, (int)sy);
    }

    case 'z':
    case 'Z':
        if (p->virtual_placement || !cmd->z.set)
            return false;
        return p->z == cmd->z.value;

    default:
        /* Unknown selectors, and 'f' (animation frames), change
         * nothing. */
        return false;
    }
}

static void
delete_image_if_unused(
    struct kitty_image_store *images, struct kitty_image_id id)
{
    struct kitty_image *image = kitty_image_store_find_id(images, id);
    if (image != NULL && image->placements == 0)
        kitty_image_store_delete_id(images, id);
}

void
kitty_placement_delete(struct terminal *term, const struct kitty_command *cmd)
{
    const char sel = cmd->delete_spec ? cmd->delete_spec : 'a';

    /* Animation frames are not supported: d=f changes nothing. */
    if (sel == 'f' || sel == 'F')
        return;

    struct kitty_placement_store *store = kitty_placement_store_for(term);
    struct kitty_image_store *images = kitty_image_store_for(term);
    const bool uppercase = sel >= 'A' && sel <= 'Z';

    /*
     * d=n/N matches the newest image with number I only. Resolve it
     * once, before any placement is removed.
     */
    struct kitty_image *number_image =
        (sel == 'n' || sel == 'N') && cmd->number.set
        ? kitty_image_store_find_number(
            images, KITTY_IMAGE_NUMBER(cmd->number.value))
        : NULL;

    tll(struct kitty_image_id) touched = tll_init();

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (!delete_matches(term, cmd, p, number_image))
            continue;

        /* Repaint the cells the placement covered. */
        kitty_placement_dirty(term, p);
        tll_push_back(touched, p->image_id);
        kitty_placement_unlink(store, p);
        kitty_placement_remove(store, it);
    }

    if (uppercase) {
        /*
         * Kitty frees an image with no placement left for I, N and R
         * even when no placement matched.
         */
        if (!cmd->placement_id.set) {
            switch (sel) {
            case 'I':
                if (cmd->id.set)
                    delete_image_if_unused(images, KITTY_IMAGE_ID(cmd->id.value));
                break;

            case 'N':
                if (number_image != NULL)
                    delete_image_if_unused(images, number_image->id);
                break;

            case 'R':
                if (cmd->x.set && cmd->y.set) {
                    tll(struct kitty_image_id) range = tll_init();
                    tll_foreach(images->images, img_it) {
                        const struct kitty_image_id id = img_it->item.id;
                        if ((int64_t)cmd->x.value <= (int64_t)id.v &&
                            (int64_t)id.v <= (int64_t)cmd->y.value)
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

