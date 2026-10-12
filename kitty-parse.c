#include "kitty-graphics-internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "xmalloc.h"

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
bool
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


