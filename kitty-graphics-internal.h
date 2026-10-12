#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kitty-graphics.h"
#include "kitty-image.h"
#include "kitty-placement.h"

/* Control data limit. */
enum { KITTY_CONTROL_DATA_MAX = 1024 };

/*
 * The graphics state of one terminal. It is private to the kitty
 * graphics code; only the internal headers name it.
 */
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

/* Init/fini the stores of one screen buffer (0 or 1). */
void kitty_screen_init(struct kitty_graphics *kitty, int idx);
void kitty_screen_fini(struct kitty_graphics *kitty, int idx);

/* Drop an in-progress chunked transfer. */
void kitty_chunk_reset(struct kitty_graphics *kitty);

/*
 * Split the APC buffer (starting with the 'G' command byte) into
 * control data and payload, then parse the control data. Returns false
 * if the control data is too long, or malformed.
 */
bool kitty_parse_apc(
    const char *data, size_t len, struct kitty_command *cmd,
    const char **payload, size_t *payload_len);
