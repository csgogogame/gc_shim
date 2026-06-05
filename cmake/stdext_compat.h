#pragma once

// Compatibility shim for newer MSVC STLs (VS 2022 17.10+ / VS 2026), which
// REMOVED the non-standard stdext::make_checked_array_iterator and
// stdext::make_unchecked_array_iterator helpers. Crypto++ still calls them
// (zdeflate.cpp, integer.cpp) under #if CRYPTOPP_MSC_VERSION >= 1500/1600, so we
// reintroduce trivial pass-through versions. These were always just bounds-check
// wrappers around a raw pointer; returning the iterator unchanged is correct in
// release builds (no debug bounds checking, which Crypto++ never relied on).
//
// Force-included into all MSVC translation units (see CMakeLists.txt). The
// functions live in namespace stdext, which is otherwise empty on modern STLs,
// so there is no conflict.

#if defined(_MSC_VER) && defined(__cplusplus)

#include <cstddef>

namespace stdext
{
    template <class Iter>
    Iter make_unchecked_array_iterator(Iter it)
    {
        return it;
    }

    template <class Iter>
    Iter make_checked_array_iterator(Iter it, std::size_t /*size*/, std::size_t offset = 0)
    {
        return it + offset;
    }
}

#endif // _MSC_VER && __cplusplus
