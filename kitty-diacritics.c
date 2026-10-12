#include "kitty-diacritics.h"

#define LOG_MODULE "kitty-diacritics"
#define LOG_ENABLE_DBG 0
#include "debug.h"

int
kitty_diacritic_index(uint32_t cp)
{
    int lo = 0;
    int hi = KITTY_DIACRITICS_COUNT - 1;

    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;

        if (kitty_diacritics[mid] < cp)
            lo = mid + 1;
        else if (kitty_diacritics[mid] > cp)
            hi = mid - 1;
        else
            return mid;
    }

    return -1;
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
