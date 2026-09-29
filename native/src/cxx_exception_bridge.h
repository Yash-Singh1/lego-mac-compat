#ifndef LP32_CXX_EXCEPTION_BRIDGE_H
#define LP32_CXX_EXCEPTION_BRIDGE_H

#include <stdint.h>

/* Guest C++ (Itanium ABI) and fragile Objective-C exceptions. Returns 1 when
   the import is handled; a throw resumes the guest elsewhere through
   compat_runtime32_resume_guest. */
int cxx_exception_bridge32_dispatch(const char *name, const uint32_t *arguments,
                                    uint32_t return_address, uint64_t *result);

/* An image whose frames can be unwound: its __TEXT range (the compact-unwind
   base) and __TEXT,__unwind_info. */
void cxx_exception_bridge32_register_image(uint32_t text_start, uint32_t text_end,
                                           uint32_t unwind_info_start,
                                           uint32_t unwind_info_end);

/* exception_ptr support for the future bridge. An exception_ptr holds the
   thrown object's address. */
void cxx_exception_bridge32_retain(uint32_t object);
void cxx_exception_bridge32_release(uint32_t object);
/* Throw a stored exception from the import now being dispatched. */
void cxx_exception_bridge32_rethrow(uint32_t object, const uint32_t *arguments,
                                    uint32_t return_address);

int cxx_exception_bridge32_run_self_test(void);

#endif
