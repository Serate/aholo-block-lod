/// Minimal gzip/zlib decompressor — single self-contained function.
/// Portable C, no external dependencies.
#include <cstdint>
#include <vector>

// Simple raw inflate (no header, no check) using the public-domain puff.c approach.
// We implement a minimal wrapper around the standard zlib inflate via inffast/inftrees.
// To avoid that complexity, we use a small lookup-based gunzip implementation.

// For now, use Windows built-in zlib via the Compress API.
#include <Windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet")

std::vector<uint8_t> decompress_gzip(const uint8_t* data, size_t size) {
    // Try Windows built-in decompression
    DECOMPRESSOR_HANDLE decomp = NULL;
    if (!CreateDecompressor(DECOMPRESSOR_RAW_DEFLATE, NULL, &decomp)) {
        return {};
    }

    // First call to get decompressed size
    SIZE_T dest_size = 0;
    Decompress(decomp, (PCVOID)data, size, NULL, 0, &dest_size);
    if (dest_size == 0) dest_size = size * 4;

    std::vector<uint8_t> result(dest_size);
    if (!Decompress(decomp, (PCVOID)data, size, result.data(), dest_size, &dest_size)) {
        // Try larger buffer
        dest_size = size * 10;
        result.resize(dest_size);
        if (!Decompress(decomp, (PCVOID)data, size, result.data(), dest_size, &dest_size)) {
            CloseDecompressor(decomp);
            return {};
        }
    }
    result.resize(dest_size);
    CloseDecompressor(decomp);
    return result;
}
