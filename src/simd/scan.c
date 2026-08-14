#include "pxml_internal.h"

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
#endif

#if defined(PXML_HAVE_NEON_SCANNER)
size_t pxml_scan_byte_neon(const char *source, size_t length, unsigned char value);
#endif

size_t pxml_scan_byte(const char *source, size_t length, unsigned char value)
{
#if defined(PXML_HAVE_AVX2_SCANNER)
    if (__builtin_cpu_supports("avx2")) {
        return pxml_scan_byte_avx2(source, length, value);
    }
#elif defined(PXML_HAVE_NEON_SCANNER)
    return pxml_scan_byte_neon(source, length, value);
#endif
    return scan_scalar(source, length, value);
}
