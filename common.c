#include "common.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <setjmp.h>

prange_t pdup(prange_t range) {
    void *buf = malloc(range.size);
    memcpy(buf, range.start, range.size);
    return (prange_t) {buf, range.size};
}

void pfree(prange_t range) {
    free(range.start);
}

void punmap(prange_t range) {
    munmap(range.start, range.size);
}

void check_range_has_addr(range_t range, addr_t addr) {
    if(addr < range.start || addr >= (range.start + range.size)) {
        die("bad address 0x%08x (not in range %08x-%08x)", addr, range.start, range.start + (uint32_t) range.size);
    }
}
    
bool is_valid_range(prange_t range) {
    char c;
    return !mincore(range.start, range.size, (void *) &c);
}

static inline char parse_hex_digit(char digit, const char *string) {
    switch(digit) {
    case '0' ... '9':
        return digit - '0';
    case 'a' ... 'f':
        return 10 + (digit - 'a');
    default:
        die("bad hex string %s", string);
    }
}

prange_t parse_hex_string(const char *string) {
    if(string[0] == '0' && string[1] == 'x') {
        string += 2;
    }
    const char *in = string;
    size_t len = strlen(string);
    size_t out_len = (len + 1)/2;
    uint8_t *out = malloc(out_len);
    prange_t result = (prange_t) {out, out_len};
    if(len % 2) {
        *out++ = parse_hex_digit(*in++, string);
    }
    while(out_len--) {
        uint8_t a = parse_hex_digit(*in++, string);
        uint8_t b = parse_hex_digit(*in++, string);
        *out++ = (a * 0x10) + b;
    }
    return result;
}

uint32_t parse_hex_uint32(const char *string) {
    char *end;
    uint32_t result = (uint32_t) strtoll(string, &end, 16);
    if(!*string || *end) {
        die("invalid hex value %x", string);
    }
    return result;
}

prange_t load_file(const char *filename, bool rw, mode_t *mode) {
#define _arg filename
    if(mode) {
        struct stat st;
        if(lstat(filename, &st)) {
            edie("could not lstat");
        }
        *mode = st.st_mode;
    }
    int fd = open(filename, O_RDONLY);
    if(fd == -1) {
        edie("could not open");
    }
    return load_fd(fd, rw);
#undef _arg
}

prange_t load_fd(int fd, bool rw) {
    off_t end = lseek(fd, 0, SEEK_END);
    if(end == 0) {
        fprintf(stderr, "load_fd: warning: mapping an empty file\n");
    }
    if(sizeof(off_t) > sizeof(size_t) && end > (off_t) SIZE_MAX) {
        die("too big: %lld", (long long) end);
    }
    void *buf = mmap(NULL, (size_t) end, PROT_READ | (rw ? PROT_WRITE : 0), MAP_PRIVATE, fd, 0);
    if(buf == MAP_FAILED) {
        edie("could not mmap buf (end=%zd)", (size_t) end);
    }
    return (prange_t) {buf, (size_t) end};
}

void store_file(prange_t range, const char *filename, mode_t mode) {
#define _arg filename
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if(fd == -1) {
        edie("could not open");
    }
    if(write(fd, range.start, range.size) != (ssize_t) range.size) {
        edie("could not write data");
    }
    close(fd);
#undef _arg
}

static jmp_buf die_handler;
static bool die_handler_valid;
static char die_message[256];

void _die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    if(die_handler_valid) {
        die_handler_valid = false;
        vsnprintf(die_message, sizeof(die_message), fmt, ap);
        longjmp(die_handler, -1);
    } else {
        vfprintf(stderr, fmt, ap);
        abort();
    }

    va_end(ap);
}

const char *data_try(void (*func)()) {
    if(setjmp(die_handler)) {
        return die_message;
    } else {
        die_handler_valid = true;
        func();
        die_handler_valid = false;
        return NULL;
    }
}
