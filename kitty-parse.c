#include "kitty-graphics-internal.h"

#include <ctype.h>
#include <stddef.h>
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
 * Keys whose value is a plain optional number. Each entry stores the
 * kind of its field and the offset of the field in the command.
 */
enum kitty_number_kind { KITTY_NUMBER_U32, KITTY_NUMBER_I32 };

struct kitty_number_key {
    char key;
    enum kitty_number_kind kind;
    size_t offset;
};

static const struct kitty_number_key number_keys[] = {
    {'s', KITTY_NUMBER_U32, offsetof(struct kitty_command, width)},
    {'v', KITTY_NUMBER_U32, offsetof(struct kitty_command, height)},
    {'i', KITTY_NUMBER_U32, offsetof(struct kitty_command, id)},
    {'I', KITTY_NUMBER_U32, offsetof(struct kitty_command, number)},
    {'p', KITTY_NUMBER_U32, offsetof(struct kitty_command, placement_id)},
    {'c', KITTY_NUMBER_U32, offsetof(struct kitty_command, cols)},
    {'r', KITTY_NUMBER_U32, offsetof(struct kitty_command, rows)},
    {'x', KITTY_NUMBER_I32, offsetof(struct kitty_command, x)},
    {'y', KITTY_NUMBER_I32, offsetof(struct kitty_command, y)},
    {'w', KITTY_NUMBER_U32, offsetof(struct kitty_command, w)},
    {'h', KITTY_NUMBER_U32, offsetof(struct kitty_command, h)},
    {'X', KITTY_NUMBER_I32, offsetof(struct kitty_command, X)},
    {'Y', KITTY_NUMBER_I32, offsetof(struct kitty_command, Y)},
    {'z', KITTY_NUMBER_I32, offsetof(struct kitty_command, z)},
    {'C', KITTY_NUMBER_U32, offsetof(struct kitty_command, cursor)},
    {'q', KITTY_NUMBER_U32, offsetof(struct kitty_command, quiet)},
    {'m', KITTY_NUMBER_U32, offsetof(struct kitty_command, more)},
    {'O', KITTY_NUMBER_U32, offsetof(struct kitty_command, offset)},
    {'S', KITTY_NUMBER_U32, offsetof(struct kitty_command, size)},
    {'U', KITTY_NUMBER_U32, offsetof(struct kitty_command, unicode_placeholder)},
    {'P', KITTY_NUMBER_U32, offsetof(struct kitty_command, parent_id)},
    {'Q', KITTY_NUMBER_U32, offsetof(struct kitty_command, parent_placement)},
};

static const struct kitty_number_key *
find_number_key(char key)
{
    for (size_t i = 0; i < sizeof(number_keys) / sizeof(number_keys[0]); i++) {
        if (number_keys[i].key == key)
            return &number_keys[i];
    }
    return NULL;
}

static bool
parse_number_value(
    const struct kitty_number_key *k, const char *value, size_t value_len,
    struct kitty_command *cmd)
{
    void *field = (char *)cmd + k->offset;

    switch (k->kind) {
    case KITTY_NUMBER_U32:
        return parse_opt_u32(value, value_len, field);
    case KITTY_NUMBER_I32:
        return parse_opt_i32(value, value_len, field);
    }

    BUG("unreachable number key kind");
}

/* What parse_special_key() did with a key. */
enum kitty_key_result {
    KITTY_KEY_OK,      /* Known key, parsed without error. */
    KITTY_KEY_IGNORED, /* Unknown key. */
    KITTY_KEY_ERROR,   /* Known key, malformed value. */
};

/*
 * Keys with a value format of their own.
 */
static enum kitty_key_result
parse_special_key(
    char key, const char *value, size_t value_len, struct kitty_command *cmd)
{
    switch (key) {
    case 'a':
        if (value_len != 1 || !parse_action(value[0], &cmd->action))
            return KITTY_KEY_ERROR;
        break;

    case 'f': {
        uint32_t format;
        if (!parse_u32(value, value_len, &format) ||
            (format != KITTY_FORMAT_RGB &&
             format != KITTY_FORMAT_RGBA &&
             format != KITTY_FORMAT_PNG))
        {
            return KITTY_KEY_ERROR;
        }
        cmd->format = (enum kitty_format)format;
        break;
    }

    case 't':
        if (value_len != 1 || !parse_medium(value[0], &cmd->medium))
            return KITTY_KEY_ERROR;
        break;

    case 'o':
        if (value_len != 1 || value[0] != 'z')
            return KITTY_KEY_ERROR;
        cmd->compression = KITTY_COMPRESSION_ZLIB;
        break;

    case 'd':
        /* A NUL byte cannot be told from an absent d key, so a=d
         * would delete every visible placement. Reject it. */
        if (value_len != 1 || value[0] == '\0')
            return KITTY_KEY_ERROR;
        cmd->delete_spec = value[0];
        break;

    default:
        /* Unknown key: ignore it and handle the rest. */
        return KITTY_KEY_IGNORED;
    }

    return KITTY_KEY_OK;
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

        const struct kitty_number_key *number_key = find_number_key(key);
        if (number_key != NULL) {
            if (!parse_number_value(number_key, value, value_len, cmd))
                return false;
            continue;
        }

        if (parse_special_key(key, value, value_len, cmd) == KITTY_KEY_ERROR)
            return false;
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
    /*
     * A NUL delete selector is rejected: it cannot be told from an
     * absent d key, and a=d would then delete every visible placement.
     */
    struct kitty_command cmd;
    static const char nul[] = {'a', '=', 'd', ',', 'd', '=', '\0'};
    kitty_command_init(&cmd);
    xassert(!parse_control_data(nul, sizeof(nul), &cmd));

    /* An absent d key still defaults to 'a' in the delete code. */
    static const char absent[] = "a=d,i=1";
    kitty_command_init(&cmd);
    xassert(parse_control_data(absent, sizeof(absent) - 1, &cmd));
    xassert(cmd.delete_spec == '\0');
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


