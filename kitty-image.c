#include "kitty-image.h"

#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <zlib.h>

#define LOG_MODULE "kitty-image"
#define LOG_ENABLE_DBG 0
#include "base64.h"
#include "debug.h"
#include "xmalloc.h"

enum { KITTY_INFLATE_CHUNK = 64 * 1024 };

static inline uint32_t
premultiply(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    /* pixman a8r8g8b8: A in bits 24-31, R 16-23, G 8-15, B 0-7. */
    const uint32_t pr = ((uint32_t)r * a + 127) / 255;
    const uint32_t pg = ((uint32_t)g * a + 127) / 255;
    const uint32_t pb = ((uint32_t)b * a + 127) / 255;
    return ((uint32_t)a << 24) | (pr << 16) | (pg << 8) | pb;
}

void
kitty_image_store_init(struct kitty_image_store *store)
{
    memset(store, 0, sizeof(*store));
    store->next_id = 1;
    store->next_serial = 1;
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
kitty_image_store_find_id(struct kitty_image_store *store, uint32_t id)
{
    tll_foreach(store->images, it) {
        if (it->item.id == id)
            return &it->item;
    }
    return NULL;
}

struct kitty_image *
kitty_image_store_find_number(struct kitty_image_store *store, uint32_t number)
{
    /* The newest image with this number wins. */
    tll_rforeach(store->images, it) {
        if (it->item.has_number && it->item.number == number)
            return &it->item;
    }
    return NULL;
}

uint32_t
kitty_image_store_alloc_id(struct kitty_image_store *store)
{
    for (;;) {
        const uint32_t id = store->next_id++;
        if (store->next_id == 0)
            store->next_id = 1;
        if (id != 0 && kitty_image_store_find_id(store, id) == NULL)
            return id;
    }
}

void
kitty_image_store_add(
    struct kitty_image_store *store, const struct kitty_image *image)
{
    /* An image with the same id is replaced. */
    tll_foreach(store->images, it) {
        if (it->item.id != image->id)
            continue;

        store->bytes -= it->item.size;
        kitty_image_destroy(&it->item);
        tll_remove(store->images, it);
        break;
    }

    struct kitty_image copy = *image;
    copy.serial = store->next_serial++;

    tll_push_back(store->images, copy);
    store->bytes += copy.size;
}

/*
 * Inflate a zlib (RFC 1950) stream. The output grows on demand and is
 * capped at 'cap'. Returns NULL on failure.
 */
static uint8_t *
inflate_zlib(
    const uint8_t *in, size_t in_len, size_t cap,
    size_t *out_len, const char **error)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    if (inflateInit(&zs) != Z_OK) {
        *error = "EINVAL:failed to initialize decompressor";
        return NULL;
    }

    size_t size = cap < KITTY_INFLATE_CHUNK ? cap : KITTY_INFLATE_CHUNK;
    if (size == 0)
        size = 1;

    uint8_t *out = xmalloc(size);

    zs.next_in = (Bytef *)in;
    zs.avail_in = in_len;
    zs.next_out = out;
    zs.avail_out = size;

    for (;;) {
        const int ret = inflate(&zs, Z_NO_FLUSH);

        if (ret == Z_STREAM_END) {
            *out_len = size - zs.avail_out;
            inflateEnd(&zs);
            return out;
        }

        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            free(out);
            inflateEnd(&zs);
            *error = "EINVAL:corrupt compressed data";
            return NULL;
        }

        if (zs.avail_out > 0) {
            /* No output space used up: the stream is truncated. */
            free(out);
            inflateEnd(&zs);
            *error = "EINVAL:corrupt compressed data";
            return NULL;
        }

        if (size >= cap) {
            free(out);
            inflateEnd(&zs);
            *error = "EINVAL:compressed data too large";
            return NULL;
        }

        size_t new_size = size * 2;
        if (new_size > cap)
            new_size = cap;

        out = xrealloc(out, new_size);
        zs.next_out = out + size;
        zs.avail_out = new_size - size;
        size = new_size;
    }
}

static bool
decode_png(
    const uint8_t *data, size_t len, size_t max_bytes,
    struct kitty_image *out, const char **error)
{
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, len)) {
        *error = "EINVAL:invalid PNG";
        return false;
    }

    const int width = image.width;
    const int height = image.height;

    if (width == 0 || height == 0 ||
        (uint64_t)width * height * 4 > max_bytes)
    {
        png_image_free(&image);
        *error = "EINVAL:image too large";
        return false;
    }

    image.format = PNG_FORMAT_RGBA;

    /* Count is bounded by the quota check above. */
    uint32_t *pixels = xmalloc((size_t)width * height * 4);

    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
        free(pixels);
        png_image_free(&image);
        *error = "EINVAL:invalid PNG";
        return false;
    }

    png_image_free(&image);

    const size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *px = (const uint8_t *)&pixels[i];
        pixels[i] = premultiply(px[0], px[1], px[2], px[3]);
    }

    out->width = width;
    out->height = height;
    out->data = pixels;
    return true;
}

bool
kitty_image_decode(
    const struct kitty_command *cmd,
    const char *payload, size_t payload_len,
    size_t max_bytes,
    struct kitty_image *out, const char **error)
{
    memset(out, 0, sizeof(*out));

    const bool png = cmd->format == KITTY_FORMAT_PNG;
    const size_t bpp =
        cmd->format == KITTY_FORMAT_RGB ? 3 :
        cmd->format == KITTY_FORMAT_RGBA ? 4 : 0;

    size_t raw_expected = 0;
    size_t inflate_cap = max_bytes;

    if (!png) {
        if (!cmd->has_width || !cmd->has_height) {
            *error = "EINVAL:missing dimensions";
            return false;
        }
        if (cmd->width == 0 || cmd->height == 0) {
            *error = "EINVAL:invalid dimensions";
            return false;
        }

        /* The stored form is always RGBA. */
        if ((uint64_t)cmd->width * cmd->height * 4 > max_bytes) {
            *error = "EINVAL:image too large";
            return false;
        }

        raw_expected = (size_t)cmd->width * cmd->height * bpp;
        inflate_cap = raw_expected;
    }

    size_t decoded_len = 0;
    char *decoded = base64_decode(payload, &decoded_len);
    if (decoded == NULL) {
        *error = "EINVAL:invalid base64";
        return false;
    }

    uint8_t *raw = (uint8_t *)decoded;
    size_t raw_len = decoded_len;

    if (cmd->compression == KITTY_COMPRESSION_ZLIB) {
        size_t inflated_len = 0;
        uint8_t *inflated = inflate_zlib(
            raw, raw_len, inflate_cap, &inflated_len, error);
        free(decoded);
        if (inflated == NULL)
            return false;
        raw = inflated;
        raw_len = inflated_len;
    }

    bool ok = false;

    if (png) {
        ok = decode_png(raw, raw_len, max_bytes, out, error);
    } else if (raw_len != raw_expected) {
        *error = "EINVAL:wrong data length";
    } else if (cmd->format == KITTY_FORMAT_RGB) {
        const size_t count = (size_t)cmd->width * cmd->height;
        out->data = xmalloc(count * 4);
        for (size_t i = 0; i < count; i++) {
            out->data[i] = 0xff000000u |
                ((uint32_t)raw[3 * i + 0] << 16) |
                ((uint32_t)raw[3 * i + 1] << 8) |
                ((uint32_t)raw[3 * i + 2]);
        }
        out->width = cmd->width;
        out->height = cmd->height;
        ok = true;
    } else {
        const size_t count = (size_t)cmd->width * cmd->height;
        out->data = xmalloc(count * 4);
        for (size_t i = 0; i < count; i++) {
            out->data[i] = premultiply(
                raw[4 * i + 0], raw[4 * i + 1],
                raw[4 * i + 2], raw[4 * i + 3]);
        }
        out->width = cmd->width;
        out->height = cmd->height;
        ok = true;
    }

    free(raw);

    if (!ok) {
        free(out->data);
        out->data = NULL;
        return false;
    }

    out->size = (size_t)out->width * out->height * 4;
    out->pix = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, out->width, out->height, out->data,
        out->width * 4);

    if (out->pix == NULL) {
        free(out->data);
        out->data = NULL;
        *error = "EINVAL:failed to create image";
        return false;
    }

    return true;
}

UNITTEST
{
    /* f=24: 2x1 RGB, both pixels opaque. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGB;
    cmd.has_width = cmd.has_height = true;
    cmd.width = 2;
    cmd.height = 1;

    static const char payload[] = "/wAAAID/";
    const char *error = NULL;
    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(error == NULL);
    xassert(img.width == 2 && img.height == 1);
    xassert(img.size == 2 * 4);
    xassert(img.data[0] == 0xffff0000u);
    xassert(img.data[1] == 0xff0080ffu);
    kitty_image_destroy(&img);

    /* Wrong data length: 2x1 RGB needs 6 bytes, not 3. */
    static const char short_payload[] = "/wAA";
    xassert(!kitty_image_decode(
                &cmd, short_payload, sizeof(short_payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(error != NULL);
    xassert(img.data == NULL);
}

UNITTEST
{
    /* f=32: 2x1 RGBA, the second pixel is half transparent. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGBA;
    cmd.has_width = cmd.has_height = true;
    cmd.width = 2;
    cmd.height = 1;

    static const char payload[] = "ChQe/8hkMoA=";
    const char *error = NULL;
    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(img.data[0] == 0xff0a141eu);
    xassert(img.data[1] == 0x80643219u);
    kitty_image_destroy(&img);
}

UNITTEST
{
    /* An image with an id replaces the old image with that id. */
    struct kitty_image_store store;
    kitty_image_store_init(&store);

    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGB;
    cmd.has_width = cmd.has_height = true;
    cmd.width = 2;
    cmd.height = 1;

    static const char payload[] = "/wAAAID/";
    const char *error = NULL;

    struct kitty_image img;
    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    img.id = 5;
    kitty_image_store_add(&store, &img);
    xassert(tll_length(store.images) == 1);
    xassert(store.bytes == img.size);

    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    img.id = 5;
    kitty_image_store_add(&store, &img);
    xassert(tll_length(store.images) == 1);
    xassert(store.bytes == img.size);
    xassert(kitty_image_store_find_id(&store, 5) != NULL);

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
    cmd.has_width = cmd.has_height = true;
    cmd.width = 1;
    cmd.height = 1;

    static const char payload[] = "/wAA";
    const char *error = NULL;

    struct kitty_image img;
    for (uint32_t i = 0; i < 2; i++) {
        xassert(kitty_image_decode(
                    &cmd, payload, sizeof(payload) - 1,
                    KITTY_IMAGE_QUOTA, &img, &error));
        img.id = kitty_image_store_alloc_id(&store);
        img.has_number = true;
        img.number = 13;
        kitty_image_store_add(&store, &img);
    }

    xassert(tll_length(store.images) == 2);
    struct kitty_image *newest = kitty_image_store_find_number(&store, 13);
    xassert(newest != NULL);
    xassert(newest == &tll_back(store.images));

    kitty_image_store_fini(&store);
}

UNITTEST
{
    /* PNG: a 2x2 RGBA image with alpha. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_PNG;

    static const char payload[] =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAGUlEQVR4nAXB"
        "AQEAAAjDIKI9ms0niOTQKg9AGAe6dfu8vgAAAABJRU5ErkJggg==";
    const char *error = NULL;
    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(img.width == 2 && img.height == 2);
    xassert(img.data[0] == 0xffff0000u);
    xassert(img.data[1] == 0x80008000u);
    xassert(img.data[2] == 0x40000040u);
    xassert(img.data[3] == 0x00000000u);
    kitty_image_destroy(&img);

    /* A truncated PNG fails. */
    static const char truncated[] =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAGUlEQQ==";
    error = NULL;
    xassert(!kitty_image_decode(
                &cmd, truncated, sizeof(truncated) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(error != NULL);
    xassert(img.data == NULL);

    /* A PNG that claims 100000x100000 fails before allocation. */
    static const char huge[] =
        "iVBORw0KGgoAAAANSUhEUgABhqAAAYagCAYAAACoUgvIAAAACElEQVR4nAMAAAAA"
        "AUgGidIAAAAASUVORK5CYII=";
    error = NULL;
    xassert(!kitty_image_decode(
                &cmd, huge, sizeof(huge) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(error != NULL);
    xassert(strcmp(error, "EINVAL:image too large") == 0);
    xassert(img.data == NULL);
}

UNITTEST
{
    /* o=z: a zlib stream that inflates to a 4x4 RGBA image. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGBA;
    cmd.compression = KITTY_COMPRESSION_ZLIB;
    cmd.has_width = cmd.has_height = true;
    cmd.width = 4;
    cmd.height = 4;

    static const char payload[] =
        "eNoBQAC//wAHDhUcIyoxOD9GTVRbYmlwd36FjJOaoaivtr3Ey9LZ4Ofu9fwDChEY"
        "HyYtNDtCSVBXXmVsc3qBiI+WnaSrsrkwzRwh";
    const char *error = NULL;
    xassert(kitty_image_decode(
                &cmd, payload, sizeof(payload) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(img.width == 4 && img.height == 4);
    xassert(img.size == 4 * 4 * 4);
    /* Pixel 0 is raw (0*7, 1*7, 2*7, 3*7) = (0,7,14,21). */
    xassert(img.data[0] == premultiply(0, 7, 14, 21));
    kitty_image_destroy(&img);

    /* Corrupt zlib data fails. */
    static const char corrupt[] = "eNoBQAC//w==";
    error = NULL;
    xassert(!kitty_image_decode(
                &cmd, corrupt, sizeof(corrupt) - 1,
                KITTY_IMAGE_QUOTA, &img, &error));
    xassert(error != NULL);
    xassert(img.data == NULL);
}
