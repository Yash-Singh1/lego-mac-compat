/* Synthetic i386 object layouts exercise casts without executing a game. */
#include "../src/rtti_bridge.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static uint32_t next = 0x10000000;
static uint32_t alloc(unsigned n) {
  uint32_t p = next;
  next += (n + 15) & ~15u;
  return p;
}
static uint32_t type(unsigned kind, const char *name, uint32_t a, int32_t af,
                     uint32_t b, int32_t bf) {
  uint32_t p = alloc(32), str = alloc((unsigned)strlen(name) + 1);
  strcpy((void *)(uintptr_t)str, name);
  uint32_t *w = (void *)(uintptr_t)p;
  w[1] = str;
  if (kind == 2)
    w[2] = a;
  if (kind == 3) {
    w[3] = b ? 2 : 1;
    w[4] = a;
    w[5] = af;
    w[6] = b;
    w[7] = bf;
  }
  types[type_count].address = p;
  types[type_count++].kind = kind;
  return p;
}
static void vptr(uint32_t object, uint32_t type, int offset,
                 int virtual_offset) {
  uint32_t table = alloc(16);
  uint32_t *w = (void *)(uintptr_t)table;
  w[0] = virtual_offset;
  w[1] = -offset;
  w[2] = type;
  *(uint32_t *)(uintptr_t)object = table + 12;
}
static void test_dyld_bind_types(void) {
  /* A pointer bind followed by a backwards unsigned delta, as emitted by
     ld64. The second RTTI record must be recognized without external relocs. */
  const char name[] = "__ZTVN10__cxxabiv120__si_class_type_infoE";
  uint8_t stream[128], *p = stream;
  *p++ = BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM;
  memcpy(p,name,sizeof(name));p+=sizeof(name);
  *p++ = BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB; *p++=32;
  *p++ = BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB;
  uint64_t delta = (uint64_t)-20;
  do {uint8_t b=delta&127;delta>>=7;*p++=b|(delta?128:0);} while(delta);
  *p++ = BIND_OPCODE_DO_BIND; *p++=BIND_OPCODE_DONE;
  struct segment_command segment = {.vmaddr=0x10000000,.vmsize=4096};
  const struct segment_command *segments[]={&segment};
  bind_types(stream,p,segments,1);
  assert(kind_of(0x10000020)==2 && kind_of(0x10000010)==2);
  type_count=0;
  /* Truncated symbol and LEB operands never read past the stream. */
  bind_types(stream,stream+3,segments,1);assert(type_count==0);
  const uint8_t truncated[]={BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB,128};
  bind_types(truncated,truncated+sizeof(truncated),segments,1);assert(type_count==0);
}
int main(void) {
  test_dyld_bind_types();
  assert(mmap((void *)0x10000000, 65536, PROT_READ | PROT_WRITE,
              MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == (void *)0x10000000);
  uint32_t A = type(1, "A", 0, 0, 0, 0), B = type(2, "B", A, 0, 0, 0),
           C = type(1, "C", 0, 0, 0, 0);
  uint32_t D = type(3, "D", B, 2, C, (16 << 8) | 2), obj = alloc(64);
  vptr(obj, D, 0, 0);
  vptr(obj + 16, D, 16, 0);
  assert(rtti_bridge32_cast(obj, A, B) == obj);
  assert(rtti_bridge32_cast(obj, A, D) == obj);
  assert(rtti_bridge32_cast(obj, A, C) == obj + 16);
  assert(rtti_bridge32_cast(obj + 16, C, B) == obj);
  assert(!rtti_bridge32_cast(0, A, D));
  uint32_t P = type(3, "P", A, 0, C, (16 << 8) | 2);
  vptr(obj, P, 0, 0);
  vptr(obj + 16, P, 16, 0);
  assert(!rtti_bridge32_cast(obj, A, P));
  assert(!rtti_bridge32_cast(obj, A, C));
  assert(!rtti_bridge32_cast(obj + 16, C, A));
  uint32_t X = type(3, "X", A, 2, C, (8 << 8) | 2),
           Y = type(3, "Y", A, 2, C, (8 << 8) | 2);
  uint32_t M = type(3, "M", X, 2, Y, (16 << 8) | 2);
  vptr(obj, M, 0, 0);
  vptr(obj + 16, M, 16, 0);
  assert(!rtti_bridge32_cast(obj, A, C));       /* two distinct C subobjects */
  assert(rtti_bridge32_cast(obj, A, X) == obj); /* unique enclosing X */
  uint32_t V = type(3, "V", A, (-12 * 256) | 3, 0, 0),
           W = type(3, "W", A, (-12 * 256) | 3, 0, 0);
  uint32_t Diamond = type(3, "Diamond", V, 2, W, (16 << 8) | 2);
  vptr(obj, Diamond, 0, 32);
  vptr(obj + 16, Diamond, 16, 16);
  vptr(obj + 32, Diamond, 32, 0);
  assert(rtti_bridge32_cast(obj + 32, A, Diamond) == obj);
  assert(rtti_bridge32_cast(obj + 32, A, W) == obj + 16);
  assert(rtti_bridge32_cast(obj, V, A) == obj + 32); /* shared virtual A */
  uint32_t Repeated = type(3, "Repeated", V, 2, V, (16 << 8) | 2);
  vptr(obj, Repeated, 0, 32);
  vptr(obj + 16, Repeated, 16, 16);
  vptr(obj + 32, Repeated, 32, 0);
  assert(!rtti_bridge32_cast(obj + 32, A, V)); /* two enclosing V objects */
  puts("RTTI bridge PASS (downcast, crosscast, private, ambiguous, virtual "
       "diamond and null)");
}
