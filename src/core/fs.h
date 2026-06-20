#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Read the entire file at path into a malloc'd, null-terminated buffer.
   *size_out receives the byte count (excluding the terminator).
   Returns NULL on error; caller frees with free(). */
char *fs_read_file(const char *path, size_t *size_out);

/* Write data to path, overwriting any existing file. */
bool fs_write(const char *path, const void *data, size_t size);

/* Atomically overwrite path by staging through a sibling .tmp file.
   fn(f, ctx) must write the payload to f and return true on success.
   If fn returns false or any I/O step fails, the original file is untouched
   and the temp file is cleaned up. */
bool fs_write_atomic(const char *path, bool (*fn)(FILE *, void *), void *ctx);

/* Create path and all missing parent directories (like mkdir -p).
   Returns true if the directory exists afterwards. */
bool fs_mkdirs(const char *path);

/* Join base and rel with a '/' separator into out[cap].
   Strips trailing slashes from base and leading slashes from rel. */
void fs_path_join(char *out, size_t cap, const char *base, const char *rel);

/* -------------------------------------------------------------------------
   Little-endian binary serialization helpers for packed binary formats.
   Each write/read advances *offset by the field width.
   ------------------------------------------------------------------------- */

void     fs_write_u8 (uint8_t *buf, size_t *off, uint8_t  v);
void     fs_write_u16(uint8_t *buf, size_t *off, uint16_t v);
void     fs_write_u32(uint8_t *buf, size_t *off, uint32_t v);
void     fs_write_i32(uint8_t *buf, size_t *off, int32_t  v);

uint8_t  fs_read_u8 (const uint8_t *buf, size_t *off);
uint16_t fs_read_u16(const uint8_t *buf, size_t *off);
uint32_t fs_read_u32(const uint8_t *buf, size_t *off);
int32_t  fs_read_i32(const uint8_t *buf, size_t *off);
