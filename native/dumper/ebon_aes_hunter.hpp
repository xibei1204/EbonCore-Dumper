#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wmmintrin.h>
#include <immintrin.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include "../common/ebon_common.hpp"
#include <intrin.h>
#include <atomic>
#include <thread>
#include <limits>
#include <memory>

namespace ebon_hunter {

// --- Fast RFC 3174 SHA-1 ---
struct SHA1_CTX {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
};

#define ROL32(val, bits) (((val) << (bits)) | ((val) >> (32 - (bits))))

static inline void SHA1_Transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)buffer[i * 4 + 0] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8)  |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (int i = 0; i < 20; ++i) {
        uint32_t t = ROL32(a, 5) + ((b & c) | ((~b) & d)) + e + 0x5A827999 + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    for (int i = 20; i < 40; ++i) {
        uint32_t t = ROL32(a, 5) + (b ^ c ^ d) + e + 0x6ED9EBA1 + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    for (int i = 40; i < 60; ++i) {
        uint32_t t = ROL32(a, 5) + ((b & c) | (b & d) | (c & d)) + e + 0x8F1BBCDC + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    for (int i = 60; i < 80; ++i) {
        uint32_t t = ROL32(a, 5) + (b ^ c ^ d) + e + 0xCA62C1D6 + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static inline void SHA1_Init(SHA1_CTX* ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89; ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476; ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = ctx->count[1] = 0;
}

static inline void SHA1_Update(SHA1_CTX* ctx, const uint8_t* data, size_t len) {
    uint32_t i = 0, j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += (uint32_t)len << 3) < ((uint32_t)len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);
    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        SHA1_Transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) SHA1_Transform(ctx->state, &data[i]);
        j = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static inline void SHA1_Final(SHA1_CTX* ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i) finalcount[i] = (uint8_t)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    uint8_t c = 0x80; SHA1_Update(ctx, &c, 1);
    while ((ctx->count[0] & 504) != 448) { c = 0x00; SHA1_Update(ctx, &c, 1); }
    SHA1_Update(ctx, finalcount, 8);
    for (int i = 0; i < 20; ++i) digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
}

static inline void calc_sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    SHA1_CTX ctx;
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, data, len);
    SHA1_Final(&ctx, digest);
}

// --- AES-256 AES-NI ---
static inline __m128i aes_keygen_assist_1(__m128i temp1, __m128i temp2) {
    __m128i temp4;
    temp2 = _mm_shuffle_epi32(temp2, 0xff);
    temp4 = _mm_slli_si128(temp1, 0x4);
    temp1 = _mm_xor_si128(temp1, temp4);
    temp4 = _mm_slli_si128(temp4, 0x4);
    temp1 = _mm_xor_si128(temp1, temp4);
    temp4 = _mm_slli_si128(temp4, 0x4);
    temp1 = _mm_xor_si128(temp1, temp4);
    return _mm_xor_si128(temp1, temp2);
}

static inline __m128i aes_keygen_assist_2(__m128i temp1, __m128i temp3) {
    __m128i temp2, temp4;
    temp4 = _mm_slli_si128(temp1, 0x4);
    temp2 = _mm_xor_si128(temp1, temp4);
    temp4 = _mm_slli_si128(temp4, 0x4);
    temp2 = _mm_xor_si128(temp2, temp4);
    temp4 = _mm_slli_si128(temp4, 0x4);
    temp2 = _mm_xor_si128(temp2, temp4);
    temp3 = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(temp3, 0x0), 0xaa);
    return _mm_xor_si128(temp2, temp3);
}

struct Aes256DecKey {
    __m128i rk[15];
};

static inline void aes256_set_decrypt_key(const uint8_t* key, Aes256DecKey* dkey) {
    __m128i ek[15];
    ek[0] = _mm_loadu_si128((const __m128i*)key);
    ek[1] = _mm_loadu_si128((const __m128i*)(key + 16));

    ek[2] = aes_keygen_assist_1(ek[0], _mm_aeskeygenassist_si128(ek[1], 0x01));
    ek[3] = aes_keygen_assist_2(ek[1], ek[2]);
    ek[4] = aes_keygen_assist_1(ek[2], _mm_aeskeygenassist_si128(ek[3], 0x02));
    ek[5] = aes_keygen_assist_2(ek[3], ek[4]);
    ek[6] = aes_keygen_assist_1(ek[4], _mm_aeskeygenassist_si128(ek[5], 0x04));
    ek[7] = aes_keygen_assist_2(ek[5], ek[6]);
    ek[8] = aes_keygen_assist_1(ek[6], _mm_aeskeygenassist_si128(ek[7], 0x08));
    ek[9] = aes_keygen_assist_2(ek[7], ek[8]);
    ek[10] = aes_keygen_assist_1(ek[8], _mm_aeskeygenassist_si128(ek[9], 0x10));
    ek[11] = aes_keygen_assist_2(ek[9], ek[10]);
    ek[12] = aes_keygen_assist_1(ek[10], _mm_aeskeygenassist_si128(ek[11], 0x20));
    ek[13] = aes_keygen_assist_2(ek[11], ek[12]);
    ek[14] = aes_keygen_assist_1(ek[12], _mm_aeskeygenassist_si128(ek[13], 0x40));

    dkey->rk[0] = ek[14];
    for (int i = 1; i < 14; ++i) {
        dkey->rk[i] = _mm_aesimc_si128(ek[14 - i]);
    }
    dkey->rk[14] = ek[0];
}

static inline __m128i aes256_decrypt_block(__m128i cipher, const Aes256DecKey* key) {
    __m128i m = _mm_xor_si128(cipher, key->rk[0]);
    m = _mm_aesdec_si128(m, key->rk[1]);
    m = _mm_aesdec_si128(m, key->rk[2]);
    m = _mm_aesdec_si128(m, key->rk[3]);
    m = _mm_aesdec_si128(m, key->rk[4]);
    m = _mm_aesdec_si128(m, key->rk[5]);
    m = _mm_aesdec_si128(m, key->rk[6]);
    m = _mm_aesdec_si128(m, key->rk[7]);
    m = _mm_aesdec_si128(m, key->rk[8]);
    m = _mm_aesdec_si128(m, key->rk[9]);
    m = _mm_aesdec_si128(m, key->rk[10]);
    m = _mm_aesdec_si128(m, key->rk[11]);
    m = _mm_aesdec_si128(m, key->rk[12]);
    m = _mm_aesdec_si128(m, key->rk[13]);
    m = _mm_aesdeclast_si128(m, key->rk[14]);
    return m;
}

// SEH is kept in a function without C++ unwinding objects.
static inline bool safe_memcpy(void* dst,const void* src,size_t size) {
    __try { std::memcpy(dst,src,size);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Function to dump unpacked HTGame.exe directly to disk
inline bool dump_unpacked_binary(const std::filesystem::path& out_path) {
    try {
        HMODULE hMod = GetModuleHandleW(L"HTGame.exe");
        if (!hMod) hMod = GetModuleHandleW(nullptr);
        if (!hMod) return false;

        const wchar_t* disk_path = L"C:\\Program Files\\Neverness To Everness\\Client\\WindowsNoEditor\\HT\\Binaries\\Win64\\HTGame.exe";
        std::ifstream src(disk_path, std::ios::binary);
        if (!src) return false;

        std::vector<uint8_t> pe_data((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
        src.close();

        if (pe_data.size() < 0x1000) return false;

        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)pe_data.data();
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(pe_data.data() + dos->e_lfanew);
        PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            DWORD raw_off = sec[i].PointerToRawData;
            DWORD raw_sz = sec[i].SizeOfRawData;
            DWORD virt_addr = sec[i].VirtualAddress;

            if (raw_off + raw_sz <= pe_data.size()) {
                const uint8_t* mem_sec = (const uint8_t*)hMod + virt_addr;
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(mem_sec, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                        safe_memcpy(pe_data.data() + raw_off, mem_sec, raw_sz);
                    }
                }
            }
        }

        std::filesystem::create_directories(out_path.parent_path());
        std::ofstream dst(out_path, std::ios::binary);
        if (!dst) return false;
        dst.write((const char*)pe_data.data(), pe_data.size());
        dst.close();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace ebon_hunter

#include "ebon_aes_scan.hpp"
