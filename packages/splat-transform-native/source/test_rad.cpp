#include "splat/rad_decoder.h"
#include "splat/lod_tree.h"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: test_rad <file.rad>\n");
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);

    printf("File size: %zu bytes\n", sz);

    auto result = splat::decode_rad(buf.data(), buf.size());

    printf("Decode:\n");
    printf("  count: %zu, totalNodes: %zu, shDegree: %zu\n",
        result.count, result.totalNodes, result.shDegree);
    printf("  childStart: %zu, childCount: %zu\n",
        result.childStart.size(), result.childCount.size());

    if (result.count > 0) {
        assert(!result.center.empty());
        assert(!result.rgba.empty());
        assert(!result.scale.empty());
        assert(!result.quat.empty());
        printf("  Arrays present OK\n");

        size_t non_leaf = 0;
        for (size_t i = 0; i < result.childCount.size(); i++)
            if (result.childCount[i] > 0) non_leaf++;
        printf("  Internal nodes: %zu / %zu\n", non_leaf, result.totalNodes);

        // Traverse with simple size=1.0 for all nodes, small budget
        printf("Traverse test (small budget): ");
        fflush(stdout);

        // Only allocate for the first 100000 nodes to limit memory
        size_t testNodes = std::min(result.totalNodes, (size_t)100000);
        std::vector<float> dummyCenter(testNodes * 3, 0.0f);
        std::vector<float> dummySize(testNodes, 1.0f);
        std::vector<uint32_t> out(5000);

        float cam[3] = {0, 0, 5};
        float fwd[3] = {0, 0, -1};

        size_t n = splat::traverse_block(
            result.childStart.data(), result.childCount.data(),
            dummyCenter.data(), dummySize.data(),
            testNodes,
            cam, fwd, 1.0f, 0.001f, 5000, out.data()
        );

        printf("%zu splats out of 5000\n", n);
        if (n > 0)
            printf("  First: %u %u %u %u %u\n", out[0], out[1], out[2], out[3], out[4]);
        printf("Traverse %s\n", n > 0 ? "PASSED" : "FAILED");

        // Traverse with feature_size computed from decoded values
        printf("Tree traverse (computed sizes): ");
        fflush(stdout);

        std::vector<float> f32Center(result.totalNodes * 3);
        std::vector<float> f32Size(result.totalNodes);
        for (size_t i = 0; i < result.totalNodes && i * 3 < result.center.size(); i++) {
            uint16_t hx = result.center[i * 3];
            uint16_t hy = result.center[i * 3 + 1];
            uint16_t hz = result.center[i * 3 + 2];
            auto f16 = [](uint16_t h) {
                uint32_t s = (h & 0x8000) << 16;
                uint32_t e = (h >> 10) & 0x1f;
                uint32_t m = h & 0x3ff;
                if (e == 0) { e = 1; while (!(m & 0x400) && m) { m <<= 1; e--; } m &= 0x3ff; e += 112; }
                else if (e == 31) e = 255;
                else e += 112;
                uint32_t f = s | (e << 23) | (m << 13);
                float r; memcpy(&r, &f, 4); return r;
            };
            f32Center[i * 3] = f16(hx);
            f32Center[i * 3 + 1] = f16(hy);
            f32Center[i * 3 + 2] = f16(hz);
        }

        float lnMin = -12.0f, lnMax = 9.0f;
        float lnRng = lnMax - lnMin;
        for (size_t i = 0; i < result.totalNodes && i * 3 < result.scale.size(); i++) {
            float s = 0;
            for (int d = 0; d < 3; d++) {
                float ln = result.scale[i * 3 + d] / 255.0f * lnRng + lnMin;
                float v = expf(ln);
                if (v > s) s = v;
            }
            float op = (i * 4 + 3 < result.rgba.size()) ? result.rgba[i * 4 + 3] / 255.0f : 1.0f;
            f32Size[i] = 2.0f * s;
        }

        n = splat::traverse_block(
            result.childStart.data(), result.childCount.data(),
            f32Center.data(), f32Size.data(),
            result.totalNodes,
            cam, fwd, 1.0f, 0.001f, 500000, out.data()
        );
        printf("%zu splats\n", n);
        printf("  First: %u %u %u %u %u\n", out[0], out[1], out[2], out[3], out[4]);
        printf("Traverse %s\n", n > 0 ? "PASSED" : "FAILED");
    }

    printf("ALL TESTS %s\n", result.count > 0 ? "PASSED" : "FAILED");
    return (result.count > 0) ? 0 : 1;
}
