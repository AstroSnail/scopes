/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "gc.hpp"
#include "error.hpp"
#include "scopes/config.h"

#include <algorithm>
#include <map>

namespace scopes {

char *g_stack_start;
size_t g_largest_stack_size = 0;

#if 0
size_t memory_stack_size() {
    char c; char *_stack_addr = &c;
    size_t ss = (size_t)(g_stack_start - _stack_addr);
    g_largest_stack_size = std::max(ss, g_largest_stack_size);
    return ss;
}
#else
size_t memory_stack_size() {
    return 0;
}
#endif

SCOPES_RESULT(size_t) verify_stack() {
    SCOPES_RESULT_TYPE(size_t);
    size_t ssz = memory_stack_size();
    if (ssz >= SCOPES_MAX_STACK_SIZE) {
        SCOPES_LOCATION_ERROR(String::from("stack overflow encountered"));
    }
    return ssz;
}

// for allocated pointers, register the size of the range
static std::map<void *, size_t> tracked_allocations;

void track(void *ptr, size_t size) {
    tracked_allocations.insert({ptr,size});
}

void *tracked_malloc(size_t size) {
    void *ptr = malloc(size);
    track(ptr, size);
    return ptr;
}

bool find_allocation(void *srcptr,  void *&start, size_t &size) {
    auto it = tracked_allocations.upper_bound(srcptr);
    if (it == tracked_allocations.begin())
        return false;
    it--;
    start = it->first;
    size = it->second;
    return (srcptr >= start)&&((uint8_t*)srcptr < ((uint8_t*)start + size));
}

} // namespace scopes