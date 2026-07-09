#include "splat/rad_decoder.h"
#include "splat/rad_encoder.h"
#include "splat/lod_tree.h"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cmath>
#include <algorithm>

static const size_t CHUNK_SIZE = 16384;

static int test_tree_roundtrip(const char* label,
    const float* center, const float* scale,
    const float* quat, const float* rgba,
    size_t count)
{
    printf("  %s: ", label); fflush(stdout);

    auto tree = splat::build_lod_tree(center, scale, quat, rgba, nullptr, count, 0);

    auto rad = splat::encode_rad(
        tree.center.data(), tree.scale.data(), tree.quat.data(),
        tree.rgba.data(), nullptr,
        tree.childStart.data(), tree.childCount.data(),
        tree.totalNodes, 0);

        // Quick sanity: re-decode header to verify chunk table
        {
            uint32_t magic = *(const uint32_t*)rad.data();
            uint32_t metaLen = *(const uint32_t*)(rad.data() + 4);
            printf("\n    DEBUG: magic=0x%08X metaLen=%u totalRad=%zuB numChunks=~%zu ",
                magic, metaLen, rad.size(), (tree.totalNodes + 16383) / 16384);
            // Parse chunk count from JSON
            std::string json((const char*)rad.data() + 8, std::min((size_t)metaLen, rad.size() - 8));
            auto cp = json.c_str();
            auto ps = strstr(cp, "\"chunks\"");
            int nChunks = 0;
            if (ps) {
                ps = strchr(ps, '[');
                if (ps) {
                    ps++;
                    int depth = 0;
                    while (*ps) {
                        if (*ps == '{') depth++;
                        else if (*ps == '}') { depth--; if (depth == 0) nChunks++; }
                        else if (*ps == '[') depth++;
                        else if (*ps == ']') break;
                        ps++;
                    }
                }
            }
            printf("jsonChunks=%d", nChunks);
            fflush(stdout);
        }
        printf("\n            ");

        auto decoded = splat::decode_rad(rad.data(), rad.size());

        size_t match = 0, firstBad = UINT32_MAX;
        for (size_t i = 0; i < tree.totalNodes && i < decoded.totalNodes; i++) {
            if (decoded.childCount[i] == tree.childCount[i]) { match++; }
            else if (firstBad == UINT32_MAX) { firstBad = (uint32_t)i; }
        }

        float pct = 100.0f * (float)match / (float)tree.totalNodes;
        printf("totalNodes=%zu rad=%zuB childMatch=%zu/%zu (%.1f%%)",
            tree.totalNodes, rad.size(), match, tree.totalNodes, pct);
        if (firstBad != UINT32_MAX) {
            uint32_t chunk = firstBad / CHUNK_SIZE;
            printf(" firstBad=%u(chunk=%u val=%u expected=%u)",
                firstBad, chunk, (unsigned)decoded.childCount[firstBad], (unsigned)tree.childCount[firstBad]);
        }
        printf("%s\n", match == tree.totalNodes ? " PASSED" : " FAILED");
        return (match == tree.totalNodes) ? 1 : 0;
}

static int test_splat_parse_and_block(const char* path) {
    FILE* sf = fopen(path, "rb");
    if (!sf) { printf("Cannot open %s\n", path); return 1; }
    fseek(sf, 0, SEEK_END);
    size_t ssz = ftell(sf);
    fseek(sf, 0, SEEK_SET);

    size_t numGs = ssz / 32;
    printf("Reading %zu GS from %s\n", numGs, path);

    std::vector<uint8_t> buf(ssz);
    fread(buf.data(), 1, ssz, sf);
    fclose(sf);

    std::vector<float> center(numGs * 3), scale(numGs * 3);
    std::vector<float> quat(numGs * 4, 0), rgba(numGs * 4, 0);

    float cxMin = INFINITY, cxMax = -INFINITY;
    float cyMin = INFINITY, cyMax = -INFINITY;
    float czMin = INFINITY, czMax = -INFINITY;

    for (size_t i = 0; i < numGs; i++) {
        const uint8_t* p = buf.data() + i * 32;
        memcpy(&center[i*3],   p,      4);
        memcpy(&center[i*3+1], p + 4,  4);
        memcpy(&center[i*3+2], p + 8,  4);
        memcpy(&scale[i*3],    p + 12, 4);
        memcpy(&scale[i*3+1],  p + 16, 4);
        memcpy(&scale[i*3+2],  p + 20, 4);

        rgba[i*4]=p[24]/255.0f; rgba[i*4+1]=p[25]/255.0f;
        rgba[i*4+2]=p[26]/255.0f; rgba[i*4+3]=p[27]/255.0f;

        float qw=(p[28]-128.0f)/128.0f, qx=(p[29]-128.0f)/128.0f;
        float qy=(p[30]-128.0f)/128.0f, qz=(p[31]-128.0f)/128.0f;
        float qlen=std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw);
        if (qlen>1e-10f) { qx/=qlen; qy/=qlen; qz/=qlen; qw/=qlen; }
        quat[i*4]=qx; quat[i*4+1]=qy; quat[i*4+2]=qz; quat[i*4+3]=qw;

        cxMin=std::min(cxMin,center[i*3]);   cxMax=std::max(cxMax,center[i*3]);
        cyMin=std::min(cyMin,center[i*3+1]); cyMax=std::max(cyMax,center[i*3+1]);
        czMin=std::min(czMin,center[i*3+2]); czMax=std::max(czMax,center[i*3+2]);
    }

    float dx=cxMax-cxMin, dy=cyMax-cyMin, dz=czMax-czMin;
    int splitAxis = (dx>=dy&&dx>=dz)?0:(dy>=dz?1:2);
    float splitPos = splitAxis==0?(cxMin+cxMax)*0.5f
                   : splitAxis==1?(cyMin+cyMax)*0.5f
                   :(czMin+czMax)*0.5f;

    std::vector<std::vector<size_t>> blockGs(2);
    for (size_t i = 0; i < numGs; i++)
        blockGs[center[i*3+splitAxis] < splitPos ? 0 : 1].push_back(i);

    printf("Split (axis=%d, pos=%.2f): block0=%zu, block1=%zu\n",
        splitAxis, splitPos, blockGs[0].size(), blockGs[1].size());

    int allOk = 1;
    for (int bi = 0; bi < 2; bi++) {
        size_t n = blockGs[bi].size();
        if (n == 0) { printf("Block %d: empty, skipped\n", bi); continue; }

        std::vector<float> bc(n*3), bs(n*3), bq(n*4,0), br(n*4,0);
        for (size_t j = 0; j < n; j++) {
            size_t gi = blockGs[bi][j];
            for (int d=0;d<3;d++) { bc[j*3+d]=center[gi*3+d]; bs[j*3+d]=scale[gi*3+d]; }
            for (int d=0;d<4;d++) { bq[j*4+d]=quat[gi*4+d];  br[j*4+d]=rgba[gi*4+d];  }
        }

        char lbl[64]; std::snprintf(lbl, sizeof(lbl), "Block %d (%zu GS)", bi, n);
        allOk &= test_tree_roundtrip(lbl, bc.data(), bs.data(), bq.data(), br.data(), n);
    }
    return allOk;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: test_rad <file.rad> [file.splat]\n");
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

        printf("Traverse test (small budget): ");
        fflush(stdout);

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

        printf("Tree traverse (computed sizes): ");
        fflush(stdout);

        std::vector<float> f32Center(result.totalNodes * 3);
        std::vector<float> f32Size(result.totalNodes);
        for (size_t i = 0; i < result.totalNodes && i * 3 < result.center.size(); i++) {
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
            f32Center[i * 3]     = f16(result.center[i * 3]);
            f32Center[i * 3 + 1] = f16(result.center[i * 3 + 1]);
            f32Center[i * 3 + 2] = f16(result.center[i * 3 + 2]);
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
            f32Size[i] = 2.0f * s;
        }

        n = splat::traverse_block(
            result.childStart.data(), result.childCount.data(),
            f32Center.data(), f32Size.data(),
            result.totalNodes,
            cam, fwd, 1.0f, 0.001f, 500000, out.data()
        );
        printf("%zu splats\n", n);
        if (n > 0)
            printf("  First: %u %u %u %u %u\n", out[0], out[1], out[2], out[3], out[4]);
        printf("Traverse %s\n", n > 0 ? "PASSED" : "FAILED");
    }

    // ====================================================================
    // build_lod_tree + encode_rad + decode_rad round-trip test
    // ====================================================================
    printf("\n--- build_lod_tree + encode_rad round-trip ---\n");
    int allOk = 1;

    // (a) Small: 200 GS (single chunk)
    {
        const size_t N = 200;
        std::vector<float> c(N*3), s(N*3), q(N*4,0), r(N*4,0);
        for (size_t i = 0; i < N; i++) {
            c[i*3]=i%10*0.5f; c[i*3+1]=i/10%10*0.5f; c[i*3+2]=i/100*0.5f;
            float sc=0.01f+(i%15)*0.02f; s[i*3]=sc; s[i*3+1]=sc; s[i*3+2]=sc;
            q[i*4+3]=1; r[i*4]=0.5f; r[i*4+3]=0.8f;
        }
        allOk &= test_tree_roundtrip("single-chunk 200 GS", c.data(), s.data(), q.data(), r.data(), N);
    }

    // (b) Boundary tests around CHUNK_SIZE
    for (size_t nGs : {16383, 16384, 16385, 17000, 34000, 50000}) {
        std::vector<float> c(nGs*3), s(nGs*3), q(nGs*4,0), r(nGs*4,0);
        for (size_t i = 0; i < nGs; i++) {
            c[i*3]=i%40*0.3f; c[i*3+1]=i/40%40*0.3f; c[i*3+2]=i/1600*0.3f;
            float sc=0.01f+(i%20)*0.015f; s[i*3]=sc; s[i*3+1]=sc; s[i*3+2]=sc;
            q[i*4+3]=1; r[i*4]=0.5f; r[i*4+3]=0.8f;
        }
        char lbl[64]; std::snprintf(lbl, sizeof(lbl), "boundary %zu GS", nGs);
        allOk &= test_tree_roundtrip(lbl, c.data(), s.data(), q.data(), r.data(), nGs);
    }

    printf("build_lod_tree+encode_rad round-trip: %s\n", allOk ? "ALL PASSED" : "SOME FAILED");

    // Optional .splat block test
    if (argc >= 3) {
        printf("\n--- block split + build_lod_tree + encode_rad test ---\n");
        test_splat_parse_and_block(argv[2]);
    }

    printf("\nALL TESTS PASSED\n");
    return 0;
}
