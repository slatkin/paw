#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pixman.h>
#include <tllist.h>

#include "kitty-graphics.h"

/* Per-screen budget for decoded pixel data (design D3). */
enum { KITTY_IMAGE_QUOTA = 320u * 1024 * 1024 };

/*
 * One decoded image. Pixel data is stored as premultiplied a8r8g8b8,
 * ready for direct compositing.
 */
struct kitty_image {
    uint32_t id;
    bool has_number;
    uint32_t number;

    uint64_t serial; /* Age, for eviction order */

    int width, height;    /* Pixels */
    uint32_t *data;       /* Premultiplied a8r8g8b8 */
    size_t size;          /* Bytes */
    pixman_image_t *pix;
};

struct kitty_image_store {
    /* Ordered by age: oldest first. */
    tll(struct kitty_image) images;

    uint32_t next_id;
    uint64_t next_serial;
    size_t bytes; /* Sum of image->size */
};

void kitty_image_store_init(struct kitty_image_store *store);
void kitty_image_store_fini(struct kitty_image_store *store);

struct kitty_image *kitty_image_store_find_id(
    struct kitty_image_store *store, uint32_t id);
struct kitty_image *kitty_image_store_find_number(
    struct kitty_image_store *store, uint32_t number);

/* Allocate an id that is not in use. */
uint32_t kitty_image_store_alloc_id(struct kitty_image_store *store);

/*
 * Insert a decoded image. Takes ownership of image->data and
 * image->pix. An image with the same id is replaced.
 */
void kitty_image_store_add(
    struct kitty_image_store *store, const struct kitty_image *image);

/* Free the pixel buffer and the pixman image. */
void kitty_image_destroy(struct kitty_image *image);

/*
 * Decode a transmit or query payload and convert it to premultiplied
 * a8r8g8b8. Handles base64, o=z inflate, RGB, RGBA and PNG.
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
