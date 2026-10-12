#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kitty-graphics.h"

/*
 * The single reply for every failure to read image data from a file,
 * temp file or shared memory object (spec: One error for all file
 * failures).
 */
#define KITTY_FILE_ERROR "EBADF:Failed to read image file"

/*
 * Load the raw image payload for a file, temp-file or shared-memory
 * transmission. The command's medium selects the source; the payload
 * is the base64 path or shared memory name.
 *
 * On success returns true and stores a malloc'd buffer (the caller
 * frees it) in *data, with its length in *len. On failure returns
 * false and sets *error to KITTY_FILE_ERROR.
 *
 * 'max_bytes' bounds the number of bytes read.
 */
bool kitty_file_load(
    const struct kitty_command *cmd,
    const char *payload, size_t payload_len,
    size_t max_bytes,
    uint8_t **data, size_t *len, const char **error);
