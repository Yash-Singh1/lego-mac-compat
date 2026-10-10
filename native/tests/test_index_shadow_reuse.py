#!/usr/bin/env python3
"""Run the bridge's index publication code against CPU-only GL mocks."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/objc_bridge.m').read_text()
start = source.index('struct index_shadow {')
end = source.index('\nstatic void discard_buffer_state(', start)
code = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef void *CGLContextObj;
typedef void *CGLShareGroupObj;
typedef unsigned GLuint;
typedef unsigned GLenum;
typedef int GLsizei;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
#define GL_ELEMENT_ARRAY_BUFFER 1
#define GL_ARRAY_BUFFER 2
#define GL_DYNAMIC_DRAW 3
#define GL_STREAM_DRAW 4
#define GL_UNSIGNED_BYTE 5
#define GL_UNSIGNED_SHORT 6
#define GL_UNSIGNED_INT 7
enum lp32_title { title_mw3 };
struct profile { enum lp32_title title; };
static struct profile profile;
static struct profile *lp32_profile(void) { return &profile; }
static bool lp32_title_is_mw_sdl(enum lp32_title t) { return true; }
static CGLContextObj current = (void *)1;
static CGLContextObj CGLGetCurrentContext(void) { return current; }
static CGLShareGroupObj CGLGetShareGroup(CGLContextObj c) { return (void *)(uintptr_t)((uintptr_t)c < 3 ? 1 : 2); }
static GLuint element, array, next_name = 100;
static GLuint bound_buffer_for_target(GLenum t) { return t == GL_ELEMENT_ARRAY_BUFFER ? element : array; }
static unsigned uploads;
static unsigned char *published;
static unsigned char *storage[256];
static size_t storage_size[256];
static bool lp32_gl_backend_is_replacement(void) { return true; }
static size_t biggest_patch;
static void glGenBuffers(GLsizei n, GLuint *p) { *p = ++next_name; }
static void glBindBuffer(GLenum t, GLuint n) { if (t == GL_ELEMENT_ARRAY_BUFFER) { element = n; published = storage[n]; } else array = n; }
static void glBufferData(GLenum t, GLsizeiptr n, const void *p, GLenum usage) { ++uploads; assert(n <= (16 << 20)); /* Old stores remain retained by pending mock draws. */ published = storage[element] = malloc(n); storage_size[element] = n; if (p) memcpy(published,p,n); else memset(published,0,n); }
static void glBufferSubData(GLenum t, GLintptr at, GLsizeiptr n, const void *p) { ++uploads; if ((size_t)n > biggest_patch) biggest_patch = n; GLuint bound = bound_buffer_for_target(t); if (!storage[bound]) { storage[bound] = calloc(1,16 << 20); storage_size[bound] = 16 << 20; } assert(at+n <= storage_size[bound]); memcpy(storage[bound]+at,p,n); }
'''
main = r'''
struct pending_draw { const unsigned char *storage; size_t offset, bytes; unsigned char expected[32]; };
static struct pending_draw pending[64];
static unsigned pending_count;
static void check_pending(void) {
    for (unsigned i = 0; i < pending_count; ++i)
        assert(memcmp(pending[i].storage + pending[i].offset, pending[i].expected, pending[i].bytes) == 0);
}
static void acquire(unsigned offset, int count, unsigned type, bool expect_upload) {
    const void *pointer = (void *)99; uint32_t restore = 0; unsigned before = uploads;
    GLuint original = element;
    assert(objc_bridge32_index_shadow_acquire(offset,count,type,&pointer,&restore));
    assert(restore == original && element != original);
    assert((uintptr_t)pointer % 4 == 0);
    assert((uploads != before) == expect_upload);
    struct index_shadow *shadow = index_shadow_find(current_share_group(),original);
    assert(memcmp(published+(uintptr_t)pointer,shadow->data+offset,count*index_type_size(type)) == 0);
    size_t bytes = count * index_type_size(type);
    if (bytes <= 32 && pending_count < 64) {
        struct pending_draw *draw = &pending[pending_count++];
        draw->storage = published; draw->offset = (uintptr_t)pointer; draw->bytes = bytes;
        memcpy(draw->expected, shadow->data + offset, bytes);
    }
    check_pending();
    objc_bridge32_index_shadow_release(restore); assert(element == original);
}
int main(void) {
    unsigned char bytes[32]; memset(bytes, 11, sizeof bytes);
    element = 7;
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,32,bytes,GL_DYNAMIC_DRAW);
    acquire(0,3,GL_UNSIGNED_SHORT,true); acquire(0,3,GL_UNSIGNED_SHORT,false);
    acquire(2,3,GL_UNSIGNED_SHORT,true); acquire(2,3,GL_UNSIGNED_SHORT,false);
    acquire(2,2,GL_UNSIGNED_SHORT,true); acquire(2,2,GL_UNSIGNED_BYTE,true);
    unsigned char patch = 42;
    assert(index_shadow_copy(7,2,1,&patch,true)); acquire(2,2,GL_UNSIGNED_BYTE,true);
    acquire(2,2,GL_UNSIGNED_BYTE,false);
    current = (void *)2; acquire(2,2,GL_UNSIGNED_BYTE,true);
    current = (void *)1; acquire(2,2,GL_UNSIGNED_BYTE,true);
    array = 7; index_shadow_alias(GL_ARRAY_BUFFER,7); array = 0;
    acquire(2,2,GL_UNSIGNED_BYTE,true);
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,32,NULL,GL_DYNAMIC_DRAW);
    acquire(2,2,GL_UNSIGNED_BYTE,true);
    index_shadow_release_buffer(7,false);
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,32,bytes,GL_DYNAMIC_DRAW);
    acquire(2,2,GL_UNSIGNED_BYTE,true);
    current = (void *)3; element = 7;
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,32,bytes,GL_DYNAMIC_DRAW);
    acquire(2,2,GL_UNSIGNED_BYTE,true); acquire(2,2,GL_UNSIGNED_BYTE,false);
    current = (void *)1; element = 7;
    acquire(2,2,GL_UNSIGNED_BYTE,false);
    /* Guest mutation of the hidden stream invalidates its cached contents. */
    struct index_shadow_stream *stream = index_shadow_stream_buffer(current_share_group());
    element = stream->buffer;
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,32,bytes,GL_DYNAMIC_DRAW);
    element = 7; acquire(2,2,GL_UNSIGNED_BYTE,true);
    struct index_shadow_stream *arena = index_shadow_stream_buffer(current_share_group());
    arena->used = arena->capacity; /* Force a wrap without allocating large fixtures. */
    acquire(0,3,GL_UNSIGNED_SHORT,true);
    if (!getenv("LP32_NO_INDEX_ARENA")) assert(arena->published_offset == 0);
    index_shadow_forget_context(current); acquire(0,3,GL_UNSIGNED_SHORT,true);
    index_shadow_release_buffer(arena->buffer,false);
    acquire(0,3,GL_UNSIGNED_SHORT,true);
    unsigned char *large = malloc(200000); memset(large,87,200000);
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,200000,large,GL_DYNAMIC_DRAW);
    acquire(0,100000,GL_UNSIGNED_SHORT,true); acquire(0,100000,GL_UNSIGNED_SHORT,false);
    if (!getenv("LP32_NO_INDEX_ARENA")) assert(biggest_patch <= 65536);
    free(large);
    check_pending();
    puts("Index publication reuse, range/write/lifetime/alias/context/share invalidation passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + code + main)
    subprocess.run(['clang', '-Werror', '-Wno-unused-function', '-pthread', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    import os
    subprocess.run([str(path / 'test')], env={**os.environ, 'LP32_NO_INDEX_ARENA': '1'}, check=True)
