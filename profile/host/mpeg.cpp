// sceMpeg for the web profile.  The ring buffer and stream bookkeeping follow
// the real library, including the game's read callback, but access units are
// not decoded yet: every packet the game feeds is consumed and reported as "no
// data", so a movie runs to its end without producing frames or sound.

#include "kernel.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorMpegNoData = 0x80618001u;
constexpr std::uint32_t kErrorMpegInvalidValue = 0x806101FEu;
constexpr std::uint32_t kMpegMemSize = 0x10000u;
constexpr std::uint32_t kPacketSize = 2048u;
constexpr std::uint32_t kAtracEsSize = 2112u;
constexpr std::uint32_t kAtracOutputSize = 8192u;
constexpr std::uint32_t kAvcEsSize = 2048u;

// SceMpegRingbuffer field offsets.
enum : std::uint32_t {
    kRbPackets = 0, kRbPacketsRead = 4, kRbPacketsWritten = 8, kRbPacketsAvail = 12, kRbPacketSize = 16,
    kRbData = 20, kRbCallback = 24, kRbCallbackArgs = 28, kRbDataUpperBound = 32, kRbSemaId = 36, kRbMpeg = 40,
};

std::uint32_t big_endian32(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return (static_cast<std::uint32_t>(memory.load8(address)) << 24u) |
           (static_cast<std::uint32_t>(memory.load8(address + 1u)) << 16u) |
           (static_cast<std::uint32_t>(memory.load8(address + 2u)) << 8u) |
           static_cast<std::uint32_t>(memory.load8(address + 3u));
}

} // namespace

void Kernel::install_mpeg() {
    auto &m = rt_.memory();
    auto simple = [this](std::uint32_t nid, const char *name, std::function<std::uint32_t(Ctx &)> body) {
        static const bool trace = std::getenv("PSPWEB_TRACE_HLE") != nullptr;
        rt_.register_hle("sceMpeg", nid, [body, name](psprecomp::Runtime &, Ctx &ctx) {
            const std::uint32_t a0 = ctx.gpr[4], a1 = ctx.gpr[5], a2 = ctx.gpr[6];
            const std::uint32_t result = body(ctx);
            if (trace) std::fprintf(stderr, "[mpeg] %s(0x%X, 0x%X, 0x%X) -> 0x%X\n", name, a0, a1, a2, result);
            finish(ctx, result);
        });
        rt_.nids().add("sceMpeg", nid, name);
    };
    // Registration keys must match install_fallback(), which uses hex NIDs.
    auto mark = [this](std::uint32_t nid) {
        char key[32];
        std::snprintf(key, sizeof key, "sceMpeg:0x%08X", nid);
        registered_.insert(key);
    };

    simple(0x682A619Bu, "sceMpegInit", [](Ctx &) { return 0u; });
    simple(0x874624D6u, "sceMpegFinish", [](Ctx &) { return 0u; });
    simple(0xC132E22Fu, "sceMpegQueryMemSize", [](Ctx &) { return kMpegMemSize; });
    simple(0xD7A29F46u, "sceMpegRingbufferQueryMemSize",
           [](Ctx &ctx) { return ctx.gpr[4] * (kPacketSize + 104u); });
    simple(0x37295ED8u, "sceMpegRingbufferConstruct", [&m](Ctx &ctx) {
        const std::uint32_t rb = ctx.gpr[4], packets = ctx.gpr[5], data = ctx.gpr[6];
        m.store32(rb + kRbPackets, packets);
        m.store32(rb + kRbPacketsRead, 0u);
        m.store32(rb + kRbPacketsWritten, 0u);
        m.store32(rb + kRbPacketsAvail, 0u);
        m.store32(rb + kRbPacketSize, kPacketSize);
        m.store32(rb + kRbData, data);
        m.store32(rb + kRbCallback, ctx.gpr[8]);
        m.store32(rb + kRbCallbackArgs, ctx.gpr[9]);
        m.store32(rb + kRbDataUpperBound, data + packets * kPacketSize);
        m.store32(rb + kRbSemaId, 0xFFFFFFFFu);
        m.store32(rb + kRbMpeg, 0u);
        return 0u;
    });
    simple(0x13407F13u, "sceMpegRingbufferDestruct", [](Ctx &) { return 0u; });
    simple(0xB5F6DC87u, "sceMpegRingbufferAvailableSize", [&m](Ctx &ctx) {
        const std::uint32_t rb = ctx.gpr[4];
        return m.load32(rb + kRbPackets) - m.load32(rb + kRbPacketsAvail);
    });
    simple(0xD8C5F121u, "sceMpegCreate", [&m](Ctx &ctx) {
        const std::uint32_t mpeg = ctx.gpr[4], data = ctx.gpr[5], size = ctx.gpr[6], rb = ctx.gpr[7];
        if (size < kMpegMemSize) return 0x80610022u; // SCE_MPEG_ERROR_NO_MEMORY
        const std::uint32_t handle = data + 0x30u;
        m.store32(mpeg, handle);
        const char magic[12] = {'L', 'I', 'B', 'M', 'P', 'E', 'G', '\0', '0', '0', '1', '\0'};
        for (std::uint32_t i = 0; i < 12u; ++i) m.store8(handle + i, static_cast<std::uint8_t>(magic[i]));
        m.store32(handle + 12u, 0xFFFFFFFFu);
        if (rb != 0u) {
            m.store32(handle + 16u, rb);
            m.store32(handle + 20u, m.load32(rb + kRbDataUpperBound));
            m.store32(rb + kRbMpeg, mpeg);
        }
        return 0u;
    });
    simple(0x606A4649u, "sceMpegDelete", [](Ctx &) { return 0u; });
    simple(0x42560F23u, "sceMpegRegistStream",
           [](Ctx &ctx) { return 0x34340000u | ((ctx.gpr[5] & 0xFFu) << 8u) | (ctx.gpr[6] & 0xFFu); });
    simple(0x591A4AA2u, "sceMpegUnRegistStream", [](Ctx &) { return 0u; });
    simple(0x707B7629u, "sceMpegFlushAllStream", [](Ctx &) { return 0u; });
    simple(0xA780CF7Eu, "sceMpegMallocAvcEsBuf", [](Ctx &) { return 1u; });
    simple(0xCEB870B1u, "sceMpegFreeAvcEsBuf", [](Ctx &) { return 0u; });
    simple(0x167AFD9Eu, "sceMpegInitAu", [&m](Ctx &ctx) {
        const std::uint32_t buffer = ctx.gpr[5], au = ctx.gpr[6];
        const bool avc = buffer >= 1u && buffer <= 4u;
        for (std::uint32_t i = 0; i < 16u; i += 4u) m.store32(au + i, 0xFFFFFFFFu); // pts, dts
        m.store32(au + 16u, buffer);
        m.store32(au + 20u, avc ? kAvcEsSize : kAtracEsSize);
        return 0u;
    });
    simple(0xF8DCB679u, "sceMpegQueryAtracEsSize", [&m](Ctx &ctx) {
        if (ctx.gpr[5] != 0u) m.store32(ctx.gpr[5], kAtracEsSize);
        if (ctx.gpr[6] != 0u) m.store32(ctx.gpr[6], kAtracOutputSize);
        return 0u;
    });
    simple(0x21FF80E4u, "sceMpegQueryStreamOffset", [&m](Ctx &ctx) {
        const std::uint32_t buffer = ctx.gpr[5], out = ctx.gpr[6];
        if (big_endian32(m, buffer) != 0x50534D46u) { // "PSMF"
            if (out != 0u) m.store32(out, 0u);
            return kErrorMpegInvalidValue;
        }
        if (out != 0u) m.store32(out, big_endian32(m, buffer + 8u));
        return 0u;
    });
    simple(0x611E9E11u, "sceMpegQueryStreamSize", [&m](Ctx &ctx) {
        const std::uint32_t buffer = ctx.gpr[4], out = ctx.gpr[5];
        if (out != 0u) m.store32(out, big_endian32(m, buffer + 12u));
        return 0u;
    });
    // Access units: consume whatever was fed and report that nothing decodes.
    auto drain = [&m](std::uint32_t mpeg) {
        if (mpeg == 0u) return;
        const std::uint32_t handle = m.load32(mpeg);
        const std::uint32_t rb = handle != 0u ? m.load32(handle + 16u) : 0u;
        if (rb == 0u) return;
        const std::uint32_t avail = m.load32(rb + kRbPacketsAvail);
        m.store32(rb + kRbPacketsRead, m.load32(rb + kRbPacketsRead) + avail);
        m.store32(rb + kRbPacketsAvail, 0u);
    };
    simple(0xFE246728u, "sceMpegGetAvcAu", [drain](Ctx &ctx) {
        drain(ctx.gpr[4]);
        return kErrorMpegNoData;
    });
    simple(0xE1CE83A7u, "sceMpegGetAtracAu", [drain](Ctx &ctx) {
        drain(ctx.gpr[4]);
        return kErrorMpegNoData;
    });
    simple(0x0E3C2E9Du, "sceMpegAvcDecode", [&m](Ctx &ctx) {
        if (ctx.gpr[8] != 0u) m.store32(ctx.gpr[8], 0u); // no picture produced
        return 0u;
    });
    simple(0x740FCCD1u, "sceMpegAvcDecodeStop", [&m](Ctx &ctx) {
        if (ctx.gpr[7] != 0u) m.store32(ctx.gpr[7], 0u);
        return 0u;
    });
    simple(0x4571CC64u, "sceMpegAvcDecodeFlush", [](Ctx &) { return 0u; });
    simple(0x800C44DFu, "sceMpegAtracDecode", [](Ctx &) { return 0u; });

    // sceMpegRingbufferPut calls the game's reader on a guest thread.
    rt_.register_hle("sceMpeg", 0xB240A59Eu, [this, &m](psprecomp::Runtime &, Ctx &ctx) {
        const std::uint32_t rb = ctx.gpr[4];
        const auto requested = static_cast<std::int32_t>(std::min(ctx.gpr[5], ctx.gpr[6]));
        const std::uint32_t callback = m.load32(rb + kRbCallback);
        if (requested <= 0 || callback == 0u) {
            finish(ctx, 0u);
            return;
        }
        const std::uint32_t packets = std::max(m.load32(rb + kRbPackets), 1u);
        const std::uint32_t write_offset = m.load32(rb + kRbPacketsWritten) % packets;
        const std::uint32_t count = std::min(static_cast<std::uint32_t>(requested), packets - write_offset);
        call_guest(ctx, callback, {m.load32(rb + kRbData) + write_offset * kPacketSize, count, m.load32(rb + kRbCallbackArgs)},
                   [&m, rb](std::uint32_t result) {
                       const auto added = static_cast<std::int32_t>(result);
                       if (added > 0) {
                           m.store32(rb + kRbPacketsWritten, m.load32(rb + kRbPacketsWritten) + static_cast<std::uint32_t>(added));
                           m.store32(rb + kRbPacketsAvail, m.load32(rb + kRbPacketsAvail) + static_cast<std::uint32_t>(added));
                       }
                       return result;
                   });
    });
    rt_.nids().add("sceMpeg", 0xB240A59Eu, "sceMpegRingbufferPut");
    for (const std::uint32_t nid : {0x682A619Bu, 0x874624D6u, 0xC132E22Fu, 0xD7A29F46u, 0x37295ED8u, 0x13407F13u,
                                    0xB5F6DC87u, 0xD8C5F121u, 0x606A4649u, 0x42560F23u, 0x591A4AA2u, 0x707B7629u,
                                    0xA780CF7Eu, 0xCEB870B1u, 0x167AFD9Eu, 0xF8DCB679u, 0x21FF80E4u, 0x611E9E11u,
                                    0xFE246728u, 0xE1CE83A7u, 0x0E3C2E9Du, 0x740FCCD1u, 0x4571CC64u, 0x800C44DFu,
                                    0xB240A59Eu})
        mark(nid);
}

} // namespace pspweb
