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

static bool
delete_matches_visible(struct terminal *term, const struct kitty_placement *p)
{
    if (p->virtual_placement)
        return false;
    int top;
    return kitty_placement_visible_top(term, p, &top);
}

static bool
delete_matches_id(
    const struct kitty_command *cmd, const struct kitty_placement *p)
{
    if (!cmd->id.set || p->image_id.v != cmd->id.value)
        return false;
    return !cmd->placement_id.set ||
        p->placement_id.v == cmd->placement_id.value;
}

static bool
delete_matches_number(
    const struct kitty_command *cmd, const struct kitty_placement *p,
    const struct kitty_image *number_image)
{
    /* Only the newest image with this number. */
    if (number_image == NULL || p->image_id.v != number_image->id.v)
        return false;
    return !cmd->placement_id.set ||
        p->placement_id.v == cmd->placement_id.value;
}

static bool
delete_matches_image_range(
    const struct kitty_command *cmd, const struct kitty_placement *p)
{
    if (!cmd->x.set || !cmd->y.set)
        return false;
    return (int64_t)cmd->x.value <= (int64_t)p->image_id.v &&
        (int64_t)p->image_id.v <= (int64_t)cmd->y.value;
}

static bool
delete_matches_cursor_cell(
    struct terminal *term, const struct kitty_placement *p)
{
    if (p->virtual_placement)
        return false;
    const int col = term->grid->cursor.point.col;
    if (col < p->col || col >= p->col + p->cols)
        return false;
    const int64_t abs_row = grid_row_absolute(
        term->grid, term->grid->cursor.point.row);
    return placement_covers_abs_row(term, p, abs_row);
}

/*
 * The p/P and q/Q selectors: the placement covers one screen cell.
 * q/Q additionally requires the placement's z value to match.
 */
static bool
delete_matches_screen_cell(
    struct terminal *term, const struct kitty_command *cmd,
    const struct kitty_placement *p, bool check_z)
{
    if (p->virtual_placement || !cmd->x.set || !cmd->y.set)
        return false;
    if (check_z && (!cmd->z.set || p->z != cmd->z.value))
        return false;
    const int64_t sy = (int64_t)cmd->y.value - 1;
    if (cmd->x.value < 1 || sy < 0 || sy >= term->grid->num_rows)
        return false;
    return placement_covers_screen_cell(
        term, p, (int)cmd->x.value - 1, (int)sy);
}

static bool
delete_matches_x(
    const struct kitty_command *cmd, const struct kitty_placement *p)
{
    if (p->virtual_placement || !cmd->x.set || cmd->x.value < 1)
        return false;
    return (int64_t)cmd->x.value - 1 >= p->col &&
        (int64_t)cmd->x.value - 1 < (int64_t)p->col + p->cols;
}

static bool
delete_matches_screen_row(
    struct terminal *term, const struct kitty_command *cmd,
    const struct kitty_placement *p)
{
    if (p->virtual_placement || !cmd->y.set)
        return false;
    const int64_t sy = (int64_t)cmd->y.value - 1;
    return placement_covers_screen_row(term, p, (int)sy);
}

static bool
delete_matches_z(
    const struct kitty_command *cmd, const struct kitty_placement *p)
{
    if (p->virtual_placement || !cmd->z.set)
        return false;
    return p->z == cmd->z.value;
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
    switch (cmd->delete_spec ? cmd->delete_spec : 'a') {
    case 'a':
    case 'A':
        return delete_matches_visible(term, p);

    case 'i':
    case 'I':
        return delete_matches_id(cmd, p);

    case 'n':
    case 'N':
        return delete_matches_number(cmd, p, number_image);

    case 'r':
    case 'R':
        return delete_matches_image_range(cmd, p);

    case 'c':
    case 'C':
        return delete_matches_cursor_cell(term, p);

    case 'p':
    case 'P':
        return delete_matches_screen_cell(term, cmd, p, false);

    case 'q':
    case 'Q':
        return delete_matches_screen_cell(term, cmd, p, true);

    case 'x':
    case 'X':
        return delete_matches_x(cmd, p);

    case 'y':
    case 'Y':
        return delete_matches_screen_row(term, cmd, p);

    case 'z':
    case 'Z':
        return delete_matches_z(cmd, p);

    default:
        /* Unknown selectors, and 'f' (animation frames), change
         * nothing. */
        return false;
    }
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

    tll_foreach(store->placements, it) {
        struct kitty_placement *p = &it->item;
        if (!delete_matches(term, cmd, p, number_image))
            continue;

        /* Repaint the cells the placement covered. */
        kitty_placement_dirty(term, p);
        kitty_placement_unlink(store, p);
        kitty_placement_remove(store, it);
    }

    if (uppercase) {
        /*
         * An uppercase variant also deletes the image data of every
         * image that has no placement left.
         */
        kitty_image_store_delete_unreferenced(images);
    }
}

