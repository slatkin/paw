#include "kitty-file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_MODULE "kitty-file"
#define LOG_ENABLE_DBG 0
#include "base64.h"
#include "debug.h"
#include "kitty-image.h"
#include "log.h"
#include "macros.h"
#include "xmalloc.h"
#include "xsnprintf.h"

/*
 * Cap on the base64 name/path in the command payload. A normal path
 * fits in PATH_MAX, which base64 grows by 4/3.
 */
enum { KITTY_FILE_PAYLOAD_MAX = 8192 };

static bool
path_inside(const char *path, const char *dir)
{
    const size_t len = strlen(dir);
    return len > 0 &&
        strncmp(path, dir, len) == 0 &&
        path[len] == '/';
}

/*
 * Refuse the paths the spec names, after symlink resolution. /dev/shm
 * is the one allowed part of /dev (design D4).
 */
static bool
path_allowed(const char *resolved)
{
    if (path_inside(resolved, "/proc") || path_inside(resolved, "/sys"))
        return false;
    if (path_inside(resolved, "/dev"))
        return path_inside(resolved, "/dev/shm");
    return true;
}

/* The resolved path is inside /tmp, /dev/shm or $TMPDIR. */
static bool
path_in_temp_dir(const char *resolved)
{
    const char *candidates[] = {
        "/tmp",
        "/dev/shm",
        getenv("TMPDIR"),
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *dir = candidates[i];
        if (dir == NULL || dir[0] == '\0')
            continue;

        char *real = realpath(dir, NULL);
        if (real == NULL)
            continue;

        const bool inside = path_inside(resolved, real);
        free(real);
        if (inside)
            return true;
    }

    return false;
}

/*
 * The exact size of the raw pixel data for an uncompressed RGB or RGBA
 * command, when the dimensions are known. Returns false for PNG,
 * compressed data, and missing or invalid dimensions.
 */
static bool
expected_raw_size(const struct kitty_command *cmd, size_t *out)
{
    if (cmd->format == KITTY_FORMAT_PNG ||
        cmd->compression != KITTY_COMPRESSION_NONE ||
        !cmd->has_width || !cmd->has_height ||
        cmd->width == 0 || cmd->height == 0)
    {
        return false;
    }

    const uint32_t bpp = cmd->format == KITTY_FORMAT_RGB ? 3 : 4;
    return kitty_checked_size(cmd->width, cmd->height, bpp, SIZE_MAX, out);
}

/*
 * Compute how many bytes to copy from the object. Returns false when
 * the object is shorter than O + S (spec: File too short). Sets
 * '*insufficient' when the uncompressed pixel data is shorter than the
 * dimensions require, which is the file error too (spec: shm short
 * data; this matches kitty).
 */
static bool
compute_read_size(
    const struct kitty_command *cmd, uint64_t object_size, size_t max_bytes,
    uint64_t *want, bool *insufficient)
{
    const uint64_t offset = cmd->has_offset ? cmd->offset : 0;
    const uint64_t size = cmd->has_size ? cmd->size : 0;

    /* Spec: a file shorter than O + S fails. */
    if (size > 0 && object_size < offset + size)
        return false;

    const uint64_t avail = object_size > offset ? object_size - offset : 0;
    uint64_t n = size > 0 ? size : avail;

    size_t expected = 0;
    if (expected_raw_size(cmd, &expected) && expected <= max_bytes) {
        if (n > expected)
            n = expected;
        if (n < expected)
            *insufficient = true;
    }

    /* Bound the allocation by the quota (the cap on file size too). */
    if (n > max_bytes)
        n = max_bytes;

    *want = n;
    return true;
}

static bool
load_regular_file(
    const struct kitty_command *cmd, const char *name, size_t max_bytes,
    uint8_t **data, size_t *len)
{
    bool ok = false;
    bool opened = false;
    int fd = -1;
    char *resolved = NULL;
    uint8_t *buf = NULL;

    *data = NULL;
    *len = 0;

    resolved = realpath(name, NULL);
    if (resolved == NULL) {
        LOG_DBG("realpath(%s) failed: %s", name, strerror(errno));
        goto out;
    }

    if (!path_allowed(resolved)) {
        LOG_DBG("refusing to read sensitive path: %s", resolved);
        goto out;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        LOG_DBG("not a regular file: %s", resolved);
        goto out;
    }

    /* O_NONBLOCK so that a FIFO can never block the open. */
    fd = open(resolved,
              O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        LOG_DBG("open(%s) failed: %s", resolved, strerror(errno));
        goto out;
    }
    opened = true;

    /* Check again, now that the file is open. */
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) {
        LOG_DBG("open file is not regular: %s", resolved);
        goto out;
    }

    uint64_t want = 0;
    bool insufficient = false;
    if (!compute_read_size(cmd, (uint64_t)fst.st_size, max_bytes,
                           &want, &insufficient))
    {
        LOG_DBG("file too short: %s", resolved);
        goto out;
    }
    if (insufficient) {
        LOG_DBG("insufficient image data: %s", resolved);
        goto out;
    }

    buf = xmalloc(want > 0 ? (size_t)want : 1);
    size_t got = 0;
    const uint64_t offset = cmd->has_offset ? cmd->offset : 0;

    while (got < want) {
        const ssize_t n = pread(
            fd, buf + got, (size_t)(want - got), (off_t)(offset + got));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG_DBG("read(%s) failed: %s", resolved, strerror(errno));
            free(buf);
            buf = NULL;
            goto out;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }

    if (got < want) {
        LOG_DBG("short read: %s", resolved);
        free(buf);
        buf = NULL;
        goto out;
    }

    *data = buf;
    *len = got;
    buf = NULL;
    ok = true;

out:
    if (fd >= 0)
        close(fd);

    /*
     * A temp file is deleted after the read attempt, when the
     * resolved path is inside a temp directory and holds the marker
     * text (spec: Temp file transmission).
     */
    if (opened &&
        cmd->medium == KITTY_MEDIUM_TEMP_FILE &&
        strstr(resolved, "tty-graphics-protocol") != NULL &&
        path_in_temp_dir(resolved))
    {
        if (unlink(resolved) != 0)
            LOG_DBG("unlink(%s) failed: %s", resolved, strerror(errno));
    }

    free(buf);
    free(resolved);
    return ok;
}

static bool
load_shm(
    const struct kitty_command *cmd, const char *name, size_t max_bytes,
    uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;

    const size_t name_len = strlen(name);
    if (name[0] != '/' ||
        strchr(name + 1, '/') != NULL ||
        name_len > NAME_MAX)
    {
        LOG_DBG("invalid shm name: %s", name);
        return false;
    }

    bool ok = false;
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        LOG_DBG("shm_open(%s) failed: %s", name, strerror(errno));
        goto out;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        LOG_DBG("fstat(%s) failed: %s", name, strerror(errno));
        goto out;
    }

    uint64_t want = 0;
    bool insufficient = false;
    if (!compute_read_size(cmd, (uint64_t)st.st_size, max_bytes,
                           &want, &insufficient))
    {
        LOG_DBG("shm object too short: %s", name);
        goto out;
    }
    if (insufficient) {
        LOG_DBG("insufficient image data: %s", name);
        goto out;
    }

    if (want > 0) {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0)
            page = 4096;

        const uint64_t offset = cmd->has_offset ? cmd->offset : 0;
        const uint64_t page_start =
            offset - (offset % (uint64_t)page);
        const size_t delta = (size_t)(offset - page_start);
        const size_t map_len = (size_t)want + delta;

        void *addr = mmap(
            NULL, map_len, PROT_READ, MAP_SHARED, fd, (off_t)page_start);
        if (addr == MAP_FAILED) {
            LOG_DBG("mmap(%s) failed: %s", name, strerror(errno));
            goto out;
        }

        uint8_t *buf = xmalloc((size_t)want);
        memcpy(buf, (uint8_t *)addr + delta, (size_t)want);
        munmap(addr, map_len);

        *data = buf;
        *len = (size_t)want;
        ok = true;
    }

out:
    if (fd >= 0)
        close(fd);

    /* After a valid name, the object is unlinked on every path. */
    if (shm_unlink(name) != 0)
        LOG_DBG("shm_unlink(%s) failed: %s", name, strerror(errno));

    return ok;
}

bool
kitty_file_load(
    const struct kitty_command *cmd,
    const char *payload, size_t payload_len,
    size_t max_bytes,
    uint8_t **data, size_t *len, const char **error)
{
    *data = NULL;
    *len = 0;
    *error = KITTY_FILE_ERROR;

    if (payload_len == 0 || payload_len > KITTY_FILE_PAYLOAD_MAX)
        return false;

    size_t name_len = 0;
    char *name = base64_decode(payload, &name_len);
    if (name == NULL) {
        LOG_DBG("invalid base64 name");
        return false;
    }

    /* A path or name cannot contain an embedded NUL. */
    if (strlen(name) != name_len) {
        free(name);
        return false;
    }

    bool ok;
    switch (cmd->medium) {
    case KITTY_MEDIUM_FILE:
    case KITTY_MEDIUM_TEMP_FILE:
        ok = load_regular_file(cmd, name, max_bytes, data, len);
        break;

    case KITTY_MEDIUM_SHM:
        ok = load_shm(cmd, name, max_bytes, data, len);
        break;

    default:
        ok = false;
        break;
    }

    free(name);
    return ok;
}

/*
 * Build a base64 payload for a test path. base64_encode() requires a
 * multiple of three bytes, so the tail goes through
 * base64_encode_final().
 */
static char * UNUSED
test_b64(const char *s)
{
    const size_t len = strlen(s);
    const size_t head = len - len % 3;

    char *encoded = base64_encode((const uint8_t *)s, head);
    if (len % 3 == 0)
        return encoded;

    char tail[4];
    base64_encode_final((const uint8_t *)s + head, len % 3, tail);

    char *full = xmalloc(strlen(encoded) + 5);
    memcpy(full, encoded, strlen(encoded));
    memcpy(full + strlen(encoded), tail, 4);
    full[strlen(encoded) + 4] = '\0';
    free(encoded);
    return full;
}

static bool UNUSED
test_load(
    const struct kitty_command *cmd, const char *name,
    uint8_t **data, size_t *len, const char **error)
{
    char *payload = test_b64(name);
    const bool ok = kitty_file_load(
        cmd, payload, strlen(payload), 4u * 1024 * 1024, data, len, error);
    free(payload);
    return ok;
}

static void UNUSED
test_cmd_init(
    struct kitty_command *cmd, enum kitty_medium medium,
    enum kitty_format format, uint32_t width, uint32_t height)
{
    kitty_command_init(cmd);
    cmd->medium = medium;
    cmd->format = format;
    cmd->has_width = cmd->has_height = true;
    cmd->width = width;
    cmd->height = height;
}

/* Create a unique file with the given contents. Returns the open fd or -1. */
static int UNUSED
test_make_file(char *template, const void *contents, size_t len)
{
    int fd = mkstemp(template);
    if (fd < 0)
        return -1;

    if (len > 0 && write(fd, contents, len) != (ssize_t)len) {
        close(fd);
        unlink(template);
        return -1;
    }

    return fd;
}

UNITTEST
{
    static const uint8_t rgb[12] = "abcdefghijkl";

    char path[] = "/tmp/paw-file-XXXXXX";
    int fd = test_make_file(path, rgb, sizeof(rgb));
    xassert(fd >= 0);
    close(fd);

    struct kitty_command cmd;
    test_cmd_init(&cmd, KITTY_MEDIUM_FILE, KITTY_FORMAT_RGB, 2, 2);

    uint8_t *data = NULL;
    size_t len = 0;
    const char *error = NULL;
    xassert(test_load(&cmd, path, &data, &len, &error));
    xassert(len == sizeof(rgb));
    xassert(memcmp(data, rgb, sizeof(rgb)) == 0);
    free(data);
    unlink(path);

    /* A regular file, but shorter than O + S. */
    char short_path[] = "/tmp/paw-short-XXXXXX";
    fd = test_make_file(short_path, rgb, 5);
    xassert(fd >= 0);
    close(fd);

    cmd.has_size = true;
    cmd.size = 100;
    data = NULL;
    len = 0;
    error = NULL;
    xassert(!test_load(&cmd, short_path, &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
    xassert(data == NULL);
    unlink(short_path);
}

UNITTEST
{
    struct kitty_command cmd;
    test_cmd_init(&cmd, KITTY_MEDIUM_FILE, KITTY_FORMAT_RGB, 2, 2);

    uint8_t *data = NULL;
    size_t len = 0;
    const char *error = NULL;

    /* Device file. */
    xassert(!test_load(&cmd, "/dev/zero", &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
    xassert(data == NULL);

    /* Symlink to a sensitive path. */
    char link[] = "/tmp/paw-link-XXXXXX";
    int fd = test_make_file(link, "", 0);
    xassert(fd >= 0);
    close(fd);
    unlink(link);
    xassert(symlink("/proc/self/environ", link) == 0);
    error = NULL;
    xassert(!test_load(&cmd, link, &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
    unlink(link);

    /* FIFO: must not block. */
    char fifo[] = "/tmp/paw-fifo-XXXXXX";
    fd = test_make_file(fifo, "", 0);
    xassert(fd >= 0);
    close(fd);
    unlink(fifo);
    xassert(mkfifo(fifo, S_IRUSR | S_IWUSR) == 0);
    error = NULL;
    xassert(!test_load(&cmd, fifo, &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
    unlink(fifo);

    /* Symlink loop. */
    char loop_a[] = "/tmp/paw-loop-a-XXXXXX";
    char loop_b[] = "/tmp/paw-loop-b-XXXXXX";
    fd = test_make_file(loop_a, "", 0);
    xassert(fd >= 0);
    close(fd);
    fd = test_make_file(loop_b, "", 0);
    xassert(fd >= 0);
    close(fd);
    unlink(loop_a);
    unlink(loop_b);
    xassert(symlink(loop_b, loop_a) == 0);
    xassert(symlink(loop_a, loop_b) == 0);
    error = NULL;
    xassert(!test_load(&cmd, loop_a, &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
    unlink(loop_a);
    unlink(loop_b);

    /* Missing file. */
    char missing[PATH_MAX];
    xsnprintf(missing, sizeof(missing), "/tmp/paw-missing-%d", (int)getpid());
    error = NULL;
    xassert(!test_load(&cmd, missing, &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
}

UNITTEST
{
    static const uint8_t rgb[12] = "abcdefghijkl";
    char path[PATH_MAX];

    struct kitty_command cmd;
    test_cmd_init(
        &cmd, KITTY_MEDIUM_TEMP_FILE, KITTY_FORMAT_RGB, 2, 2);

    uint8_t *data = NULL;
    size_t len = 0;
    const char *error = NULL;

    /* Inside /tmp, with the marker text: deleted. */
    xsnprintf(path, sizeof(path),
              "/tmp/tty-graphics-protocol-%d", (int)getpid());
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    xassert(fd >= 0);
    xassert(write(fd, rgb, sizeof(rgb)) == (ssize_t)sizeof(rgb));
    close(fd);

    xassert(test_load(&cmd, path, &data, &len, &error));
    free(data);
    xassert(access(path, F_OK) != 0);

    /* Inside /tmp, without the marker text: kept. */
    xsnprintf(path, sizeof(path), "/tmp/photo-%d", (int)getpid());
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    xassert(fd >= 0);
    xassert(write(fd, rgb, sizeof(rgb)) == (ssize_t)sizeof(rgb));
    close(fd);

    data = NULL;
    len = 0;
    error = NULL;
    xassert(test_load(&cmd, path, &data, &len, &error));
    free(data);
    xassert(access(path, F_OK) == 0);
    unlink(path);

    /* The marker text outside a temp directory: kept. */
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        xsnprintf(path, sizeof(path),
                  "%s/tty-graphics-protocol-%d", home, (int)getpid());
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            xassert(write(fd, rgb, sizeof(rgb)) == (ssize_t)sizeof(rgb));
            close(fd);

            data = NULL;
            len = 0;
            error = NULL;
            xassert(test_load(&cmd, path, &data, &len, &error));
            free(data);
            xassert(access(path, F_OK) == 0);
            unlink(path);
        }
    }
}

UNITTEST
{
    struct kitty_command cmd;
    test_cmd_init(&cmd, KITTY_MEDIUM_SHM, KITTY_FORMAT_RGBA, 2, 2);

    char name[64];
    xsnprintf(name, sizeof(name), "/paw-unittest-%d", (int)getpid());

    uint8_t *data = NULL;
    size_t len = 0;
    const char *error = NULL;

    /* A valid object: read, then unlinked. */
    static const uint8_t rgba[16] = {0};
    int fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    xassert(fd >= 0);
    xassert(write(fd, rgba, sizeof(rgba)) == (ssize_t)sizeof(rgba));
    close(fd);

    xassert(test_load(&cmd, name, &data, &len, &error));
    xassert(len == sizeof(rgba));
    free(data);
    xassert(shm_unlink(name) == -1 && errno == ENOENT);

    /* Too little data for the dimensions: error, and unlinked. */
    char short_name[64];
    xsnprintf(short_name, sizeof(short_name),
              "/paw-unittest-short-%d", (int)getpid());
    fd = shm_open(short_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    xassert(fd >= 0);
    xassert(write(fd, rgba, 4) == 4);
    close(fd);

    data = NULL;
    len = 0;
    error = NULL;
    xassert(!test_load(&cmd, short_name, &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
    xassert(shm_unlink(short_name) == -1 && errno == ENOENT);

    /* An invalid name: error, and nothing unlinked. */
    error = NULL;
    xassert(!test_load(&cmd, "/a/b", &data, &len, &error));
    xassert(error != NULL && strcmp(error, KITTY_FILE_ERROR) == 0);
}
