/* Exercise metadata validation without mapping or executing guest addresses. */
#include "../src/macho_loader.c"
#include <assert.h>

static void test_defined_indirect_symbols(void)
{
    /* A tiny mapped image with one local definition and one host import. */
    uint8_t *bytes = mmap((void *)0x10000000, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(bytes != MAP_FAILED && (uintptr_t)bytes <= UINT32_MAX - 4096);
    uint32_t base = (uint32_t)(uintptr_t)bytes;
    struct mach_header *header = (void *)bytes;
    header->ncmds = 4;
    struct segment_command *text = (void *)(header + 1);
    *text = (struct segment_command){.cmd = LC_SEGMENT,
        .cmdsize = sizeof(*text) + sizeof(struct section), .nsects = 1};
    struct section *section = (void *)(text + 1);
    section->flags = S_NON_LAZY_SYMBOL_POINTERS;
    section->addr = base + 0x600;
    section->size = 8;
    struct segment_command *linkedit = (void *)(section + 1);
    *linkedit = (struct segment_command){.cmd = LC_SEGMENT,
        .cmdsize = sizeof(*linkedit), .segname = SEG_LINKEDIT,
        .vmaddr = base + 0x800, .fileoff = 0x800};
    struct symtab_command *symtab = (void *)(linkedit + 1);
    *symtab = (struct symtab_command){.cmd = LC_SYMTAB, .cmdsize = sizeof(*symtab),
        .symoff = 0x900, .nsyms = 2, .stroff = 0xa00, .strsize = 20};
    struct dysymtab_command *dysymtab = (void *)(symtab + 1);
    *dysymtab = (struct dysymtab_command){.cmd = LC_DYSYMTAB,
        .cmdsize = sizeof(*dysymtab), .indirectsymoff = 0xb00, .nindirectsyms = 2};
    struct nlist *symbols = (void *)(bytes + 0x900);
    symbols[0].n_un.n_strx = 1;
    symbols[0].n_type = N_SECT; /* Private-external template, defined in guest. */
    symbols[0].n_value = base + 0x700;
    symbols[1].n_un.n_strx = 10;
    symbols[1].n_type = N_UNDF | N_EXT;
    memcpy(bytes + 0xa00, "\0_defined\0_external\0", 20);
    uint32_t *indirect = (void *)(bytes + 0xb00);
    indirect[0] = 0;
    indirect[1] = 1;
    struct macho_image32 image = {.header = header, .min_address = base,
                                  .max_address = base + 4096};
    assert(collect_imports(&image) == 0);
    assert(image.import_count == 2);
    assert(image.imports[0].target == base + 0x700);
    assert(!strcmp(image.imports[0].name, "_defined"));
    assert(!image.imports[1].target && !strcmp(image.imports[1].name, "_external"));
    image.import_count = 0;
    symbols[0].n_value = base + 4096;
    assert(collect_imports(&image) == -1);
    munmap(bytes, 4096);
}

int main(void)
{
    union { uint64_t alignment; uint8_t bytes[4096]; } file = {0};
    struct mach_header *header = (void *)file.bytes;
    *header = (struct mach_header){.magic = MH_MAGIC, .cputype = CPU_TYPE_I386,
                                  .filetype = MH_EXECUTE, .ncmds = 2};
    /* LC_MAIN may precede __TEXT in the command list. */
    struct entry_point_command *main = (void *)(header + 1);
    *main = (struct entry_point_command){.cmd = LC_MAIN, .cmdsize = sizeof(*main),
                                         .entryoff = 0x800};
    struct segment_command *segment = (void *)(main + 1);
    *segment = (struct segment_command){.cmd = LC_SEGMENT, .cmdsize = sizeof(*segment),
        .segname = SEG_TEXT, .vmaddr = 0x1000, .vmsize = 4096, .filesize = 4096,
        .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    header->sizeofcmds = sizeof(*main) + sizeof(*segment);
    struct source_file source = {.bytes = file.bytes, .size = sizeof(file.bytes)};
    struct macho_image32 image = {0};
    assert(inspect_commands(&source, &image) == 0);
    assert(image.entry_eip == 0x1800 && image.main_address == 0x1800);
    /* Offset is in the file, even when __TEXT does not start at file offset 0. */
    segment->fileoff = 0x400;
    segment->filesize = 0xc00;
    assert(inspect_commands(&source, &image) == 0);
    assert(image.entry_eip == 0x1400);
    main->entryoff = 0x3ff;
    assert(inspect_commands(&source, &image) == -1);
    main->entryoff = 4096;
    assert(inspect_commands(&source, &image) == -1);
    main->entryoff = UINT64_MAX;
    assert(inspect_commands(&source, &image) == -1);
    main->entryoff = 0x800;
    segment->initprot = VM_PROT_READ;
    assert(inspect_commands(&source, &image) == -1);
    segment->initprot |= VM_PROT_EXECUTE;
    main->cmdsize = sizeof(struct load_command);
    assert(inspect_commands(&source, &image) == -1);

    /* Original titles still use the exact LC_UNIXTHREAD entry, not main. */
    memset(file.bytes + sizeof(*header), 0, sizeof(file.bytes) - sizeof(*header));
    segment = (void *)(header + 1);
    *segment = (struct segment_command){.cmd = LC_SEGMENT, .cmdsize = sizeof(*segment),
        .segname = SEG_TEXT, .vmaddr = 0x1000, .vmsize = 4096, .filesize = 4096,
        .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    struct load_command *thread = (void *)(segment + 1);
    thread->cmd = LC_UNIXTHREAD;
    thread->cmdsize = sizeof(*thread) + 8 + sizeof(x86_thread_state32_t);
    uint32_t *words = (void *)(thread + 1);
    words[0] = x86_THREAD_STATE32;
    words[1] = x86_THREAD_STATE32_COUNT;
    x86_thread_state32_t *state = (void *)(words + 2);
    state->__eip = 0x1234;
    header->sizeofcmds = sizeof(*segment) + thread->cmdsize;
    memset(&image, 0, sizeof(image));
    assert(inspect_commands(&source, &image) == 0);
    assert(image.entry_eip == 0x1234 && !image.main_address);
    test_defined_indirect_symbols();
    puts("Mach-O PASS (LC_MAIN, LC_UNIXTHREAD, defined indirect symbols, invalid metadata)");
    return 0;
}
