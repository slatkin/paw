#include "kitty-graphics.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "kitty-image.h"

#define LOG_MODULE "kitty-graphics"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "vt.h"
#include "xmalloc.h"
#include "xsnprintf.h"

/* Control data limit, from design D2. */
enum { KITTY_CONTROL_DATA_MAX = 1024 };

/*
 * A single non-chunked command can carry a full image. Cap the APC
 * buffer at the image quota so untrusted input cannot grow it without
 * bound (design D2). The quota is 320 MB; base64 inflates by 4/3.
 */
enum { KITTY_APC_MAX = 320u * 1024 * 1024 };

struct kitty_graphics {
    struct terminal *term;

    struct {
        char *data;
        size_t len;
        size_t size;
        bool overflow; /* command exceeded KITTY_APC_MAX; drop it */
    } apc;

    /* One image store per screen buffer (design D3). */
    struct kitty_image_store screens[2];
};

static struct kitty_image_store *
kitty_image_store_for(struct terminal *term)
{
    /*
     * The main and alternate screens have separate stores. The grid
     * addresses are stable: the interactive-resize code replaces the
     * contents of term->normal, not its address.
     */
    return term->grid == &term->alt
        ? &term->kitty->screens[1]
        : &term->kitty->screens[0];
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
kitty_init(struct terminal *term)
{
    xassert(term->kitty == NULL);
    term->kitty = xcalloc(1, sizeof(struct kitty_graphics));
    term->kitty->term = term;
    kitty_image_store_init(&term->kitty->screens[0]);
    kitty_image_store_init(&term->kitty->screens[1]);
}

void
kitty_fini(struct terminal *term)
{
    if (term->kitty == NULL)
        return;

    kitty_image_store_fini(&term->kitty->screens[0]);
    kitty_image_store_fini(&term->kitty->screens[1]);
    free(term->kitty->apc.data);
    free(term->kitty);
    term->kitty = NULL;
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
            if (!parse_u32(value, value_len, &cmd->width))
                return false;
            cmd->has_width = true;
            break;

        case 'v':
            if (!parse_u32(value, value_len, &cmd->height))
                return false;
            cmd->has_height = true;
            break;

        case 'i':
            if (!parse_u32(value, value_len, &cmd->id))
                return false;
            cmd->has_id = true;
            break;

        case 'I':
            if (!parse_u32(value, value_len, &cmd->number))
                return false;
            cmd->has_number = true;
            break;

        case 'p':
            if (!parse_u32(value, value_len, &cmd->placement_id))
                return false;
            cmd->has_placement_id = true;
            break;

        case 'c':
            if (!parse_u32(value, value_len, &cmd->cols))
                return false;
            cmd->has_cols = true;
            break;

        case 'r':
            if (!parse_u32(value, value_len, &cmd->rows))
                return false;
            cmd->has_rows = true;
            break;

        case 'x':
            if (!parse_i32(value, value_len, &cmd->x))
                return false;
            cmd->has_x = true;
            break;

        case 'y':
            if (!parse_i32(value, value_len, &cmd->y))
                return false;
            cmd->has_y = true;
            break;

        case 'w':
            if (!parse_u32(value, value_len, &cmd->w))
                return false;
            cmd->has_w = true;
            break;

        case 'h':
            if (!parse_u32(value, value_len, &cmd->h))
                return false;
            cmd->has_h = true;
            break;

        case 'X':
            if (!parse_i32(value, value_len, &cmd->X))
                return false;
            cmd->has_X = true;
            break;

        case 'Y':
            if (!parse_i32(value, value_len, &cmd->Y))
                return false;
            cmd->has_Y = true;
            break;

        case 'z':
            if (!parse_i32(value, value_len, &cmd->z))
                return false;
            cmd->has_z = true;
            break;

        case 'C':
            if (!parse_u32(value, value_len, &cmd->cursor))
                return false;
            cmd->has_cursor = true;
            break;

        case 'q':
            if (!parse_u32(value, value_len, &cmd->quiet))
                return false;
            cmd->has_quiet = true;
            break;

        case 'm':
            if (!parse_u32(value, value_len, &cmd->more))
                return false;
            cmd->has_more = true;
            break;

        case 'd':
            if (value_len != 1)
                return false;
            cmd->delete_spec = value[0];
            cmd->has_delete = true;
            break;

        case 'O':
            if (!parse_u32(value, value_len, &cmd->offset))
                return false;
            cmd->has_offset = true;
            break;

        case 'S':
            if (!parse_u32(value, value_len, &cmd->size))
                return false;
            cmd->has_size = true;
            break;

        case 'U':
            if (!parse_u32(value, value_len, &cmd->unicode_placeholder))
                return false;
            cmd->has_unicode_placeholder = true;
            break;

        case 'P':
            if (!parse_u32(value, value_len, &cmd->parent_id))
                return false;
            cmd->has_parent_id = true;
            break;

        case 'Q':
            if (!parse_u32(value, value_len, &cmd->parent_placement))
                return false;
            cmd->has_parent_placement = true;
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
    if (cmd->has_number)
        n += xsnprintf(buf + n, size - n, ",I=%u", cmd->number);
    if (cmd->has_placement_id)
        n += xsnprintf(buf + n, size - n, ",p=%u", cmd->placement_id);
    n += xsnprintf(buf + n, size - n, ";%s\x1b\\", msg);
    return n;
}

static void
kitty_reply(struct terminal *term, const struct kitty_command *cmd,
            uint32_t id, bool ok, const char *msg)
{
    /* Without i or I, no reply is sent. */
    if (!cmd->has_id && !cmd->has_number)
        return;

    const uint32_t quiet = cmd->has_quiet ? cmd->quiet : 0;

    /* q=2 suppresses every reply, q=1 suppresses OK replies. */
    if (quiet >= 2)
        return;
    if (ok && quiet >= 1)
        return;

    char buf[512];
    const size_t n = kitty_format_reply(buf, sizeof(buf), cmd, id, msg);
    term_to_slave(term, buf, n);
}

static void
kitty_handle_transmit(struct terminal *term, const struct kitty_command *cmd,
                      const char *payload, size_t payload_len)
{
    if (cmd->medium != KITTY_MEDIUM_DIRECT) {
        kitty_reply(
            term, cmd, cmd->has_id ? cmd->id : 0, false,
            "EINVAL:unsupported transmission medium");
        return;
    }

    struct kitty_image_store *store = kitty_image_store_for(term);

    const uint32_t id = cmd->has_id
        ? cmd->id
        : kitty_image_store_alloc_id(store);

    struct kitty_image image;
    const char *error = NULL;
    if (!kitty_image_decode(
            cmd, payload, payload_len, KITTY_IMAGE_QUOTA, &image, &error))
    {
        kitty_reply(term, cmd, id, false, error);
        return;
    }

    image.id = id;
    image.has_number = cmd->has_number;
    image.number = cmd->number;
    kitty_image_store_add(store, &image);

    kitty_reply(term, cmd, id, true, "OK");
}

static void
kitty_handle_query(struct terminal *term, const struct kitty_command *cmd,
                   const char *payload, size_t payload_len)
{
    struct kitty_image_store *store = kitty_image_store_for(term);

    /* An I-only query still names the id paw would have assigned. */
    uint32_t id = 0;
    if (cmd->has_id)
        id = cmd->id;
    else if (cmd->has_number)
        id = kitty_image_store_alloc_id(store);

    if (cmd->medium != KITTY_MEDIUM_DIRECT) {
        kitty_reply(
            term, cmd, id, false, "EINVAL:unsupported transmission medium");
        return;
    }

    /* Load and check the image data as a transmit would, without
     * storing it. */
    struct kitty_image image;
    const char *error = NULL;
    const bool ok = kitty_image_decode(
        cmd, payload, payload_len, KITTY_IMAGE_QUOTA, &image, &error);
    if (ok)
        kitty_image_destroy(&image);

    kitty_reply(term, cmd, id, ok, ok ? "OK" : error);
}

static void
kitty_handle_apc(struct terminal *term, const char *data, size_t len)
{
    struct kitty_command cmd;
    kitty_command_init(&cmd);

    const char *payload;
    size_t payload_len;
    if (!kitty_parse_apc(data, len, &cmd, &payload, &payload_len)) {
        kitty_reply(
            term, &cmd, cmd.has_id ? cmd.id : 0, false,
            "EINVAL:invalid control data");
        return;
    }

    if (cmd.has_id && cmd.has_number) {
        kitty_reply(
            term, &cmd, cmd.id, false, "EINVAL:both i and I given");
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

    default:
        /* Put, delete and animation actions are added later. */
        kitty_reply(
            term, &cmd, cmd.has_id ? cmd.id : 0, false,
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
kitty_scroll_up(struct terminal *term, struct scroll_region region, int rows)
{
}

void
kitty_scroll_down(struct terminal *term, struct scroll_region region, int rows)
{
}

void
kitty_clear_visible(struct terminal *term)
{
}

void
kitty_alt_screen_clear(struct terminal *term)
{
}

void
kitty_reset(struct terminal *term)
{
}

void
kitty_cell_size_changed(struct terminal *term)
{
}

void
kitty_reflow_begin(struct terminal *term)
{
}

void
kitty_reflow_end(struct terminal *term)
{
}

void
kitty_render_overlay(
    struct terminal *term, pixman_image_t *pix,
    pixman_region32_t *damage, const struct coord *cursor)
{
}

void
kitty_render_underlay(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
}

bool
kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix,
    const struct row *row, int row_no, int col, int x, int y)
{
    return false;
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

    static const char control[] = "a=T,f=100,i=5,q=1";
    kitty_command_init(&cmd);
    xassert(parse_control_data(control, sizeof(control) - 1, &cmd));
    xassert(cmd.action == KITTY_ACTION_TRANSMIT_AND_DISPLAY);
    xassert(cmd.format == KITTY_FORMAT_PNG);
    xassert(cmd.has_id && cmd.id == 5);
    xassert(cmd.has_quiet && cmd.quiet == 1);

    /* A bad number fails the command. */
    static const char bad_number[] = "a=T,f=abc";
    kitty_command_init(&cmd);
    xassert(!parse_control_data(bad_number, sizeof(bad_number) - 1, &cmd));

    /* Unknown keys are ignored. */
    static const char unknown[] = "N=1,i=5";
    kitty_command_init(&cmd);
    xassert(parse_control_data(unknown, sizeof(unknown) - 1, &cmd));
    xassert(cmd.has_id && cmd.id == 5);
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
    xassert(!cmd.has_id);

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
    cmd.has_id = true;
    cmd.id = 31;

    char buf[64];
    static const char expected[] = "\x1b_Gi=31;OK\x1b\\";
    const size_t n = kitty_format_reply(buf, sizeof(buf), &cmd, 31, "OK");
    xassert(n == sizeof(expected) - 1);
    xassert(memcmp(buf, expected, n) == 0);
}
