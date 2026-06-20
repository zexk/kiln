#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define FS_MKDIR(p) _mkdir(p)
#else
#  define FS_MKDIR(p) mkdir(p, 0755)
#endif

char *fs_read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[fs] cannot open '%s'\n", path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);

    if (size_out) {
        *size_out = (size_t)len;
    }
    return buf;
}

bool fs_write_atomic(const char *path, bool (*fn)(FILE *, void *), void *ctx) {
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        fprintf(stderr, "[fs] path too long: '%s'\n", path);
        return false;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[fs] cannot create '%s'\n", tmp);
        return false;
    }

    if (!fn(f, ctx)) {
        fclose(f);
        remove(tmp);
        return false;
    }

    if (fclose(f) != 0) {
        remove(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        remove(tmp);
        fprintf(stderr, "[fs] rename '%s' -> '%s' failed\n", tmp, path);
        return false;
    }

    return true;
}

void fs_path_join(char *out, size_t cap, const char *base, const char *rel) {
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') {
        blen--;
    }
    while (*rel == '/') {
        rel++;
    }
    snprintf(out, cap, "%.*s/%s", (int)blen, base, rel);
}

bool fs_write(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = (size == 0 || fwrite(data, 1, size, f) == size);
    fclose(f);
    return ok;
}

bool fs_mkdirs(const char *path) {
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return false;

    /* strip trailing slash */
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (FS_MKDIR(tmp) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (FS_MKDIR(tmp) != 0 && errno != EEXIST) return false;

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* --- little-endian binary serialization ---------------------------------- */

void fs_write_u8(uint8_t *buf, size_t *off, uint8_t v) {
    buf[(*off)++] = v;
}

void fs_write_u16(uint8_t *buf, size_t *off, uint16_t v) {
    buf[(*off)++] = (uint8_t)(v);
    buf[(*off)++] = (uint8_t)(v >> 8);
}

void fs_write_u32(uint8_t *buf, size_t *off, uint32_t v) {
    buf[(*off)++] = (uint8_t)(v);
    buf[(*off)++] = (uint8_t)(v >> 8);
    buf[(*off)++] = (uint8_t)(v >> 16);
    buf[(*off)++] = (uint8_t)(v >> 24);
}

void fs_write_i32(uint8_t *buf, size_t *off, int32_t v) {
    fs_write_u32(buf, off, (uint32_t)v);
}

uint8_t fs_read_u8(const uint8_t *buf, size_t *off) {
    return buf[(*off)++];
}

uint16_t fs_read_u16(const uint8_t *buf, size_t *off) {
    uint16_t v = (uint16_t)(buf[*off]) | ((uint16_t)(buf[*off + 1]) << 8);
    *off += 2;
    return v;
}

uint32_t fs_read_u32(const uint8_t *buf, size_t *off) {
    uint32_t v = (uint32_t)(buf[*off])
               | ((uint32_t)(buf[*off + 1]) << 8)
               | ((uint32_t)(buf[*off + 2]) << 16)
               | ((uint32_t)(buf[*off + 3]) << 24);
    *off += 4;
    return v;
}

int32_t fs_read_i32(const uint8_t *buf, size_t *off) {
    return (int32_t)fs_read_u32(buf, off);
}
