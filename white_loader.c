#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include "common.h"
#include "binary.h"
#include "find.h"
#include "cc.h"
#include "running_kernel.h"
#include "loader.h"
#include "link.h"

static struct binary kern;

int main(int argc, char **argv) {
    b_init(&kern);
    argv++;
    while(1) {
        char *arg = *argv++;
        if(!arg) goto usage;
        if(arg[0] != '-' || arg[1] == '\0' || arg[2] != '\0') goto usage;
        switch(arg[1]) {
        case 'k': {
            char *kern_fn;
            if(!(kern_fn = *argv++)) goto usage;
            b_load_macho(&kern, kern_fn, false);
            break;
        }
#ifdef IMG3_SUPPORT
        case 'i': {
            uint32_t key_bits;
            prange_t data = parse_img3_file(*argv++, &key_bits);
            prange_t key = parse_hex_string(*argv++);
            prange_t iv = parse_hex_string(*argv++);
            prange_t decompressed = decrypt_and_decompress(key_bits, key, iv, data);
            b_prange_load_macho(&kern, decompressed, false);
            break;
        }
#endif
#ifdef __APPLE__
        case 'l': {
            if(!kern.valid) goto usage;
            if(!*argv) goto usage;
            char *to_load_fn;
            while(to_load_fn = *argv++) {
                struct binary to_load;
                b_init(&to_load);
                b_load_macho(&to_load, to_load_fn, true);
                uint32_t slide = b_allocate_from_running_kernel(&to_load);
                if(!(to_load.mach_hdr->flags & MH_PREBOUND)) {
                    b_relocate(&to_load, &kern, slide);
                }
                b_inject_into_running_kernel(&to_load, b_find_sysent(&kern));
            }
            return 0;
        }
#endif
        case 'p': {
            if(!kern.valid) goto usage;
            if(!*argv) goto usage;
            char *to_load_fn, *output_fn;
            uint32_t slide = 0xf0000000;
            while(to_load_fn = *argv++) {
                if(!(output_fn = *argv++)) goto usage;
                struct binary to_load;
                b_init(&to_load);
                b_load_macho(&to_load, to_load_fn, true);
                if(!(to_load.mach_hdr->flags & MH_PREBOUND)) {
                    b_relocate(&to_load, &kern, slide);
                    slide += 0x10000;
                }
                to_load.mach_hdr->flags |= MH_PREBOUND;
                b_macho_store(&to_load, output_fn);
            }
            return 0;
        }
        case 'q': {
            if(!kern.valid) goto usage;
            char *out_kern = *argv++;
            if(!out_kern) goto usage;
            b_macho_store(&kern, out_kern);

            int fd = open(out_kern, O_RDWR);
            if(fd == -1) {
                edie("couldn't re-open output kc"); 
            }

            if(!*argv) goto usage;
            char *to_load_fn;
            while(to_load_fn = *argv++) {
                struct binary to_load;
                b_init(&to_load);
                b_load_macho(&to_load, to_load_fn, true);
                if(!(to_load.mach_hdr->flags & MH_PREBOUND)) {
                    b_relocate(&to_load, &kern, b_allocate_from_macho_fd(fd));
                }
                b_inject_into_macho_fd(&to_load, fd);
            }
            close(fd);

            return 0;
        }
#ifdef __APPLE__
        case 'u': {
            char *baseaddr_hex;
            if(!(baseaddr_hex = *argv++)) goto usage;
            unload_from_running_kernel(parse_hex_uint32(baseaddr_hex));
            return 0;
        }
#endif
        }
    }

    usage:
    printf("Usage: loader -k kern "
#ifdef __APPLE__
                                 "-l kcode.dylib                load\n"
           "                      "
#endif
                                 "-p kcode.dylib out.dylib      prelink\n"
           "                      -q out_kern kcode.dylib       insert into kc\n"
#ifdef __APPLE__
           "              -u f0000000                           unload\n"
#endif
           );
}

