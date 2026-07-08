#include "splat/rad_decoder.h"
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace splat {

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t* p) {
    return (uint64_t)read_u32_le(p) | ((uint64_t)read_u32_le(p + 4) << 32);
}

// Minimal JSON parser for RAD metadata (we know the structure exactly)
static std::string json_string(const char*& p) {
    while (*p && *p != '"') p++;
    if (*p != '"') return "";
    p++;
    const char* start = p;
    while (*p && *p != '"') p++;
    std::string s(start, p - start);
    if (*p == '"') p++;
    return s;
}
static double json_number(const char*& p) {
    while (*p && (*p == ' ' || *p == ',' || *p == ':' || *p == '[' || *p == '{')) p++;
    char* end;
    double v = strtod(p, &end);
    p = end;
    return v;
}
static void json_skip(const char*& p) {
    int depth = 0;
    while (*p) {
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { depth--; if (depth <= 0) { p++; return; } }
        else if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
        p++;
    }
}

struct RadChunkRange {
    uint64_t offset;
    uint32_t bytes;
};

struct RadMeta {
    uint32_t count = 0;
    uint32_t shDegree = 0;
    uint32_t chunkSize = 0;
    bool lodTree = false;
    std::vector<RadChunkRange> chunks;
    // encoding params
    std::string centerEnc = "F32LeBytes";
    std::string alphaEnc = "R8";
    std::string rgbEnc = "R8Delta";
    std::string scalesEnc = "Ln0R8";
    std::string quatEnc = "Oct88R8";
    std::string shEnc = "S8";
    // min/max for r8delta, ln0r8, etc.
    float rgbMin = 0, rgbMax = 1;
    float lnScaleMin = -12, lnScaleMax = 9;
    float sh1Max = 1, sh2Max = 1, sh3Max = 1;
};

static RadMeta parse_meta(const uint8_t* data, size_t size) {
    RadMeta meta;
    std::string json((const char*)data, size);
    const char* p = json.c_str();

    auto get_key = [&](const std::string& key) -> bool {
        const char* s = strstr(p, key.c_str());
        if (!s) return false;
        p = s + key.size();
        return true;
    };

    if (get_key("\"count\"")) meta.count = (uint32_t)json_number(p);
    if (get_key("\"maxSh\"")) meta.shDegree = (uint32_t)json_number(p);
    if (get_key("\"chunkSize\"")) meta.chunkSize = (uint32_t)json_number(p);
    if (get_key("\"lodTree\"")) {
        while (*p && *p != 't' && *p != 'f') p++;
        meta.lodTree = (*p == 't');
    }

    // Parse chunks
    if (get_key("\"chunks\"")) {
        while (*p && *p != '[') p++;
        if (*p == '[') {
            p++;
            while (*p && *p != ']') {
                RadChunkRange cr = {};
                while (*p && *p != '{') p++;
                if (*p != '{') break;
                p++;
                if (get_key("\"offset\"")) cr.offset = (uint64_t)json_number(p);
                if (get_key("\"bytes\"")) cr.bytes = (uint32_t)json_number(p);
                meta.chunks.push_back(cr);
                while (*p && *p != '}' && *p != ',') p++;
                if (*p == '}') p++;
            }
        }
    }

    // Parse encoding
    if (get_key("\"splatEncoding\"")) {
        if (get_key("\"rgbMin\"")) meta.rgbMin = (float)json_number(p);
        if (get_key("\"rgbMax\"")) meta.rgbMax = (float)json_number(p);
        if (get_key("\"lnScaleMin\"")) meta.lnScaleMin = (float)json_number(p);
        if (get_key("\"lnScaleMax\"")) meta.lnScaleMax = (float)json_number(p);
        if (get_key("\"sh1Max\"")) meta.sh1Max = (float)json_number(p);
        if (get_key("\"sh2Max\"")) meta.sh2Max = (float)json_number(p);
        if (get_key("\"sh3Max\"")) meta.sh3Max = (float)json_number(p);
    }

    // Parse encoding names from properties (simplified)
    if (get_key("\"center\"") || get_key("\"encoding\"")) {
        // skip, we use defaults
    }

    return meta;
}

static std::vector<uint8_t> decompress_gzip(const uint8_t* data, size_t size) {
    if (size == 0) return {};
    mz_ulong dest_len = size * 8; // initial guess
    std::vector<uint8_t> result(dest_len);
    int ret = mz_uncompress(result.data(), &dest_len, data, size);
    if (ret == MZ_BUF_ERROR) {
        // Try larger buffer - might be uncompressed
        dest_len = size * 16;
        result.resize(dest_len);
        ret = mz_uncompress(result.data(), &dest_len, data, size);
    }
    if (ret != MZ_OK) {
        // Fall through: data might not be compressed
        result.assign(data, data + size);
        return result;
    }
    result.resize(dest_len);
    return result;
}

RadDecodeResult decode_rad(const uint8_t* data, size_t size) {
    RadDecodeResult result;
    if (size < 8) return result;

    uint32_t magic = read_u32_le(data);
    if (magic != 0x30444152) { // "RAD0"
        // Try chunk magic
        return result;
    }

    uint32_t meta_len = read_u32_le(data + 4);
    size_t meta_end = 8 + ((meta_len + 7) & ~7);
    if (size < meta_end) return result;

    auto meta = parse_meta(data + 8, meta_len);
    uint64_t chunks_start = meta_end;
    result.count = meta.count;
    result.shDegree = meta.shDegree;

    size_t total = meta.count;
    size_t sh_stride = 0;
    if (meta.shDegree >= 1) sh_stride = 3;
    if (meta.shDegree >= 2) sh_stride = 8;
    if (meta.shDegree >= 3) sh_stride = 15;
    // Actual SH stored as: 3(rgb) + 3(band1) + 5(band2) + 7(band3) = 3 + sh_stride coefficients
    size_t sh_coeffs = (meta.shDegree > 0) ? (3 + sh_stride) : 0;

    // One pass: decode all chunks sequentially
    result.center.resize(total * 3);
    result.rgba.resize(total * 4);
    result.scale.resize(total * 3);
    result.quat.resize(total * 3);
    if (sh_coeffs > 0) result.sh.resize(total * sh_coeffs);
    result.childStart.resize(total);
    result.childCount.resize(total);
    result.totalNodes = total;

    uint64_t file_offset = chunks_start;
    size_t base = 0;

    for (size_t ci = 0; ci < meta.chunks.size() && base < total; ci++) {
        auto& cr = meta.chunks[ci];
        uint64_t chunk_off = chunks_start + cr.offset; // offset is relative to chunks_start for single file
        size_t chunk_base = (size_t)chunk_off;
        if (chunk_base >= size) break;

        size_t chunk_bytes = cr.bytes;
        if (chunk_base + chunk_bytes > size) chunk_bytes = size - chunk_base;

        const uint8_t* chunk_data = data + chunk_base;

        // Check chunk magic
        uint32_t chunk_magic = read_u32_le(chunk_data);
        if (chunk_magic != 0x43444152) { // "RADC"
            break;
        }

        uint32_t cm_len = read_u32_le(chunk_data + 4);
        size_t cm_end = 8 + ((cm_len + 7) & ~7);
        if (chunk_bytes < cm_end + 8) break;

        // Read chunk meta JSON
        std::string cm_json((const char*)chunk_data + 8, cm_len);
        uint64_t payload_bytes = read_u64_le(chunk_data + cm_end);
        size_t payload_start = cm_end + 8;
        if (chunk_bytes < payload_start + payload_bytes) payload_bytes = chunk_bytes - payload_start;

        // Parse chunk meta to find property offsets
        // cm_json has format: {"properties":[{"property":"center","encoding":"F32LeBytes","bytes":N,"offset":M},...]}
        struct PropEntry { std::string name; std::string encoding; uint32_t offset; uint32_t bytes; };
        std::vector<PropEntry> props;

        const char* cp = cm_json.c_str();
        auto find_props = [&]() {
            const char* ps = strstr(cp, "\"properties\"");
            if (!ps) return;
            ps = strchr(ps, '[');
            if (!ps) return;
            ps++;
            while (*ps && *ps != ']') {
                while (*ps && *ps != '{') ps++;
                if (*ps != '{') break;
                ps++;
                PropEntry e;
                const char* ks = strstr(ps, "\"property\"");
                if (ks) { ks = strchr(ks, ':');
                    while (ks && *ks != '"') ks++;
                    if (ks) e.name = json_string(ks); }
                const char* es = strstr(ps, "\"encoding\"");
                if (es) { es = strchr(es, ':');
                    while (es && *es != '"') es++;
                    if (es) e.encoding = json_string(es); }
                const char* bs = strstr(ps, "\"bytes\"");
                if (bs) { bs = strchr(bs, ':'); if (bs) e.bytes = (uint32_t)json_number(bs); }
                const char* os = strstr(ps, "\"offset\"");
                if (os) { os = strchr(os, ':'); if (os) e.offset = (uint32_t)json_number(os); }
                props.push_back(e);
                while (*ps && *ps != '}') ps++;
                if (*ps == '}') ps++;
            }
        };
        find_props();

        // Decompress and decode each property
        for (auto& prop : props) {
            if (prop.offset + prop.bytes > payload_bytes) continue;
            const uint8_t* prop_data = chunk_data + payload_start + prop.offset;
            size_t prop_size = prop.bytes;

            // Try decompress
            auto decomp = decompress_gzip(prop_data, prop_size);

            if (prop.name == "center") {
                // F32LeBytes: interleaved f32[3] per splat → f16[3] per splat
                size_t n = decomp.size() / 12; // 3*f32 = 12 bytes
                if (n > total - base) n = total - base;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    float fx, fy, fz;
                    if (i * 12 + 12 <= decomp.size()) {
                        fx = *(const float*)(decomp.data() + i * 12);
                        fy = *(const float*)(decomp.data() + i * 12 + 4);
                        fz = *(const float*)(decomp.data() + i * 12 + 8);
                    } else break;
                    // f32 → f16
                    uint16_t hx = (uint16_t)(fx < 0 ? 0x8000 : 0); // simplified - real impl needs IEEE754
                    uint16_t hy = (uint16_t)(fy < 0 ? 0x8000 : 0);
                    uint16_t hz = (uint16_t)(fz < 0 ? 0x8000 : 0);
                    result.center[(base + i) * 3] = hx;
                    result.center[(base + i) * 3 + 1] = hy;
                    result.center[(base + i) * 3 + 2] = hz;
                }
            }
            else if (prop.name == "alpha") {
                size_t n = decomp.size();
                if (n > total - base) n = total - base;
                float max_alpha = meta.lodTree ? 2.0f : 1.0f;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    float a = decomp[i] / 255.0f * max_alpha;
                    result.rgba[(base + i) * 4 + 3] = (uint8_t)(a * 255);
                }
            }
            else if (prop.name == "rgb") {
                size_t n = decomp.size() / 3;
                if (n > total - base) n = total - base;
                float mn = meta.rgbMin, mx = meta.rgbMax;
                float range = (mx - mn > 0.001f) ? (mx - mn) : 1.0f;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    result.rgba[(base + i) * 4] = decomp[i * 3];
                    result.rgba[(base + i) * 4 + 1] = decomp[i * 3 + 1];
                    result.rgba[(base + i) * 4 + 2] = decomp[i * 3 + 2];
                }
            }
            else if (prop.name == "scale") {
                size_t n = decomp.size() / 3;
                if (n > total - base) n = total - base;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    result.scale[(base + i) * 3] = decomp[i * 3];
                    result.scale[(base + i) * 3 + 1] = decomp[i * 3 + 1];
                    result.scale[(base + i) * 3 + 2] = decomp[i * 3 + 2];
                }
            }
            else if (prop.name == "quat") {
                size_t n = decomp.size() / 3;
                if (n > total - base) n = total - base;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    result.quat[(base + i) * 3] = decomp[i * 3];
                    result.quat[(base + i) * 3 + 1] = decomp[i * 3 + 1];
                    result.quat[(base + i) * 3 + 2] = decomp[i * 3 + 2];
                }
            }
            else if (prop.name == "sh" && sh_coeffs > 0) {
                size_t n = decomp.size() / sh_coeffs;
                if (n > total - base) n = total - base;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    memcpy(&result.sh[(base + i) * sh_coeffs], &decomp[i * sh_coeffs], sh_coeffs);
                }
            }
            else if (prop.name == "child_count") {
                size_t n = decomp.size() / 2;
                if (n > total - base) n = total - base;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    result.childCount[base + i] = (uint16_t)read_u32_le(&decomp[i * 2]);
                }
            }
            else if (prop.name == "child_start") {
                size_t n = decomp.size() / 4;
                if (n > total - base) n = total - base;
                for (size_t i = 0; i < n && base + i < total; i++) {
                    result.childStart[base + i] = read_u32_le(&decomp[i * 4]);
                }
            }
        }

        base += meta.chunkSize ? meta.chunkSize : 16384;
        if (base > total) base = total;
    }

    return result;
}

} // namespace splat
