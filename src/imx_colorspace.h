/**
 * By the book yuv420p to yuyv422 colorspace conversion
 */

static inline void i420_to_yuy2_c(
    uint8_t *dst,
    const uint8_t *ysrc,
    const uint8_t *usrc,
    const uint8_t *vsrc,
    int width,
    int height,
    int dst_stride,
    int lum_stride,
    int chr_stride)
{
    const int chroma_width = width / 2;
    int i;

    for (i = 0; i < height; ++i) {
        uint32_t *pack_dst = dst;
        const uint8_t *two_ysrc = ysrc;
        const uint8_t *one_usrc = usrc;
        const uint8_t *one_vsrc = vsrc;

        int j;
        for (j = 0; j < chroma_width; ++j) {
            *pack_dst++ =   (two_ysrc[0] <<  0) +
                            (one_usrc[0] <<  8) +
                            (two_ysrc[1] << 16) +
                            (one_vsrc[0] << 24);

            two_ysrc += 2;
            one_usrc++;
            one_vsrc++;
        }

        dst  += dst_stride;
        ysrc += lum_stride;

        if  (i & 1) {
            usrc += chr_stride;
            vsrc += chr_stride;
        }
    }
}
