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

/* Control data limit. */
enum { KITTY_CONTROL_DATA_MAX = 1024 };

/*
 * A single non-chunked command can carry a full image. Cap the APC
 * buffer at the image quota so untrusted input cannot grow it without
 * bound. The quota is 320 MB; base64 inflates by 4/3.
 */
enum { KITTY_APC_MAX = 320u * 1024 * 1024 };

struct kitty_graphics {
    struct {
        char *data;
        size_t len;
        size_t size;
        bool overflow; /* command exceeded KITTY_APC_MAX; drop it */
    } apc;

    /*
     * Chunked direct transmission. The first chunk
     * carries the full control data; the payloads are joined and
     * decoded when the final chunk arrives.
     */
    struct {
        bool active;
        struct kitty_command cmd;
        char *data;
        size_t len;
        size_t size;
    } chunk;

    /*
     * One image and placement store per screen buffer.
     */
    struct {
        struct kitty_image_store image_store;
        struct kitty_placement_store placement_store;
    } screens[2];

    /* Reflow state. */
    struct {
        bool active;
        int old_offset;
        int old_num_rows;
        int old_screen_rows;
        int cursor_sb;
    } reflow;
};

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

static void
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

static void
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

static void
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

static bool
parse_u32(const char *s, size_t len, uint32_t *out)
{
    if (len == 0)
        return false;

    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i]))
            return false;
        value = value * 10 + (uint64_t)(s[i] - '0');
        if (value > UINT32_MAX)
            return false;
    }

    *out = (uint32_t)value;
    return true;
}

static bool
parse_i32(const char *s, size_t len, int32_t *out)
{
    bool neg = false;
    if (len > 0 && s[0] == '-') {
        neg = true;
        s++;
        len--;
    }

    uint32_t magnitude;
    if (!parse_u32(s, len, &magnitude))
        return false;

    if (neg) {
        if (magnitude > (uint32_t)INT32_MAX + 1u)
            return false;
        *out = (int32_t)-(int64_t)magnitude;
    } else {
        if (magnitude > (uint32_t)INT32_MAX)
            return false;
        *out = (int32_t)magnitude;
    }

    return true;
}

static bool
parse_opt_u32(const char *s, size_t len, struct kitty_u32 *out)
{
    if (!parse_u32(s, len, &out->value))
        return false;
    out->set = true;
    return true;
}

static bool
parse_opt_i32(const char *s, size_t len, struct kitty_i32 *out)
{
    if (!parse_i32(s, len, &out->value))
        return false;
    out->set = true;
    return true;
}

static bool
parse_action(char c, enum kitty_action *out)
{
    switch (c) {
    case 't': *out = KITTY_ACTION_TRANSMIT; return true;
    case 'T': *out = KITTY_ACTION_TRANSMIT_AND_DISPLAY; return true;
    case 'q': *out = KITTY_ACTION_QUERY; return true;
    case 'p': *out = KITTY_ACTION_PUT; return true;
    case 'd': *out = KITTY_ACTION_DELETE; return true;
    case 'f': *out = KITTY_ACTION_FRAME; return true;
    case 'a': *out = KITTY_ACTION_ANIMATE; return true;
    case 'c': *out = KITTY_ACTION_COMPOSE; return true;
    }

    return false;
}

static bool
parse_medium(char c, enum kitty_medium *out)
{
    switch (c) {
    case 'd': *out = KITTY_MEDIUM_DIRECT; return true;
    case 'f': *out = KITTY_MEDIUM_FILE; return true;
    case 't': *out = KITTY_MEDIUM_TEMP_FILE; return true;
    case 's': *out = KITTY_MEDIUM_SHM; return true;
    }

    return false;
}

/*
 * Parse the comma-separated key=value list. Unknown keys are ignored.
 * A value that is not valid for a known key fails the whole command.
 */
static bool
parse_control_data(const char *data, size_t len, struct kitty_command *cmd)
{
    size_t i = 0;
    while (i < len) {
        const size_t start = i;
        while (i < len && data[i] != ',')
            i++;

        const size_t item_len = i - start;
        if (i < len)
            i++; /* skip comma */

        /* Leading/trailing/duplicate commas are tolerated. */
        if (item_len == 0)
            continue;

        const char *item = data + start;
        const char *eq = memchr(item, '=', item_len);
        if (eq == NULL)
            return false;

        const char key = item[0];
        const char *value = eq + 1;
        const size_t value_len = item_len - (size_t)(value - item);

        switch (key) {
        case 'a':
            if (value_len != 1 || !parse_action(value[0], &cmd->action))
                return false;
            break;

        case 'f': {
            uint32_t format;
            if (!parse_u32(value, value_len, &format) ||
                (format != KITTY_FORMAT_RGB &&
                 format != KITTY_FORMAT_RGBA &&
                 format != KITTY_FORMAT_PNG))
            {
                return false;
            }
            cmd->format = (enum kitty_format)format;
            break;
        }

        case 't':
            if (value_len != 1 || !parse_medium(value[0], &cmd->medium))
                return false;
            break;

        case 'o':
            if (value_len != 1 || value[0] != 'z')
                return false;
            cmd->compression = KITTY_COMPRESSION_ZLIB;
            break;

        case 's':
            if (!parse_opt_u32(value, value_len, &cmd->width))
                return false;
            break;

        case 'v':
            if (!parse_opt_u32(value, value_len, &cmd->height))
                return false;
            break;

        case 'i':
            if (!parse_opt_u32(value, value_len, &cmd->id))
                return false;
            break;

        case 'I':
            if (!parse_opt_u32(value, value_len, &cmd->number))
                return false;
            break;

        case 'p':
            if (!parse_opt_u32(value, value_len, &cmd->placement_id))
                return false;
            break;

        case 'c':
            if (!parse_opt_u32(value, value_len, &cmd->cols))
                return false;
            break;

        case 'r':
            if (!parse_opt_u32(value, value_len, &cmd->rows))
                return false;
            break;

        case 'x':
            if (!parse_opt_i32(value, value_len, &cmd->x))
                return false;
            break;

        case 'y':
            if (!parse_opt_i32(value, value_len, &cmd->y))
                return false;
            break;

        case 'w':
            if (!parse_opt_u32(value, value_len, &cmd->w))
                return false;
            break;

        case 'h':
            if (!parse_opt_u32(value, value_len, &cmd->h))
                return false;
            break;

        case 'X':
            if (!parse_opt_i32(value, value_len, &cmd->X))
                return false;
            break;

        case 'Y':
            if (!parse_opt_i32(value, value_len, &cmd->Y))
                return false;
            break;

        case 'z':
            if (!parse_opt_i32(value, value_len, &cmd->z))
                return false;
            break;

        case 'C':
            if (!parse_opt_u32(value, value_len, &cmd->cursor))
                return false;
            break;

        case 'q':
            if (!parse_opt_u32(value, value_len, &cmd->quiet))
                return false;
            break;

        case 'm':
            if (!parse_opt_u32(value, value_len, &cmd->more))
                return false;
            break;

        case 'd':
            if (value_len != 1)
                return false;
            cmd->delete_spec = value[0];
            break;

        case 'O':
            if (!parse_opt_u32(value, value_len, &cmd->offset))
                return false;
            break;

        case 'S':
            if (!parse_opt_u32(value, value_len, &cmd->size))
                return false;
            break;

        case 'U':
            if (!parse_opt_u32(value, value_len, &cmd->unicode_placeholder))
                return false;
            break;

        case 'P':
            if (!parse_opt_u32(value, value_len, &cmd->parent_id))
                return false;
            break;

        case 'Q':
            if (!parse_opt_u32(value, value_len, &cmd->parent_placement))
                return false;
            break;

        default:
            /* Unknown key: ignore it and handle the rest. */
            break;
        }
    }

    return true;
}

/*
 * Split the APC buffer (starting with the 'G' command byte) into
 * control data and payload, then parse the control data. Returns false
 * if the control data is too long, or malformed.
 */
static bool
kitty_parse_apc(const char *data, size_t len, struct kitty_command *cmd,
                const char **payload, size_t *payload_len)
{
    xassert(len > 0);
    xassert(data[0] == 'G');

    const char *ctrl = data + 1;
    const size_t rest = len - 1;
    const char *sep = memchr(ctrl, ';', rest);
    const size_t ctrl_len = sep != NULL ? (size_t)(sep - ctrl) : rest;

    if (ctrl_len > KITTY_CONTROL_DATA_MAX)
        return false;

    if (sep != NULL) {
        *payload = sep + 1;
        *payload_len = rest - ctrl_len - 1;
    } else {
        *payload = "";
        *payload_len = 0;
    }

    return parse_control_data(ctrl, ctrl_len, cmd);
}

static size_t
kitty_format_reply(char *buf, size_t size, const struct kitty_command *cmd,
                   uint32_t id, const char *msg)
{
    size_t n = xsnprintf(buf, size, "\x1b_Gi=%u", id);
    if (cmd->number.set)
        n += xsnprintf(buf + n, size - n, ",I=%u", cmd->number.value);
    if (cmd->placement_id.set)
        n += xsnprintf(buf + n, size - n, ",p=%u", cmd->placement_id.value);
    n += xsnprintf(buf + n, size - n, ";%s\x1b\\", msg);
    return n;
}

static void
kitty_reply(struct terminal *term, const struct kitty_command *cmd,
            uint32_t id, bool ok, const char *msg)
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
    struct terminal *term, const struct kitty_command *cmd, uint32_t id,
    struct kitty_image *image)
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
    const uint32_t id = kitty_or(cmd->id, kitty_image_store_alloc_id(store));

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
            kitty_reply(term, cmd, kitty_or(cmd->id, 0), false,
                        "EINVAL:image too large");
            return;
        }

        /* m=0, or no m key, ends the transfer. */
        if (kitty_or(cmd->more, 0) != 0)
            return;

        const struct kitty_command first = kitty->chunk.cmd;
        const uint32_t id = kitty_or(
            first.id, kitty_image_store_alloc_id(kitty_image_store_for(term)));

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
            kitty_reply(term, cmd, kitty_or(cmd->id, 0), false,
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
    uint32_t id = 0;
    if (cmd->id.set)
        id = cmd->id.value;
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
    uint32_t id = 0;

    if (cmd->id.set) {
        id = cmd->id.value;
        image = kitty_image_store_find_id(store, id);
    } else if (cmd->number.set) {
        image = kitty_image_store_find_number(store, cmd->number.value);
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
            term, &cmd, kitty_or(cmd.id, 0), false,
            "EINVAL:invalid control data");
        return;
    }

    if (cmd.id.set && cmd.number.set) {
        /* A malformed command aborts any in-progress transfer. */
        kitty_chunk_reset(kitty);
        kitty_reply(
            term, &cmd, cmd.id.value, false, "EINVAL:both i and I given");
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
            term, &cmd, kitty_or(cmd.id, 0), false,
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
            term, &cmd, kitty_or(cmd.id, 0), false,
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

void
kitty_alt_screen_clear(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    /* The alternate screen always starts empty. */
    kitty_screen_fini(term->kitty, 1);
    kitty_screen_init(term->kitty, 1);
}

void
kitty_reset(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    for (int i = 0; i < 2; i++) {
        kitty_screen_fini(term->kitty, i);
        kitty_screen_init(term->kitty, i);
    }

    kitty_chunk_reset(term->kitty);
    term->kitty->reflow.active = false;
}

void
kitty_cell_size_changed(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    /*
     * The cell span of a placement stays; only the scaled cache is
     * rebuilt at the new cell size, on the next frame.
     */
    for (int i = 0; i < 2; i++) {
        struct kitty_placement_store *store =
            &term->kitty->screens[i].placement_store;
        tll_foreach(store->placements, it)
            kitty_placement_cache_free(store, &it->item);
    }
}

void
kitty_reflow_begin(struct terminal *term)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL)
        return;

    /*
     * During an interactive resize the reflow source is the stashed
     * original grid; otherwise it is the normal grid.
     */
    const struct grid *grid =
        term->interactive_resizing.grid != NULL
        ? term->interactive_resizing.grid
        : &term->normal;

    if (grid->num_rows <= 0 || term->rows <= 0)
        return;

    const int abs_row =
        (grid->offset + grid->cursor.point.row) & (grid->num_rows - 1);

    kitty->reflow.old_offset = grid->offset;
    kitty->reflow.old_num_rows = grid->num_rows;
    kitty->reflow.old_screen_rows = term->rows;
    kitty->reflow.cursor_sb =
        grid_row_abs_to_sb(grid, term->rows, abs_row);
    kitty->reflow.active = true;
}

void
kitty_reflow_end(struct terminal *term)
{
    struct kitty_graphics *kitty = term->kitty;
    if (kitty == NULL || !kitty->reflow.active)
        return;

    kitty->reflow.active = false;

    const struct grid *grid = &term->normal;
    if (grid->num_rows <= 0 || term->rows <= 0)
        return;

    const int abs_row =
        (grid->offset + grid->cursor.point.row) & (grid->num_rows - 1);
    const int new_sb = grid_row_abs_to_sb(grid, term->rows, abs_row);
    const int delta = new_sb - kitty->reflow.cursor_sb;

    kitty_placement_reflow(
        term, &kitty->screens[0].placement_store,
        kitty->reflow.old_offset, kitty->reflow.old_num_rows,
        kitty->reflow.old_screen_rows, delta);
}

/* The render hooks (A, B, C) live in kitty-render.c. */

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

    static const char control[] = "a=T,f=100,i=5,q=1";
    kitty_command_init(&cmd);
    xassert(parse_control_data(control, sizeof(control) - 1, &cmd));
    xassert(cmd.action == KITTY_ACTION_TRANSMIT_AND_DISPLAY);
    xassert(cmd.format == KITTY_FORMAT_PNG);
    xassert(cmd.id.set && cmd.id.value == 5);
    xassert(cmd.quiet.set && cmd.quiet.value == 1);

    /* A bad number fails the command. */
    static const char bad_number[] = "a=T,f=abc";
    kitty_command_init(&cmd);
    xassert(!parse_control_data(bad_number, sizeof(bad_number) - 1, &cmd));

    /* Unknown keys are ignored. */
    static const char unknown[] = "N=1,i=5";
    kitty_command_init(&cmd);
    xassert(parse_control_data(unknown, sizeof(unknown) - 1, &cmd));
    xassert(cmd.id.set && cmd.id.value == 5);
}

UNITTEST
{
    /* Control data longer than 1 KiB is dropped. */
    struct kitty_command cmd;
    kitty_command_init(&cmd);

    char *apc = xmalloc(KITTY_CONTROL_DATA_MAX + 2);
    apc[0] = 'G';
    memset(apc + 1, 'n', KITTY_CONTROL_DATA_MAX + 1);

    const char *payload;
    size_t payload_len;
    xassert(!kitty_parse_apc(
                apc, KITTY_CONTROL_DATA_MAX + 2,
                &cmd, &payload, &payload_len));
    xassert(!cmd.id.set);

    free(apc);
}

UNITTEST
{
    /* a=q accepts direct RGB data and rejects a wrong length. */
    struct kitty_command cmd;
    struct kitty_image image;

    static const char rgb[] = "Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA";
    kitty_command_init(&cmd);
    const char *payload;
    size_t payload_len;
    xassert(kitty_parse_apc(
                rgb, sizeof(rgb) - 1, &cmd, &payload, &payload_len));
    const char *error = NULL;
    xassert(kitty_image_decode(
                &cmd, payload, payload_len, KITTY_IMAGE_QUOTA,
                &image, &error));
    xassert(error == NULL);
    kitty_image_destroy(&image);

    /* 1x1 RGBA needs 4 bytes, but only 3 are supplied. */
    static const char rgba[] = "Gi=31,s=1,v=1,a=q,f=32;AAAA";
    kitty_command_init(&cmd);
    xassert(kitty_parse_apc(
                rgba, sizeof(rgba) - 1, &cmd, &payload, &payload_len));
    error = NULL;
    xassert(!kitty_image_decode(
                &cmd, payload, payload_len, KITTY_IMAGE_QUOTA,
                &image, &error));
    xassert(error != NULL);
    xassert(image.data == NULL);
}

UNITTEST
{
    struct kitty_command cmd;
    kitty_command_init(&cmd);
    cmd.id = kitty_some_u32(31);

    char buf[64];
    static const char expected[] = "\x1b_Gi=31;OK\x1b\\";
    const size_t n = kitty_format_reply(buf, sizeof(buf), &cmd, 31, "OK");
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

    struct kitty_image *img =
        kitty_image_store_find_id(&term.kitty->screens[0].image_store, 1);
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
    xassert(kitty_image_store_find_id(&term.kitty->screens[0].image_store, 9) == NULL);

    /* The partial data is discarded. */
    xassert(tll_length(term.kitty->screens[0].image_store.images) == 0);

    /* The next complete transfer starts from a fresh state. */
    static const char done[] =
        "\x1b_Ga=t,i=8,f=24,s=1,v=1;AAAA\x1b\\";
    vt_from_slave(&term, (const uint8_t *)done, sizeof(done) - 1);
    xassert(kitty_image_store_find_id(&term.kitty->screens[0].image_store, 8) != NULL);

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

    struct kitty_image *img =
        kitty_image_store_find_id(&term.kitty->screens[0].image_store, 1);
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

    struct kitty_image *img =
        kitty_image_store_find_id(&term.kitty->screens[0].image_store, 2);
    xassert(img != NULL);
    xassert(img->width == 2 && img->height == 2);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * Leaving the alternate screen drops all of its images and
     * placements, and leaves the main screen alone.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char img[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AAAA\x1b\\";
    static const char img2[] =
        "\x1b_Ga=t,i=2,f=24,s=1,v=1;AAAA\x1b\\";

    /* Image 1 and image 2 on the main screen. */
    vt_from_slave(&term, (const uint8_t *)img, sizeof(img) - 1);
    vt_from_slave(&term, (const uint8_t *)img2, sizeof(img2) - 1);
    xassert(kitty_image_store_find_id(
                &term.kitty->screens[0].image_store, 1) != NULL);
    xassert(kitty_image_store_find_id(
                &term.kitty->screens[0].image_store, 2) != NULL);

    /* Image 1 on the alternate screen too (same id, separate store). */
    term.grid = &term.alt;
    vt_from_slave(&term, (const uint8_t *)img, sizeof(img) - 1);
    xassert(kitty_image_store_find_id(
                &term.kitty->screens[1].image_store, 1) != NULL);

    kitty_alt_screen_clear(&term);

    xassert(tll_length(term.kitty->screens[1].image_store.images) == 0);
    xassert(kitty_image_store_find_id(
                &term.kitty->screens[0].image_store, 1) != NULL);
    xassert(kitty_image_store_find_id(
                &term.kitty->screens[0].image_store, 2) != NULL);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * RIS deletes all placements and images on both screens, and
     * cancels an in-progress chunked transfer.
     */
    struct terminal term = {0};
    term.ptmx = -1;
    kitty_init(&term);

    static const char main_img[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AAAA\x1b\\";
    static const char alt_img[] =
        "\x1b_Ga=t,i=2,f=24,s=1,v=1;AAAA\x1b\\";
    static const char chunk[] =
        "\x1b_Ga=t,i=3,f=24,s=1,v=1,m=1;AAAA\x1b\\";

    vt_from_slave(&term, (const uint8_t *)main_img, sizeof(main_img) - 1);
    term.grid = &term.alt;
    vt_from_slave(&term, (const uint8_t *)alt_img, sizeof(alt_img) - 1);
    term.grid = &term.normal;
    vt_from_slave(&term, (const uint8_t *)chunk, sizeof(chunk) - 1);

    xassert(tll_length(term.kitty->screens[0].image_store.images) == 1);
    xassert(tll_length(term.kitty->screens[1].image_store.images) == 1);
    xassert(term.kitty->chunk.active);

    kitty_reset(&term);

    xassert(tll_length(term.kitty->screens[0].image_store.images) == 0);
    xassert(tll_length(term.kitty->screens[1].image_store.images) == 0);
    xassert(!term.kitty->chunk.active);

    kitty_fini(&term);
}

UNITTEST
{
    /*
     * DECSTR (soft reset) while on the alternate screen leaves the
     * alternate grid. Its images and placements must not survive, or
     * the next CSI ?1049h, which relies on the exit path having
     * cleared them, would show them again.
     */
    static struct config conf;
    static struct wayland wl;

    struct terminal term = {
        .ptmx = -1,
        .wl = &wl,
        .rows = 0,
        .cols = 0,
        .grid = &term.alt,
        .conf = &conf,
        .selection = {
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .auto_scroll = {
                .fd = -1,
            },
        },
    };
    conf.locked_title = true;
    term.window_title_has_been_set = true;

    kitty_init(&term);

    static const char img[] =
        "\x1b_Ga=t,i=1,f=24,s=1,v=1;AAAA\x1b\\";
    vt_from_slave(&term, (const uint8_t *)img, sizeof(img) - 1);
    xassert(kitty_image_store_find_id(
                &term.kitty->screens[1].image_store, 1) != NULL);

    term_reset(&term, false);

    xassert(term.grid == &term.normal);
    xassert(tll_length(term.kitty->screens[1].image_store.images) == 0);

    kitty_fini(&term);
}
