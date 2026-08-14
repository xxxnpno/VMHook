/*
    The single translation unit of the compiled library (vmhook.lib / libvmhook.a).

    vmhook is header-only: everything it defines is `inline` or a template, so a
    consumer's own TU emits whatever it uses and this archive holds only what is
    reachable from here.  What the target really carries is the PRECOMPILED
    HEADER built from vmhook.hpp: linking vmhook::compiled hands that PCH to your
    targets, and the header is then parsed once for the whole build instead of
    once per TU.  Measured on GCC 15 (-O2): 1.68 s -> 0.67 s for a TU that does
    nothing but include the header.

    The unit is not empty on purpose.  `vmhook::compiled_version()` is a real,
    non-inline symbol, so the archive always has one member and every toolchain's
    archiver is happy; it also lets a consumer verify at run time that the .lib
    it linked was built from the same header it compiled against, which is the
    one mismatch a static library can silently introduce.
*/

#include <vmhook/vmhook.hpp>

namespace vmhook
{
    /*
        @brief The library version this archive was compiled from.
        @details
        Compare against vmhook::version (the value your TU sees) to catch a .lib
        built from a different header than the one you included.  They differ
        only if the two came from different checkouts.

        It reads a `constexpr int` rather than the VMHOOK_VERSION macro it used
        to, because that macro no longer exists anywhere: a macro cannot cross a
        module boundary, so the version became a value when vmhook did.
    */
    auto compiled_version() noexcept
        -> std::uint32_t
    {
        return static_cast<std::uint32_t>(vmhook::version);
    }

    /*
        @brief The library version this archive was compiled from, as text.
    */
    auto compiled_version_string() noexcept
        -> const char*
    {
        return vmhook::version_string.data();
    }
}
