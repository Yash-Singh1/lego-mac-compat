#include "../src/stl_bridge.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
struct node {
  uint32_t color, parent, left, right;
  int key;
};
static struct node *n(uint32_t p) { return (void *)(uintptr_t)p; }
static uint64_t call(const char *s, uint32_t a, uint32_t b, uint32_t c,
                     uint32_t d) {
  uint32_t args[] = {a, b, c, d};
  uint64_t out = 0;
  assert(stl_bridge32_dispatch(s, args, &out));
  return out;
}
static const char *ins =
    "__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_";
static const char *del =
    "__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_";
static const char *inc = "__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base";
static const char *dec = "__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base";
static int check(uint32_t p, uint32_t parent, int lo, int hi, int *count) {
  if (!p)
    return 1;
  assert(n(p)->parent == parent && n(p)->key > lo && n(p)->key < hi);
  ++*count;
  assert(n(p)->color <= 1);
  if (!n(p)->color) {
    assert(!n(p)->left || n(n(p)->left)->color);
    assert(!n(p)->right || n(n(p)->right)->color);
  }
  int l = check(n(p)->left, p, lo, n(p)->key, count),
      r = check(n(p)->right, p, n(p)->key, hi, count);
  assert(l == r);
  return l + n(p)->color;
}
static void verify(uint32_t h, int total) {
  int count = 0;
  check(n(h)->parent, h, -1, 10000, &count);
  assert(count == total);
  if (total) {
    assert(n(n(h)->parent)->color == 1);
    uint32_t p = n(h)->left;
    int prev = -1;
    for (int i = 0; i < total; i++) {
      assert(p != h && n(p)->key > prev);
      prev = n(p)->key;
      p = (uint32_t)call(inc, p, 0, 0, 0);
    }
    assert(p == h);
    p = h;
    prev = 10000;
    for (int i = 0; i < total; i++) {
      p = (uint32_t)call(dec, p, 0, 0, 0);
      assert(p != h && n(p)->key < prev);
      prev = n(p)->key;
    }
    assert(p == n(h)->left);
  } else
    assert(n(h)->left == h && n(h)->right == h);
}
int main(void) {
  enum { N = 512 };
  void *mem = mmap((void *)0x10000000, 0x10000, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
  assert(mem == (void *)0x10000000);
  uint32_t h = 0x10000000;
  int order[N];
  for (unsigned round = 0; round < 8; round++) {
    *n(h) = (struct node){0, 0, h, h, -1};
    for (int i = 0; i < N; i++)
      order[i] = i;
    srand(round);
    for (int i = N - 1; i > 0; i--) {
      int j = rand() % (i + 1), t = order[i];
      order[i] = order[j];
      order[j] = t;
    }
    for (int i = 0; i < N; i++) {
      uint32_t x = h + 32 + (uint32_t)order[i] * 32, p = h, q = n(h)->parent;
      while (q) {
        p = q;
        q = order[i] < n(q)->key ? n(q)->left : n(q)->right;
      }
      n(x)->key = order[i];
      call(ins, p == h || order[i] < n(p)->key, x, p, h);
      verify(h, i + 1);
    }
    for (int i = N - 1; i > 0; i--) {
      int j = rand() % (i + 1), t = order[i];
      order[i] = order[j];
      order[j] = t;
    }
    for (int i = 0; i < N; i++) {
      uint32_t x = h + 32 + (uint32_t)order[i] * 32;
      assert(call(del, x, h, 0, 0) == x);
      verify(h, N - i - 1);
    }
  }
  uint32_t a = h, b = h + 32, x = h + 64, y = h + 96;
  uint32_t *pa = (void *)(uintptr_t)a, *pb = (void *)(uintptr_t)b,
           *px = (void *)(uintptr_t)x, *py = (void *)(uintptr_t)y;
  pa[0] = pa[1] = a;
  pb[0] = pb[1] = b;
  call("__ZNSt15_List_node_base4hookEPS_", x, a, 0, 0);
  call("__ZNSt15_List_node_base4hookEPS_", y, a, 0, 0);
  assert(pa[0] == x && pa[1] == y && px[0] == y && py[1] == x);
  call("__ZNSt15_List_node_base8transferEPS_S0_", b, x, a, 0);
  assert(pa[0] == a && pa[1] == a && pb[0] == x && pb[1] == y);
  call("__ZNSt15_List_node_base4swapERS_S0_", a, b, 0, 0);
  assert(pb[0] == b && pb[1] == b && pa[0] == x && pa[1] == y && px[1] == a &&
         py[0] == a);
  call("__ZNSt15_List_node_base6unhookEv", x, 0, 0, 0);
  assert(pa[0] == y && py[1] == a);
  puts("STL bridge PASS (4096 insertions/deletions with RB invariants, ordered "
       "iteration, end iterator and list splice/swap)");
  return 0;
}
