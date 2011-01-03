#pragma once
#include "common.h"
struct binary;
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
// Specify align as 0 if you only expect to find it at one place.
addr_t find_data(range_t range, char *to_find, int align, bool must_find);
addr_t find_string(range_t range, const char *string, int align, bool must_find);
addr_t find_bytes(range_t range, const char *bytes, size_t len, int align, bool must_find);
addr_t find_int32(range_t range, uint32_t number, bool must_find);

// helper functions
addr_t find_bof(range_t range, addr_t eof, bool is_thumb);
uint32_t resolve_ldr(struct binary *binary, addr_t addr);

addr_t find_bl(range_t *range);

addr_t b_find_anywhere(struct binary *binary, char *to_find, int align, bool must_find);
