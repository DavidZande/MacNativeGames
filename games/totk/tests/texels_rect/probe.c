/* CPU only: rectangle block-linear copies (nvn_deswizzle_rect / nvn_swizzle_rect / nvn_rect_span, used by ReadTexels
 * and WriteTexels) against the whole-level functions, on random storage. For random formats, targets, sizes, levels,
 * layers and rectangles:
 *   read:  the rectangle equals that crop of the whole-level deswizzle;
 *   write: storage after a rectangle write equals storage after deswizzle all / patch / swizzle all;
 *   span:  every byte the rectangle write changed lies in [lo, hi).
 * Negative controls: a rectangle one row off reads different bytes; a rectangle outside the level is refused. */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? (uint32_t)(rng % n) : 0; }
static void fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(256); }
static uint32_t mipv(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 20) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

int main(void) {
    int fmts[64], nf = 0;   /* formats the layout code knows, a mix of block sizes */
    for (int f = 1; f < 0x110 && nf < 64; f++) { int bw, bh, b = nvn_fmt_bpb(f, &bw, &bh); if (b == 1 || b == 2 || b == 4 || b == 8 || b == 16) fmts[nf++] = f; }
    int targets[] = {1, 1, 1, 2, 4, 7};   /* 2D (most), 3D, 2D array, rectangle */
    long cases = 0, rects = 0;
    for (int it = 0; it < 600; it++) {
        nvn_texdesc d; memset(&d, 0, sizeof d);
        d.format = fmts[rnd(nf)]; d.target = targets[rnd(6)];
        d.width = 1 + rnd(it % 5 == 0 ? 1100 : 300); d.height = 1 + rnd(it % 5 == 0 ? 700 : 300);
        d.depth = d.target == 2 ? 1 + rnd(24) : d.target == 4 ? 1 + rnd(6) : 1;
        if (d.target == 7) d.flags = rnd(2) ? 0x10 : 0;   /* some linear (pitch) storage */
        int lin = nvn_tex_is_linear(&d);
        unsigned maxl = 1; while ((d.width >> maxl) || (d.height >> maxl)) maxl++;
        d.levels = lin ? 1 : 1 + rnd(maxl);
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        if (!L.size || L.size > (64u << 20)) continue;
        uint8_t *orig = malloc(L.size), *a = malloc(L.size), *b = malloc(L.size); fill(orig, L.size);
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
        cases++;
        for (int r = 0; r < 6; r++) {
            unsigned level = rnd(d.levels), layer = d.target == 4 ? rnd(d.depth) : 0;
            uint32_t lw = mipv(d.width, level), lh = mipv(d.height, level), lz = d.target == 2 ? mipv(d.depth, level) : 1;
            uint32_t cols = (lw + bw - 1) / bw, rows = (lh + bh - 1) / bh, row_bytes = cols * bpb;
            uint32_t cx = rnd(cols), cy = rnd(rows), cw = 1 + rnd(cols - cx), ch = 1 + rnd(rows - cy), cz = rnd(lz), cn = 1 + rnd(lz - cz);
            uint32_t x0 = cx * bpb, w = cw * bpb;
            size_t lp = row_bytes, limg = (size_t)lp * rows, pitch = w + rnd(3) * 16, img = pitch * ch + rnd(2) * 64;
            uint8_t *full = malloc(limg * lz), *got = malloc(img * cn), *src = malloc(img * cn);
            if (nvn_deswizzle(&d, level, layer, orig, full, lp)) { free(full); free(got); free(src); continue; }
            rects++;
            /* read */
            CHECK(!nvn_deswizzle_rect(&d, level, layer, orig, got, pitch, img, x0, cy, cz, w, ch, cn), "deswizzle_rect refused fmt %#x tgt %d %ux%ux%u lv%u", d.format, d.target, d.width, d.height, d.depth, level);
            int same = 1;
            for (uint32_t z = 0; z < cn; z++) for (uint32_t y = 0; y < ch; y++)
                if (memcmp(got + z * img + y * pitch, full + (cz + z) * limg + (size_t)(cy + y) * lp + x0, w)) same = 0;
            CHECK(same, "read mismatch fmt %#x tgt %d %ux%ux%u lv%u rect %u,%u,%u %ux%ux%u", d.format, d.target, d.width, d.height, d.depth, level, x0, cy, cz, w, ch, cn);
            /* write */
            fill(src, img * cn);
            memcpy(a, orig, L.size); memcpy(b, orig, L.size);
            CHECK(!nvn_swizzle_rect(&d, level, layer, src, pitch, img, a, x0, cy, cz, w, ch, cn), "swizzle_rect refused");
            for (uint32_t z = 0; z < cn; z++) for (uint32_t y = 0; y < ch; y++)
                memcpy(full + (cz + z) * limg + (size_t)(cy + y) * lp + x0, src + z * img + y * pitch, w);
            nvn_swizzle(&d, level, layer, full, lp, b);
            CHECK(!memcmp(a, b, L.size), "write mismatch fmt %#x tgt %d %ux%ux%u lv%u rect %u,%u,%u %ux%ux%u", d.format, d.target, d.width, d.height, d.depth, level, x0, cy, cz, w, ch, cn);
            /* span */
            uint64_t lo, hi; int ok = !nvn_rect_span(&d, level, layer, cy, ch, cz, cn, &lo, &hi);
            CHECK(ok && lo < hi && hi <= L.size, "span refused or out of storage (%llu-%llu of %llu)", (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)L.size);
            if (ok) { int inside = 1; for (uint64_t i = 0; i < L.size; i++) if (a[i] != orig[i] && (i < lo || i >= hi)) { inside = 0; break; }
                CHECK(inside, "write outside span fmt %#x tgt %d lv%u", d.format, d.target, level); }
            /* negative controls */
            if (rows > 1 && ch < rows) {
                uint32_t oy = cy + 1 < rows - ch + 1 ? cy + 1 : cy - 1;
                nvn_deswizzle_rect(&d, level, layer, orig, got, pitch, img, x0, oy, cz, w, ch, 1);
                int diff = 0; for (uint32_t y = 0; y < ch && !diff; y++) diff = memcmp(got + y * pitch, full + (size_t)(cy + y) * lp + x0, w) != 0;
                CHECK(diff || oy == cy, "control: a rectangle one row off read the same bytes");
            }
            CHECK(nvn_deswizzle_rect(&d, level, layer, orig, got, pitch, img, x0, rows, 0, w, 1, 1) != 0, "control: rectangle below the level accepted");
            free(full); free(got); free(src);
        }
        free(orig); free(a); free(b);
    }
    printf("%ld textures, %ld rectangles, %d checks, %d failed\n", cases, rects, checks, fails);
    printf("{\"status\":\"%s\",\"checks\":%d,\"failed\":%d}\n", fails ? "fail" : "pass", checks, fails);
    return fails != 0;
}
