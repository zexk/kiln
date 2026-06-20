#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
