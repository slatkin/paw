#include "kitty-image.h"

#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <zlib.h>

#define LOG_MODULE "kitty-image"
#define LOG_ENABLE_DBG 0
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
        *error = "EBADPNG:invalid PNG";
        return false;
    }

    /* Wrap-safe: image.width/height can be up to 2^32-1 on hostile
     * input, and width * height * 4 can overflow size_t. */
    size_t rgba_size = 0;
    if (!kitty_checked_size(
            image.width, image.height, 4, max_bytes, &rgba_size))
    {
        png_image_free(&image);
        *error = "EINVAL:image too large";
        return false;
    }

    const int width = (int)image.width;
    const int height = (int)image.height;
    const size_t count = rgba_size / 4;

    image.format = PNG_FORMAT_RGBA;

    /* For 8-bit RGBA the row stride is width * 4, so rgba_size is
     * exactly the buffer size. */
    uint32_t *pixels = xmalloc(rgba_size);

    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
        free(pixels);
        png_image_free(&image);
        *error = "EBADPNG:invalid PNG";
        return false;
    }

    png_image_free(&image);

    for (size_t i = 0; i < count; i++) {
        const uint8_t *px = (const uint8_t *)&pixels[i];
        pixels[i] = premultiply(px[0], px[1], px[2], px[3]);
    }

    out->width = width;
    out->height = height;
    out->size = rgba_size;
    out->data = pixels;
    return true;
}

bool
kitty_image_decode_raw(
    const struct kitty_command *cmd,
    const uint8_t *data, size_t len,
    size_t max_bytes,
    struct kitty_image *out, const char **error)
{
    memset(out, 0, sizeof(*out));

    const bool png = cmd->format == KITTY_FORMAT_PNG;
    const size_t bpp =
        cmd->format == KITTY_FORMAT_RGB ? 3 :
        cmd->format == KITTY_FORMAT_RGBA ? 4 : 0;

    size_t raw_expected = 0;
    size_t rgba_size = 0;
    size_t inflate_cap = max_bytes;

    if (!png) {
        if (!cmd->width.set || !cmd->height.set) {
            *error = "EINVAL:missing dimensions";
            return false;
        }
        if (cmd->width.value == 0 || cmd->height.value == 0) {
            *error = "EINVAL:invalid dimensions";
            return false;
        }

        /* The stored form is always RGBA. kitty_checked_size() is
         * wrap-safe. */
        if (!kitty_checked_size(
                cmd->width.value, cmd->height.value, 4, max_bytes, &rgba_size))
        {
            *error = "EINVAL:image too large";
            return false;
        }

        /* The raw size (3 or 4 bytes per pixel) is at most the RGBA
         * size, but check it too so that every product is wrap-safe. */
        if (!kitty_checked_size(
                cmd->width.value, cmd->height.value, (uint32_t)bpp,
                SIZE_MAX, &raw_expected))
        {
            *error = "EINVAL:image too large";
            return false;
        }

        inflate_cap = raw_expected;
    }

    const uint8_t *raw = data;
    size_t raw_len = len;
    uint8_t *inflated = NULL;

    if (cmd->compression == KITTY_COMPRESSION_ZLIB) {
        size_t inflated_len = 0;
        inflated = inflate_zlib(data, len, inflate_cap, &inflated_len, error);
        if (inflated == NULL)
            return false;
        raw = inflated;
        raw_len = inflated_len;
    }

    bool ok = false;

    if (png) {
        ok = decode_png(raw, raw_len, max_bytes, out, error);
    } else if (raw_len != raw_expected) {
        *error = "ENODATA:wrong data length";
    } else {
        const size_t count = rgba_size / 4;
        out->data = xmalloc(rgba_size);
        for (size_t i = 0; i < count; i++) {
            /* RGB has no alpha. With a = 0xff, premultiply() keeps the
             * color unchanged. */
            const uint8_t *px = &raw[bpp * i];
            out->data[i] = premultiply(
                px[0], px[1], px[2], bpp == 4 ? px[3] : 0xff);
        }
        out->width = cmd->width.value;
        out->height = cmd->height.value;
        out->size = rgba_size;
        ok = true;
    }

    free(inflated);

    if (!ok) {
        free(out->data);
        out->data = NULL;
        return false;
    }

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

bool
kitty_image_decode(
    const struct kitty_command *cmd,
    const char *payload, size_t payload_len,
    size_t max_bytes,
    struct kitty_image *out, const char **error)
{
    memset(out, 0, sizeof(*out));

    size_t decoded_len = 0;
    char *decoded = kitty_base64_decode(payload, &decoded_len);
    if (decoded == NULL) {
        *error = "EINVAL:invalid base64";
        return false;
    }

    const bool ok = kitty_image_decode_raw(
        cmd, (const uint8_t *)decoded, decoded_len, max_bytes, out, error);
    free(decoded);
    return ok;
}

UNITTEST
{
    /*
     * 2^31 x 2^31 must fail: width * height * 4 wraps to 0 in 64-bit
     * arithmetic. The guard must reject it before any allocation.
     */
    for (int pass = 0; pass < 2; pass++) {
        struct kitty_image img;
        struct kitty_command cmd;
        kitty_command_init(&cmd);
        cmd.format = pass == 0 ? KITTY_FORMAT_RGBA : KITTY_FORMAT_RGB;
        cmd.width = kitty_some_u32(2147483648u);
        cmd.height = kitty_some_u32(2147483648u);

        const char *error = NULL;
        xassert(!kitty_image_decode(
                    &cmd, "", 0, KITTY_IMAGE_QUOTA, &img, &error));
        xassert(error != NULL);
        xassert(img.data == NULL);
    }
}

UNITTEST
{
    /* f=24: 2x1 RGB, both pixels opaque. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGB;
    cmd.width = kitty_some_u32(2);
    cmd.height = kitty_some_u32(1);

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
    xassert(strcmp(error, "ENODATA:wrong data length") == 0);
    xassert(img.data == NULL);
}

UNITTEST
{
    /* f=32: 2x1 RGBA, the second pixel is half transparent. */
    struct kitty_image img;
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.format = KITTY_FORMAT_RGBA;
    cmd.width = kitty_some_u32(2);
    cmd.height = kitty_some_u32(1);

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
    xassert(strcmp(error, "EBADPNG:invalid PNG") == 0);
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
    cmd.width = kitty_some_u32(4);
    cmd.height = kitty_some_u32(4);

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

