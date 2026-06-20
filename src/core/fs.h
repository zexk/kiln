#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Read the entire file at path into a malloc'd, null-terminated buffer.
   *size_out receives the byte count (excluding the terminator).
   Returns NULL on error; caller frees with free(). */
char *fs_read_file(const char *path, size_t *size_out);

/* Atomically overwrite path by staging through a sibling .tmp file.
   fn(f, ctx) must write the payload to f and return true on success.
   If fn returns false or any I/O step fails, the original file is untouched
   and the temp file is cleaned up. */
bool fs_write_atomic(const char *path, bool (*fn)(FILE *, void *), void *ctx);

/* Join base and rel with a '/' separator into out[cap].
   Strips trailing slashes from base and leading slashes from rel. */
void fs_path_join(char *out, size_t cap, const char *base, const char *rel);
