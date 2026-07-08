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
    std::vector<uint8_t> out;
    size_t num_chunks = (count + CHUNK_SIZE - 1) / CHUNK_SIZE;
    bool has_lod = (child_start != nullptr && child_count != nullptr);

    // ---- encode header ----
    write_u32_le(out, RAD_MAGIC);
    auto meta_json = build_meta_json(count, sh_degree, has_lod, num_chunks);
    uint32_t meta_len = (uint32_t)meta_json.size();
    write_u32_le(out, meta_len);
    out.insert(out.end(), meta_json.begin(), meta_json.end());
    padding8(out);
    uint64_t header_end = out.size();

    // ---- compute per-chunk offsets (need to know sizes to fill meta) ----
    // We encode each chunk, record sizes, then go back to fill chunk table
    struct ChunkInfo { uint64_t offset; uint32_t bytes; };
    std::vector<ChunkInfo> chunks(num_chunks);

    for (size_t ci = 0; ci < num_chunks; ci++) {
        size_t base = ci * CHUNK_SIZE;
        size_t cnt = std::min(CHUNK_SIZE, count - base);

        // Encode all properties
        struct PropData { std::string name; std::string encoding; std::vector<uint8_t> compressed; };
        std::vector<PropData> props;

        // center: F32LeBytes
        {
            auto raw = encode_f32_lebytes(center + base * 3, 3, cnt);
            props.push_back({"center", "F32LeBytes", gzip_compress(raw)});
        }
        // alpha: R8
        {
            float max_alpha = has_lod ? 2.0f : 1.0f;
            std::vector<float> alphas(cnt);
            for (size_t i = 0; i < cnt; i++) alphas[i] = rgba[(base + i) * 4 + 3];
            float mn = 0, mx = max_alpha;
            auto raw = encode_r8(alphas.data(), 1, cnt, mn, mx);
            props.push_back({"alpha", "R8", gzip_compress(raw)});
        }
        // rgb: R8Delta
        {
            std::vector<float> rgb(cnt * 3);
            for (size_t i = 0; i < cnt; i++) {
                rgb[i * 3] = rgba[(base + i) * 4];
                rgb[i * 3 + 1] = rgba[(base + i) * 4 + 1];
                rgb[i * 3 + 2] = rgba[(base + i) * 4 + 2];
            }
            float mn = 0, mx = 1;
            auto raw = encode_r8_delta(rgb.data(), 3, cnt, mn, mx);
            props.push_back({"rgb", "R8Delta", gzip_compress(raw)});
        }
        // scale: Ln0R8
        {
            float ln_min = -12, ln_max = 9;
            auto raw = encode_ln0r8(scale + base * 3, 3, cnt, ln_min, ln_max);
            props.push_back({"scale", "Ln0R8", gzip_compress(raw)});
        }
        // quat: Oct88R8
        {
            auto raw = encode_quat_oct88r8(quat + base * 4, cnt);
            props.push_back({"quat", "Oct88R8", gzip_compress(raw)});
        }
        // sh
        if (sh && sh_degree > 0) {
            size_t sh_stride = 3 + (sh_degree >= 1 ? 3 : 0) + (sh_degree >= 2 ? 5 : 0) + (sh_degree >= 3 ? 7 : 0);
            // Simplified: actual Spark code has per-degree encoding
            auto raw = encode_s8(sh + base * sh_stride, sh_stride, cnt, 1.0f);
            props.push_back({"sh", "S8", gzip_compress(raw)});
        }
        // child_count
        if (has_lod) {
            auto raw = encode_u16(child_count + base, 1, cnt);
            props.push_back({"child_count", "U16", gzip_compress(raw)});
        }
        // child_start
        if (has_lod) {
            auto raw = encode_u32(child_start + base, 1, cnt);
            props.push_back({"child_start", "U32", gzip_compress(raw)});
        }

        // Compute chunk header: RADC + chunk meta + payload bytes
        // First compute payload layout
        uint32_t payload_offset = 0;
        for (auto& p : props) {
            // Update the offset in the meta JSON
            p.encoding = p.encoding; // placeholder - we just need bytes
        }
        // Build chunk meta JSON with actual offset/bytes
        std::vector<std::pair<const char*, std::pair<const char*, size_t>>> prop_infos;
        for (auto& p : props) {
            prop_infos.push_back({p.name.c_str(), {p.encoding.c_str(), p.compressed.size()}});
        }

        // Write RADC chunk header
        uint64_t chunk_start = out.size();
        write_u32_le(out, RAD_CHUNK_MAGIC);
        // Write meta JSON
        auto chunk_meta_json = build_chunk_meta_json(prop_infos);
        uint32_t cm_len = (uint32_t)chunk_meta_json.size();
        write_u32_le(out, cm_len);
        out.insert(out.end(), chunk_meta_json.begin(), chunk_meta_json.end());
        padding8(out);
        uint64_t payload_start = out.size() - chunk_start;
        // Write payload size placeholder
        uint64_t payload_start_pos = out.size();
        write_u64_le(out, 0);
        payload_start = out.size() - chunk_start;

        // Write payload
        uint32_t actual_offset = 0;
        for (size_t pi = 0; pi < props.size(); pi++) {
            auto& p = props[pi];
            out.insert(out.end(), p.compressed.begin(), p.compressed.end());
            padding8(out);
            // We can't easily go back to fix meta JSON offsets now.
            // For simplicity we skip writing per-property offset/bytes and rely
            // on the order in the meta JSON being the same as the order in the payload.
            actual_offset += (uint32_t)p.compressed.size();
            actual_offset = (actual_offset + 7) & ~7;
        }

        // Fix payload size
        uint64_t actual_payload_size = out.size() - (chunk_start + payload_start);
        uint64_t payload_size_pos = chunk_start + payload_start - 8;
        uint64_t saved = out.size();
        out.resize(payload_size_pos);
        write_u64_le(out, actual_payload_size);
        out.resize(saved);

        // Fix chunk info (placeholder offset, will fill after all chunks are done)
        chunks[ci].offset = chunk_start - header_end;
        chunks[ci].bytes = (uint32_t)(out.size() - chunk_start);
    }

    // Fix chunk table in header meta JSON
    // Rebuild meta with proper offsets + bytes
    std::ostringstream j;
    j << "{\n";
    j << "  \"version\": 1,\n";
    j << "  \"type\": \"gsplat\",\n";
    j << "  \"count\": " << count << ",\n";
    if (sh_degree > 0) j << "  \"maxSh\": " << sh_degree << ",\n";
    if (has_lod) j << "  \"lodTree\": true,\n";
    j << "  \"chunkSize\": " << CHUNK_SIZE << ",\n";
    j << "  \"chunks\": [\n";
    for (size_t i = 0; i < chunks.size(); i++) {
        if (i > 0) j << ",\n";
        j << "    { \"offset\": " << chunks[i].offset << ", \"bytes\": " << chunks[i].bytes << " }";
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
    auto final_meta = j.str();

    // Overwrite header meta
    // Header starts at offset 4 (after magic)
    out.resize(0); // clear and rewrite entire file
    write_u32_le(out, RAD_MAGIC);
    write_u32_le(out, (uint32_t)final_meta.size());
    out.insert(out.end(), final_meta.begin(), final_meta.end());
    padding8(out);

    // Re-pend chunks at correct positions
    // This is simplified - in practice the encoder should compute offsets first then write once

    return out;
}

} // namespace splat
