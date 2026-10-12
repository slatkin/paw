#pragma once

#include "kitty-placement.h"

/*
 * Internal API shared by kitty-placement.c, kitty-scroll.c and
 * kitty-placement-delete.c.
 */

/*
 * Remove a placement from the store. The per-frame underlay list holds
 * pointers to placement nodes, so any removal invalidates it. Hook A
 * also runs from render_sixel_images(), before the next
 * kitty_render_prepass() rebuilds the list.
 */
#define kitty_placement_remove(store, it)       \
    do {                                        \
        (store)->underlay_count = 0;            \
        tll_remove((store)->placements, (it));  \
    } while (0)

/* Release a placement's scaled cache and give the image's count back. */
static inline void
kitty_placement_unlink(
    struct kitty_placement_store *store, struct kitty_placement *p)
{
    if (p->image != NULL && p->image->placements > 0)
        p->image->placements--;
    kitty_placement_cache_free(store, p);
}
