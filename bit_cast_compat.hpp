#pragma once

#include <bit>

#ifndef __cpp_lib_bit_cast
namespace std {

template <class To, class From>
constexpr To bit_cast(const From& value) noexcept {
    static_assert(sizeof(To) == sizeof(From));
    return __builtin_bit_cast(To, value);
}

}
#endif

#ifdef NXDK
#include <stdlib.h>

static inline int rand_s(unsigned int* value) noexcept {
    if (!value) {
        return 22;
    }

    unsigned int a = (unsigned int)rand();
    unsigned int b = (unsigned int)rand();
    *value = (a << 16) ^ b;

    return 0;
}
#endif
