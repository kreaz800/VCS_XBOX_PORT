#pragma once

#ifndef __cpp_lib_bit_cast
namespace std {

template <class To, class From>
constexpr To bit_cast(const From& value) noexcept {
    static_assert(sizeof(To) == sizeof(From));
    return __builtin_bit_cast(To, value);
}

}
#endif
