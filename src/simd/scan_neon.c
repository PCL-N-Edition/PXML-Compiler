#include <arm_neon.h>
#include <stddef.h>

size_t pxml_scan_byte_neon(const char *source, size_t length, unsigned char value)
{
    size_t index = 0U;
    uint8x16_t needle = vdupq_n_u8(value);
    while (length - index >= 16U) {
        uint8x16_t block = vld1q_u8((const uint8_t *)(const void *)(source + index));
        uint8x16_t matches = vceqq_u8(block, needle);
        uint64x2_t lanes = vreinterpretq_u64_u8(matches);
        if (vgetq_lane_u64(lanes, 0) != 0U || vgetq_lane_u64(lanes, 1) != 0U) {
            size_t lane;
            for (lane = 0U; lane < 16U; ++lane) {
                if ((unsigned char)source[index + lane] == value) return index + lane;
            }
        }
        index += 16U;
    }
    while (index < length && (unsigned char)source[index] != value) index++;
    return index;
}
