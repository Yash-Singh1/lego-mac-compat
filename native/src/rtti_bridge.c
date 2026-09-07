#include "rtti_bridge.h"
#include "macho_loader.h"
#include <mach-o/nlist.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Itanium RTTI records themselves are present in the guest image. Their
 * type-info vtable references use external relocations, not indirect symbol
 * pointers. Read those relocations to distinguish class, single-base and
 * multiple-base records without calling a 64-bit C++ ABI on 32-bit objects. */
static struct {
  uint32_t address;
  unsigned kind;
} types[4096];
static unsigned type_count;
static unsigned rtti_kind(const char *name) {
  if (!strcmp(name, "__ZTVN10__cxxabiv117__class_type_infoE")) return 1;
  if (!strcmp(name, "__ZTVN10__cxxabiv120__si_class_type_infoE")) return 2;
  if (!strcmp(name, "__ZTVN10__cxxabiv121__vmi_class_type_infoE")) return 3;
  return 0;
}
static bool leb(const uint8_t **cursor, const uint8_t *end, uint64_t *value) {
  *value = 0;
  for (unsigned shift = 0; shift < 64 && *cursor < end; shift += 7) {
    uint8_t byte = *(*cursor)++;
    if (shift == 63 && (byte & 0x7e)) return false;
    *value |= (uint64_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return true;
  }
  return false;
}
/* LC_DYLD_INFO replaced external relocations in newer executables. Decode
 * the binding locations, not vtable contents (which dyld has not bound). */
static void bind_types(const uint8_t *p, const uint8_t *end,
                       const struct segment_command **segments, unsigned count) {
  unsigned segment = count, kind = 0, type = BIND_TYPE_POINTER;
  uint64_t offset = 0, value = 0, skip = 0, repeat = 0;
  while (p < end) {
    uint8_t byte = *p++, opcode = byte & BIND_OPCODE_MASK, imm = byte & BIND_IMMEDIATE_MASK;
    switch (opcode) {
    case BIND_OPCODE_DONE: return;
    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: break;
    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
    case BIND_OPCODE_SET_ADDEND_SLEB:
      if (!leb(&p,end,&value)) return; break;
    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
      const uint8_t *zero = memchr(p,0,(size_t)(end-p)); if (!zero) return;
      kind = rtti_kind((const char *)p); p = zero+1; break;
    }
    case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
      segment = imm; if (!leb(&p,end,&offset)) return; break;
    case BIND_OPCODE_ADD_ADDR_ULEB:
      if (!leb(&p,end,&value)) return;
      offset += value; if (offset > UINT32_MAX) return; break;
    case BIND_OPCODE_DO_BIND:
    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
      repeat = 1; skip = 0;
      if (opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB && !leb(&p,end,&skip)) return;
      if (opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) skip = imm*4;
      if (opcode == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB &&
          (!leb(&p,end,&repeat) || !leb(&p,end,&skip))) return;
      if (segment >= count || repeat > UINT32_MAX/4) return;
      for (uint64_t i=0;i<repeat;++i) {
        if (offset > segments[segment]->vmsize || segments[segment]->vmsize-offset < 8) return;
        if (kind && type == BIND_TYPE_POINTER && type_count < 4096) {
          types[type_count].address = segments[segment]->vmaddr+(uint32_t)offset;
          types[type_count++].kind = kind;
        }
        /* Linkers encode backwards movement as a wrapping unsigned delta. */
        offset += 4+skip;
        if (offset > UINT32_MAX) return;
      }
      break;
    default: return;
    }
  }
}
void rtti_bridge32_initialize(const struct macho_image32 *image) {
  type_count = 0;
  const struct symtab_command *sym = NULL;
  const struct dysymtab_command *dysym = NULL;
  const struct segment_command *linkedit = NULL, *segments[16];
  const struct dyld_info_command *dyld = NULL;
  unsigned segment_count = 0;
  const unsigned char *p = (const void *)(image->header + 1);
  for (uint32_t i = 0; i < image->header->ncmds; ++i) {
    const struct load_command *cmd = (const void *)p;
    if (cmd->cmd == LC_DYLD_INFO || cmd->cmd == LC_DYLD_INFO_ONLY) dyld = (const void *)p;
    if (cmd->cmd == LC_SEGMENT && segment_count < 16) segments[segment_count++] = (const void *)p;
    if (cmd->cmd == LC_SYMTAB)
      sym = (const void *)p;
    if (cmd->cmd == LC_DYSYMTAB)
      dysym = (const void *)p;
    if (cmd->cmd == LC_SEGMENT &&
        !strncmp(((const struct segment_command *)p)->segname, SEG_LINKEDIT,
                 16))
      linkedit = (const void *)p;
    p += cmd->cmdsize;
  }
  if (!sym || !dysym || !linkedit || linkedit->vmaddr < linkedit->fileoff)
    return;
  uintptr_t base = linkedit->vmaddr - linkedit->fileoff;
  uint64_t end = (uint64_t)linkedit->fileoff + linkedit->filesize;
  if (dyld && dyld->bind_off >= linkedit->fileoff &&
      (uint64_t)dyld->bind_off + dyld->bind_size <= end)
    bind_types((const void *)(base+dyld->bind_off), (const void *)(base+dyld->bind_off+dyld->bind_size), segments, segment_count);
  if (!dysym->nextrel) return;
  if (dysym->extreloff < linkedit->fileoff ||
      (uint64_t)dysym->extreloff + dysym->nextrel * 8ull > end ||
      sym->symoff < linkedit->fileoff ||
      (uint64_t)sym->symoff + sym->nsyms * sizeof(struct nlist) > end ||
      sym->stroff < linkedit->fileoff ||
      (uint64_t)sym->stroff + sym->strsize > end)
    return;
  const uint32_t *relocations = (const void *)(base + dysym->extreloff);
  const struct nlist *symbols = (const void *)(base + sym->symoff);
  const char *strings = (const void *)(base + sym->stroff);
  for (uint32_t i = 0; i < dysym->nextrel && type_count < 4096; ++i) {
    uint32_t address = relocations[i * 2], bits = relocations[i * 2 + 1],
             index = bits & 0xffffff;
    if ((bits >> 24) != 0x0c || index >= sym->nsyms ||
        address < image->min_address || address > image->max_address - 8)
      continue;
    uint32_t offset = symbols[index].n_un.n_strx;
    if (offset >= sym->strsize ||
        !memchr(strings + offset, 0, sym->strsize - offset))
      continue;
    const char *name = strings + offset;
    unsigned kind = rtti_kind(name);
    if (kind) {
      types[type_count].address = address;
      types[type_count++].kind = kind;
    }
  }
}
static const uint32_t *words(uint32_t p) { return (const void *)(uintptr_t)p; }
static unsigned kind_of(uint32_t type) {
  for (unsigned i = 0; i < type_count; ++i)
    if (types[i].address == type)
      return types[i].kind;
  return 0;
}
static bool same_type(uint32_t a, uint32_t b) {
  return a == b || (a && b && kind_of(a) && kind_of(b) &&
                    !strcmp((const char *)(uintptr_t)words(a)[1],
                            (const char *)(uintptr_t)words(b)[1]));
}
struct subobject {
  uint32_t type, address;
  bool public_path;
};
struct hierarchy {
  struct subobject objects[512];
  unsigned count;
  bool overflow;
};
static void visit(struct hierarchy *tree, uint32_t type, uint32_t object,
                  bool public_path, unsigned depth) {
  if (depth >= 64 || tree->count >= 512) {
    tree->overflow = true;
    return;
  }
  tree->objects[tree->count++] = (struct subobject){type, object, public_path};
  unsigned kind = kind_of(type);
  const uint32_t *info = words(type);
  if (kind == 2) {
    visit(tree, info[2], object, public_path, depth + 1);
    return;
  }
  if (kind != 3)
    return;
  unsigned count = info[3];
  if (count > 256) {
    tree->overflow = true;
    return;
  }
  for (unsigned i = 0; i < count; ++i) {
    uint32_t base_type = info[4 + i * 2];
    int32_t flags = (int32_t)info[5 + i * 2];
    int32_t offset = flags >> 8;
    if (flags & 1)
      offset = *(const int32_t *)(uintptr_t)(words(object)[0] + offset);
    visit(tree, base_type, object + offset, public_path && (flags & 2),
          depth + 1);
  }
}
uint32_t rtti_bridge32_cast(uint32_t object, uint32_t source, uint32_t target) {
  if (getenv("LP32_TRACE_RTTI")) {
    fprintf(stderr,"compat32: cast object=%#x source=%#x(%u) target=%#x(%u) types=%u\n",object,source,kind_of(source),target,kind_of(target),type_count);
    if(object) {uint32_t v=words(object)[0];fprintf(stderr,"compat32: cast vtable=%#x type=%#x(%u)\n",v,v>=8?words(v-4)[0]:0,v>=8?kind_of(words(v-4)[0]):0);}
  }
  if (!object || !kind_of(source) || !kind_of(target))
    return 0;
  uint32_t vtable = words(object)[0];
  if (vtable < 8)
    return 0;
  uint32_t complete = object + (int32_t)words(vtable - 8)[0],
           type = words(vtable - 4)[0];
  if (!kind_of(type))
    return 0;
  struct hierarchy all = {0};
  visit(&all, type, complete, true, 0);
  if (all.overflow)
    return 0;
  uint32_t result = 0;
  bool source_public = false;
  for (unsigned i = 0; i < all.count; ++i) {
    struct subobject *candidate = &all.objects[i];
    if (candidate->address == object && same_type(candidate->type, source) &&
        candidate->public_path)
      source_public = true;
    if (!same_type(candidate->type, target))
      continue;
    struct hierarchy descendants = {0};
    visit(&descendants, target, candidate->address, true, 0);
    if (descendants.overflow)
      return 0;
    for (unsigned j = 0; j < descendants.count; ++j)
      if (descendants.objects[j].address == object &&
          descendants.objects[j].public_path &&
          same_type(descendants.objects[j].type, source)) {
        if (result && result != candidate->address)
          return 0;
        result = candidate->address;
        break;
      }
  }
  if (result)
    return result;
  if (!source_public)
    return 0;
  bool target_public = false;
  for (unsigned i = 0; i < all.count; ++i)
    if (same_type(all.objects[i].type, target)) {
      if (result && result != all.objects[i].address)
        return 0;
      result = all.objects[i].address;
      target_public |= all.objects[i].public_path;
    }
  return target_public ? result : 0;
}
