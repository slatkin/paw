#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kitty-image.h"
#include "kitty-placement.h"
#include "terminal.h"

/*
 * Internal API between the placeholder decoder (kitty-placeholder.c)
 * and the placeholder renderer (kitty-render.c).
 */

/* The kitty Unicode placeholder code point (U+10EEEE). */
#define KITTY_IMAGE_PLACEHOLDER_CHAR 0x10EEEEu

/*
 * Decode the placeholder cell at 'col'. Returns false when the base
 * character is not U+10EEEE. On success, '*out_id' is the image id,
 * '*out_placement_id' the placement id, and '*out_row' / '*out_col'
 * are the 0-based row and column inside the virtual placement.
 */
bool kitty_placeholder_decode(
    struct terminal *term, const struct row *row, int col,
    uint32_t *out_id, uint32_t *out_placement_id,
    int *out_row, int *out_col);

/*
 * The first virtual placement for 'image_id' (and 'placement_id', when
 * non-zero). A placement id of 0 means "any" (kitty).
 */
struct kitty_placement *kitty_placeholder_find_virtual(
    struct kitty_placement_store *store, struct kitty_image_id image_id,
    struct kitty_placement_id placement_id);
