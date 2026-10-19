// Tegra X1 块线性排布（公开描述见 deko3d / Ryujinx BlockLinearLayout）：
//   GOB = 64 字节 × 8 行 = 512 字节；GOB 内 (x 字节, y 行) 的偏移 =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   块 = 1 个 GOB 宽 × 2^tileH 个 GOB 高 × 2^tileD 个 GOB 深，块内先排 y 再排 z；块按 x → y → z 行优先铺满本级。
// 级 / 层起点与本级实际块高块深取自 nvn_tex.c，这里只做级内寻址与搬运。
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // 一个块的字节 | bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // 第几层块（深度方向） | which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // 第几行块 | which block row
         + (x / 64) * block                                            // 第几列块 | which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // 块内第几个 GOB | which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// 本级几何：块宽字节数、块行数、切片数，以及本级本层在存储里的起点；不支持的贴图返回 -1
// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || nvn_tex_is_linear(d) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (pitch < row || stride < row) return -1;
        for (uint32_t y = 0; y < rows; y++) {
            if (to_linear) memcpy(lin + y * pitch, bl + (size_t)y * stride, row);
            else memcpy(bl + (size_t)y * stride, lin + y * pitch, row);
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {             // 16 字节一段：GOB 内 16 字节连续，且整除所有块宽 | 16-byte segments: 16 bytes are contiguous within a GOB, and 16 divides all block widths
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, row_bytes, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

/* TOTK: one rectangle of a level / layer instead of all of it: (x0 bytes, y0 block rows, z0 slices) + w bytes × h block
   rows × nz slices (z only for 3D; other targets take z0 = 0, nz = 1). lin: pitch bytes a row, img bytes a slice.
   Inside a GOB 16 aligned bytes are contiguous, so a row is copied in runs that stop at 16-byte boundaries. The game
   writes small tiles into a 5120x5120 texture with WriteTexels; the whole-level copy each time was ~2 x 13 MB. */
static int move_rect(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, size_t img,
                     uint32_t x0, uint32_t y0, uint32_t z0, uint32_t w, uint32_t h, uint32_t nz, int to_linear) {
    if (!w || !h || !nz) return 0;
    if (nvn_tex_is_linear(d)) {
        if (level || layer || z0 || nz != 1 || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        uint32_t row = (uint32_t)((d->width + bw - 1) / bw) * bpb, rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (stride < row || pitch < w || x0 + w > row || y0 + h > rows) return -1;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *p = bl + (size_t)(y0 + y) * stride + x0;
            if (to_linear) memcpy(lin + y * pitch, p, w); else memcpy(p, lin + y * pitch, w);
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < w || x0 + w > row_bytes || y0 + h > rows || z0 + nz > slices || (nz > 1 && img < pitch * h)) return -1;
    bl += base;
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = lin + z * img + (size_t)y * pitch;
            for (uint32_t x = 0; x < w;) {
                uint32_t ax = x0 + x, n = 16 - ax % 16; if (n > w - x) n = w - x;
                uint8_t *p = bl + nvn_bl_offset(ax, y0 + y, z0 + z, row_bytes, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
                x += n;
            }
        }
    return 0;
}
int nvn_deswizzle_rect(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t pitch, size_t img,
                       uint32_t x0, uint32_t y0, uint32_t z0, uint32_t w, uint32_t h, uint32_t nz) {
    return move_rect(d, level, layer, (uint8_t *)src, (uint8_t *)dst, pitch, img, x0, y0, z0, w, h, nz, 1);
}
int nvn_swizzle_rect(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t pitch, size_t img, void *dst,
                     uint32_t x0, uint32_t y0, uint32_t z0, uint32_t w, uint32_t h, uint32_t nz) {
    return move_rect(d, level, layer, (uint8_t *)dst, (uint8_t *)src, pitch, img, x0, y0, z0, w, h, nz, 0);
}
/* The storage bytes [*lo, *hi) (from the start of the whole texture) that rows y0..y0+h-1 of slices z0..z0+nz-1 of this
   level / layer lie in: whole block rows (all block columns), a superset of what move_rect touches. -1 when unsupported. */
int nvn_rect_span(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t y0, uint32_t h, uint32_t z0, uint32_t nz, uint64_t *lo, uint64_t *hi) {
    if (!h || !nz) return -1;
    if (nvn_tex_is_linear(d)) {
        if (level || layer || z0 || nz != 1) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        uint32_t row = (uint32_t)((d->width + bw - 1) / bw) * bpb, stride = nvn_tex_linear_stride(d);
        *lo = (uint64_t)y0 * stride; *hi = (uint64_t)(y0 + h - 1) * stride + row;
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (y0 + h > rows || z0 + nz > slices) return -1;
    uint64_t bh = 1u << th, bd = 1u << td, gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t brow = gobs_x * 512 * bh * bd, zl = blocks_y * brow;   // one block row; one layer of blocks along depth
    *lo = base + (z0 / bd) * zl + (y0 / (8 * bh)) * brow;
    *hi = base + ((z0 + nz - 1) / bd) * zl + ((y0 + h - 1) / (8 * bh) + 1) * brow;
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}
