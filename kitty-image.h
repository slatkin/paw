#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pixman.h>
#include <tllist.h>

#include "kitty-graphics.h"

/* Per-screen budget for decoded pixel data. */
enum { KITTY_IMAGE_QUOTA = 320u * 1024 * 1024 };

/*
 * One decoded image. Pixel data is stored as premultiplied a8r8g8b8,
 * ready for direct compositing.
 */
struct kitty_image {
    uint32_t id;
    struct kitty_u32 number;

    int width, height;    /* Pixels */
    uint32_t *data;       /* Premultiplied a8r8g8b8 */
    size_t size;          /* Bytes */
    pixman_image_t *pix;

    /* Number of placements that refer to this image. Eviction
     * deletes an image without placements first. */
    unsigned int placements;
};

/* Called when an image is removed from a store (replaced or evicted). */
typedef void (*kitty_image_removed_cb)(void *user, uint32_t image_id);

struct kitty_image_store {
    /* Ordered by age: oldest first. */
    tll(struct kitty_image) images;

    uint32_t next_id;
    size_t bytes; /* Sum of image->size */

    /*
     * Per-screen budget.
     *
     * Defaults to KITTY_IMAGE_QUOTA. Unit tests lower it to exercise
     * the eviction order without large allocations.
     */
    size_t quota;

    /*
     * Placement cleanup. The image store does not know about
     * placements, so it calls back into the owner (kitty-graphics.c)
     * whenever an image is removed. Without this, placements would
     * keep a dangling 'image' pointer.
     */
    kitty_image_removed_cb on_remove;
    void *on_remove_user;
};

void kitty_image_store_init(struct kitty_image_store *store);
void kitty_image_store_fini(struct kitty_image_store *store);

struct kitty_image_store *kitty_image_store_for(struct terminal *term);

struct kitty_image *kitty_image_store_find_id(
    struct kitty_image_store *store, uint32_t id);
struct kitty_image *kitty_image_store_find_number(
    struct kitty_image_store *store, uint32_t number);

/* Allocate an id that is not in use. */
uint32_t kitty_image_store_alloc_id(struct kitty_image_store *store);

/*
 * Remove the image with the given id, if present. Calls the on_remove
 * callback first, so placements that refer to it are dropped. Returns
 * false when no such image exists.
 */
bool kitty_image_store_delete_id(struct kitty_image_store *store, uint32_t id);

/* Remove every image that has no placement left. */
void kitty_image_store_delete_unreferenced(struct kitty_image_store *store);

/*
 * Insert a decoded image. Takes ownership of image->data and
 * image->pix. An image with the same id is replaced.
 *
 * Deletes old images, oldest first, until the new image fits in the
 * store's quota. Images without placements are deleted before images
 * with placements.
 *
 * Returns false, and changes nothing, if the image itself is larger
 * than the quota.
 */
bool kitty_image_store_add(
    struct kitty_image_store *store, const struct kitty_image *image);

/* Free the pixel buffer and the pixman image. */
void kitty_image_destroy(struct kitty_image *image);

/*
 * Decode a base64 transmit or query payload and convert it to
 * premultiplied a8r8g8b8. Handles base64, o=z inflate, RGB, RGBA and
 * PNG.
 *
 * On success 'out' owns data and pix. On failure returns false and
 * sets *error to a printable error code and text.
 *
 * 'max_bytes' is the decoded-pixel budget: an image whose RGBA form
 * exceeds it is rejected before the pixel buffer is allocated.
 */
bool kitty_image_decode(
    const struct kitty_command *cmd,
    const char *payload, size_t payload_len,
    size_t max_bytes,
    struct kitty_image *out, const char **error);

/*
 * As kitty_image_decode(), but for a raw (already base64-decoded)
 * payload, for example the bytes read from a file or shared memory
 * object. Does not take ownership of 'data'.
 */
bool kitty_image_decode_raw(
    const struct kitty_command *cmd,
    const uint8_t *data, size_t len,
    size_t max_bytes,
    struct kitty_image *out, const char **error);

/*
 * Wrap-safe check that 'width * height * bpp' fits in size_t and does
 * not exceed 'max_bytes'. On success stores the product in '*out'.
 */
bool kitty_checked_size(
    uint32_t width, uint32_t height, uint32_t bpp, size_t max_bytes,
    size_t *out);

/*
 * Decode base64 text, padded or not. Returns NULL on error. The caller
 * frees the result.
 */
char *kitty_base64_decode(const char *s, size_t *out_len);
