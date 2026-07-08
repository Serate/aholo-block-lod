#include "splat/rad_decoder.h"
#include "splat/lod_tree.h"
#include <cstdio>
#include <cassert>
#include <cstring>

// Simple f16→f32 (same as N-API binding)
static float f16_to_f32(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    if (exp == 0) { while (!(mant & 0x400) && mant) { mant <<= 1; exp--; } mant &= 0x3ff; exp += 112; }
    else if (exp == 31) exp = 255;
    else exp += 112;
    uint32_t f = sign | (exp << 23) | (mant << 13);
    float r; memcpy(&r, &f, 4); return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: test_rad <file.rad>\n");
        return 1;
    }

    // Read file
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);

    printf("File size: %zu bytes\n", sz);

    // Decode
    auto result = splat::decode_rad(buf.data(), buf.size());

    printf("Decode result:\n");
    printf("  count:      %zu\n", result.count);
    printf("  totalNodes: %zu\n", result.totalNodes);
    printf("  shDegree:   %zu\n", result.shDegree);
    printf("  childStart: %zu x u32\n", result.childStart.size());
    printf("  childCount: %zu x u16\n", result.childCount.size());

    if (result.count > 0) {
        printf("\nFirst splat sample:\n");
        printf("  GS count: %zu\n", result.count);

        assert(!result.center.empty());
        assert(!result.rgba.empty());
        assert(!result.scale.empty());
        assert(!result.quat.empty());
        printf("  All required arrays present ✓\n");

        if (!result.childStart.empty() && !result.childCount.empty()) {
            size_t non_leaf = 0;
            for (size_t i = 0; i < result.childCount.size(); i++)
                if (result.childCount[i] > 0) non_leaf++;
            printf("  Internal nodes (with children): %zu / %zu\n", non_leaf, result.totalNodes);
        }
    }

    printf("\nDecode test %s!\n", result.count > 0 ? "PASSED" : "FAILED");
    return (result.count > 0) ? 0 : 1;
}
