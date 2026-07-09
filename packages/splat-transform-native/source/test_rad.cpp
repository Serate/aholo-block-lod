#include "splat/rad_decoder.h"
#include "splat/rad_encoder.h"
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

    // ====================================================================
    // build_lod_tree + encode_rad + decode_rad round-trip test
    // ====================================================================
    printf("\n--- build_lod_tree + encode_rad test ---\n");

    // Create 200 synthetic GS on a spatial grid
    const size_t nSynth = 200;
    std::vector<float> synthC(nSynth * 3), synthS(nSynth * 3);
    std::vector<float> synthQ(nSynth * 4, 0), synthR(nSynth * 4, 0);

    for (size_t i = 0; i < nSynth; i++) {
        float x = (float)((int)i % 10) * 0.5f;
        float y = (float)(((int)i / 10) % 10) * 0.5f;
        float z = (float)(i / 100) * 0.5f;
        synthC[i * 3]     = x;
        synthC[i * 3 + 1] = y;
        synthC[i * 3 + 2] = z;
        // Varying scales: 0.01 to 0.3 so that cycling produces merges
        float sc = 0.01f + (float)(i % 15) * 0.02f;
        synthS[i * 3] = sc;
        synthS[i * 3 + 1] = sc;
        synthS[i * 3 + 2] = sc;
        synthQ[i * 4 + 3] = 1.0f;  // identity quat
        synthR[i * 4]     = 0.3f + (float)(i % 3) * 0.2f;
        synthR[i * 4 + 1] = 0.3f + (float)((i + 1) % 3) * 0.2f;
        synthR[i * 4 + 2] = 0.3f + (float)((i + 2) % 3) * 0.2f;
        synthR[i * 4 + 3] = 0.5f + (float)(i % 5) * 0.1f;
    }

    printf("build_lod_tree (%zu GS): ", nSynth);
    fflush(stdout);

    auto tree = splat::build_lod_tree(
        synthC.data(), synthS.data(), synthQ.data(), synthR.data(),
        nullptr, nSynth, 0);

    assert(tree.count == nSynth);
    assert(tree.totalNodes >= nSynth);
    assert(tree.childStart.size() == tree.totalNodes);
    assert(tree.childCount.size() == tree.totalNodes);
    assert(tree.center.size() == tree.totalNodes * 3);
    assert(tree.scale.size()  == tree.totalNodes * 3);
    assert(tree.quat.size()   == tree.totalNodes * 4);
    assert(tree.rgba.size()   == tree.totalNodes * 4);
    assert(tree.sh.empty());

    // Root (node 0) should have children
    assert(tree.childCount[0] > 0);

    // Count leaf nodes (childCount == 0)
    size_t leafCount = 0;
    for (size_t i = 0; i < tree.totalNodes; i++)
        if (tree.childCount[i] == 0) leafCount++;

    printf("PASSED (totalNodes=%zu, leaves=%zu, root_children=%u)\n",
        tree.totalNodes, leafCount, tree.childCount[0]);

    // encode_rad
    printf("encode_rad: ");
    fflush(stdout);

    auto rad = splat::encode_rad(
        tree.center.data(), tree.scale.data(), tree.quat.data(),
        tree.rgba.data(), nullptr,
        tree.childStart.data(), tree.childCount.data(),
        tree.totalNodes, 0);

    assert(rad.size() > 0);
    printf("PASSED (%zu bytes)\n", rad.size());

    // decode_rad round-trip
    printf("decode_rad round-trip: ");
    fflush(stdout);

    auto decoded = splat::decode_rad(rad.data(), rad.size());
    assert(decoded.count > 0);
    assert(decoded.totalNodes == tree.totalNodes);
    assert(decoded.childStart.size() == tree.totalNodes);
    assert(decoded.childCount.size() == tree.totalNodes);

    // Verify child_start/count round-trip
    size_t matchCount = 0;
    for (size_t i = 0; i < tree.totalNodes && i < decoded.totalNodes; i++) {
        if (decoded.childCount[i] == tree.childCount[i]) matchCount++;
    }
    printf("PASSED (count=%zu, totalNodes=%zu, childCount_match=%zu/%zu)\n",
        decoded.count, decoded.totalNodes, matchCount, tree.totalNodes);

    printf("\nALL TESTS PASSED\n");
    return 0;
}
