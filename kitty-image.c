#include "kitty-image.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "kitty-image"
#define LOG_ENABLE_DBG 0
#include "base64.h"
#include "debug.h"
#include "xmalloc.h"

/*
 * Wrap-safe check that 'width * height * bpp' fits in size_t and does
 * not exceed 'max_bytes'. On success stores the product in '*out'.
 *
 * Uses division instead of multiplication so that 32-bit width and
 * height values from hostile input cannot overflow size_t.
 */
bool
kitty_checked_size(
    uint32_t width, uint32_t height, uint32_t bpp, size_t max_bytes,
    size_t *out)
{
    if (width == 0 || height == 0 || bpp == 0)
        return false;

    if (width > max_bytes / bpp)
        return false;

    const size_t row = (size_t)width * bpp;
    if (height > max_bytes / row)
        return false;

    *out = row * height;
    return true;
}

/*
 * Decode base64 text with or without the '=' padding. Some clients
 * (for example kitten) omit it. base64_decode() needs the padding, so
 * a padded copy is decoded.
 */
char *
kitty_base64_decode(const char *s, size_t *out_len)
{
    const size_t len = strlen(s);
    if (len % 4 == 0)
        return base64_decode(s, out_len);

    const size_t padded_len = (len + 3) / 4 * 4;
    char *padded = xmalloc(padded_len + 1);
    memcpy(padded, s, len);
    memset(padded + len, '=', padded_len - len);
    padded[padded_len] = '\0';

    char *ret = base64_decode(padded, out_len);
    free(padded);
    return ret;
}

UNITTEST
{
    size_t size = 0;

    char *decoded = kitty_base64_decode("TWE", &size);
    xassert(decoded != NULL && size == 2 && memcmp(decoded, "Ma", 2) == 0);
    free(decoded);

    decoded = kitty_base64_decode("TQ", &size);
    xassert(decoded != NULL && size == 1 && decoded[0] == 'M');
    free(decoded);
}

void
kitty_image_store_init(struct kitty_image_store *store)
{
    memset(store, 0, sizeof(*store));
    store->next_id = 1;
    store->quota = KITTY_IMAGE_QUOTA;
}

void
kitty_image_destroy(struct kitty_image *image)
{
    if (image->pix != NULL)
        pixman_image_unref(image->pix);
    free(image->data);
    image->pix = NULL;
    image->data = NULL;
    image->size = 0;
}

void
kitty_image_store_fini(struct kitty_image_store *store)
{
    tll_foreach(store->images, it)
        kitty_image_destroy(&it->item);
    tll_free(store->images);
    memset(store, 0, sizeof(*store));
}

struct kitty_image *
kitty_image_store_find_id(
    struct kitty_image_store *store, struct kitty_image_id id)
{
    tll_foreach(store->images, it) {
        if (it->item.id.v == id.v)
            return &it->item;
    }
    return NULL;
}

struct kitty_image *
kitty_image_store_find_number(
    struct kitty_image_store *store, struct kitty_image_number number)
{
    /* The newest image with this number wins. */
    tll_rforeach(store->images, it) {
        if (it->item.number.set && it->item.number.value == number.v)
            return &it->item;
    }
    return NULL;
}

struct kitty_image_id
kitty_image_store_alloc_id(struct kitty_image_store *store)
{
    for (;;) {
        const struct kitty_image_id id = KITTY_IMAGE_ID(store->next_id++);
        if (store->next_id == 0)
            store->next_id = 1;
        if (id.v != 0 && kitty_image_store_find_id(store, id) == NULL)
            return id;
    }
}

/*
 * Remove one image. The removal callback runs first, so that no
 * placement refers to a freed image.
 */
#define image_remove(store, it)                                        \
    do {                                                               \
        if ((store)->on_remove != NULL)                                \
            (store)->on_remove(                                        \
                (store)->on_remove_user, (it)->item.id);               \
        (store)->bytes -= (it)->item.size;                             \
        kitty_image_destroy(&(it)->item);                              \
        tll_remove((store)->images, (it));                             \
    } while (0)

bool
kitty_image_store_delete_id(
    struct kitty_image_store *store, struct kitty_image_id id)
{
    tll_foreach(store->images, it) {
        if (it->item.id.v != id.v)
            continue;

        image_remove(store, it);
        return true;
    }

    return false;
}

/*
 * Delete every image that has no placement. Kitty frees such images
 * after a screen clear.
 */
void
kitty_image_store_delete_unreferenced(struct kitty_image_store *store)
{
    tll_foreach(store->images, it) {
        if (it->item.placements > 0)
            continue;

        image_remove(store, it);
    }
}

/*
 * Delete the oldest image that does ('with_placements') or does not
 * ('!with_placements') have placements. Returns false when the list
 * holds no such image.
 */
static bool
store_remove_oldest(
    struct kitty_image_store *store, bool with_placements)
{
    tll_foreach(store->images, it) {
        if ((it->item.placements > 0) != with_placements)
            continue;

        image_remove(store, it);
        return true;
    }

    return false;
}

bool
kitty_image_store_add(
    struct kitty_image_store *store, const struct kitty_image *image)
{
    /* An image that does not fit on its own is never stored, and
     * never evicts another image. */
    if (image->size > store->quota)
        return false;

    /* An image with the same id is replaced. */
    tll_foreach(store->images, it) {
        if (it->item.id.v != image->id.v)
            continue;

        /* Placements of the replaced image must not outlive it. */
        image_remove(store, it);
        break;
    }

    /* Make room: images without placements first, oldest first, then
     * the oldest images with placements. The
     * subtraction cannot underflow, because image->size <= quota. */
    while (store->bytes > store->quota - image->size) {
        if (!store_remove_oldest(store, false) &&
            !store_remove_oldest(store, true))
        {
            break;
        }
    }

    tll_push_back(store->images, *image);
    store->bytes += image->size;
    return true;
}


UNITTEST
{
    /* An image with an id replaces the old image with that id. */
    struct kitty_image_store store;
    kitty_image_store_init(&store);

    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGB;
    cmd.width = kitty_some_u32(2);
    cmd.height = kitty_some_u32(1);

    static const char payload[] = "/wAAAID/";
    const char *error = NULL;

    struct kitty_image img;
    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    img.id = KITTY_IMAGE_ID(5);
    kitty_image_store_add(&store, &img);
    xassert(tll_length(store.images) == 1);
    xassert(store.bytes == img.size);

    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    img.id = KITTY_IMAGE_ID(5);
    kitty_image_store_add(&store, &img);
    xassert(tll_length(store.images) == 1);
    xassert(store.bytes == img.size);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(5)) != NULL);

    kitty_image_store_fini(&store);
    xassert(store.bytes == 0);
}

UNITTEST
{
    /* I= numbers: the newest image wins, and ids are generated. */
    struct kitty_image_store store;
    kitty_image_store_init(&store);

    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGB;
    cmd.width = kitty_some_u32(1);
    cmd.height = kitty_some_u32(1);

    static const char payload[] = "/wAA";
    const char *error = NULL;

    struct kitty_image img;
    for (uint32_t i = 0; i < 2; i++) {
        xassert(kitty_image_decode(
                    &cmd, payload, sizeof(payload) - 1,
                    KITTY_IMAGE_QUOTA, &img, &error));
        img.id = kitty_image_store_alloc_id(&store);
        img.number = kitty_some_u32(13);
        kitty_image_store_add(&store, &img);
    }

    xassert(tll_length(store.images) == 2);
    struct kitty_image *newest =
        kitty_image_store_find_number(&store, KITTY_IMAGE_NUMBER(13));
    xassert(newest != NULL);
    xassert(newest == &tll_back(store.images));

    kitty_image_store_fini(&store);
}


UNITTEST
{
    /*
     * Quota: with room for three 100-byte images, a fourth evicts the
     * oldest image. The store is exercised with fabricated sizes so
     * the test needs no large allocation.
     */
    struct kitty_image_store store;
    kitty_image_store_init(&store);
    store.quota = 300;

    for (uint32_t i = 1; i <= 4; i++) {
        struct kitty_image img = {0};
        img.id = KITTY_IMAGE_ID(i);
        img.size = 100;
        xassert(kitty_image_store_add(&store, &img));
    }

    xassert(store.bytes == 300);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(1)) == NULL);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(2)) != NULL);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(3)) != NULL);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(4)) != NULL);

    kitty_image_store_fini(&store);
}

UNITTEST
{
    /*
     * Eviction order: an image without placements is deleted before an
     * older image that has one.
     */
    struct kitty_image_store store;
    kitty_image_store_init(&store);
    store.quota = 200;

    struct kitty_image a = {0};
    a.id = KITTY_IMAGE_ID(1);
    a.size = 100;
    a.placements = 1;
    xassert(kitty_image_store_add(&store, &a));

    struct kitty_image b = {0};
    b.id = KITTY_IMAGE_ID(2);
    b.size = 100;
    xassert(kitty_image_store_add(&store, &b));

    struct kitty_image c = {0};
    c.id = KITTY_IMAGE_ID(3);
    c.size = 100;
    xassert(kitty_image_store_add(&store, &c));

    xassert(store.bytes == 200);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(1)) != NULL);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(2)) == NULL);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(3)) != NULL);

    kitty_image_store_fini(&store);
}

UNITTEST
{
    /* An image larger than the quota fails and evicts nothing. */
    struct kitty_image_store store;
    kitty_image_store_init(&store);
    store.quota = 200;

    struct kitty_image a = {0};
    a.id = KITTY_IMAGE_ID(1);
    a.size = 100;
    xassert(kitty_image_store_add(&store, &a));

    struct kitty_image big = {0};
    big.id = KITTY_IMAGE_ID(2);
    big.size = 201;
    xassert(!kitty_image_store_add(&store, &big));

    xassert(store.bytes == 100);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(1)) != NULL);
    xassert(kitty_image_store_find_id(&store, KITTY_IMAGE_ID(2)) == NULL);

    /* The decode path rejects an over-quota image before allocating. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGBA;
    cmd.width = kitty_some_u32(4);
    cmd.height = kitty_some_u32(4);

    const char *error = NULL;
    xassert(!kitty_image_decode(&cmd, "AAAA", 4, 32, &img, &error));
    xassert(error != NULL);
    xassert(strcmp(error, "EINVAL:image too large") == 0);
    xassert(img.data == NULL);

    kitty_image_store_fini(&store);
}
