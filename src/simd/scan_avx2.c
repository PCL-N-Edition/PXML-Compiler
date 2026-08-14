#include <immintrin.h>
#include <stddef.h>

size_t pxml_scan_byte_avx2(const char *source, size_t length, unsigned char value)
{
    size_t index = 0U;
    __m256i needle = _mm256_set1_epi8((char)value);
    while (length - index >= 32U) {
        __m256i block = _mm256_loadu_si256((const __m256i *)(const void *)(source + index));
        unsigned int mask = (unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(block, needle));
        if (mask != 0U) {
#if defined(_MSC_VER)
            unsigned long bit;
            _BitScanForward(&bit, mask);
            return index + (size_t)bit;
#else
            return index + (size_t)__builtin_ctz(mask);
#endif
        }
        index += 32U;
    }
    while (index < length && (unsigned char)source[index] != value) index++;
    return index;
}
