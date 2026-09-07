#include "stl_bridge.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* GCC's i386 intrusive nodes use uint32_t links. The host's C++ library
 * cannot operate on them. See the GCC 4.1 _Rb_tree_node_base ABI reference:
 * https://gcc.gnu.org/onlinedocs/libstdc++/libstdc++-html-USERS-4.1/structstd_1_1___rb__tree__node__base.html
 */
struct rb32 {
  uint32_t color, parent, left, right;
};
static struct rb32 *node(uint32_t p) { return (void *)(uintptr_t)p; }
static bool black(uint32_t p) { return !p || node(p)->color == 1; }
static uint32_t minimum(uint32_t p) {
  while (node(p)->left)
    p = node(p)->left;
  return p;
}
static uint32_t maximum(uint32_t p) {
  while (node(p)->right)
    p = node(p)->right;
  return p;
}
static void rotate_left(uint32_t x, uint32_t header) {
  uint32_t y = node(x)->right, parent = node(x)->parent;
  node(x)->right = node(y)->left;
  if (node(y)->left)
    node(node(y)->left)->parent = x;
  node(y)->parent = parent;
  if (parent == header)
    node(header)->parent = y;
  else if (node(parent)->left == x)
    node(parent)->left = y;
  else
    node(parent)->right = y;
  node(y)->left = x;
  node(x)->parent = y;
}
static void rotate_right(uint32_t x, uint32_t header) {
  uint32_t y = node(x)->left, parent = node(x)->parent;
  node(x)->left = node(y)->right;
  if (node(y)->right)
    node(node(y)->right)->parent = x;
  node(y)->parent = parent;
  if (parent == header)
    node(header)->parent = y;
  else if (node(parent)->left == x)
    node(parent)->left = y;
  else
    node(parent)->right = y;
  node(y)->right = x;
  node(x)->parent = y;
}
static void insert(bool left, uint32_t x, uint32_t parent, uint32_t header) {
  *node(x) = (struct rb32){0, parent, 0, 0};
  if (parent == header) {
    node(header)->parent = x;
    node(header)->left = x;
    node(header)->right = x;
  } else if (left) {
    node(parent)->left = x;
    if (node(header)->left == parent)
      node(header)->left = x;
  } else {
    node(parent)->right = x;
    if (node(header)->right == parent)
      node(header)->right = x;
  }
  while (x != node(header)->parent && !black(node(x)->parent)) {
    uint32_t p = node(x)->parent, g = node(p)->parent;
    if (p == node(g)->left) {
      uint32_t uncle = node(g)->right;
      if (!black(uncle)) {
        node(p)->color = node(uncle)->color = 1;
        node(g)->color = 0;
        x = g;
      } else {
        if (x == node(p)->right) {
          x = p;
          rotate_left(x, header);
          p = node(x)->parent;
          g = node(p)->parent;
        }
        node(p)->color = 1;
        node(g)->color = 0;
        rotate_right(g, header);
      }
    } else {
      uint32_t uncle = node(g)->left;
      if (!black(uncle)) {
        node(p)->color = node(uncle)->color = 1;
        node(g)->color = 0;
        x = g;
      } else {
        if (x == node(p)->left) {
          x = p;
          rotate_right(x, header);
          p = node(x)->parent;
          g = node(p)->parent;
        }
        node(p)->color = 1;
        node(g)->color = 0;
        rotate_left(g, header);
      }
    }
  }
  node(node(header)->parent)->color = 1;
}
static void replace(uint32_t old, uint32_t value, uint32_t header) {
  uint32_t p = node(old)->parent;
  if (p == header)
    node(header)->parent = value;
  else if (old == node(p)->left)
    node(p)->left = value;
  else
    node(p)->right = value;
  if (value)
    node(value)->parent = p;
}
static void erase(uint32_t z, uint32_t header) {
  uint32_t y = z, x, parent;
  bool removed_black = black(y);
  if (!node(z)->left) {
    x = node(z)->right;
    parent = node(z)->parent;
    replace(z, x, header);
  } else if (!node(z)->right) {
    x = node(z)->left;
    parent = node(z)->parent;
    replace(z, x, header);
  } else {
    y = minimum(node(z)->right);
    removed_black = black(y);
    x = node(y)->right;
    if (node(y)->parent == z) {
      parent = y;
      if (x)
        node(x)->parent = y;
    } else {
      parent = node(y)->parent;
      replace(y, x, header);
      node(y)->right = node(z)->right;
      node(node(y)->right)->parent = y;
    }
    replace(z, y, header);
    node(y)->left = node(z)->left;
    node(node(y)->left)->parent = y;
    node(y)->color = node(z)->color;
  }
  if (removed_black) {
    while (x != node(header)->parent && black(x)) {
      if (x == node(parent)->left) {
        uint32_t w = node(parent)->right;
        if (!black(w)) {
          node(w)->color = 1;
          node(parent)->color = 0;
          rotate_left(parent, header);
          w = node(parent)->right;
        }
        if (!w) {
          x = parent;
          parent = node(x)->parent;
          continue;
        }
        if (black(node(w)->left) && black(node(w)->right)) {
          node(w)->color = 0;
          x = parent;
          parent = node(x)->parent;
        } else {
          if (black(node(w)->right)) {
            if (node(w)->left)
              node(node(w)->left)->color = 1;
            node(w)->color = 0;
            rotate_right(w, header);
            w = node(parent)->right;
          }
          node(w)->color = node(parent)->color;
          node(parent)->color = 1;
          if (node(w)->right)
            node(node(w)->right)->color = 1;
          rotate_left(parent, header);
          x = node(header)->parent;
          break;
        }
      } else {
        uint32_t w = node(parent)->left;
        if (!black(w)) {
          node(w)->color = 1;
          node(parent)->color = 0;
          rotate_right(parent, header);
          w = node(parent)->left;
        }
        if (!w) {
          x = parent;
          parent = node(x)->parent;
          continue;
        }
        if (black(node(w)->left) && black(node(w)->right)) {
          node(w)->color = 0;
          x = parent;
          parent = node(x)->parent;
        } else {
          if (black(node(w)->left)) {
            if (node(w)->right)
              node(node(w)->right)->color = 1;
            node(w)->color = 0;
            rotate_left(w, header);
            w = node(parent)->left;
          }
          node(w)->color = node(parent)->color;
          node(parent)->color = 1;
          if (node(w)->left)
            node(node(w)->left)->color = 1;
          rotate_right(parent, header);
          x = node(header)->parent;
          break;
        }
      }
    }
    if (x)
      node(x)->color = 1;
  }
  uint32_t root = node(header)->parent;
  node(header)->left = root ? minimum(root) : header;
  node(header)->right = root ? maximum(root) : header;
}
static uint32_t *link(uint32_t p) { return (void *)(uintptr_t)p; }
int stl_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
#define IS(s) (!strcmp(name, s))
  if (strncmp(name, "__Z", 3))
    return 0;
  if (IS("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_"
         "RS_")) {
    insert(a[0] != 0, a[1], a[2], a[3]);
    *out = 0;
    return 1;
  }
  if (IS("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_")) {
    erase(a[0], a[1]);
    *out = a[0];
    return 1;
  }
  if (IS("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base") ||
      IS("__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base")) {
    uint32_t x = a[0];
    if (node(x)->right)
      x = minimum(node(x)->right);
    else {
      uint32_t y = node(x)->parent;
      while (x == node(y)->right) {
        x = y;
        y = node(y)->parent;
      }
      if (node(x)->right != y)
        x = y;
    }
    *out = x;
    return 1;
  }
  if (IS("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base")) {
    uint32_t x = a[0];
    if (!black(x) && node(node(x)->parent)->parent == x)
      x = node(x)->right;
    else if (node(x)->left)
      x = maximum(node(x)->left);
    else {
      uint32_t y = node(x)->parent;
      while (x == node(y)->left) {
        x = y;
        y = node(y)->parent;
      }
      x = y;
    }
    *out = x;
    return 1;
  }
  if (IS("__ZNSt15_List_node_base4hookEPS_")) {
    link(a[0])[0] = a[1];
    link(a[0])[1] = link(a[1])[1];
    link(link(a[1])[1])[0] = a[0];
    link(a[1])[1] = a[0];
    *out = 0;
    return 1;
  }
  if (IS("__ZNSt15_List_node_base6unhookEv")) {
    link(link(a[0])[0])[1] = link(a[0])[1];
    link(link(a[0])[1])[0] = link(a[0])[0];
    *out = 0;
    return 1;
  }
  if (IS("__ZNSt15_List_node_base8transferEPS_S0_")) {
    uint32_t pos = a[0], first = a[1], last = a[2];
    if (first != last && pos != last && pos != first) {
      uint32_t before = link(first)[1], tail = link(last)[1],
               dest = link(pos)[1];
      link(before)[0] = last;
      link(last)[1] = before;
      link(dest)[0] = first;
      link(first)[1] = dest;
      link(tail)[0] = pos;
      link(pos)[1] = tail;
    }
    *out = 0;
    return 1;
  }
  if (IS("__ZNSt15_List_node_base4swapERS_S0_")) {
    uint32_t x = a[0], y = a[1];
    if (x != y) {
      uint32_t xn = link(x)[0], xp = link(x)[1], yn = link(y)[0],
               yp = link(y)[1];
      if (yn == y)
        link(x)[0] = link(x)[1] = x;
      else {
        link(x)[0] = yn;
        link(x)[1] = yp;
        link(yn)[1] = x;
        link(yp)[0] = x;
      }
      if (xn == x)
        link(y)[0] = link(y)[1] = y;
      else {
        link(y)[0] = xn;
        link(y)[1] = xp;
        link(xn)[1] = y;
        link(xp)[0] = y;
      }
    }
    *out = 0;
    return 1;
  }
  return 0;
#undef IS
}
