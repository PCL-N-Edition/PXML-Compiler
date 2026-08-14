#include "pxml_internal.h"

#if defined(PXML_HAVE_AVX2_SCANNER) && defined(_WIN32)
#include <intrin.h>
#endif

static size_t scan_scalar(const char *source, size_t length, unsigned char value)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if ((unsigned char)source[index] == value) return index;
    }
    return length;
}

#if defined(PXML_HAVE_AVX2_SCANNER)
size_t pxml_scan_byte_avx2(const char *source, size_t length, unsigned char value);

static bool cpu_has_avx2(void)
{
#if defined(_WIN32)
    int registers[4];
    uint64_t enabled_xstate;
    const unsigned int required_avx_state = (1U << 27U) | (1U << 28U);

    __cpuidex(registers, 0, 0);
    if (registers[0] < 7) return false;

    __cpuidex(registers, 1, 0);
    if (((unsigned int)registers[2] & required_avx_state) != required_avx_state) {
        return false;
    }

    enabled_xstate = _xgetbv(0);
    if ((enabled_xstate & 0x6U) != 0x6U) return false;

    __cpuidex(registers, 7, 0);
    return ((unsigned int)registers[1] & (1U << 5U)) != 0U;
#else
    return __builtin_cpu_supports("avx2");
#endif
}
#endif

#if defined(PXML_HAVE_NEON_SCANNER)
size_t pxml_scan_byte_neon(const char *source, size_t length, unsigned char value);
#endif

size_t pxml_scan_byte(const char *source, size_t length, unsigned char value)
{
#if defined(PXML_HAVE_AVX2_SCANNER)
    if (cpu_has_avx2()) {
        return pxml_scan_byte_avx2(source, length, value);
    }
#elif defined(PXML_HAVE_NEON_SCANNER)
    return pxml_scan_byte_neon(source, length, value);
#endif
    return scan_scalar(source, length, value);
}
