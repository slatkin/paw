#include "kitty-diacritics.h"

#include <stdlib.h>

#define LOG_MODULE "kitty-diacritics"
#define LOG_ENABLE_DBG 0
#include "debug.h"

static int
diacritic_cmp(const void *key, const void *elem)
{
    const uint32_t cp = *(const uint32_t *)key;
    const uint32_t entry = *(const uint32_t *)elem;
    return (cp > entry) - (cp < entry);
}

int
kitty_diacritic_index(uint32_t cp)
{
    const uint32_t *found = bsearch(
        &cp, kitty_diacritics, KITTY_DIACRITICS_COUNT,
        sizeof(kitty_diacritics[0]), diacritic_cmp);

    return found == NULL ? -1 : (int)(found - kitty_diacritics);
}

UNITTEST
{
    /* Anchors of design D9: the first three entries and the count. */
    xassert(KITTY_DIACRITICS_COUNT == 297);
    xassert(kitty_diacritic_index(0x0305) == 0);
    xassert(kitty_diacritic_index(0x030D) == 1);
    xassert(kitty_diacritic_index(0x030E) == 2);

    /* The table is sorted, and the last entry is reachable. */
    for (int i = 1; i < KITTY_DIACRITICS_COUNT; i++)
        xassert(kitty_diacritics[i - 1] < kitty_diacritics[i]);
    xassert(kitty_diacritic_index(kitty_diacritics[296]) == 296);

    /* Non-diacritics are not found. */
    xassert(kitty_diacritic_index(0x0300) == -1);
    xassert(kitty_diacritic_index(0x0041) == -1);
}
