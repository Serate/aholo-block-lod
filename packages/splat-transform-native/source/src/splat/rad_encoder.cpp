#include "splat/rad_encoder.h"
#include "splat/morton_code.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace splat {

static const uint32_t RAD_MAGIC = 0x30444152;  // "RAD0"
static const uint32_t RAD_CHUNK_MAGIC = 0x43444152; // "RADC"
static const size_t CHUNK_SIZE = 16384;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static void write_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(uint8_t(v & 0xff));
    buf.push_back(uint8_t((v >> 8) & 0xff));
    buf.push_back(uint8_t((v >> 16) & 0xff));
    buf.push_back(uint8_t((v >> 24) & 0xff));
}
static void write_u64_le(std::vector<uint8_t>& buf, uint64_t v) {
    write_u32_le(buf, uint32_t(v & 0xffffffff));
    write_u32_le(buf, uint32_t(v >> 32));
}
static void write_f32_le(std::vector<uint8_t>& buf, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    write_u32_le(buf, u);
}

static void padding8(std::vector<uint8_t>& buf) {
    while (buf.size() & 7) buf.push_back(0);
}

static std::vector<uint8_t> gzip_compress(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    mz_ulong dest_len = data.size() + (data.size() / 100) + 32;
    std::vector<uint8_t> out(dest_len);
    int ret = mz_compress(out.data(), &dest_len, data.data(), data.size());
    if (ret != MZ_OK) return data;
    out.resize(dest_len);
    return out;
}

// ---------------------------------------------------------------------------
// floating-point to half conversion (IEEE 754-2008)
// ---------------------------------------------------------------------------
static uint16_t f32_to_f16(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    int sign = (u >> 16) & 0x8000;
    int exp = ((u >> 23) & 0xff) - 127 + 15;
    int mant = u & 0x007fffff;
    if (exp <= 0) {
        // subnormal
        mant = (mant | 0x00800000) >> (1 - exp);
        exp = 0;
    } else if (exp >= 31) {
        // infinity/nan
        return (uint16_t)(sign | 0x7c00 | (mant ? 0x0200 : 0));
    }
    mant >>= 13;
    return (uint16_t)(sign | (exp << 10) | mant);
}

// ---------------------------------------------------------------------------
// per-property encoding functions (matching Spark rad.rs)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> encode_f16(const float* data, size_t dims, size_t count) {
    std::vector<uint8_t> out(count * dims * 2);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            uint16_t h = f32_to_f16(data[i * dims + d]);
            out[(i * dims + d) * 2] = uint8_t(h & 0xff);
            out[(i * dims + d) * 2 + 1] = uint8_t(h >> 8);
        }
    }
    return out;
}

static std::vector<uint8_t> encode_f32_lebytes(const float* data, size_t dims, size_t count) {
    // Interleaved: each dim is an independent plane in memory, but we output
    // the bytes of each float in little-endian order, dim-major
    std::vector<uint8_t> out(count * dims * 4);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            uint32_t u;
            memcpy(&u, &data[i * dims + d], 4);
            out[(d * count + i) * 4]     = uint8_t(u & 0xff);
            out[(d * count + i) * 4 + 1] = uint8_t((u >> 8) & 0xff);
            out[(d * count + i) * 4 + 2] = uint8_t((u >> 16) & 0xff);
            out[(d * count + i) * 4 + 3] = uint8_t(u >> 24);
        }
    }
    return out;
}

static std::vector<uint8_t> encode_r8(const float* data, size_t dims, size_t count, float min_val, float max_val) {
    float range = (max_val - min_val > 1e-6f) ? (max_val - min_val) : 1.0f;
    std::vector<uint8_t> out(count * dims);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            float v = (data[i * dims + d] - min_val) / range;
            out[i * dims + d] = uint8_t(std::max(0.0f, std::min(255.0f, v * 255.0f)));
        }
    }
    return out;
}

static std::vector<uint8_t> encode_r8_delta(const float* data, size_t dims, size_t count, float min_val, float max_val) {
    // Same as r8 but per-component encoding
    return encode_r8(data, dims, count, min_val, max_val);
}

static std::vector<uint8_t> encode_s8(const float* data, size_t dims, size_t count, float max_val) {
    float scale = (max_val > 1e-6f) ? (127.0f / max_val) : 1.0f;
    std::vector<uint8_t> out(count * dims);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            float v = data[i * dims + d] * scale;
            out[i * dims + d] = uint8_t((int8_t)std::max(-127.0f, std::min(127.0f, v)));
        }
    }
    return out;
}

static std::vector<uint8_t> encode_ln0r8(const float* data, size_t dims, size_t count, float ln_min, float ln_max) {
    float scale = (ln_max - ln_min > 1e-6f) ? (255.0f / (ln_max - ln_min)) : 1.0f;
    std::vector<uint8_t> out(count * dims);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            float v = data[i * dims + d];
            float ln_v;
            if (v <= 0) ln_v = ln_min;
            else ln_v = std::log(v);
            float q = (ln_v - ln_min) * scale;
            out[i * dims + d] = uint8_t(std::max(0.0f, std::min(255.0f, q)));
        }
    }
    return out;
}

static std::vector<uint8_t> encode_quat_oct88r8(const float* data, size_t count) {
    std::vector<uint8_t> out(count * 3);
    for (size_t i = 0; i < count; i++) {
        float qx = data[i * 4];
        float qy = data[i * 4 + 1];
        float qz = data[i * 4 + 2];
        float qw = data[i * 4 + 3];
        // normalize
        float len = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
        if (len > 1e-8f) { qx /= len; qy /= len; qz /= len; qw /= len; }
        // octahedral encoding of 3D direction + angle
        float denom = 1.0f / (std::abs(qx) + std::abs(qy) + std::abs(qz));
        float u = qx * denom;
        float v = qy * denom;
        // fold negative z
        if (qz < 0) {
            u = (1.0f - std::abs(v)) * (qx >= 0 ? 1.0f : -1.0f);
            v = (1.0f - std::abs(u)) * (qy >= 0 ? 1.0f : -1.0f);
        }
        out[i * 3]     = uint8_t(std::max(0, std::min(255, int((u * 0.5f + 0.5f) * 255.0f))));
        out[i * 3 + 1] = uint8_t(std::max(0, std::min(255, int((v * 0.5f + 0.5f) * 255.0f))));
        // angle (from qw) → u8
        float angle = std::acos(std::min(1.0f, std::max(-1.0f, qw)));
        out[i * 3 + 2] = uint8_t(std::max(0, std::min(255, int(angle / 3.14159265f * 255.0f))));
    }
    return out;
}

static std::vector<uint8_t> encode_u16(const uint16_t* data, size_t dims, size_t count) {
    std::vector<uint8_t> out(count * dims * 2);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            uint16_t v = data[i * dims + d];
            out[(i * dims + d) * 2] = uint8_t(v & 0xff);
            out[(i * dims + d) * 2 + 1] = uint8_t(v >> 8);
        }
    }
    return out;
}

static std::vector<uint8_t> encode_u32(const uint32_t* data, size_t dims, size_t count) {
    std::vector<uint8_t> out(count * dims * 4);
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < dims; d++) {
            uint32_t v = data[i * dims + d];
            out[(i * dims + d) * 4]     = uint8_t(v & 0xff);
            out[(i * dims + d) * 4 + 1] = uint8_t((v >> 8) & 0xff);
            out[(i * dims + d) * 4 + 2] = uint8_t((v >> 16) & 0xff);
            out[(i * dims + d) * 4 + 3] = uint8_t(v >> 24);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// percentile computation (matching Spark heuristic)
// ---------------------------------------------------------------------------
static float percentile(const float* data, size_t count, float pct) {
    if (count == 0) return 0;
    std::vector<float> sorted(data, data + count);
    std::sort(sorted.begin(), sorted.end());
    size_t idx = size_t((count - 1) * pct);
    return sorted[idx];
}

// ---------------------------------------------------------------------------
// JSON building (simple, no external dep)
// ---------------------------------------------------------------------------
static std::string build_meta_json(size_t count, int sh_degree, bool has_lod, size_t num_chunks) {
    std::ostringstream j;
    j << "{\n";
    j << "  \"version\": 1,\n";
    j << "  \"type\": \"gsplat\",\n";
    j << "  \"count\": " << count << ",\n";
    if (sh_degree > 0) j << "  \"maxSh\": " << sh_degree << ",\n";
    if (has_lod) j << "  \"lodTree\": true,\n";
    j << "  \"chunkSize\": " << CHUNK_SIZE << ",\n";
    j << "  \"chunks\": [\n";
    for (size_t i = 0; i < num_chunks; i++) {
        if (i > 0) j << ",\n";
        j << "    { \"offset\": 0, \"bytes\": 0 }";
    }
    j << "\n  ],\n";
    j << "  \"splatEncoding\": {\n";
    j << "    \"rgbMin\": 0.0,\n";
    j << "    \"rgbMax\": 1.0,\n";
    if (has_lod) j << "    \"lodOpacity\": true,\n";
    j << "    \"lnScaleMin\": -12.0,\n";
    j << "    \"lnScaleMax\": 9.0,\n";
    j << "    \"sh1Max\": 1.0,\n";
    j << "    \"sh2Max\": 1.0,\n";
    j << "    \"sh3Max\": 1.0\n";
    j << "  }\n";
    j << "}\n";
    return j.str();
}

static std::string build_chunk_meta_json(
    const std::vector<std::pair<const char*, std::pair<const char*, size_t>>>& props)
{
    std::ostringstream j;
    j << "{\"properties\":[";
    for (size_t i = 0; i < props.size(); i++) {
        if (i > 0) j << ",";
        j << "{\"property\":\"" << props[i].first
          << "\",\"encoding\":\"" << props[i].second.first
          << "\",\"bytes\":" << props[i].second.second
          << ",\"offset\":0}";
    }
    j << "]}";
    return j.str();
}

// ---------------------------------------------------------------------------
// encode a single chunk → full RADC buffer
// ---------------------------------------------------------------------------
struct ChunkResult {
    std::vector<uint8_t> data;  // complete RADC chunk (header + payload)
    uint32_t bytes;             // total chunk bytes
    uint32_t payload_bytes;     // compressed payload size
};

static ChunkResult encode_one_chunk(
    const float* center,
    const float* scale,
    const float* quat,
    const float* rgba,
    const float* sh,
    const uint32_t* child_start,
    const uint16_t* child_count,
    size_t base,
    size_t cnt,
    int sh_degree,
    bool has_lod
) {
    struct PropInfo { std::string name; std::string encoding; std::vector<uint8_t> compressed; uint32_t offset; };
    std::vector<PropInfo> props;

    // center: F32LeBytes
    {
        auto raw = encode_f32_lebytes(center + base * 3, 3, cnt);
        props.push_back({"center", "F32LeBytes", gzip_compress(raw), 0});
    }
    // alpha: R8
    {
        float max_alpha = has_lod ? 2.0f : 1.0f;
        std::vector<float> alphas(cnt);
        for (size_t i = 0; i < cnt; i++) alphas[i] = rgba[(base + i) * 4 + 3];
        auto raw = encode_r8(alphas.data(), 1, cnt, 0, max_alpha);
        props.push_back({"alpha", "R8", gzip_compress(raw), 0});
    }
    // rgb: R8Delta
    {
        std::vector<float> rgb(cnt * 3);
        for (size_t i = 0; i < cnt; i++) {
            rgb[i * 3]     = rgba[(base + i) * 4];
            rgb[i * 3 + 1] = rgba[(base + i) * 4 + 1];
            rgb[i * 3 + 2] = rgba[(base + i) * 4 + 2];
        }
        auto raw = encode_r8_delta(rgb.data(), 3, cnt, 0, 1);
        props.push_back({"rgb", "R8Delta", gzip_compress(raw), 0});
    }
    // scale: Ln0R8
    {
        auto raw = encode_ln0r8(scale + base * 3, 3, cnt, -12, 9);
        props.push_back({"scale", "Ln0R8", gzip_compress(raw), 0});
    }
    // quat: Oct88R8
    {
        auto raw = encode_quat_oct88r8(quat + base * 4, cnt);
        props.push_back({"quat", "Oct88R8", gzip_compress(raw), 0});
    }
    // sh (simplified: S8 for all bands)
    if (sh && sh_degree > 0) {
        size_t sh_stride = 3;
        if (sh_degree >= 1) sh_stride += 3;
        if (sh_degree >= 2) sh_stride += 5;
        if (sh_degree >= 3) sh_stride += 7;
        auto raw = encode_s8(sh + base * sh_stride, sh_stride, cnt, 1.0f);
        props.push_back({"sh", "S8", gzip_compress(raw), 0});
    }
    // child_count / child_start (LOD tree metadata)
    if (has_lod && child_count) {
        auto raw = encode_u16(child_count + base, 1, cnt);
        props.push_back({"child_count", "U16", gzip_compress(raw), 0});
    }
    if (has_lod && child_start) {
        auto raw = encode_u32(child_start + base, 1, cnt);
        props.push_back({"child_start", "U32", gzip_compress(raw), 0});
    }

    // Compute payload layout: each prop is at an 8-byte-aligned offset
    uint32_t payload_off = 0;
    for (auto& p : props) {
        p.offset = payload_off;
        payload_off += (uint32_t)p.compressed.size();
        payload_off = (payload_off + 7) & ~7u;  // pad to 8 bytes
    }
    uint32_t payload_bytes = payload_off;

    // Build chunk meta JSON with property offsets
    std::ostringstream cmj;
    cmj << "{\"properties\":[";
    for (size_t i = 0; i < props.size(); i++) {
        if (i > 0) cmj << ",";
        cmj << "{\"property\":\"" << props[i].name
            << "\",\"encoding\":\"" << props[i].encoding
            << "\",\"bytes\":" << props[i].compressed.size()
            << ",\"offset\":" << props[i].offset << "}";
    }
    cmj << "]}";
    auto chunk_meta = cmj.str();

    // Build RADC buffer:
    // [magic:4][meta_len:4][meta_json:variable][padding][payload_size:8][payload:variable]
    std::vector<uint8_t> buf;
    write_u32_le(buf, RAD_CHUNK_MAGIC);
    write_u32_le(buf, (uint32_t)chunk_meta.size());
    buf.insert(buf.end(), chunk_meta.begin(), chunk_meta.end());
    padding8(buf);
    write_u64_le(buf, payload_bytes);
    for (auto& p : props) {
        buf.insert(buf.end(), p.compressed.begin(), p.compressed.end());
        padding8(buf);
    }

    ChunkResult cr;
    cr.data = std::move(buf);
    cr.bytes = (uint32_t)cr.data.size();
    cr.payload_bytes = payload_bytes;
    return cr;
}

// ---------------------------------------------------------------------------
// main entry point
// ---------------------------------------------------------------------------
std::vector<uint8_t> encode_rad(
    const float* center,
    const float* scale,
    const float* quat,
    const float* rgba,
    const float* sh,
    const uint32_t* child_start,
    const uint16_t* child_count,
    size_t count,
    int sh_degree
) {
    size_t num_chunks = (count + CHUNK_SIZE - 1) / CHUNK_SIZE;
    bool has_lod = (child_start != nullptr && child_count != nullptr);

    // ---- first pass: encode all chunks to compute sizes ----
    struct ChunkInfo {
        std::vector<uint8_t> data;
        uint64_t offset;  // relative to end of header
        uint32_t bytes;
    };
    std::vector<ChunkInfo> chunks(num_chunks);

    for (size_t ci = 0; ci < num_chunks; ci++) {
        size_t base = ci * CHUNK_SIZE;
        size_t cnt = std::min(CHUNK_SIZE, count - base);
        auto cr = encode_one_chunk(
            center, scale, quat, rgba, sh,
            child_start, child_count,
            base, cnt, sh_degree, has_lod
        );
        chunks[ci].data = std::move(cr.data);
        chunks[ci].bytes = cr.bytes;
    }

    // ---- build header meta JSON with correct chunk table ----
    std::ostringstream j;
    j << "{\n";
    j << "  \"version\": 1,\n";
    j << "  \"type\": \"gsplat\",\n";
    j << "  \"count\": " << count << ",\n";
    if (sh_degree > 0) j << "  \"maxSh\": " << sh_degree << ",\n";
    if (has_lod) j << "  \"lodTree\": true,\n";
    j << "  \"chunkSize\": " << CHUNK_SIZE << ",\n";
    j << "  \"chunks\": [\n";

    // Chunk offsets are relative to the end of the header (after padding)
    // We compute them by walking: header_size + all previous chunk sizes
    uint64_t hdr_est = 256 + (num_chunks * 80) + j.tellp();  // rough estimate
    uint64_t chunk_cursor = 0;
    for (size_t i = 0; i < num_chunks; i++) {
        if (i > 0) j << ",\n";
        // Write placeholder offset for now, we'll compute actual offsets below
        j << "    { \"offset\": 0, \"bytes\": " << chunks[i].bytes << " }";
    }

    j << "\n  ],\n";
    j << "  \"splatEncoding\": {\n";
    j << "    \"rgbMin\": 0.0,\n";
    j << "    \"rgbMax\": 1.0,\n";
    if (has_lod) j << "    \"lodOpacity\": true,\n";
    j << "    \"lnScaleMin\": -12.0,\n";
    j << "    \"lnScaleMax\": 9.0,\n";
    j << "    \"sh1Max\": 1.0,\n";
    j << "    \"sh2Max\": 1.0,\n";
    j << "    \"sh3Max\": 1.0\n";
    j << "  }\n";
    j << "}\n";
    auto meta_json = j.str();

    // ---- compute header size and chunk offsets ----
    // Header = magic(4) + meta_len(4) + meta_json(variable) + padding(0-7)
    size_t hdr_base = 8 + meta_json.size();
    size_t hdr_pad = (8 - (hdr_base & 7)) & 7;
    size_t hdr_size = hdr_base + hdr_pad;

    // Compute chunk offsets relative to end of header
    uint64_t running_offset = 0;
    for (size_t i = 0; i < num_chunks; i++) {
        chunks[i].offset = running_offset;
        running_offset += chunks[i].bytes;
        running_offset = (running_offset + 7) & ~7ull;  // pad chunk to 8 bytes
    }

    // Rebuild meta with correct offsets
    std::ostringstream j2;
    j2 << "{\n";
    j2 << "  \"version\": 1,\n";
    j2 << "  \"type\": \"gsplat\",\n";
    j2 << "  \"count\": " << count << ",\n";
    if (sh_degree > 0) j2 << "  \"maxSh\": " << sh_degree << ",\n";
    if (has_lod) j2 << "  \"lodTree\": true,\n";
    j2 << "  \"chunkSize\": " << CHUNK_SIZE << ",\n";
    j2 << "  \"chunks\": [\n";
    for (size_t i = 0; i < num_chunks; i++) {
        if (i > 0) j2 << ",\n";
        j2 << "    { \"offset\": " << chunks[i].offset << ", \"bytes\": " << chunks[i].bytes << " }";
    }
    j2 << "\n  ],\n";
    j2 << "  \"splatEncoding\": {\n";
    j2 << "    \"rgbMin\": 0.0,\n";
    j2 << "    \"rgbMax\": 1.0,\n";
    if (has_lod) j2 << "    \"lodOpacity\": true,\n";
    j2 << "    \"lnScaleMin\": -12.0,\n";
    j2 << "    \"lnScaleMax\": 9.0,\n";
    j2 << "    \"sh1Max\": 1.0,\n";
    j2 << "    \"sh2Max\": 1.0,\n";
    j2 << "    \"sh3Max\": 1.0\n";
    j2 << "  }\n";
    j2 << "}\n";
    auto final_meta = j2.str();

    // Compute final header size with correct meta
    hdr_base = 8 + final_meta.size();
    hdr_pad = (8 - (hdr_base & 7)) & 7;
    hdr_size = hdr_base + hdr_pad;

    // ---- write final output ----
    std::vector<uint8_t> out;
    out.reserve(hdr_size + running_offset);
    write_u32_le(out, RAD_MAGIC);
    write_u32_le(out, (uint32_t)final_meta.size());
    out.insert(out.end(), final_meta.begin(), final_meta.end());
    out.resize(hdr_size, 0);  // zero padding

    // Append all chunks (aligned)
    for (size_t ci = 0; ci < num_chunks; ci++) {
        out.insert(out.end(), chunks[ci].data.begin(), chunks[ci].data.end());
        if (ci + 1 < num_chunks) {
            // Pad to 8-byte alignment between chunks
            while (out.size() & 7) out.push_back(0);
        }
    }

    return out;
}

} // namespace splat
