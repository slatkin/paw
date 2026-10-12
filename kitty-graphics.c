#include "kitty-graphics.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "kitty-image.h"
#include "kitty-file.h"
#include "kitty-placement.h"

#include "grid.h"

#define LOG_MODULE "kitty-graphics"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "vt.h"
#include "xmalloc.h"
#include "xsnprintf.h"
#include "kitty-graphics-internal.h"

/*
 * A single non-chunked command can carry a full image. Cap the APC
 * buffer at the image quota so untrusted input cannot grow it without
 * bound. The quota is 320 MB; base64 inflates by 4/3.
 */
enum { KITTY_APC_MAX = 320u * 1024 * 1024 };

static int
kitty_screen_idx(struct terminal *term)
{
    /*
     * The main and alternate screens have separate stores. The grid
     * addresses are stable: the interactive-resize code replaces the
     * contents of term->normal, not its address.
     */
    return term->grid == &term->alt ? 1 : 0;
}

struct kitty_image_store *
kitty_image_store_for(struct terminal *term)
{
    return &term->kitty->screens[kitty_screen_idx(term)].image_store;
}

struct kitty_placement_store *
kitty_placement_store_for(struct terminal *term)
{
    return &term->kitty->screens[kitty_screen_idx(term)].placement_store;
}

void
kitty_command_init(struct kitty_command *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->action = KITTY_ACTION_TRANSMIT;
    cmd->format = KITTY_FORMAT_RGBA;
    cmd->medium = KITTY_MEDIUM_DIRECT;
    cmd->compression = KITTY_COMPRESSION_NONE;
}

void
kitty_screen_init(struct kitty_graphics *kitty, int idx)
{
    kitty_image_store_init(&kitty->screens[idx].image_store);
    kitty_placement_store_init(&kitty->screens[idx].placement_store);

    /* Keep placements in sync when an image is replaced or evicted. */
    kitty->screens[idx].image_store.on_remove =
        kitty_placement_on_image_removed;
    kitty->screens[idx].image_store.on_remove_user =
        &kitty->screens[idx].placement_store;
}

void
kitty_screen_fini(struct kitty_graphics *kitty, int idx)
{
    kitty_placement_store_fini(&kitty->screens[idx].placement_store);
    kitty_image_store_fini(&kitty->screens[idx].image_store);
}

void
kitty_init(struct terminal *term)
{
    xassert(term->kitty == NULL);
    term->kitty = xcalloc(1, sizeof(struct kitty_graphics));

    for (int i = 0; i < 2; i++)
        kitty_screen_init(term->kitty, i);
}

void
kitty_fini(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    for (int i = 0; i < 2; i++)
        kitty_screen_fini(term->kitty, i);

    free(term->kitty->apc.data);
    free(term->kitty->chunk.data);
    free(term->kitty);
    term->kitty = NULL;
}

void
kitty_chunk_reset(struct kitty_graphics *kitty)
{
    /* Free the payload, so that a finished transfer does not keep its
     * peak size for the life of the window. */
    free(kitty->chunk.data);
    kitty->chunk.data = NULL;
    kitty->chunk.size = 0;
    kitty->chunk.active = false;
    kitty->chunk.len = 0;
}

/*
 * Append payload bytes to the in-progress chunked transfer. Returns
 * false, and cancels the transfer, if the accumulated base64 payload
 * would exceed the APC limit.
 */
static bool
kitty_chunk_append(struct kitty_graphics *kitty, const char *data, size_t len)
{
    /* Reserve room for the NUL terminator that base64_decode needs. */
    if (len > KITTY_APC_MAX - 1 - kitty->chunk.len)
        return false;

    const size_t needed = kitty->chunk.len + len + 1;
    if (needed > kitty->chunk.size) {
        size_t new_size = kitty->chunk.size == 0 ? 1024 : kitty->chunk.size;
        while (new_size < needed) {
            if (new_size > (KITTY_APC_MAX - 1) / 2) {
                new_size = KITTY_APC_MAX - 1;
                break;
            }
            new_size *= 2;
        }
        if (new_size < needed)
            new_size = needed;
        kitty->chunk.data = xrealloc(kitty->chunk.data, new_size);
        kitty->chunk.size = new_size;
    }

    memcpy(kitty->chunk.data + kitty->chunk.len, data, len);
    kitty->chunk.len += len;
    kitty->chunk.data[kitty->chunk.len] = '\0';
    return true;
}


/* The id to name in a reply: the one the command gave, or 0. */
static struct kitty_image_id
reply_id(const struct kitty_command *cmd)
{
    return KITTY_IMAGE_ID(kitty_or(cmd->id, 0));
}

/* The id the command gives, or a new one. */
static struct kitty_image_id
command_image_id(
    const struct kitty_command *cmd, struct kitty_image_store *store)
{
    return cmd->id.set
        ? KITTY_IMAGE_ID(cmd->id.value)
        : kitty_image_store_alloc_id(store);
}

static size_t
kitty_format_reply(char *buf, size_t size, const struct kitty_command *cmd,
                   struct kitty_image_id id, const char *msg)
{
    size_t n = xsnprintf(buf, size, "\x1b_Gi=%u", id.v);
    if (cmd->number.set)
        n += xsnprintf(buf + n, size - n, ",I=%u", cmd->number.value);
    if (cmd->placement_id.set)
        n += xsnprintf(buf + n, size - n, ",p=%u", cmd->placement_id.value);
    n += xsnprintf(buf + n, size - n, ";%s\x1b\\", msg);
    return n;
}

static void
kitty_reply(struct terminal *term, const struct kitty_command *cmd,
            struct kitty_image_id id, bool ok, const char *msg)
{
    /* Without i or I, no reply is sent. */
    if (!cmd->id.set && !cmd->number.set)
        return;

    const uint32_t quiet = kitty_or(cmd->quiet, 0);

    /* q=2 suppresses every reply, q=1 suppresses OK replies. */
    if (quiet >= 2)
        return;
    if (ok && quiet >= 1)
        return;

    char buf[512];
    const size_t n = kitty_format_reply(buf, sizeof(buf), cmd, id, msg);
    term_to_slave(term, buf, n);
}

/*
 * Load the image data of a transmit or query command. Direct data is
 * base64 in the escape code. File, temp file and shared memory read
 * the bytes from the named object.
 */
static bool
kitty_load(
    const struct kitty_command *cmd, const char *payload, size_t payload_len,
    size_t quota, struct kitty_image *image, const char **error)
{
    if (cmd->medium == KITTY_MEDIUM_DIRECT) {
        return kitty_image_decode(
            cmd, payload, payload_len, quota, image, error);
    }

    uint8_t *data = NULL;
    size_t len = 0;
    bool ok = kitty_file_load(
        cmd, payload, payload_len, quota, &data, &len, error);
    if (ok)
        ok = kitty_image_decode_raw(cmd, data, len, quota, image, error);
    free(data);
    return ok;
}

/* Store a loaded image under 'id', place it for a=T, and reply. */
static void
kitty_store_and_reply(
    struct terminal *term, const struct kitty_command *cmd,
    struct kitty_image_id id, struct kitty_image *image)
{
    struct kitty_image_store *store = kitty_image_store_for(term);

    image->id = id;
    image->number = cmd->number;
    if (!kitty_image_store_add(store, image)) {
        kitty_image_destroy(image);
        kitty_reply(term, cmd, id, false, "EINVAL:image too large");
        return;
    }

    /* a=T transmits and places in one command. */
    if (cmd->action == KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
        struct kitty_image *stored = kitty_image_store_find_id(store, id);
        const char *error = stored != NULL
            ? kitty_placement_put(term, cmd, stored)
            : "ENOENT:no such image";
        if (error != NULL) {
            kitty_reply(term, cmd, id, false, error);
            return;
        }
    }

    kitty_reply(term, cmd, id, true, "OK");
}

static void
kitty_transmit(
    struct terminal *term, const struct kitty_command *cmd,
    const char *payload, size_t payload_len)
{
    struct kitty_image_store *store = kitty_image_store_for(term);
    const struct kitty_image_id id = command_image_id(cmd, store);

    struct kitty_image image;
    const char *error = NULL;
    if (!kitty_load(
            cmd, payload, payload_len, store->quota, &image, &error))
    {
        kitty_reply(term, cmd, id, false, error);
        return;
    }

    kitty_store_and_reply(term, cmd, id, &image);
}

static void
kitty_handle_transmit(struct terminal *term, const struct kitty_command *cmd,
                      const char *payload, size_t payload_len)
{
    struct kitty_graphics *kitty = term->kitty;

    /*
     * Chunked transfer. A transmit command that arrives while a
     * transfer is in progress is a continuation of it; its control
     * data (apart from m) is ignored. In particular, the medium of
     * the first chunk applies to the whole transfer, so a continuation
     * is never rejected for a non-direct medium.
     */
    if (kitty->chunk.active) {
        if (!kitty_chunk_append(kitty, payload, payload_len)) {
            kitty_chunk_reset(kitty);
            kitty_reply(term, cmd, reply_id(cmd), false,
                        "EINVAL:image too large");
            return;
        }

        /* m=0, or no m key, ends the transfer. */
        if (kitty_or(cmd->more, 0) != 0)
            return;

        const struct kitty_command first = kitty->chunk.cmd;
        const struct kitty_image_id id =
            command_image_id(&first, kitty_image_store_for(term));

        struct kitty_image image;
        const char *error = NULL;
        const bool ok = kitty_load(
            &first, kitty->chunk.data, kitty->chunk.len,
            kitty_image_store_for(term)->quota, &image, &error);

        kitty_chunk_reset(kitty);

        if (!ok) {
            kitty_reply(term, &first, id, false, error);
            return;
        }

        kitty_store_and_reply(term, &first, id, &image);
        return;
    }

    /*
     * File, temp file and shared memory carry their own data, so a
     * chunked transfer never starts for them.
     */
    if (cmd->medium != KITTY_MEDIUM_DIRECT) {
        kitty_transmit(term, cmd, payload, payload_len);
        return;
    }

    /* m=1 starts a transfer. Save the control data for the reply. */
    if (kitty_or(cmd->more, 0) != 0) {
        kitty->chunk.active = true;
        kitty->chunk.cmd = *cmd;
        if (!kitty_chunk_append(kitty, payload, payload_len)) {
            kitty_chunk_reset(kitty);
            kitty_reply(term, cmd, reply_id(cmd), false,
                        "EINVAL:image too large");
        }
        return;
    }

    kitty_transmit(term, cmd, payload, payload_len);
}

static void
kitty_handle_query(struct terminal *term, const struct kitty_command *cmd,
                   const char *payload, size_t payload_len)
{
    struct kitty_image_store *store = kitty_image_store_for(term);

    /* An I-only query still names the id paw would have assigned. */
    struct kitty_image_id id = {0};
    if (cmd->id.set)
        id = KITTY_IMAGE_ID(cmd->id.value);
    else if (cmd->number.set)
        id = kitty_image_store_alloc_id(store);

    /* Load and check the image data as a transmit would, without
     * storing it. */
    struct kitty_image image;
    const char *error = NULL;
    const bool ok = kitty_load(
        cmd, payload, payload_len, store->quota, &image, &error);

    if (ok)
        kitty_image_destroy(&image);

    kitty_reply(term, cmd, id, ok, ok ? "OK" : error);
}

static void
kitty_handle_put(struct terminal *term, const struct kitty_command *cmd)
{
    struct kitty_image_store *store = kitty_image_store_for(term);

    struct kitty_image *image = NULL;
    struct kitty_image_id id = {0};

    if (cmd->id.set) {
        id = KITTY_IMAGE_ID(cmd->id.value);
        image = kitty_image_store_find_id(store, id);
    } else if (cmd->number.set) {
        image = kitty_image_store_find_number(
            store, KITTY_IMAGE_NUMBER(cmd->number.value));
        if (image != NULL)
            id = image->id;
    }

    if (image == NULL) {
        /* Without i and I there is no reply anyway. */
        kitty_reply(term, cmd, id, false, "ENOENT:no such image");
        return;
    }

    const char *error = kitty_placement_put(term, cmd, image);
    kitty_reply(term, cmd, id, error == NULL, error == NULL ? "OK" : error);
}

static void
kitty_handle_apc(struct terminal *term, const char *data, size_t len)
{
    struct kitty_graphics *kitty = term->kitty;
    struct kitty_command cmd;
    kitty_command_init(&cmd);

    const char *payload;
    size_t payload_len;
    if (!kitty_parse_apc(data, len, &cmd, &payload, &payload_len)) {
        /* A malformed command aborts any in-progress transfer. */
        kitty_chunk_reset(kitty);
        kitty_reply(
            term, &cmd, reply_id(&cmd), false,
            "EINVAL:invalid control data");
        return;
    }

    if (cmd.id.set && cmd.number.set) {
        /* A malformed command aborts any in-progress transfer. */
        kitty_chunk_reset(kitty);
        kitty_reply(
            term, &cmd, reply_id(&cmd), false, "EINVAL:both i and I given");
        return;
    }

    /* A delete command cancels an in-progress chunked transfer, then
     * runs as usual. */
    if (kitty->chunk.active && cmd.action == KITTY_ACTION_DELETE)
        kitty_chunk_reset(kitty);

    /*
     * Relative placements are not supported. This is a rejection:
     * it changes no state, so an in-progress transfer is left alone.
     */
    if (cmd.parent_id.set || cmd.parent_placement.set) {
        kitty_reply(
            term, &cmd, reply_id(&cmd), false,
            "EINVAL:relative placement not supported");
        return;
    }

    switch (cmd.action) {
    case KITTY_ACTION_TRANSMIT:
    case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
        kitty_handle_transmit(term, &cmd, payload, payload_len);
        return;

    case KITTY_ACTION_QUERY:
        kitty_handle_query(term, &cmd, payload, payload_len);
        return;

    case KITTY_ACTION_PUT:
        kitty_handle_put(term, &cmd);
        return;

    case KITTY_ACTION_DELETE:
        /* Delete sends no reply. */
        kitty_placement_delete(term, &cmd);
        return;

    case KITTY_ACTION_FRAME:
    case KITTY_ACTION_ANIMATE:
    case KITTY_ACTION_COMPOSE:
    default:
        /* Animation is not supported, and neither is an unknown action is
         * rejected. */
        kitty_reply(
            term, &cmd, reply_id(&cmd), false,
            "EINVAL:unsupported action");
        return;
    }
}

void
kitty_apc_start(struct terminal *term)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL)
        return;

    kitty->apc.len = 0;
    kitty->apc.overflow = false;
}

void
kitty_apc_put(struct terminal *term, uint8_t c)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL)
        return;

    /* Leave room for the NUL terminator used by the parser. */
    if (kitty->apc.len + 1 >= KITTY_APC_MAX) {
        kitty->apc.overflow = true;
        return;
    }

    if (kitty->apc.len + 1 >= kitty->apc.size) {
        size_t new_size =
            kitty->apc.size == 0 ? 1024 : kitty->apc.size * 2;
        if (new_size > KITTY_APC_MAX)
            new_size = KITTY_APC_MAX;
        kitty->apc.data = xrealloc(kitty->apc.data, new_size);
        kitty->apc.size = new_size;
    }

    kitty->apc.data[kitty->apc.len++] = (char)c;
}

void
kitty_apc_end(struct terminal *term)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL)
        return;

    const size_t len = kitty->apc.len;
    if (kitty->apc.data == NULL || len == 0)
        return;

    /* Over-long commands are dropped without a reply. */
    if (kitty->apc.overflow)
        return;

    /* Null-terminate without dropping the recorded bytes. */
    kitty->apc.data[len] = '\0';

    /* Only graphics commands are handled. */
    if (kitty->apc.data[0] != 'G')
        return;

    kitty_handle_apc(term, kitty->apc.data, len);
}


/* The render hooks (A, B, C) live in kitty-render.c. */

/* The image 'id' of screen buffer 'screen' (0 or 1), or NULL. */
static struct kitty_image *
test_screen_image(struct terminal *term, int screen, uint32_t id)
{
    return kitty_image_store_find_id(
        &term->kitty->screens[screen].image_store, KITTY_IMAGE_ID(id));
}

UNITTEST
{
    /* The vt parser forwards the APC string, byte for byte. */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char apc[] = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
    vt_from_slave(&term, (const uint8_t *)apc, sizeof(apc) - 1);

    static const char expected[] = "Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA";
    xassert(term.kitty->apc.len == sizeof(expected) - 1);
    xassert(memcmp(
                term.kitty->apc.data, expected, sizeof(expected) - 1) == 0);

    kitty_fini(&term);
}


UNITTEST
{
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.id = kitty_some_u32(31);

    char buf[64];
    static const char expected[] = "\x1b_Gi=31;OK\x1b\\";
    const size_t n = kitty_format_reply(
        buf, sizeof(buf), &cmd, KITTY_IMAGE_ID(31), "OK");
    xassert(n == sizeof(expected) - 1);
    xassert(memcmp(buf, expected, n) == 0);
}

UNITTEST
{
    /* Chunked direct transfer: three chunks build one image. */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char chunk1[] =
        "\x1b_Ga=t,i=1,f=24,s=2,v=2,m=1;AAAA\x1b\\";
    static const char chunk2[] = "\x1b_Gm=1;AAAA\x1b\\";
    static const char chunk3[] = "\x1b_Gm=0;AAAAAAAA\x1b\\";

    vt_from_slave(&term, (const uint8_t *)chunk1, sizeof(chunk1) - 1);
    xassert(term.kitty->chunk.active);
    vt_from_slave(&term, (const uint8_t *)chunk2, sizeof(chunk2) - 1);
    xassert(term.kitty->chunk.active);
    vt_from_slave(&term, (const uint8_t *)chunk3, sizeof(chunk3) - 1);
    xassert(!term.kitty->chunk.active);

    struct kitty_image *img = test_screen_image(&term, 0, 1);
    xassert(img != NULL);
    xassert(img->width == 2 && img->height == 2);
    xassert(img->data[0] == 0xff000000u);

    kitty_fini(&term);
}

UNITTEST
{
    /* A delete command cancels an in-progress chunked transfer. */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char other[] =
        "\x1b_Ga=t,i=9,f=24,s=1,v=1;AAAA\x1b\\";
    static const char start[] =
        "\x1b_Ga=t,i=7,f=24,s=2,v=2,m=1;AAAA\x1b\\";
    static const char cancel[] = "\x1b_Ga=d,d=I,i=9\x1b\\";

    vt_from_slave(&term, (const uint8_t *)other, sizeof(other) - 1);
    vt_from_slave(&term, (const uint8_t *)start, sizeof(start) - 1);
    xassert(term.kitty->chunk.active);
    vt_from_slave(&term, (const uint8_t *)cancel, sizeof(cancel) - 1);
    xassert(!term.kitty->chunk.active);

    /* The delete command also ran. */
    xassert(test_screen_image(&term, 0, 9) == NULL);

    /* The partial data is discarded. */
    xassert(tll_length(term.kitty->screens[0].image_store.images) == 0);

    /* The next complete transfer starts from a fresh state. */
    static const char done[] =
        "\x1b_Ga=t,i=8,f=24,s=1,v=1;AAAA\x1b\\";
    vt_from_slave(&term, (const uint8_t *)done, sizeof(done) - 1);
    xassert(test_screen_image(&term, 0, 8) != NULL);

    kitty_fini(&term);
}

UNITTEST
{
    /* Animation actions and relative placements change no state. */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char frame[] = "\x1b_Ga=f,i=1;AAAA\x1b\\";
    static const char relative[] =
        "\x1b_Ga=t,i=2,f=24,s=1,v=1,P=1;AAAA\x1b\\";

    vt_from_slave(&term, (const uint8_t *)frame, sizeof(frame) - 1);
    vt_from_slave(&term, (const uint8_t *)relative, sizeof(relative) - 1);

    xassert(tll_length(term.kitty->screens[0].image_store.images) == 0);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * A continuation chunk may carry a non-direct medium. Its control
     * data is ignored, so the transfer must still complete with the
     * correct data (the medium of the first chunk applies throughout).
     */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char chunk1[] =
        "\x1b_Ga=t,i=1,f=24,s=2,v=2,m=1;AAAA\x1b\\";
    static const char chunk2[] = "\x1b_Gt=f,m=1;AAAA\x1b\\";
    static const char chunk3[] = "\x1b_Gm=0;AAAAAAAA\x1b\\";

    vt_from_slave(&term, (const uint8_t *)chunk1, sizeof(chunk1) - 1);
    vt_from_slave(&term, (const uint8_t *)chunk2, sizeof(chunk2) - 1);
    xassert(term.kitty->chunk.active);

    vt_from_slave(&term, (const uint8_t *)chunk3, sizeof(chunk3) - 1);
    xassert(!term.kitty->chunk.active);

    struct kitty_image *img = test_screen_image(&term, 0, 1);
    xassert(img != NULL);
    xassert(img->width == 2 && img->height == 2);
    xassert(img->data[0] == 0xff000000u);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * Rejected actions and relative placements change no state, and
     * only a=d cancels a transfer. An in-progress transfer therefore
     * survives a rejection and completes with all of its data.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char chunk1[] =
        "\x1b_Ga=t,i=2,f=24,s=2,v=2,m=1;AAAA\x1b\\";
    static const char frame[] = "\x1b_Ga=f,i=2;AAAA\x1b\\";
    static const char relative[] =
        "\x1b_Ga=t,i=2,P=1,f=24,s=2,v=2,m=1;AAAA\x1b\\";
    static const char chunk2[] = "\x1b_Gm=1;AAAA\x1b\\";
    static const char chunk3[] = "\x1b_Gm=0;AAAAAAAA\x1b\\";

    vt_from_slave(&term, (const uint8_t *)chunk1, sizeof(chunk1) - 1);
    xassert(term.kitty->chunk.active);
    const size_t len = term.kitty->chunk.len;

    /* a=f: rejected, transfer untouched. */
    vt_from_slave(&term, (const uint8_t *)frame, sizeof(frame) - 1);
    xassert(term.kitty->chunk.active);
    xassert(term.kitty->chunk.len == len);

    /* P=1: rejected, transfer untouched. */
    vt_from_slave(&term, (const uint8_t *)relative, sizeof(relative) - 1);
    xassert(term.kitty->chunk.active);
    xassert(term.kitty->chunk.len == len);

    vt_from_slave(&term, (const uint8_t *)chunk2, sizeof(chunk2) - 1);
    vt_from_slave(&term, (const uint8_t *)chunk3, sizeof(chunk3) - 1);
    xassert(!term.kitty->chunk.active);

    struct kitty_image *img = test_screen_image(&term, 0, 2);
    xassert(img != NULL);
    xassert(img->width == 2 && img->height == 2);

    kitty_fini(&term);
}


