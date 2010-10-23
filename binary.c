#define BINARY_C
#include "common.h"
#include "binary.h"
#include "loader.h"
#include "nlist.h"
#include "fat.h"
#include "dyld_cache_format.h"
#ifdef __APPLE__
#include <mach/mach.h>
#endif

const int desired_cputype = 12; // ARM
const int desired_cpusubtype = 0; // v7=9, v6=6

void b_init(struct binary *binary) {
    memset(binary, 0, sizeof(*binary));
}

static void *reserve_memory() {
    void *result = mmap(NULL, 0x10000000, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if(result == MAP_FAILED) {
        edie("could not do so");
    }
    return result;
}

void b_macho_load_symbols(struct binary *binary) {
    CMD_ITERATE(binary->mach_hdr, cmd) {
        if(cmd->cmd == LC_SYMTAB) {
            struct symtab_command *scmd = (void *) cmd;
            if(scmd->nsyms >= 0x1000000) {
                die("ridiculous number of symbols (%u)", scmd->nsyms);
            }
            binary->nsyms = scmd->nsyms;
            binary->strsize = scmd->strsize;
            binary->symtab = macho_rangeconv_off((range_t) {binary, scmd->symoff, scmd->nsyms * sizeof(struct nlist)}).start;
            binary->strtab = macho_rangeconv_off((range_t) {binary, scmd->stroff, scmd->strsize}).start;
        } else if(cmd->cmd == LC_DYSYMTAB) {
            binary->dysymtab = (void *) cmd;
        } else if(cmd->cmd == LC_DYLD_INFO_ONLY) {
            fprintf(stderr, "b_load_symbols: warning: file is fancy, symbols might be missing\n");
        }
    }
    if(binary->symtab && binary->dysymtab) {
        uint32_t iextdefsym = binary->dysymtab->iextdefsym;
        uint32_t nextdefsym = binary->dysymtab->nextdefsym;
        if(iextdefsym >= binary->nsyms) {
            die("bad iextdefsym (%u)", iextdefsym);
        }
        if(nextdefsym > binary->nsyms - iextdefsym) {
            die("bad nextdefsym (%u)", nextdefsym);
        }
        binary->ext_symtab = binary->symtab + iextdefsym;
        binary->ext_nsyms = nextdefsym;
    } else {
        binary->ext_symtab = binary->symtab;
        binary->ext_nsyms = binary->nsyms;
    }
}

void b_load_dyldcache(struct binary *binary, const char *path, bool pre_loaded) {
#define _arg path
    int fd = open(path, O_RDONLY);
    if(fd == -1) { 
        edie("could not open");
    }
    binary->dyld_hdr = malloc(sizeof(*binary->dyld_hdr));
    if(read(fd, binary->dyld_hdr, sizeof(binary->dyld_hdr)) != sizeof(binary->dyld_hdr)) {
        die("truncated");
    }
    if(memcmp(binary->dyld_hdr->magic, "dyld", 4)) {
        die("not a dyld cache");
    }
    char *thing = binary->dyld_hdr->magic + sizeof(binary->dyld_hdr->magic) - 7;
    if(!memcmp(thing, " armv7", 7)) {
        binary->actual_cpusubtype = 9;
    } else if(!memcmp(thing, " armv6", 7)) {
        binary->actual_cpusubtype = 6;
    } else {
        die("unknown processor in magic: %.6s", thing);
    }

    if(binary->dyld_hdr->mappingCount > 1000) {
        die("insane mapping count: %u", binary->dyld_hdr->mappingCount);
    }
    binary->dyld_mapping_count = binary->dyld_hdr->mappingCount;
    size_t sz = binary->dyld_mapping_count * sizeof(struct shared_file_mapping_np);
    binary->dyld_mappings = malloc(sz);
    if(pread(fd, binary->dyld_mappings, sz, binary->dyld_hdr->mappingOffset) != sz) {
        edie("could not read mappings");
    }
    if(pre_loaded) {
        binary->load_base = (void *) 0x30000000;
        // verify!
        for(int i = 0; i < binary->dyld_mapping_count; i++) {
            struct shared_file_mapping_np mapping = binary->dyld_mappings[i];
            prange_t pr = rangeconv_checkof((range_t) {binary, mapping.sfm_address, mapping.sfm_size});
            if(!is_valid_range(pr)) {
                die("segment %d wasn't found in memory at %p", i, pr.start);
            }
        }
    } else {
        binary->load_base = reserve_memory();
        for(int i = 0; i < binary->dyld_mapping_count; i++) {
            struct shared_file_mapping_np mapping = binary->dyld_mappings[i];
            prange_t pr = rangeconv_checkof((range_t) {binary, (addr_t) mapping.sfm_address, (size_t) mapping.sfm_size});
            if(mmap(pr.start, pr.size, PROT_READ, MAP_SHARED | MAP_FIXED, fd, (off_t) mapping.sfm_file_offset) == MAP_FAILED) {
                edie("could not map segment %d of this crappy binary format", i);
            }
        }
    }
    binary->dyld_fd = fd;
#undef _arg
}

range_t b_dyldcache_nth_segment(const struct binary *binary, int n) {
    if(n < binary->dyld_mapping_count) {
        return (range_t) {binary, (addr_t) binary->dyld_mappings[n].sfm_address, (size_t) binary->dyld_mappings[n].sfm_size};
    } else {
        return (range_t) {binary, 0, 0};
    }
}

void b_dyldcache_load_macho(struct binary *binary, const char *filename) {
    if(binary->dyld_hdr->imagesCount > 1000) {
        die("insane images count");
    }
    for(int i = 0; i < binary->dyld_hdr->imagesCount; i++) {
        struct dyld_cache_image_info info;
        if(pread(binary->dyld_fd, &info, sizeof(info), binary->dyld_hdr->imagesOffset + i * sizeof(struct dyld_cache_image_info)) != sizeof(info)) {
            die("could not read image");
        }
        char name[128];
        if(pread(binary->dyld_fd, name, sizeof(name), info.pathFileOffset) <= 0) {
            die("could not read image name");
        }
        if(strncmp(name, filename, 128)) {
            continue;
        }
        // we found it
        binary->mach_hdr = macho_rangeconv((range_t) {binary, (addr_t) info.address, 0x1000}).start;
        break;
    }
    b_macho_load_symbols(binary);
}

void b_load_macho(struct binary *binary, const char *path) {
#define _arg path
    int fd = open(path, O_RDONLY);
    if(fd == -1) { 
        edie("could not open");
    }
    void *fhdr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if(fhdr == MAP_FAILED) {
        edie("could not map file header");
    }
    uint32_t magic = *(uint32_t *)fhdr;
    uint32_t fat_offset;
    if(magic == MH_MAGIC) {
        // thin file
        binary->mach_hdr = fhdr;
        if(binary->mach_hdr->cputype != desired_cputype || (binary->mach_hdr->cpusubtype != 0 && desired_cpusubtype != 0 && binary->mach_hdr->cpusubtype != desired_cpusubtype)) {
            die("thin file doesn't have the right architecture");
        }
        fat_offset = 0;
    } else if(magic == FAT_MAGIC) {
        if(desired_cpusubtype == 0) {
            die("fat, but we don't even know what we want (desired_cpusubtype == 0)");
        }
        struct fat_header *fathdr = fhdr;
        struct fat_arch *arch = (void *)(fathdr + 1);
        uint32_t nfat_arch = fathdr->nfat_arch;
        if(sizeof(struct fat_header) + nfat_arch * sizeof(struct fat_arch) >= 4096) {
            die("fat header is too big");
        }
        while(nfat_arch--) {
            if(arch->cputype == desired_cputype && (arch->cpusubtype == 0 || arch->cpusubtype == desired_cpusubtype)) {
                munmap(fhdr, 4096);
                fat_offset = arch->offset;
                binary->mach_hdr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, fat_offset);
                if(binary->mach_hdr == MAP_FAILED) {
                    edie("could not map mach-o header from fat file", path);
                }
                break;
            }
            arch++;
        }
    } else {
        die("(%08x) what is this I don't even", magic);
    }

    binary->actual_cpusubtype = binary->mach_hdr->cpusubtype;

    if(binary->mach_hdr->sizeofcmds > 4096 - sizeof(*binary->mach_hdr)) {
        die("sizeofcmds is too big");
    }

    binary->symtab = NULL;

    binary->load_base = reserve_memory();

    CMD_ITERATE(binary->mach_hdr, cmd) {
        if(cmd->cmd == LC_SEGMENT) {
            struct segment_command *scmd = (void *) cmd;
            if(scmd->vmsize == 0) scmd->filesize = 0; // __CTF
            if(scmd->filesize != 0) {
                prange_t pr = rangeconv_checkof((range_t) {binary, scmd->vmaddr, scmd->filesize});
                
                if(mmap(pr.start, pr.size, PROT_READ, MAP_SHARED | MAP_FIXED, fd, fat_offset + scmd->fileoff) == MAP_FAILED) {
                    edie("could not map segment %.16s at %u+%u,%u", scmd->segname, scmd->fileoff, fat_offset, scmd->filesize);
                }
            }
        }
    }
    
    b_macho_load_symbols(binary);
#undef _arg
}

void b_running_kernel_load_macho(struct binary *binary) {
#ifdef __APPLE__
    kern_return_t kr;

    mach_port_name_t kernel_task;
    kr = task_for_pid(mach_task_self(), 0, &kernel_task);
    if(kr) {
        die("task_for_pid failed.  u probably need kernel patches. kr=%d", kr);
    }

    kr = vm_allocate(mach_task_self(), (vm_address_t *) &binary->mach_hdr, 0x1000, true);
    if(kr) {
        die("vm_allocate mach_hdr failed");
    }
    addr_t mh_addr;
    vm_size_t size;
    for(addr_t hugebase = 0x80000000; hugebase; hugebase += 0x40000000) {
        for(addr_t pagebase = 0x1000; pagebase < 0x10000; pagebase += 0x1000) {
            // vm read, compare to MH_MAGIC, hurf durf
            mh_addr = (vm_address_t) (hugebase + pagebase);
            size = 0x1000;
            // This will return either KERN_PROTECTION_FAILURE if it's a good address, and KERN_INVALID_ADDRESS otherwise.
            // But if we use a shorter size, it will read if it's a good address, and /crash/ otherwise.
            // So we do two.
            kr = vm_read_overwrite(kernel_task, (vm_address_t) mh_addr, size, (vm_address_t) binary->mach_hdr, &size);
            if(kr == KERN_INVALID_ADDRESS) {
                continue;
            } else if(kr && kr != KERN_PROTECTION_FAILURE) {
                die("unexpected error from vm_read_overwrite: %d", kr);
            }
            // ok, it's valid, but is it the actual header?
            size = 0xfff;
            kr = vm_read_overwrite(kernel_task, (vm_address_t) mh_addr, size, (vm_address_t) binary->mach_hdr, &size);
            if(kr) {
                die("second vm_read_overwrite failed: %d", kr);
            }
            if(binary->mach_hdr->magic == MH_MAGIC) {
                printf("found running kernel at 0x%08x\n", mh_addr);
                goto ok;
            }
        }
    }
    die("didn't find the kernel anywhere");

    ok:;

    binary->actual_cpusubtype = binary->mach_hdr->cpusubtype;

    if(binary->mach_hdr->sizeofcmds > size - sizeof(*binary->mach_hdr)) {
        die("sizeofcmds is too big");
    }
    addr_t maxaddr = mh_addr;
    CMD_ITERATE(binary->mach_hdr, cmd) {
        if(cmd->cmd == LC_SEGMENT) {
            struct segment_command *scmd = (void *) cmd;
            addr_t newmax = scmd->vmaddr + scmd->filesize;
            if(newmax > maxaddr) maxaddr = newmax;
        }
    }

    // Well, uh, this sucks.  But there's some block on reading.  In fact, it's probably a bug that this works.
    size_t read_size = maxaddr - mh_addr;
    char *p = malloc(read_size);
    binary->load_base = p - 0x1000;
#ifdef PROFILING
    clock_t a = clock();
#endif
    while(read_size > 0) {
        vm_size_t this_size = (vm_size_t) read_size;
        if(this_size > 0xfff) this_size = 0xfff;
        kr = vm_read_overwrite(kernel_task, (vm_address_t) mh_addr, this_size, (vm_address_t) p, &this_size);
        if(kr) {
            die("vm_read_overwrite failed: %d", kr);
        }
        mh_addr += this_size;
        p += this_size;
        read_size -= this_size;
    }
#ifdef PROFILING
    clock_t b = clock();
    printf("it took %d clocks to read the kernel\n", (int)(b - a));
#endif
    b_macho_load_symbols(binary);
/*
    vm_address_t mine = 0x10000000;
    vm_prot_t curprot, maxprot;

    printf("%x %x %x\n", maxaddr, mh_addr, maxaddr - mh_addr);

    for(addr_t i = 0xc0000000; i < 0xc0100000; i += 0x1000) {
        vm_size_t outsize;
        vm_address_t data;
        kr = vm_read(kernel_task, (vm_address_t) i, 0xfff, &data, &outsize);
        printf("%08x -> %d\n", i, kr);
    }
    die("...\n");

    kr = vm_remap(mach_task_self(), &mine, 0x1000, 0, true, kernel_task, (mach_vm_address_t) mh_addr, false, &curprot, &maxprot, VM_INHERIT_NONE);
    if(kr) {
        die("load_running_kernel: vm_remap returned %d\n", mine, kr);
    }
    printf("curprot=%d maxprot=%d mine=%x\n", curprot, maxprot, mine);

    load_base = (void *) (mine - 0x1000);
*/
#else
    die("load_running_kernel: not on Apple");
#endif
}
 

range_t b_macho_segrange(const struct binary *binary, const char *segname) {
    CMD_ITERATE(binary->mach_hdr, cmd) {
        if(cmd->cmd == LC_SEGMENT) {
            struct segment_command *seg = (void *) cmd;
            if(!strncmp(seg->segname, segname, 16)) {
                // still semantically const
                ((struct binary *) binary)->last_seg = seg;
                return (range_t) {binary, seg->vmaddr, seg->filesize};
            }
        }
    }
    die("no such segment %s", segname);
}

#define DEFINE_RANGECONV(name, field, intro) \
prange_t name(range_t range) { \
    struct segment_command *seg = range.binary->last_seg; \
    if(seg && seg->field <= range.start && seg->filesize - (seg->field - range.start) >= range.size) { \
        goto ok; \
    } \
    CMD_ITERATE(range.binary->mach_hdr, cmd) { \
        if(cmd->cmd == LC_SEGMENT) { \
            seg = (void *) cmd; \
            if(seg && seg->field <= range.start && seg->filesize - (seg->field - range.start) >= range.size) { \
                /* ditto */ \
                ((struct binary *) (range.binary))->last_seg = seg; \
                goto ok; \
            } \
        } \
    } \
    die(intro " (%08x, %zx) not valid", range.start, range.size); \
    ok: \
    return (prange_t) {b_addrconv_unsafe(range.binary, range.start), range.size}; \
}

#ifndef I_TRUST_YOU
DEFINE_RANGECONV(macho_rangeconv, vmaddr, "range")
#endif
DEFINE_RANGECONV(macho_rangeconv_off, fileoff, "offset range")

prange_t rangeconv_checkof(range_t range) {
    if((range.size & ~0x0fffffff) || (((range.start & 0x0fffffff) + range.size) & ~0x0fffffff)) {
        die("range (%08x, %zx) overflowing", range.start, range.size);
    }
    return (prange_t) {b_addrconv_unsafe(range.binary, range.start), range.size};
}

// return value is |1 if to_execute is set and it is a thumb symbol
addr_t b_sym(const struct binary *binary, const char *name, bool to_execute) {
    if(!binary->ext_symtab) {
        die("we wanted %s but there is no symbol table", name);
    }
    // I stole dyld's codez
    const struct nlist *base = binary->ext_symtab;
    for(uint32_t n = binary->ext_nsyms; n > 0; n /= 2) {
        const struct nlist *pivot = base + n/2;
        uint32_t strx = pivot->n_un.n_strx;
        if(strx >= binary->strsize) {
            die("insane strx: %u", strx);
        }
        const char *pivot_str = binary->strtab + strx;
        int cmp = strncmp(name, pivot_str, binary->strsize - strx);
        if(cmp == 0) {
            // we found it
            addr_t result = pivot->n_value;
            if(to_execute && (pivot->n_desc & N_ARM_THUMB_DEF)) {
                result |= 1;
            }
            return result;
        } else if(cmp > 0) {
            base = pivot + 1; 
            n--;
        }
    }
    die("symbol %s not found", name);
}

void b_macho_store(struct binary *binary, const char *path) {
#define _arg path
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd <= 0) {
        edie("unable to open");
    }
    CMD_ITERATE(binary->mach_hdr, cmd) {
        if(cmd->cmd == LC_SEGMENT) {
            struct segment_command *scmd = (void *) cmd;
            lseek(fd, scmd->fileoff, SEEK_SET);
            if(write(fd, b_addrconv_unsafe(binary, scmd->vmaddr), scmd->filesize) != scmd->filesize) {
                edie("couldn't write segment data");
            }
        }
    }
#undef _arg
}
