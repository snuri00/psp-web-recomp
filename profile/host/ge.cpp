#include "ge.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "ge_gl.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <sstream>

namespace pspweb {
namespace {

// GE command opcodes used below.
enum : std::uint32_t {
    kNop = 0x00, kVaddr = 0x01, kIaddr = 0x02, kPrim = 0x04, kJump = 0x08, kCall = 0x0A, kRet = 0x0B,
    kEnd = 0x0C, kFinish = 0x0F, kBase = 0x10, kVertexType = 0x12, kOffsetAddr = 0x13, kOrigin = 0x14,
    kLightingEnable = 0x17, kLightEnable0 = 0x18, kCullEnable = 0x1D, kTextureEnable = 0x1E,
    kBlendEnable = 0x21, kAlphaTestEnable = 0x22, kZTestEnable = 0x23,
    kBoneMatrixNumber = 0x2A, kBoneMatrixData = 0x2B, kMorphWeight0 = 0x2C,
    kWorldMatrixNumber = 0x3A, kWorldMatrixData = 0x3B, kViewMatrixNumber = 0x3C, kViewMatrixData = 0x3D,
    kProjMatrixNumber = 0x3E, kProjMatrixData = 0x3F, kTgenMatrixNumber = 0x40, kTgenMatrixData = 0x41,
    kViewportXScale = 0x42, kViewportYScale = 0x43, kViewportZScale = 0x44,
    kViewportXCenter = 0x45, kViewportYCenter = 0x46, kViewportZCenter = 0x47,
    kTexScaleU = 0x48, kTexScaleV = 0x49, kTexOffsetU = 0x4A, kTexOffsetV = 0x4B,
    kOffsetX = 0x4C, kOffsetY = 0x4D, kShadeMode = 0x50, kMaterialUpdate = 0x53,
    kMaterialEmissive = 0x54, kMaterialAmbient = 0x55, kMaterialDiffuse = 0x56, kMaterialAlpha = 0x58,
    kAmbientColor = 0x5C, kAmbientAlpha = 0x5D, kLightType0 = 0x5F, kLightPos0 = 0x63, kLightDir0 = 0x6F,
    kLightAtt0 = 0x7B, kLightColor0 = 0x8F, kCull = 0x9B,
    kFrameBufPtr = 0x9C, kFrameBufWidth = 0x9D, kZBufPtr = 0x9E, kZBufWidth = 0x9F,
    kTexAddr0 = 0xA0, kTexBufWidth0 = 0xA8, kClutAddr = 0xB0, kClutAddrUpper = 0xB1,
    kTransferSrc = 0xB2, kTransferSrcW = 0xB3, kTransferDst = 0xB4, kTransferDstW = 0xB5,
    kTexSize0 = 0xB8, kTexMapMode = 0xC0, kTexMode = 0xC2, kTexFormat = 0xC3, kLoadClut = 0xC4,
    kClutFormat = 0xC5, kTexWrap = 0xC7, kTexFunc = 0xC9, kTexEnvColor = 0xCA,
    kFrameBufPixFormat = 0xD2, kClearMode = 0xD3, kScissor1 = 0xD4, kScissor2 = 0xD5,
    kAlphaTest = 0xDB, kZTest = 0xDE, kBlendMode = 0xDF, kBlendFixA = 0xE0, kBlendFixB = 0xE1,
    kZWriteDisable = 0xE7, kMaskRgb = 0xE8, kMaskAlpha = 0xE9, kTransferStart = 0xEA,
    kTransferSrcPos = 0xEB, kTransferDstPos = 0xEC, kTransferSize = 0xEE,
};

constexpr std::uint32_t kVramBase = 0x04000000u;

inline std::uint8_t expand5(std::uint32_t v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }
inline std::uint8_t expand6(std::uint32_t v) { return static_cast<std::uint8_t>((v << 2) | (v >> 4)); }
inline std::uint8_t expand4(std::uint32_t v) { return static_cast<std::uint8_t>(v * 17u); }

inline std::uint32_t pack(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Converts a 16-bit PSP colour (format 0 5650, 1 5551, 2 4444) to RGBA8.
inline std::uint32_t decode16(std::uint32_t v, std::uint32_t format) {
    switch (format) {
    case 0: return pack(expand5(v & 31u), expand6((v >> 5) & 63u), expand5((v >> 11) & 31u), 255u);
    case 1: return pack(expand5(v & 31u), expand5((v >> 5) & 31u), expand5((v >> 10) & 31u), (v >> 15) ? 255u : 0u);
    default: return pack(expand4(v & 15u), expand4((v >> 4) & 15u), expand4((v >> 8) & 15u), expand4((v >> 12) & 15u));
    }
}

inline float clamp255(float v) { return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); }

std::uint64_t hash_bytes(const std::uint8_t *data, std::size_t length) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ length;
    std::size_t i = 0;
    for (; i + 8u <= length; i += 8u) {
        std::uint64_t word;
        std::memcpy(&word, data + i, 8u);
        h = (h ^ word) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32u;
    }
    for (; i < length; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

constexpr std::uint64_t kTextureCacheTexelLimit = 16ull * 1024ull * 1024ull;

// PSPWEB_GE_PROFILE diagnostics.
std::map<std::uint64_t, std::uint64_t> g_target_pixels;
const bool g_profile_targets = std::getenv("PSPWEB_GE_PROFILE") != nullptr;
double g_section_ms[6] = {}; // decode, emit, texture, transfer, gl state, transfer GL sync
const char *const g_section_names[6] = {"decode", "emit", "texture", "transfer", "state", "xfer-gl"};
double g_transfer_bytes = 0.0;
// Milliseconds from a monotonic clock; in the browser performance.now() directly,
// which is far cheaper than std::chrono's path there.
double now_ms() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// Reads the clock only while profiling: in the browser every read is a call into JS.
struct SectionTimer {
    int index;
    double start = g_profile_targets ? now_ms() : 0.0;
    ~SectionTimer() {
        if (g_profile_targets) g_section_ms[index] += now_ms() - start;
    }
};
// Debugging aids: PSPWEB_DUMP_FRAME=n logs every primitive of frame n with its
// state and writes the textures it decodes to PSPWEB_DUMP_DIR (default
// build/dump); PSPWEB_SKIP_PRIMS=a-b[,c-d...] drops those primitives of every frame.
std::uint32_t env_number(const char *name, std::uint32_t fallback) {
    const char *v = std::getenv(name);
    return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : fallback;
}
const std::uint32_t g_dump_frame = env_number("PSPWEB_DUMP_FRAME", 0u);
const std::string g_dump_dir = std::getenv("PSPWEB_DUMP_DIR") != nullptr ? std::getenv("PSPWEB_DUMP_DIR") : "build/dump";
const std::vector<std::pair<std::uint32_t, std::uint32_t>> g_skip_prims = [] {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    const char *v = std::getenv("PSPWEB_SKIP_PRIMS");
    while (v != nullptr && *v != '\0') {
        char *end = nullptr;
        const auto lo = static_cast<std::uint32_t>(std::strtoul(v, &end, 10));
        const auto hi = *end == '-' ? static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 10)) : lo;
        ranges.emplace_back(lo, hi);
        v = *end == ',' ? end + 1 : nullptr;
    }
    return ranges;
}();
bool skipped_prim(std::uint32_t index) {
    for (const auto &[lo, hi] : g_skip_prims)
        if (index >= lo && index <= hi) return true;
    return false;
}

#ifdef __EMSCRIPTEN__
// ?profile in the page sets PSPWEB_FRAME_PROFILE: list timing only, cheap enough
// not to distort the numbers it shows.
const bool g_list_timing = g_profile_targets || std::getenv("PSPWEB_FRAME_PROFILE") != nullptr;
#else
const bool g_list_timing = true;
#endif

void transform43(const std::array<float, 12> &m, const float in[3], float out[3]) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6] + m[9];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7] + m[10];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8] + m[11];
}

void rotate43(const std::array<float, 12> &m, const float in[3], float out[3]) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8];
}

void color24(std::uint32_t data, float out[3]) {
    out[0] = static_cast<float>(data & 0xFFu);
    out[1] = static_cast<float>((data >> 8) & 0xFFu);
    out[2] = static_cast<float>((data >> 16) & 0xFFu);
}

bool compare(std::uint32_t func, std::uint32_t value, std::uint32_t reference) {
    switch (func & 7u) {
    case 0: return false;
    case 1: return true;
    case 2: return value == reference;
    case 3: return value != reference;
    case 4: return value < reference;
    case 5: return value <= reference;
    case 6: return value > reference;
    default: return value >= reference;
    }
}

} // namespace

Ge::Ge(psprecomp::GuestMemory &memory) : memory_(memory) {
    world_ = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    view_ = world_;
    tgen_ = world_;
    for (auto &bone : bones_) bone = world_;
    proj_ = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    morph_weights_[0] = 1.0f;
    call_stack_.reserve(64);
}

float Ge::cmd_float(std::uint32_t data) { return std::bit_cast<float>(data << 8u); }

std::uint32_t Ge::relative_address(std::uint32_t data) const {
    return ((((regs_[kBase] & 0x000F0000u) << 8u) | data) + offset_) & 0x0FFFFFFFu;
}

bool Ge::run_list(std::uint32_t start, std::uint32_t stall) {
    const double started = g_list_timing ? now_ms() : 0.0;
    struct Timer {
        Stats &stats;
        double start;
        ~Timer() {
            if (g_list_timing) stats.nanoseconds += static_cast<std::uint64_t>((now_ms() - start) * 1e6);
        }
    } timer{stats_, started};
    ++stats_.lists;
    pc_ = start & 0x0FFFFFFFu;
    stall &= 0x0FFFFFFFu;
    call_stack_.clear();
    // Commands are read through a raw pointer to the current 4 KiB page.
    const std::uint8_t *page = nullptr;
    std::uint32_t page_start = 1u;
    for (std::uint32_t guard = 0; guard < 8'000'000u; ++guard) {
        if (stall != 0u && pc_ == stall) return false;
        if ((pc_ & ~0xFFFu) != page_start) {
            page_start = pc_ & ~0xFFFu;
            page = memory_.raw_pointer(page_start, 0x1000u);
        }
        std::uint32_t op;
        if (page != nullptr) std::memcpy(&op, page + (pc_ - page_start), 4u);
        else op = memory_.load32(pc_);
        pc_ += 4u;
        if ((op >> 24u) == kEnd) return true;
        execute(op);
    }
    return true;
}

void Ge::execute(std::uint32_t op) {
    const std::uint32_t cmd = op >> 24u;
    const std::uint32_t data = op & 0x00FFFFFFu;
    switch (cmd) {
    case kNop: return;
    case kVaddr: vertex_address_ = relative_address(data); break;
    case kIaddr: index_address_ = relative_address(data); break;
    case kPrim: draw_primitive((data >> 16u) & 7u, data & 0xFFFFu); break;
    case kJump: pc_ = relative_address(data & 0xFFFFFCu); break;
    case kCall:
        call_stack_.push_back(pc_);
        call_stack_.push_back(offset_);
        pc_ = relative_address(data & 0xFFFFFCu);
        break;
    case kRet:
        if (call_stack_.size() >= 2u) {
            offset_ = call_stack_.back();
            call_stack_.pop_back();
            pc_ = call_stack_.back();
            call_stack_.pop_back();
        }
        break;
    case kOffsetAddr: offset_ = data << 8u; break;
    case kOrigin: offset_ = pc_ - 4u; break;
    case kWorldMatrixNumber: world_index_ = data & 0xFu; break;
    case kWorldMatrixData: if (world_index_ < 12u) world_[world_index_++] = cmd_float(data); break;
    case kViewMatrixNumber: view_index_ = data & 0xFu; break;
    case kViewMatrixData: if (view_index_ < 12u) view_[view_index_++] = cmd_float(data); break;
    case kProjMatrixNumber: proj_index_ = data & 0xFu; break;
    case kProjMatrixData: if (proj_index_ < 16u) proj_[proj_index_++] = cmd_float(data); break;
    case kTgenMatrixNumber: tgen_index_ = data & 0xFu; break;
    case kTgenMatrixData: if (tgen_index_ < 12u) tgen_[tgen_index_++] = cmd_float(data); break;
    case kBoneMatrixNumber: bone_index_ = data & 0x7Fu; break;
    case kBoneMatrixData:
        if (bone_index_ < 96u) bones_[bone_index_ / 12u][bone_index_ % 12u] = cmd_float(data);
        ++bone_index_;
        break;
    default:
        if (cmd >= kMorphWeight0 && cmd < kMorphWeight0 + 8u) morph_weights_[cmd - kMorphWeight0] = cmd_float(data);
        break;
    }
    regs_[cmd] = data;
    switch (cmd) {
    case kNop: case kVaddr: case kIaddr: case kPrim: case kJump: case kCall: case kRet: case kEnd: case kFinish:
    case kBase: case kOffsetAddr: case kOrigin: case kVertexType: case kBoneMatrixNumber: case kBoneMatrixData:
    case kWorldMatrixNumber: case kViewMatrixNumber: case kProjMatrixNumber: case kTgenMatrixNumber: case kTgenMatrixData:
        break;
    case kWorldMatrixData:
        matrix_dirty_ = true;
        break;
    case kViewMatrixData:
        matrix_dirty_ = true;
        pv_dirty_ = true;
        break;
    case kProjMatrixData:
        matrix_dirty_ = true;
        pv_dirty_ = true;
        state_dirty_ = true; // fog terms depend on the projection
        break;
    default:
        if (cmd < kMorphWeight0 || cmd >= kMorphWeight0 + 8u) state_dirty_ = true;
        if ((cmd >= kLightEnable0 && cmd <= kLightEnable0 + 3u) || (cmd >= kMaterialUpdate && cmd <= 0x8Eu)) light_dirty_ = true;
        if ((cmd >= kTexScaleU && cmd <= kTexOffsetV) || cmd == kTexSize0) tex_dirty_ = true;
        break;
    }
    if (cmd == kLoadClut) load_clut();
    else if (cmd == kTransferStart) block_transfer();
}

std::vector<std::uint32_t> Ge::save_context() const {
    // Commands that act rather than set state are left out; matrices are
    // re-uploaded through their own commands.
    const auto stateful = [](std::uint32_t cmd) {
        return cmd != kNop && !(cmd >= kPrim && cmd <= kFinish) && cmd != kOrigin && cmd != kLoadClut &&
               cmd != kTransferStart && cmd != 0xCBu && cmd != 0xCCu && cmd != kBoneMatrixNumber &&
               cmd != kBoneMatrixData && !(cmd >= kWorldMatrixNumber && cmd <= kTgenMatrixData);
    };
    const auto f24 = [](float f) { return std::bit_cast<std::uint32_t>(f) >> 8u; };
    std::vector<std::uint32_t> out;
    out.push_back((kBase << 24u) | regs_[kBase]);
    for (std::uint32_t cmd = 0; cmd < 256u; ++cmd)
        if (cmd != kBase && stateful(cmd)) out.push_back((cmd << 24u) | regs_[cmd]);
    const auto matrix = [&](std::uint32_t number, std::uint32_t data, const float *values, std::size_t count) {
        out.push_back(number << 24u);
        for (std::size_t i = 0; i < count; ++i) out.push_back((data << 24u) | f24(values[i]));
    };
    matrix(kWorldMatrixNumber, kWorldMatrixData, world_.data(), world_.size());
    matrix(kViewMatrixNumber, kViewMatrixData, view_.data(), view_.size());
    matrix(kProjMatrixNumber, kProjMatrixData, proj_.data(), proj_.size());
    matrix(kTgenMatrixNumber, kTgenMatrixData, tgen_.data(), tgen_.size());
    out.push_back(kBoneMatrixNumber << 24u);
    for (const auto &bone : bones_)
        for (float v : bone) out.push_back((kBoneMatrixData << 24u) | f24(v));
    return out;
}

void Ge::restore_context(const std::vector<std::uint32_t> &commands) {
    for (const std::uint32_t op : commands) {
        const std::uint32_t cmd = op >> 24u;
        if (cmd == kLoadClut || cmd == kTransferStart || (cmd >= kPrim && cmd <= kFinish)) continue;
        execute(op);
    }
    state_dirty_ = matrix_dirty_ = pv_dirty_ = light_dirty_ = tex_dirty_ = true;
}

// ---------------------------------------------------------------------------
// Vertices

void Ge::compute_layout() {
    const std::uint32_t vt = regs_[kVertexType];
    Layout l;
    l.tc = vt & 3u;
    l.col = (vt >> 2u) & 7u;
    l.nrm = (vt >> 5u) & 3u;
    l.pos = (vt >> 7u) & 3u;
    l.weight = (vt >> 9u) & 3u;
    l.idx = (vt >> 11u) & 3u;
    l.weights = ((vt >> 14u) & 7u) + 1u;
    l.morphs = ((vt >> 18u) & 7u) + 1u;
    l.through = (vt & 0x800000u) != 0u;
    static constexpr std::uint32_t comp[4] = {0, 1, 2, 4};
    static constexpr std::uint32_t col_size[8] = {0, 0, 0, 0, 2, 2, 2, 4};
    std::uint32_t off = 0, biggest = 1;
    auto place = [&](std::uint32_t align, std::uint32_t bytes) {
        if (align == 0u) return off;
        off = (off + align - 1u) & ~(align - 1u);
        const std::uint32_t at = off;
        off += bytes;
        biggest = std::max(biggest, align);
        return at;
    };
    l.weight_off = place(comp[l.weight], comp[l.weight] * l.weights);
    l.tc_off = place(comp[l.tc], comp[l.tc] * 2u);
    l.col_off = place(col_size[l.col], col_size[l.col]);
    l.nrm_off = place(comp[l.nrm], comp[l.nrm] * 3u);
    l.pos_off = place(comp[l.pos], comp[l.pos] * 3u);
    l.size = (off + biggest - 1u) & ~(biggest - 1u);
    layout_ = l;
}

void Ge::decode_vertex(std::uint32_t address, ClipVertex &out) const {
    const Layout &l = layout_;
    float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, uv[2] = {0, 0}, col[4] = {0, 0, 0, 0};
    float weights[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool has_color = l.col >= 4u;

    // Common case (no morphing, vertex data directly addressable): one switch
    // per attribute instead of one per component.
    const std::uint32_t passes = vertex_raw_ != nullptr && l.morphs == 1u ? 0u : l.morphs;
    if (passes == 0u) {
        const std::uint8_t *p = vertex_raw_ + (address - vertex_raw_base_);
        auto load = [](const std::uint8_t *q, std::uint32_t type, bool sign, float scale, float *dst, std::uint32_t n) {
            switch (type) {
            case 1:
                for (std::uint32_t i = 0; i < n; ++i)
                    dst[i] = (sign ? static_cast<float>(static_cast<std::int8_t>(q[i])) : static_cast<float>(q[i])) * scale;
                break;
            case 2:
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::uint16_t v;
                    std::memcpy(&v, q + i * 2u, 2u);
                    dst[i] = (sign ? static_cast<float>(static_cast<std::int16_t>(v)) : static_cast<float>(v)) * scale;
                }
                break;
            case 3: std::memcpy(dst, q, n * 4u); break;
            default: break;
            }
        };
        if (l.weight != 0u) load(p + l.weight_off, l.weight, false, l.weight == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f, weights, l.weights);
        if (l.tc != 0u) load(p + l.tc_off, l.tc, false, l.through ? 1.0f : (l.tc == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f), uv, 2u);
        if (has_color) {
            std::uint32_t rgba;
            if (l.col == 7u) {
                std::memcpy(&rgba, p + l.col_off, 4u);
            } else {
                std::uint16_t packed;
                std::memcpy(&packed, p + l.col_off, 2u);
                rgba = decode16(packed, l.col - 4u);
            }
            col[0] = static_cast<float>(rgba & 0xFFu);
            col[1] = static_cast<float>((rgba >> 8u) & 0xFFu);
            col[2] = static_cast<float>((rgba >> 16u) & 0xFFu);
            col[3] = static_cast<float>(rgba >> 24u);
        }
        if (l.nrm != 0u) load(p + l.nrm_off, l.nrm, true, l.nrm == 1u ? 1.0f / 127.0f : 1.0f / 32767.0f, nrm, 3u);
        if (l.pos != 0u) {
            if (!l.through) {
                load(p + l.pos_off, l.pos, true, l.pos == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f, pos, 3u);
            } else {
                load(p + l.pos_off, l.pos, true, 1.0f, pos, 2u);
                load(p + l.pos_off + 2u * (l.pos == 3u ? 4u : l.pos), l.pos, l.pos != 2u, 1.0f, pos + 2, 1u);
            }
        }
    }

    for (std::uint32_t m = 0; m < passes; ++m) {
        const std::uint32_t base = address + m * l.size;
        const float mw = l.morphs > 1u ? morph_weights_[m] : 1.0f;
        auto read = [&](std::uint32_t at, std::uint32_t type, bool sign, float scale) -> float {
            switch (type) {
            case 1: {
                const std::uint8_t v8 = vertex_raw_ != nullptr ? vertex_raw_[at - vertex_raw_base_] : memory_.load8(at);
                return sign ? static_cast<float>(static_cast<std::int8_t>(v8)) * scale : static_cast<float>(v8) * scale;
            }
            case 2: {
                std::uint16_t v16;
                if (vertex_raw_ != nullptr) std::memcpy(&v16, vertex_raw_ + (at - vertex_raw_base_), 2u);
                else v16 = memory_.load16(at);
                return sign ? static_cast<float>(static_cast<std::int16_t>(v16)) * scale : static_cast<float>(v16) * scale;
            }
            case 3: {
                std::uint32_t v32;
                if (vertex_raw_ != nullptr) std::memcpy(&v32, vertex_raw_ + (at - vertex_raw_base_), 4u);
                else v32 = memory_.load32(at);
                return std::bit_cast<float>(v32);
            }
            default: return 0.0f;
            }
        };
        static constexpr std::uint32_t comp[4] = {0, 1, 2, 4};
        if (l.weight != 0u) {
            const float scale = l.weight == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
            for (std::uint32_t i = 0; i < l.weights; ++i)
                weights[i] += mw * read(base + l.weight_off + i * comp[l.weight], l.weight, false, scale);
        }
        if (l.tc != 0u) {
            const float scale = l.through ? 1.0f : (l.tc == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f);
            for (std::uint32_t i = 0; i < 2u; ++i)
                uv[i] += mw * read(base + l.tc_off + i * comp[l.tc], l.tc, false, scale);
        }
        if (has_color) {
            std::uint32_t rgba;
            const std::uint32_t at = base + l.col_off;
            if (l.col == 7u) {
                if (vertex_raw_ != nullptr) std::memcpy(&rgba, vertex_raw_ + (at - vertex_raw_base_), 4u);
                else rgba = memory_.load32(at);
            } else {
                std::uint16_t packed;
                if (vertex_raw_ != nullptr) std::memcpy(&packed, vertex_raw_ + (at - vertex_raw_base_), 2u);
                else packed = memory_.load16(at);
                rgba = decode16(packed, l.col - 4u);
            }
            col[0] += mw * static_cast<float>(rgba & 0xFFu);
            col[1] += mw * static_cast<float>((rgba >> 8u) & 0xFFu);
            col[2] += mw * static_cast<float>((rgba >> 16u) & 0xFFu);
            col[3] += mw * static_cast<float>(rgba >> 24u);
        }
        if (l.nrm != 0u) {
            const float scale = l.nrm == 1u ? 1.0f / 127.0f : 1.0f / 32767.0f;
            for (std::uint32_t i = 0; i < 3u; ++i) {
                const float value = read(base + l.nrm_off + i * comp[l.nrm], l.nrm, true, scale);
                nrm[i] = (m == 0u ? 0.0f : nrm[i]) + mw * value;
            }
        }
        if (l.pos != 0u) {
            for (std::uint32_t i = 0; i < 3u; ++i) {
                float value;
                if (l.through) {
                    const bool z_unsigned = i == 2u && l.pos == 2u;
                    value = read(base + l.pos_off + i * comp[l.pos], l.pos, !z_unsigned, 1.0f);
                } else {
                    const float scale = l.pos == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
                    value = read(base + l.pos_off + i * comp[l.pos], l.pos, true, scale);
                }
                pos[i] += mw * value;
            }
        }
    }

    if (!has_color) {
        float material[3];
        color24(regs_[kMaterialAmbient], material);
        col[0] = material[0];
        col[1] = material[1];
        col[2] = material[2];
        col[3] = static_cast<float>(regs_[kMaterialAlpha] & 0xFFu);
    }

    if (l.through) {
        out = ClipVertex{pos[0], pos[1], pos[2], 1.0f, uv[0], uv[1], col[0], col[1], col[2], col[3]};
        return;
    }

    float model[3] = {pos[0], pos[1], pos[2]};
    float normal[3] = {nrm[0], nrm[1], nrm[2]};
    if (l.weight != 0u) {
        float skinned[3] = {0, 0, 0}, skinned_n[3] = {0, 0, 0};
        for (std::uint32_t i = 0; i < l.weights; ++i) {
            if (weights[i] == 0.0f) continue;
            float p[3], n[3];
            transform43(bones_[i], pos, p);
            rotate43(bones_[i], nrm, n);
            for (int k = 0; k < 3; ++k) {
                skinned[k] += weights[i] * p[k];
                skinned_n[k] += weights[i] * n[k];
            }
        }
        std::memcpy(model, skinned, sizeof model);
        std::memcpy(normal, skinned_n, sizeof normal);
    }

    if (use_wvp_) {
        out.cx = model[0] * wvp_[0] + model[1] * wvp_[4] + model[2] * wvp_[8] + wvp_[12];
        out.cy = model[0] * wvp_[1] + model[1] * wvp_[5] + model[2] * wvp_[9] + wvp_[13];
        out.cz = model[0] * wvp_[2] + model[1] * wvp_[6] + model[2] * wvp_[10] + wvp_[14];
        out.cw = model[0] * wvp_[3] + model[1] * wvp_[7] + model[2] * wvp_[11] + wvp_[15];
        out.r = col[0];
        out.g = col[1];
        out.b = col[2];
        out.a = col[3];
        out.u = (uv[0] * tex_.su + tex_.ou) * tex_.tw;
        out.v = (uv[1] * tex_.sv + tex_.ov) * tex_.th;
        return;
    }

    // Lighting works on world-space positions and normals; the clip position
    // comes from those through the cached proj * view.
    const bool lighting = (regs_[kLightingEnable] & 1u) != 0u;
    const std::uint32_t mapping = regs_[kTexMapMode] & 3u;
    float world[3], world_n[3] = {0, 0, 1};
    transform43(world_, model, world);
    if (lighting || mapping >= 2u) rotate43(world_, normal, world_n);
    out.cx = world[0] * pv_[0] + world[1] * pv_[4] + world[2] * pv_[8] + pv_[12];
    out.cy = world[0] * pv_[1] + world[1] * pv_[5] + world[2] * pv_[9] + pv_[13];
    out.cz = world[0] * pv_[2] + world[1] * pv_[6] + world[2] * pv_[10] + pv_[14];
    out.cw = world[0] * pv_[3] + world[1] * pv_[7] + world[2] * pv_[11] + pv_[15];

    if (lighting) light_vertex(world, world_n, col);
    out.r = col[0];
    out.g = col[1];
    out.b = col[2];
    out.a = col[3];

    // Texture coordinates in texels.
    const float tex_w = tex_.tw, tex_h = tex_.th;
    const std::uint32_t map_mode = regs_[kTexMapMode] & 3u;
    float s = uv[0], t = uv[1];
    if (map_mode == 0u) {
        s = s * tex_.su + tex_.ou;
        t = t * tex_.sv + tex_.ov;
    } else if (map_mode == 1u) {
        const std::uint32_t source = (regs_[kTexMapMode] >> 8u) & 3u;
        float in[3] = {model[0], model[1], model[2]};
        if (source == 1u) { in[0] = uv[0]; in[1] = uv[1]; in[2] = 0.0f; }
        else if (source >= 2u) { in[0] = normal[0]; in[1] = normal[1]; in[2] = normal[2]; }
        float projected[3];
        transform43(tgen_, in, projected);
        const float q = projected[2] != 0.0f ? projected[2] : 1.0f;
        s = projected[0] / q;
        t = projected[1] / q;
    } else {
        s = (world_n[0] + 1.0f) * 0.5f;
        t = (world_n[1] + 1.0f) * 0.5f;
    }
    out.u = s * tex_w;
    out.v = t * tex_h;
}

void Ge::build_light_setup() {
    light_dirty_ = false;
    LightSetup &l = light_;
    l.update = regs_[kMaterialUpdate];
    color24(regs_[kMaterialAmbient], l.mat_ambient);
    color24(regs_[kMaterialDiffuse], l.mat_diffuse);
    color24(regs_[kMaterialEmissive], l.emissive);
    color24(regs_[kAmbientColor], l.global);
    l.alpha = static_cast<float>(regs_[kMaterialAlpha] & 0xFFu);
    l.global_alpha = static_cast<float>(regs_[kAmbientAlpha] & 0xFFu) / 255.0f;
    l.count = 0;
    for (std::uint32_t i = 0; i < 4u; ++i) {
        if ((regs_[kLightEnable0 + i] & 1u) == 0u) continue;
        LightSetup::Light &light = l.lights[l.count++];
        light.type = (regs_[kLightType0 + i] >> 8u) & 3u;
        for (std::uint32_t k = 0; k < 3u; ++k) {
            light.pos[k] = cmd_float(regs_[kLightPos0 + i * 3u + k]);
            light.att[k] = cmd_float(regs_[kLightAtt0 + i * 3u + k]);
        }
        if (light.type == 0u) { // directional: normalise once
            const float len = std::sqrt(light.pos[0] * light.pos[0] + light.pos[1] * light.pos[1] + light.pos[2] * light.pos[2]);
            if (len > 0.0f) for (float &c : light.pos) c /= len;
        }
        color24(regs_[kLightColor0 + i * 3u], light.ambient);
        color24(regs_[kLightColor0 + i * 3u + 1u], light.diffuse);
        for (int k = 0; k < 3; ++k) {
            light.ambient[k] /= 255.0f;
            light.diffuse[k] /= 255.0f;
        }
    }
}

void Ge::build_tex_setup() {
    tex_dirty_ = false;
    const bool has_scale = regs_[kTexScaleU] != 0u || regs_[kTexScaleV] != 0u;
    tex_.su = has_scale ? cmd_float(regs_[kTexScaleU]) : 1.0f;
    tex_.sv = has_scale ? cmd_float(regs_[kTexScaleV]) : 1.0f;
    tex_.ou = cmd_float(regs_[kTexOffsetU]);
    tex_.ov = cmd_float(regs_[kTexOffsetV]);
    tex_.tw = static_cast<float>(1u << std::min(regs_[kTexSize0] & 0xFu, 9u));
    tex_.th = static_cast<float>(1u << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u));
}

void Ge::light_vertex(const float pos[3], const float normal[3], float color[4]) const {
    const LightSetup &setup = light_;
    const float *mat_ambient = (setup.update & 1u) != 0u ? color : setup.mat_ambient;
    const float *mat_diffuse = (setup.update & 2u) != 0u ? color : setup.mat_diffuse;
    const float alpha = (setup.update & 1u) != 0u ? color[3] : setup.alpha;

    float n[3] = {normal[0], normal[1], normal[2]};
    const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if (len2 > 0.0f) {
        const float inv = 1.0f / std::sqrt(len2);
        for (float &c : n) c *= inv;
    }
    float out[3];
    for (int k = 0; k < 3; ++k) out[k] = setup.emissive[k] + setup.global[k] * mat_ambient[k] * (1.0f / 255.0f);
    for (int i = 0; i < setup.count; ++i) {
        const LightSetup::Light &light = setup.lights[i];
        float l[3];
        float attenuation = 1.0f;
        if (light.type == 0u) {
            l[0] = light.pos[0]; l[1] = light.pos[1]; l[2] = light.pos[2];
        } else {
            for (int k = 0; k < 3; ++k) l[k] = light.pos[k] - pos[k];
            const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            const float denom = light.att[0] + light.att[1] * d + light.att[2] * d * d;
            attenuation = denom > 0.0f ? std::min(1.0f / denom, 1.0f) : 1.0f;
            if (d > 0.0f) for (float &c : l) c /= d;
        }
        const float diffuse = std::max(0.0f, n[0] * l[0] + n[1] * l[1] + n[2] * l[2]);
        for (int k = 0; k < 3; ++k)
            out[k] += attenuation * (light.ambient[k] * mat_ambient[k] + light.diffuse[k] * mat_diffuse[k] * diffuse);
    }
    for (int k = 0; k < 3; ++k) color[k] = clamp255(out[k]);
    // Like the colour, the alpha is global ambient x material ambient (emissive adds no alpha).
    color[3] = clamp255(alpha * setup.global_alpha);
}

Ge::Vertex Ge::to_screen(const ClipVertex &cv) const {
    Vertex v;
    if (layout_.through) {
        v.x = cv.cx;
        v.y = cv.cy;
        v.z = cv.cz;
        v.w = 1.0f;
    } else {
        const float inv_w = 1.0f / cv.cw;
        v.x = cv.cx * inv_w * viewport_[0] + viewport_[3];
        v.y = cv.cy * inv_w * viewport_[1] + viewport_[4];
        v.z = cv.cz * inv_w * viewport_[2] + viewport_[5];
        v.w = cv.cw;
    }
    v.u = cv.u;
    v.v = cv.v;
    v.r = cv.r;
    v.g = cv.g;
    v.b = cv.b;
    v.a = cv.a;
    return v;
}

// ---------------------------------------------------------------------------
// Primitives

void Ge::draw_primitive(std::uint32_t type, std::uint32_t count) {
    ++stats_.prims;
    compute_layout();
    viewport_[0] = cmd_float(regs_[kViewportXScale]);
    viewport_[1] = cmd_float(regs_[kViewportYScale]);
    viewport_[2] = cmd_float(regs_[kViewportZScale]);
    viewport_[3] = cmd_float(regs_[kViewportXCenter]) - static_cast<float>(regs_[kOffsetX] & 0xFFFFu) / 16.0f;
    viewport_[4] = cmd_float(regs_[kViewportYCenter]) - static_cast<float>(regs_[kOffsetY] & 0xFFFFu) / 16.0f;
    viewport_[5] = cmd_float(regs_[kViewportZCenter]);
    if (tex_dirty_) build_tex_setup();
    if (light_dirty_ && (regs_[kLightingEnable] & 1u) != 0u) build_light_setup();
    const bool was_wvp = use_wvp_;
    use_wvp_ = !layout_.through && layout_.weight == 0u && (regs_[kLightingEnable] & 1u) == 0u &&
               (regs_[kTexMapMode] & 3u) == 0u;
    // Column-major 4x4: W and V are 4x3 with an implicit (0, 0, 0, 1) row.
    auto expand = [](const std::array<float, 12> &m) {
        return std::array<float, 16>{m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0, m[9], m[10], m[11], 1};
    };
    auto multiply = [](const std::array<float, 16> &a, const std::array<float, 16> &b) {
        std::array<float, 16> r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] +
                                 a[12 + row] * b[c * 4 + 3];
        return r;
    };
    if (!layout_.through && pv_dirty_) {
        pv_ = multiply(proj_, expand(view_));
        pv_dirty_ = false;
    }
    if (use_wvp_ && (matrix_dirty_ || !was_wvp)) {
        matrix_dirty_ = false;
        wvp_ = multiply(pv_, expand(world_));
    }
    if (gl_ == nullptr) {
        prepare_pixel_state();
        bind_texture();
    } else if (state_dirty_ || layout_.through != submitted_through_) {
        // Through mode decides fog, so switching it needs fresh state too.
        prepare_pixel_state();
        submit_gl_state();
        state_dirty_ = false;
        submitted_through_ = layout_.through;
    }
    // Software drawing writes VRAM, which may hold textures; GL drawing does not.
    struct GenerationBump {
        std::uint64_t &generation;
        bool active;
        ~GenerationBump() { if (active) ++generation; }
    } bump{vram_generation_, gl_ == nullptr};
    const Layout &l = layout_;
    if (scratch_vertices_.size() < count) scratch_vertices_.resize(count);
    std::vector<ClipVertex> &vertices = scratch_vertices_;
    const std::uint32_t index_size = l.idx == 1u ? 1u : (l.idx == 2u ? 2u : 0u);
    SectionTimer decode_timer{0};
    const std::uint32_t stride = l.size * l.morphs;
    if (scratch_indices_.size() < count) scratch_indices_.resize(count);
    std::vector<std::uint32_t> &indices = scratch_indices_;
    const std::uint8_t *index_raw = index_size != 0u ? memory_.raw_pointer(index_address_, count * index_size) : nullptr;
    std::uint32_t max_index = count == 0u ? 0u : count - 1u, min_index = 0u;
    if (index_size != 0u) {
        max_index = 0u;
        min_index = ~0u;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t index;
            if (index_size == 1u) index = index_raw != nullptr ? index_raw[i] : memory_.load8(index_address_ + i);
            else if (index_raw != nullptr) {
                std::uint16_t v16;
                std::memcpy(&v16, index_raw + i * 2u, 2u);
                index = v16;
            } else {
                index = memory_.load16(index_address_ + i * 2u);
            }
            indices[i] = index;
            max_index = std::max(max_index, index);
            min_index = std::min(min_index, index);
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) indices[i] = i;
    }
    // Indexed draws share vertices: decode each one in the used range once and
    // address it through `indices` (unless the range is much larger than the draw).
    const bool shared = index_size != 0u && count != 0u && max_index - min_index < count;
    const std::uint32_t first = shared ? min_index : 0u;
    const std::uint32_t decoded = shared ? max_index - min_index + 1u : count;
    if (scratch_vertices_.size() < decoded) scratch_vertices_.resize(decoded);
    const std::uint32_t decode_base = vertex_address_ + first * stride;
    vertex_raw_base_ = decode_base;
    vertex_raw_ = count != 0u ? memory_.raw_pointer(decode_base, (max_index + 1u - first) * stride) : nullptr;
    if (shared) {
        for (std::uint32_t i = 0; i < count; ++i) indices[i] -= min_index;
        for (std::uint32_t i = 0; i < decoded; ++i) decode_vertex(decode_base + i * stride, vertices[i]);
    } else {
        // Plain or sparsely indexed: one decode per position.
        for (std::uint32_t i = 0; i < count; ++i) {
            decode_vertex(decode_base + indices[i] * stride, vertices[i]);
            indices[i] = i;
        }
    }
    vertex_raw_ = nullptr;
    if (index_size != 0u) index_address_ += count * index_size;
    else vertex_address_ += count * l.size * l.morphs;
    g_section_ms[0] += 0.0; // decode timer ends with scope below
    const std::uint32_t prim_index = prim_in_frame_++;
    if (frame_ == g_dump_frame) log_primitive(prim_index, type, count);
    if (!g_skip_prims.empty() && skipped_prim(prim_index)) return;
    SectionTimer emit_timer{1};

    const std::uint64_t pixels_before = stats_.pixels;
    struct HeavyDrawReport {
        Ge &ge;
        std::uint64_t before;
        std::uint32_t type, count;
        const std::vector<ClipVertex> &vertices;
        ~HeavyDrawReport() {
            if (!g_profile_targets || ge.frame_ < 900u) return;
            const std::uint64_t drawn = ge.stats_.pixels - before;
            if (drawn < 100000u) return;
            std::cerr << "[ge-heavy] frame=" << ge.frame_ << " type=" << type << " count=" << count << " pixels=" << drawn
                      << (ge.layout_.through ? " through" : " 3d") << " vtype=0x" << std::hex << ge.regs_[kVertexType]
                      << std::dec;
            for (std::size_t i = 0; i < std::min<std::size_t>(vertices.size(), 4u); ++i) {
                const Vertex sv = ge.to_screen(vertices[i]);
                std::cerr << " v" << i << "=(" << sv.x << "," << sv.y << "," << sv.z << " w=" << vertices[i].cw << ")";
            }
            std::cerr << "\n";
        }
    } heavy{*this, pixels_before, type, count, vertices};

    if (gl_ != nullptr && type >= 3u && type <= 5u) {
        // Convert each vertex to drawing space once; triangles fully in front of
        // the near plane are appended directly, the rest go through clipping.
        if (scratch_screen_.size() < decoded) {
            scratch_screen_.resize(decoded);
            scratch_inside_.resize(decoded);
        }
        constexpr float kEpsilon = 1e-4f;
        for (std::uint32_t i = 0; i < decoded; ++i) {
            const ClipVertex &cv = vertices[i];
            const bool inside = layout_.through || (cv.cz + cv.cw >= 0.0f && cv.cw >= kEpsilon);
            scratch_inside_[i] = inside ? 1u : 0u;
            if (!inside) continue;
            const Vertex v = to_screen(cv);
            scratch_screen_[i] = GlVertex{v.x, v.y, v.z, v.w, v.u, v.v,
                                          static_cast<std::uint8_t>(clamp255(v.r)), static_cast<std::uint8_t>(clamp255(v.g)),
                                          static_cast<std::uint8_t>(clamp255(v.b)), static_cast<std::uint8_t>(clamp255(v.a))};
        }
        const bool flat = (regs_[kShadeMode] & 1u) == 0u;
        // Backface culling (3D only), decided on the screen-space winding.  A
        // zero-area triangle (common in strips) covers no pixels either way.
        // In drawing space (y down) a positive area means clockwise; CULL bit 0 clear
        // removes clockwise triangles (checked against frames: the other way empties the scene).
        const bool cull = !layout_.through && (regs_[kCullEnable] & 1u) != 0u;
        const bool cull_clockwise = (regs_[kCull] & 1u) == 0u;
        // Each decoded vertex enters the GL batch once, the first time a kept triangle uses it.
        if (scratch_batch_index_.size() < decoded) scratch_batch_index_.resize(decoded);
        std::fill(scratch_batch_index_.begin(), scratch_batch_index_.begin() + decoded, ~0u);
        auto batch_index = [&](std::uint32_t i) {
            std::uint32_t &slot = scratch_batch_index_[i];
            if (slot == ~0u) slot = gl_->vertex(scratch_screen_[i]);
            return slot;
        };
        const std::uint32_t *at = indices.data();
        auto triangle = [&](std::uint32_t pa, std::uint32_t pb, std::uint32_t pc) {
            const std::uint32_t a = at[pa], b = at[pb], c = at[pc];
            if (scratch_inside_[a] == 0u || scratch_inside_[b] == 0u || scratch_inside_[c] == 0u) {
                draw_triangle(vertices[a], vertices[b], vertices[c]);
                return;
            }
            {
                const GlVertex &p = scratch_screen_[a], &q = scratch_screen_[b], &r = scratch_screen_[c];
                const float area = (q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y);
                if (area == 0.0f || (cull && (area > 0.0f) == cull_clockwise)) {
                    ++stats_.culled;
                    return;
                }
            }
            ++stats_.triangles;
            if (!flat) {
                gl_->indexed_triangle(batch_index(a), batch_index(b), batch_index(c));
                return;
            }
            GlVertex va = scratch_screen_[a], vb = scratch_screen_[b];
            const GlVertex &vc = scratch_screen_[c];
            va.r = vb.r = vc.r;
            va.g = vb.g = vc.g;
            va.b = vb.b = vc.b;
            va.a = vb.a = vc.a;
            gl_->triangle(va, vb, vc);
        };
        if (type == 3u) {
            for (std::uint32_t i = 0; i + 2u < count; i += 3u) triangle(i, i + 1u, i + 2u);
        } else if (type == 4u) {
            for (std::uint32_t i = 2u; i < count; ++i) {
                if ((i & 1u) == 0u) triangle(i - 2u, i - 1u, i);
                else triangle(i - 1u, i - 2u, i);
            }
        } else {
            for (std::uint32_t i = 2u; i < count; ++i) triangle(0u, i - 1u, i);
        }
        return;
    }

    auto vert = [&](std::uint32_t i) -> const ClipVertex & { return vertices[indices[i]]; };
    switch (type) {
    case 3: // triangles
        for (std::uint32_t i = 0; i + 2u < count; i += 3u) draw_triangle(vert(i), vert(i + 1u), vert(i + 2u));
        break;
    case 4: // strip
        for (std::uint32_t i = 2u; i < count; ++i) {
            if ((i & 1u) == 0u) draw_triangle(vert(i - 2u), vert(i - 1u), vert(i));
            else draw_triangle(vert(i - 1u), vert(i - 2u), vert(i));
        }
        break;
    case 5: // fan
        for (std::uint32_t i = 2u; i < count; ++i) draw_triangle(vert(0), vert(i - 1u), vert(i));
        break;
    case 6: // sprites
        for (std::uint32_t i = 0; i + 1u < count; i += 2u) emit_sprite(to_screen(vert(i)), to_screen(vert(i + 1u)));
        break;
    case 0: // points
        for (std::uint32_t i = 0; i < count; ++i) {
            Vertex v = to_screen(vert(i)), w = v;
            w.x += 1.0f;
            w.y += 1.0f;
            emit_sprite(v, w);
        }
        break;
    default: // lines: not rasterized yet
        break;
    }
}

void Ge::draw_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c) {
    if (layout_.through) {
        emit_triangle(to_screen(a), to_screen(b), to_screen(c));
        return;
    }
    // Clip against the near plane (z >= -w) and w > epsilon.
    std::array<ClipVertex, 8> in{a, b, c}, out{};
    std::size_t in_count = 3, out_count = 0;
    constexpr float kEpsilon = 1e-4f;
    auto distance = [](const ClipVertex &v) { return std::min(v.cz + v.cw, v.cw - kEpsilon); };
    if (distance(a) >= 0.0f && distance(b) >= 0.0f && distance(c) >= 0.0f) {
        emit_triangle(to_screen(a), to_screen(b), to_screen(c));
        return;
    }
    for (std::size_t i = 0; i < in_count; ++i) {
        const ClipVertex &p = in[i], &q = in[(i + 1u) % in_count];
        const float dp = distance(p), dq = distance(q);
        if (dp >= 0.0f) out[out_count++] = p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            const float t = dp / (dp - dq);
            auto lerp = [t](float x, float y) { return x + (y - x) * t; };
            out[out_count++] = ClipVertex{lerp(p.cx, q.cx), lerp(p.cy, q.cy), lerp(p.cz, q.cz), lerp(p.cw, q.cw),
                                          lerp(p.u, q.u), lerp(p.v, q.v), lerp(p.r, q.r), lerp(p.g, q.g),
                                          lerp(p.b, q.b), lerp(p.a, q.a)};
        }
    }
    if (out_count < 3u) return;
    const Vertex first = to_screen(out[0]);
    for (std::size_t i = 1; i + 1u < out_count; ++i) emit_triangle(first, to_screen(out[i]), to_screen(out[i + 1u]));
}

void Ge::raster_triangle(const Vertex &a, const Vertex &b, const Vertex &c) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f || !std::isfinite(area)) return;
    ++stats_.triangles;

    const int sx1 = ps_.sx1, sy1 = ps_.sy1, sx2 = ps_.sx2, sy2 = ps_.sy2;
    int min_x = static_cast<int>(std::floor(std::min({a.x, b.x, c.x})));
    int max_x = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
    int min_y = static_cast<int>(std::floor(std::min({a.y, b.y, c.y})));
    int max_y = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
    min_x = std::max(min_x, sx1);
    min_y = std::max(min_y, sy1);
    max_x = std::min(max_x, sx2);
    max_y = std::min(max_y, sy2);
    if (min_x > max_x || min_y > max_y) return;

    const float inv_area = 1.0f / area;
    const bool flat = (regs_[kShadeMode] & 1u) == 0u;
    const bool perspective = !layout_.through;
    const float iwa = 1.0f / a.w, iwb = 1.0f / b.w, iwc = 1.0f / c.w;

    for (int y = min_y; y <= max_y; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv_area;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float z = w0 * a.z + w1 * b.z + w2 * c.z;
            float u, v;
            if (perspective) {
                const float q = w0 * iwa + w1 * iwb + w2 * iwc;
                u = (w0 * a.u * iwa + w1 * b.u * iwb + w2 * c.u * iwc) / q;
                v = (w0 * a.v * iwa + w1 * b.v * iwb + w2 * c.v * iwc) / q;
            } else {
                u = w0 * a.u + w1 * b.u + w2 * c.u;
                v = w0 * a.v + w1 * b.v + w2 * c.v;
            }
            if (flat) shade_pixel(x, y, z, u, v, c.r, c.g, c.b, c.a);
            else
                shade_pixel(x, y, z, u, v, w0 * a.r + w1 * b.r + w2 * c.r, w0 * a.g + w1 * b.g + w2 * c.g,
                            w0 * a.b + w1 * b.b + w2 * c.b, w0 * a.a + w1 * b.a + w2 * c.a);
        }
    }
}

void Ge::draw_sprite(const Vertex &a, const Vertex &b) {
    ++stats_.sprites;
    const int sx1 = ps_.sx1, sy1 = ps_.sy1, sx2 = ps_.sx2, sy2 = ps_.sy2;
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    const int ix0 = std::max(static_cast<int>(std::floor(x0)), sx1);
    const int iy0 = std::max(static_cast<int>(std::floor(y0)), sy1);
    const int ix1 = std::min(static_cast<int>(std::ceil(x1)) - 1, sx2);
    const int iy1 = std::min(static_cast<int>(std::ceil(y1)) - 1, sy2);
    const float dx = b.x - a.x, dy = b.y - a.y;
    for (int y = iy0; y <= iy1; ++y) {
        const float ty = dy != 0.0f ? (static_cast<float>(y) + 0.5f - a.y) / dy : 0.0f;
        const float v = a.v + (b.v - a.v) * ty;
        for (int x = ix0; x <= ix1; ++x) {
            const float tx = dx != 0.0f ? (static_cast<float>(x) + 0.5f - a.x) / dx : 0.0f;
            shade_pixel(x, y, b.z, a.u + (b.u - a.u) * tx, v, b.r, b.g, b.b, b.a);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-pixel pipeline


void Ge::take_sections(double out[6]) {
    for (int i = 0; i < 6; ++i) {
        out[i] = g_section_ms[i];
        g_section_ms[i] = 0.0;
    }
}

void Ge::report_targets() {
    if (!g_profile_targets) return;
    std::cerr << "[ge-sections]";
    for (int i = 0; i < 6; ++i) {
        std::cerr << " " << g_section_names[i] << "=" << g_section_ms[i] / 60.0 << "ms";
        g_section_ms[i] = 0.0;
    }
    std::cerr << " transfer=" << g_transfer_bytes / 60.0 / 1024.0 << "KiB (per frame)\n";
    g_transfer_bytes = 0.0;
    for (const auto &[key, count] : g_target_pixels)
        std::cerr << "[ge-target] fb=0x" << std::hex << (key >> 32u) << " stride=" << std::dec << ((key >> 16u) & 0xFFFFu)
                  << (key & 1u ? " through" : " 3d") << ((key & 2u) ? " tex" : "") << ((key & 4u) ? " blend" : "")
                  << " pixels=" << count << "\n";
    g_target_pixels.clear();
}

void Ge::shade_pixel(int x, int y, float z, float u, float v, float r, float g, float b, float a) {
    const PixelState &p = ps_;
    if (p.vram == nullptr || x < 0 || y < 0 || x >= 1024 || y >= 1024) return;
    ++stats_.pixels;
    if (g_profile_targets) {
        const std::uint64_t key = (static_cast<std::uint64_t>(regs_[kFrameBufPtr] & 0xFFFFFFu) << 32u) |
                                  (static_cast<std::uint64_t>(p.fb_stride) << 16u) | (layout_.through ? 1u : 0u) |
                                  (p.texture ? 2u : 0u) | (p.blend ? 4u : 0u);
        ++g_target_pixels[key];
    }
    const auto ux = static_cast<std::uint32_t>(x), uy = static_cast<std::uint32_t>(y);
    std::uint8_t *fb = p.vram + ((p.fb_offset + (uy * p.fb_stride + ux) * p.fb_bpp) & (0x1FFFFFu & ~(p.fb_bpp - 1u)));
    std::uint8_t *zb = p.vram + ((p.z_offset + (uy * p.z_stride + ux) * 2u) & 0x1FFFFEu);
    const std::uint32_t depth = static_cast<std::uint32_t>(std::clamp(z, 0.0f, 65535.0f));
    auto load_fb = [&]() -> std::uint32_t {
        if (p.fb_bpp == 4u) {
            std::uint32_t value;
            std::memcpy(&value, fb, 4u);
            return value;
        }
        return decode16(static_cast<std::uint32_t>(fb[0]) | (static_cast<std::uint32_t>(fb[1]) << 8u), p.fb_format);
    };
    auto store_fb = [&](std::uint32_t rgba, std::uint32_t old) {
        rgba = (rgba & ~p.write_mask) | (old & p.write_mask);
        if (p.fb_bpp == 4u) {
            std::memcpy(fb, &rgba, 4u);
            return;
        }
        const std::uint32_t cr = rgba & 0xFFu, cg = (rgba >> 8u) & 0xFFu, cb = (rgba >> 16u) & 0xFFu, ca = rgba >> 24u;
        std::uint32_t packed;
        if (p.fb_format == 0u) packed = (cr >> 3u) | ((cg >> 2u) << 5u) | ((cb >> 3u) << 11u);
        else if (p.fb_format == 1u) packed = (cr >> 3u) | ((cg >> 3u) << 5u) | ((cb >> 3u) << 10u) | ((ca >> 7u) << 15u);
        else packed = (cr >> 4u) | ((cg >> 4u) << 4u) | ((cb >> 4u) << 8u) | ((ca >> 4u) << 12u);
        fb[0] = static_cast<std::uint8_t>(packed);
        fb[1] = static_cast<std::uint8_t>(packed >> 8u);
    };

    if (p.clear) {
        const std::uint32_t old = load_fb();
        std::uint32_t value = old;
        const std::uint32_t src = pack(static_cast<std::uint32_t>(clamp255(r)), static_cast<std::uint32_t>(clamp255(g)),
                                       static_cast<std::uint32_t>(clamp255(b)), static_cast<std::uint32_t>(clamp255(a)));
        if ((p.clear_flags & 0x100u) != 0u) value = (value & 0xFF000000u) | (src & 0x00FFFFFFu);
        if ((p.clear_flags & 0x200u) != 0u) value = (value & 0x00FFFFFFu) | (src & 0xFF000000u);
        store_fb(value, old);
        if ((p.clear_flags & 0x400u) != 0u && p.z_stride != 0u) {
            zb[0] = static_cast<std::uint8_t>(depth);
            zb[1] = static_cast<std::uint8_t>(depth >> 8u);
        }
        return;
    }

    if (p.z_test) {
        const std::uint32_t stored = static_cast<std::uint32_t>(zb[0]) | (static_cast<std::uint32_t>(zb[1]) << 8u);
        if (!compare(p.z_func, depth, stored)) return;
    }

    float color[4] = {r, g, b, a};
    if (p.texture) {
        float tex[4];
        sample_texture(u, v, tex);
        switch (p.tex_func) {
        case 0: // modulate
            for (int k = 0; k < 3; ++k) color[k] = color[k] * tex[k] * (1.0f / 255.0f);
            if (p.tex_alpha) color[3] = color[3] * tex[3] * (1.0f / 255.0f);
            break;
        case 1: // decal
            if (p.tex_alpha) for (int k = 0; k < 3; ++k) color[k] += (tex[k] - color[k]) * tex[3] * (1.0f / 255.0f);
            else for (int k = 0; k < 3; ++k) color[k] = tex[k];
            break;
        case 2: // blend
            for (int k = 0; k < 3; ++k) color[k] = (color[k] * (255.0f - tex[k]) + p.env[k] * tex[k]) * (1.0f / 255.0f);
            if (p.tex_alpha) color[3] = color[3] * tex[3] * (1.0f / 255.0f);
            break;
        case 3: // replace
            for (int k = 0; k < 3; ++k) color[k] = tex[k];
            if (p.tex_alpha) color[3] = tex[3];
            break;
        default: // add
            for (int k = 0; k < 3; ++k) color[k] = color[k] + tex[k];
            if (p.tex_alpha) color[3] = color[3] * tex[3] * (1.0f / 255.0f);
            break;
        }
        if (p.tex_double) for (int k = 0; k < 3; ++k) color[k] *= 2.0f;
    }
    for (float &channel : color) channel = clamp255(channel);

    if (p.alpha_test && !compare(p.alpha_func, static_cast<std::uint32_t>(color[3]) & p.alpha_mask, p.alpha_ref)) return;

    const std::uint32_t old = load_fb();
    if (p.blend) {
        const float dst[4] = {static_cast<float>(old & 0xFFu), static_cast<float>((old >> 8u) & 0xFFu),
                              static_cast<float>((old >> 16u) & 0xFFu), static_cast<float>(old >> 24u)};
        const float sa = color[3] * (1.0f / 255.0f), da = dst[3] * (1.0f / 255.0f);
        auto factor = [&](std::uint32_t f, bool is_src, int k) -> float {
            switch (f) {
            case 0: return (is_src ? dst[k] : color[k]) * (1.0f / 255.0f);
            case 1: return 1.0f - (is_src ? dst[k] : color[k]) * (1.0f / 255.0f);
            case 2: return sa;
            case 3: return 1.0f - sa;
            case 4: return da;
            case 5: return 1.0f - da;
            case 6: return 2.0f * sa;
            case 7: return 1.0f - 2.0f * sa;
            case 8: return 2.0f * da;
            case 9: return 1.0f - 2.0f * da;
            default: return (is_src ? p.fix_a[k] : p.fix_b[k]) * (1.0f / 255.0f);
            }
        };
        for (int k = 0; k < 3; ++k) {
            const float s = color[k] * factor(p.src_factor, true, k), d = dst[k] * factor(p.dst_factor, false, k);
            float result;
            switch (p.equation) {
            case 0: result = s + d; break;
            case 1: result = s - d; break;
            case 2: result = d - s; break;
            case 3: result = std::min(color[k], dst[k]); break;
            case 4: result = std::max(color[k], dst[k]); break;
            default: result = std::fabs(color[k] - dst[k]); break;
            }
            color[k] = clamp255(result);
        }
    }

    store_fb(pack(static_cast<std::uint32_t>(color[0]), static_cast<std::uint32_t>(color[1]),
                  static_cast<std::uint32_t>(color[2]), static_cast<std::uint32_t>(color[3])),
             old);
    if (p.z_write) {
        zb[0] = static_cast<std::uint8_t>(depth);
        zb[1] = static_cast<std::uint8_t>(depth >> 8u);
    }
}

std::uint32_t Ge::read_pixel(std::uint32_t address) const {
    const std::uint32_t format = regs_[kFrameBufPixFormat] & 3u;
    if (format == 3u) return memory_.load32(address);
    return decode16(memory_.load16(address), format);
}

void Ge::write_pixel(std::uint32_t address, std::uint32_t rgba, std::uint32_t old) {
    const std::uint32_t format = regs_[kFrameBufPixFormat] & 3u;
    const std::uint32_t mask = (regs_[kMaskRgb] & 0xFFFFFFu) | ((regs_[kMaskAlpha] & 0xFFu) << 24u);
    rgba = (rgba & ~mask) | (old & mask);
    const std::uint32_t r = rgba & 0xFFu, g = (rgba >> 8u) & 0xFFu, b = (rgba >> 16u) & 0xFFu, a = rgba >> 24u;
    switch (format) {
    case 3: memory_.store32(address, rgba); break;
    case 0: memory_.store16(address, static_cast<std::uint16_t>((r >> 3u) | ((g >> 2u) << 5u) | ((b >> 3u) << 11u))); break;
    case 1:
        memory_.store16(address, static_cast<std::uint16_t>((r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) |
                                                            ((a >> 7u) << 15u)));
        break;
    default:
        memory_.store16(address, static_cast<std::uint16_t>((r >> 4u) | ((g >> 4u) << 4u) | ((b >> 4u) << 8u) |
                                                            ((a >> 4u) << 12u)));
        break;
    }
}

// ---------------------------------------------------------------------------
// Textures

void Ge::load_clut() {
    const std::uint32_t address = ((regs_[kClutAddr] & 0xFFFFF0u) | ((regs_[kClutAddrUpper] & 0x0F0000u) << 8u));
    const std::uint32_t bytes = std::min<std::uint32_t>((regs_[kLoadClut] & 0x3Fu) * 32u, static_cast<std::uint32_t>(clut_.size()));
    const std::uint8_t *src = memory_.raw_pointer(address, bytes);
    if (src != nullptr) std::memcpy(clut_.data(), src, bytes);
    clut_hash_ = hash_bytes(clut_.data(), clut_.size());
}

std::uint32_t Ge::texture_bytes(std::uint32_t format, std::uint32_t buf_width, std::uint32_t height) const {
    if (format >= 8u) {
        const std::uint32_t blocks = ((buf_width + 3u) / 4u) * ((height + 3u) / 4u);
        return blocks * (format == 8u ? 8u : 16u);
    }
    static constexpr std::uint32_t bits_per_texel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    return (buf_width * height * bits_per_texel[format & 7u] + 7u) / 8u;
}

// Finds or decodes the bound level-0 texture.  RAM textures are trusted for
// the rest of the frame once validated; VRAM textures (render targets) are
// revalidated whenever the GE may have drawn since.
void Ge::bind_texture() {
    SectionTimer timer{2};
    texture_ = nullptr;
    if ((regs_[kTextureEnable] & 1u) == 0u || (regs_[kClearMode] & 1u) != 0u) return;
    const std::uint32_t format = regs_[kTexFormat] & 0xFu;
    if (format > 10u) return;
    const std::uint32_t address = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
    const std::uint32_t width = 1u << std::min(regs_[kTexSize0] & 0xFu, 9u);
    const std::uint32_t height = 1u << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u);
    const std::uint32_t buf_width = std::max(regs_[kTexBufWidth0] & 0x7FFu, 1u);
    const bool clut = format >= 4u && format <= 7u;
    const TextureKey key{address, format, width, height, buf_width, regs_[kTexMode] & 1u,
                         clut ? regs_[kClutFormat] : 0u, clut ? clut_hash_ : 0u};
    texture_width_ = static_cast<int>(width);
    texture_height_ = static_cast<int>(height);

    const bool in_vram = (address & 0x0F000000u) == 0x04000000u;
    const std::uint32_t bytes = texture_bytes(format, buf_width, std::max(height, 1u));
    // GL drawing does not write VRAM; pull in GPU pixels this texture covers.
    if (gl_ != nullptr && in_vram) gl_->prepare_vram_read(address, bytes); // may touch_vram()
    const std::uint64_t stamp = in_vram ? vram_stamp(address, bytes) : 0u;
    auto found = textures_.find(key);
    if (found != textures_.end() && found->second.frame == frame_ &&
        (!in_vram || found->second.vram_generation == stamp)) {
        texture_ = &found->second;
        return;
    }
    const std::uint8_t *source = memory_.raw_pointer(address, bytes);
    if (source == nullptr) return; // fall back to direct sampling
    const std::uint64_t hash = hash_bytes(source, bytes);
    if (found != textures_.end() && found->second.source_hash == hash) {
        found->second.frame = frame_;
        found->second.vram_generation = stamp;
        texture_ = &found->second;
        return;
    }
    if (found == textures_.end()) {
        if (texture_cache_texels_ + width * height > kTextureCacheTexelLimit) evict_textures(width * height);
        found = textures_.emplace(key, CachedTexture{}).first;
        texture_cache_texels_ += width * height;
    }
    CachedTexture &cached = found->second;
    if (gl_ != nullptr) gl_->flush(); // pending GPU draws must not see the new contents
    cached.texels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            cached.texels[y * width + x] = fetch_texel(static_cast<int>(x), static_cast<int>(y));
    cached.source_hash = hash;
    cached.frame = frame_;
    cached.vram_generation = stamp;
    texture_ = &cached;
}

void Ge::evict_textures(std::uint64_t need) {
    if (gl_ != nullptr) gl_->flush();
    std::vector<std::pair<std::uint32_t, TextureKey>> order;
    order.reserve(textures_.size());
    for (const auto &[key, texture] : textures_) order.emplace_back(texture.frame, key);
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const std::uint64_t goal = kTextureCacheTexelLimit * 3u / 4u;
    for (const auto &[frame, key] : order) {
        if (texture_cache_texels_ + need <= goal) break;
        const auto it = textures_.find(key);
        if (gl_ != nullptr) gl_->forget_texture(&it->second);
        texture_cache_texels_ -= static_cast<std::uint64_t>(key.width) * key.height;
        textures_.erase(it);
    }
}

void Ge::dump_texture(const CachedTexture &texture, std::uint32_t address, std::uint32_t format, std::uint32_t width,
                      std::uint32_t height) {
    std::ostringstream name;
    name << g_dump_dir << "/tex_" << std::hex << address << std::dec << "_f" << format << "_" << width << "x" << height;
    if ((format >= 4u && format <= 7u)) name << "_clut" << std::hex << regs_[kClutFormat] << std::dec;
    // RGB, then alpha as a grey image next to it.
    std::ofstream out(name.str() + ".ppm", std::ios::binary);
    out << "P6\n" << width * 2u << " " << height << "\n255\n";
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t t = texture.texels[y * width + x];
            const char rgb[3] = {static_cast<char>(t & 0xFFu), static_cast<char>((t >> 8u) & 0xFFu), static_cast<char>((t >> 16u) & 0xFFu)};
            out.write(rgb, 3);
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            const char a = static_cast<char>(texture.texels[y * width + x] >> 24u);
            const char grey[3] = {a, a, a};
            out.write(grey, 3);
        }
    }
}

void Ge::log_primitive(std::uint32_t index, std::uint32_t type, std::uint32_t count) {
    const std::uint32_t tex_addr = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
    std::cerr << std::hex << "[prim] " << std::dec << index << " type=" << type << " n=" << count << std::hex
              << " vtype=" << regs_[kVertexType] << " fb=" << ps_.fb_offset << "/" << ps_.fb_stride << "/f" << ps_.fb_format;
    if (ps_.clear) std::cerr << " CLEAR=" << ps_.clear_flags;
    if ((regs_[kTextureEnable] & 1u) != 0u)
        std::cerr << " tex=" << tex_addr << "/f" << (regs_[kTexFormat] & 0xFu) << "/" << (regs_[kTexSize0] & 0xFFFu)
                  << "/bw" << (regs_[kTexBufWidth0] & 0x7FFu) << " clut=" << regs_[kClutFormat] << " tfunc=" << regs_[kTexFunc]
                  << " env=" << regs_[kTexEnvColor] << " map=" << regs_[kTexMapMode];
    if ((regs_[kBlendEnable] & 1u) != 0u)
        std::cerr << " blend=" << regs_[kBlendMode] << " fa=" << regs_[kBlendFixA] << " fb=" << regs_[kBlendFixB];
    if ((regs_[kAlphaTestEnable] & 1u) != 0u) std::cerr << " atest=" << regs_[kAlphaTest];
    if ((regs_[kZTestEnable] & 1u) != 0u) std::cerr << " z=" << regs_[kZTest] << (ps_.z_write ? "w" : "");
    if ((regs_[0x24] & 1u) != 0u) std::cerr << " STENCIL=" << regs_[0xDC] << "/" << regs_[0xDD];
    if ((regs_[0x1F] & 1u) != 0u) std::cerr << " FOG=" << regs_[0xCF];
    if ((regs_[0x27] & 1u) != 0u) std::cerr << " COLORTEST";
    if ((regs_[0x28] & 1u) != 0u) std::cerr << " LOGICOP=" << regs_[0xE6];
    if ((regs_[kLightingEnable] & 1u) != 0u) {
        std::cerr << " light upd=" << regs_[kMaterialUpdate] << " emis=" << regs_[kMaterialEmissive]
                  << " diff=" << regs_[kMaterialDiffuse] << " global=" << regs_[kAmbientColor];
        for (std::uint32_t i = 0; i < 4u; ++i)
            if ((regs_[kLightEnable0 + i] & 1u) != 0u)
                std::cerr << " L" << i << "=" << regs_[kLightType0 + i] << ":" << regs_[kLightColor0 + i * 3u] << "/"
                          << regs_[kLightColor0 + i * 3u + 1u] << "/" << regs_[kLightColor0 + i * 3u + 2u];
    }
    if ((regs_[kMaskRgb] & 0xFFFFFFu) != 0u || (regs_[kMaskAlpha] & 0xFFu) != 0u)
        std::cerr << " mask=" << regs_[kMaskRgb] << "/" << regs_[kMaskAlpha];
    std::cerr << " mat=" << regs_[kMaterialAmbient] << "/" << regs_[kMaterialAlpha] << std::dec << "\n";
    static std::set<std::uintptr_t> dumped;
    if ((regs_[kTextureEnable] & 1u) != 0u && texture_ != nullptr &&
        dumped.insert(reinterpret_cast<std::uintptr_t>(texture_)).second)
        dump_texture(*texture_, tex_addr, regs_[kTexFormat] & 0xFu, static_cast<std::uint32_t>(texture_width_),
                     static_cast<std::uint32_t>(texture_height_));
}

void Ge::prepare_pixel_state() {
    PixelState &p = ps_;
    p.vram = memory_.raw_pointer(kVramBase, 0x200000u);
    p.fb_format = regs_[kFrameBufPixFormat] & 3u;
    p.fb_bpp = p.fb_format == 3u ? 4u : 2u;
    p.fb_stride = regs_[kFrameBufWidth] & 0x7FCu;
    p.fb_offset = ((regs_[kFrameBufPtr] & 0xFFFFFFu) | ((regs_[kFrameBufWidth] & 0xFF0000u) << 8u)) & 0x1FFFF0u;
    p.z_stride = regs_[kZBufWidth] & 0x7FCu;
    p.z_offset = ((regs_[kZBufPtr] & 0xFFFFFFu) | ((regs_[kZBufWidth] & 0xFF0000u) << 8u)) & 0x1FFFF0u;
    p.z_test = (regs_[kZTestEnable] & 1u) != 0u && p.z_stride != 0u;
    p.z_func = regs_[kZTest] & 7u;
    p.z_write = p.z_test && (regs_[kZWriteDisable] & 1u) == 0u;
    p.clear = (regs_[kClearMode] & 1u) != 0u;
    p.clear_flags = regs_[kClearMode];
    p.texture = (regs_[kTextureEnable] & 1u) != 0u;
    p.tex_func = regs_[kTexFunc] & 7u;
    p.tex_alpha = (regs_[kTexFunc] & 0x100u) != 0u;
    p.tex_double = (regs_[kTexFunc] & 0x10000u) != 0u;
    color24(regs_[kTexEnvColor], p.env);
    p.alpha_test = (regs_[kAlphaTestEnable] & 1u) != 0u;
    p.alpha_func = regs_[kAlphaTest] & 7u;
    p.alpha_mask = (regs_[kAlphaTest] >> 16u) & 0xFFu;
    p.alpha_ref = ((regs_[kAlphaTest] >> 8u) & 0xFFu) & p.alpha_mask;
    p.blend = (regs_[kBlendEnable] & 1u) != 0u;
    p.src_factor = regs_[kBlendMode] & 0xFu;
    p.dst_factor = (regs_[kBlendMode] >> 4u) & 0xFu;
    p.equation = (regs_[kBlendMode] >> 8u) & 7u;
    color24(regs_[kBlendFixA], p.fix_a);
    color24(regs_[kBlendFixB], p.fix_b);
    p.write_mask = (regs_[kMaskRgb] & 0xFFFFFFu) | ((regs_[kMaskAlpha] & 0xFFu) << 24u);
    p.sx1 = static_cast<int>(regs_[kScissor1] & 0x3FFu);
    p.sy1 = static_cast<int>((regs_[kScissor1] >> 10u) & 0x3FFu);
    p.sx2 = std::min(static_cast<int>(regs_[kScissor2] & 0x3FFu), static_cast<int>(p.fb_stride) - 1);
    p.sy2 = static_cast<int>((regs_[kScissor2] >> 10u) & 0x3FFu);
}

std::uint32_t Ge::clut_lookup(std::uint32_t index) const {
    const std::uint32_t mode = regs_[kClutFormat];
    const std::uint32_t format = mode & 3u, shift = (mode >> 2u) & 0x1Fu, mask = (mode >> 8u) & 0xFFu;
    const std::uint32_t start = ((mode >> 16u) & 0x1Fu) << 4u;
    const std::uint32_t i = ((index >> shift) & mask) | start;
    if (format == 3u) {
        const std::uint32_t at = (i * 4u) & 0x7FCu;
        return static_cast<std::uint32_t>(clut_[at]) | (static_cast<std::uint32_t>(clut_[at + 1u]) << 8u) |
               (static_cast<std::uint32_t>(clut_[at + 2u]) << 16u) | (static_cast<std::uint32_t>(clut_[at + 3u]) << 24u);
    }
    const std::uint32_t at = (i * 2u) & 0x7FEu;
    return decode16(static_cast<std::uint32_t>(clut_[at]) | (static_cast<std::uint32_t>(clut_[at + 1u]) << 8u), format);
}

std::uint32_t Ge::fetch_texel(int x, int y) const {
    const std::uint32_t format = regs_[kTexFormat] & 0xFu;
    const std::uint32_t address = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
    const std::uint32_t buf_width = std::max(regs_[kTexBufWidth0] & 0x7FFu, 1u);
    const auto ux = static_cast<std::uint32_t>(x), uy = static_cast<std::uint32_t>(y);

    if (format >= 8u) { // DXT1/3/5: 4x4 blocks
        const std::uint32_t block_bytes = format == 8u ? 8u : 16u;
        const std::uint32_t block = (uy / 4u) * (buf_width / 4u) + ux / 4u;
        const std::uint32_t at = address + block * block_bytes;
        const std::uint32_t lx = ux & 3u, ly = uy & 3u;
        const std::uint32_t lines = memory_.load32(at);
        const std::uint32_t c1 = memory_.load16(at + 4u), c2 = memory_.load16(at + 6u);
        // DXT colours are RGB565 with red in the top bits, unlike PSP 16-bit pixels.
        auto dxt565 = [](std::uint32_t c) {
            const std::uint32_t r = (c >> 11u) & 31u, g = (c >> 5u) & 63u, b = c & 31u;
            return ((r << 3u) | (r >> 2u)) | (((g << 2u) | (g >> 4u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u);
        };
        const std::uint32_t rgb1 = dxt565(c1), rgb2 = dxt565(c2);
        const std::uint32_t sel = (lines >> (ly * 8u + lx * 2u)) & 3u;
        auto mix = [](std::uint32_t p, std::uint32_t q, std::uint32_t wp, std::uint32_t wq, std::uint32_t div) {
            std::uint32_t out = 0;
            for (std::uint32_t s = 0; s < 24u; s += 8u)
                out |= ((((p >> s) & 0xFFu) * wp + ((q >> s) & 0xFFu) * wq) / div) << s;
            return out;
        };
        std::uint32_t rgb;
        std::uint32_t alpha = 255u;
        const bool four_colors = format != 8u || c1 > c2;
        if (sel == 0u) rgb = rgb1 & 0xFFFFFFu;
        else if (sel == 1u) rgb = rgb2 & 0xFFFFFFu;
        else if (four_colors) rgb = sel == 2u ? mix(rgb1, rgb2, 2u, 1u, 3u) : mix(rgb1, rgb2, 1u, 2u, 3u);
        else if (sel == 2u) rgb = mix(rgb1, rgb2, 1u, 1u, 2u);
        else { rgb = 0u; alpha = 0u; }
        if (format == 9u) {
            const std::uint32_t row = memory_.load16(at + 8u + ly * 2u);
            alpha = ((row >> (lx * 4u)) & 0xFu) * 17u;
        } else if (format == 10u) {
            // 48 bits of 3-bit indices: the low 32 at +8, the high 16 at +12.
            const std::uint64_t bits = static_cast<std::uint64_t>(memory_.load32(at + 8u)) |
                                       (static_cast<std::uint64_t>(memory_.load16(at + 12u)) << 32u);
            const std::uint32_t a1 = memory_.load8(at + 14u), a2 = memory_.load8(at + 15u);
            const std::uint32_t idx = static_cast<std::uint32_t>((bits >> ((ly * 4u + lx) * 3u)) & 7u);
            if (idx == 0u) alpha = a1;
            else if (idx == 1u) alpha = a2;
            else if (a1 > a2) alpha = (a1 * (8u - idx) + a2 * (idx - 1u)) / 7u;
            else if (idx < 6u) alpha = (a1 * (6u - idx) + a2 * (idx - 1u)) / 5u;
            else alpha = idx == 6u ? 0u : 255u;
        }
        return (rgb & 0xFFFFFFu) | (alpha << 24u);
    }

    static constexpr std::uint32_t bits_per_texel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    const std::uint32_t bpp = bits_per_texel[format & 7u];
    std::uint32_t byte_offset;
    const std::uint32_t x_bits = ux * bpp;
    if ((regs_[kTexMode] & 1u) != 0u) {
        const std::uint32_t row_bytes = buf_width * bpp / 8u;
        const std::uint32_t x_bytes = x_bits / 8u;
        const std::uint32_t block = (uy / 8u) * (row_bytes / 16u) + x_bytes / 16u;
        byte_offset = block * 128u + (uy % 8u) * 16u + x_bytes % 16u;
    } else {
        byte_offset = (uy * buf_width * bpp + x_bits) / 8u;
    }
    const std::uint32_t at = address + byte_offset;
    switch (format) {
    case 0: case 1: case 2: return decode16(memory_.load16(at), format);
    case 3: return memory_.load32(at);
    case 4: return clut_lookup((memory_.load8(at) >> ((ux & 1u) * 4u)) & 0xFu);
    case 5: return clut_lookup(memory_.load8(at));
    case 6: return clut_lookup(memory_.load16(at));
    default: return clut_lookup(memory_.load32(at));
    }
}

void Ge::sample_texture(float u, float v, float rgba[4]) const {
    const int width = 1 << std::min(regs_[kTexSize0] & 0xFu, 9u);
    const int height = 1 << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u);
    int x = static_cast<int>(std::floor(u));
    int y = static_cast<int>(std::floor(v));
    if ((regs_[kTexWrap] & 1u) != 0u) x = std::clamp(x, 0, width - 1);
    else x &= width - 1;
    if ((regs_[kTexWrap] & 0x100u) != 0u) y = std::clamp(y, 0, height - 1);
    else y &= height - 1;
    const std::uint32_t texel = texture_ != nullptr
        ? texture_->texels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]
        : fetch_texel(x, y);
    rgba[0] = static_cast<float>(texel & 0xFFu);
    rgba[1] = static_cast<float>((texel >> 8u) & 0xFFu);
    rgba[2] = static_cast<float>((texel >> 16u) & 0xFFu);
    rgba[3] = static_cast<float>(texel >> 24u);
}

// ---------------------------------------------------------------------------
// Block transfer (TRXKICK)

void Ge::block_transfer() {
    SectionTimer timer{3};
    ++stats_.transfers;
    const std::uint32_t src = (regs_[kTransferSrc] & 0xFFFFF0u) | ((regs_[kTransferSrcW] & 0xFF0000u) << 8u);
    const std::uint32_t dst = (regs_[kTransferDst] & 0xFFFFF0u) | ((regs_[kTransferDstW] & 0xFF0000u) << 8u);
    const std::uint32_t src_stride = regs_[kTransferSrcW] & 0x7F8u;
    const std::uint32_t dst_stride = regs_[kTransferDstW] & 0x7F8u;
    const std::uint32_t sx = regs_[kTransferSrcPos] & 0x3FFu, sy = (regs_[kTransferSrcPos] >> 10u) & 0x3FFu;
    const std::uint32_t dx = regs_[kTransferDstPos] & 0x3FFu, dy = (regs_[kTransferDstPos] >> 10u) & 0x3FFu;
    const std::uint32_t width = (regs_[kTransferSize] & 0x3FFu) + 1u;
    const std::uint32_t height = ((regs_[kTransferSize] >> 10u) & 0x3FFu) + 1u;
    const std::uint32_t bpp = (regs_[kTransferStart] & 1u) != 0u ? 4u : 2u;
    const std::uint32_t src_first = src + (sy * src_stride + sx) * bpp, dst_first = dst + (dy * dst_stride + dx) * bpp;
    const std::uint32_t src_span = ((height - 1u) * src_stride + width) * bpp, dst_span = ((height - 1u) * dst_stride + width) * bpp;
    static const std::uint32_t log_frame = [] {
        const char *v = std::getenv("PSPWEB_XFER_FRAME");
        return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 900u;
    }();
    if (g_profile_targets && frame_ >= log_frame && frame_ < log_frame + 1u)
        std::cerr << "[ge-transfer] frame=" << frame_ << " src=0x" << std::hex << src_first << " stride=" << std::dec
                  << src_stride << " dst=0x" << std::hex << dst_first << " stride=" << std::dec << dst_stride << " size="
                  << width << "x" << height << " bpp=" << bpp << "\n";
    (void)src_first;
    (void)dst_first;
    (void)src_span;
    (void)dst_span;
    const TransferRect src_rect{src, src_stride, sx, sy, width, height, bpp};
    const TransferRect dst_rect{dst, dst_stride, dx, dy, width, height, bpp};
    touch_vram(dst_first, dst_span);
    state_dirty_ = true;
    g_transfer_bytes += static_cast<double>(width) * height * bpp;
    if (gl_ != nullptr) {
        SectionTimer gl_timer{5};
        gl_->before_transfer(src_rect, dst_rect);
    }
    struct AfterTransfer {
        GeGl *gl;
        TransferRect rect;
        ~AfterTransfer() {
            if (gl == nullptr) return;
            SectionTimer gl_timer{5};
            gl->after_transfer(rect);
        }
    } after{gl_, dst_rect};
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t from = src + ((sy + row) * src_stride + sx) * bpp;
        const std::uint32_t to = dst + ((dy + row) * dst_stride + dx) * bpp;
        const std::uint8_t *s = memory_.raw_pointer(from, width * bpp);
        std::uint8_t *d = memory_.raw_pointer(to, width * bpp);
        if (s != nullptr && d != nullptr) std::memmove(d, s, width * bpp);
    }
}

// ---------------------------------------------------------------------------
// GL backend glue

void Ge::set_gl(GeGl *gl) {
    gl_ = gl;
    if (gl_ != nullptr) gl_->set_vram_write_hook([this](std::uint32_t address, std::uint32_t bytes) { touch_vram(address, bytes); });
}

void Ge::touch_vram(std::uint32_t address, std::uint32_t bytes) {
    const std::uint32_t c = address & 0x1FFFFFFFu;
    if (c < kVramBase || c >= kVramBase + 0x800000u || bytes == 0u) return;
    const std::uint32_t offset = (c - kVramBase) & 0x1FFFFFu;
    const std::uint32_t first = offset >> 12u, last = std::min((offset + bytes - 1u) >> 12u, 511u);
    const std::uint64_t stamp = ++vram_write_counter_;
    for (std::uint32_t page = first; page <= last; ++page) vram_pages_[page] = stamp;
}

std::uint64_t Ge::vram_stamp(std::uint32_t address, std::uint32_t bytes) const {
    const std::uint32_t c = address & 0x1FFFFFFFu;
    const std::uint32_t offset = (c - kVramBase) & 0x1FFFFFu;
    const std::uint32_t first = offset >> 12u, last = std::min((offset + std::max(bytes, 1u) - 1u) >> 12u, 511u);
    std::uint64_t newest = vram_generation_; // software drawing invalidates all of VRAM
    for (std::uint32_t page = first; page <= last; ++page) newest = std::max(newest, vram_pages_[page]);
    return newest;
}

void Ge::submit_gl_state() {
    SectionTimer timer{4};
    const PixelState &p = ps_;
    GlDrawState st{};
    st.fb_offset = p.fb_offset;
    st.fb_stride = p.fb_stride;
    st.fb_format = p.fb_format;
    st.sx1 = p.sx1;
    st.sy1 = p.sy1;
    st.sx2 = p.sx2;
    st.sy2 = p.sy2;
    st.clear = p.clear;
    st.clear_flags = p.clear ? (p.clear_flags & 0x700u) : 0u;
    if (!p.clear) {
        st.z_test = p.z_test;
        st.z_write = p.z_write;
        st.z_func = p.z_test ? p.z_func : 0u;
        st.blend = p.blend;
        if (p.blend) {
            st.src_factor = p.src_factor;
            st.dst_factor = p.dst_factor;
            st.equation = p.equation;
            st.fix_a = regs_[kBlendFixA] & 0xFFFFFFu;
            st.fix_b = regs_[kBlendFixB] & 0xFFFFFFu;
        }
        st.alpha_test = p.alpha_test;
        if (p.alpha_test) {
            st.alpha_func = p.alpha_func;
            st.alpha_ref = p.alpha_ref;
            st.alpha_mask = p.alpha_mask;
        }
        st.stencil = (regs_[0x24] & 1u) != 0u;
        if (st.stencil) {
            st.stencil_func = regs_[0xDC] & 0xFFFFFFu;
            st.stencil_ops = regs_[0xDD] & 0xFFFFFFu;
        }
        st.mask_rgb = regs_[kMaskRgb] & 0xFFFFFFu;
        st.mask_alpha = regs_[kMaskAlpha] & 0xFFu;
        st.texture = p.texture;
        // Fog factor = (eye z + FOG1) * FOG2.  With a perspective projection
        // (bottom row 0, 0, p, 0) the clip w is p * eye z, so the factor is
        // linear in w and the shader derives it from the w it already has.
        if ((regs_[0x1F] & 1u) != 0u && !layout_.through && proj_[3] == 0.0f && proj_[7] == 0.0f &&
            proj_[15] == 0.0f && proj_[11] != 0.0f) {
            auto finite = [](float v) { return std::isfinite(v) ? v : (std::signbit(v) ? -65535.0f : 65535.0f); };
            const float end = finite(cmd_float(regs_[0xCD])), scale = finite(cmd_float(regs_[0xCE]));
            st.fog = true;
            st.fog_w = scale / proj_[11];
            st.fog_bias = end * scale;
            st.fog_color = regs_[0xCF] & 0xFFFFFFu;
        }
    }
    if (st.texture) {
        st.tex_func = p.tex_func;
        st.tex_alpha = p.tex_alpha;
        st.tex_double = p.tex_double;
        st.env = regs_[kTexEnvColor] & 0xFFFFFFu;
        st.clamp_u = (regs_[kTexWrap] & 1u) != 0u;
        st.clamp_v = (regs_[kTexWrap] & 0x100u) != 0u;
        st.linear = (regs_[0xC6] & 0x100u) != 0u;
        const std::uint32_t address = (regs_[kTexAddr0] & 0xFFFFF0u) | ((regs_[kTexBufWidth0] & 0x0F0000u) << 8u);
        const std::uint32_t format = regs_[kTexFormat] & 0xFu;
        const std::uint32_t buf_width = regs_[kTexBufWidth0] & 0x7FFu;
        if (format <= 3u && gl_->is_gpu_target(address, format, buf_width)) {
            st.tex_from_target = true;
            st.tex_address = address;
            st.tex_format = format;
            st.tex_buf_width = buf_width;
            st.tex_width = 1u << std::min(regs_[kTexSize0] & 0xFu, 9u);
            st.tex_height = 1u << std::min((regs_[kTexSize0] >> 8u) & 0xFu, 9u);
        } else {
            bind_texture();
            if (texture_ != nullptr) {
                st.texels = texture_->texels.data();
                st.texture_identity = texture_;
                st.texture_hash = texture_->source_hash;
                st.tex_width = static_cast<std::uint32_t>(texture_width_);
                st.tex_height = static_cast<std::uint32_t>(texture_height_);
            } else {
                st.texture = false;
            }
        }
    }
    gl_->set_state(st);
}

void Ge::emit_triangle(const Vertex &a, const Vertex &b, const Vertex &c) {
    if (gl_ == nullptr) {
        raster_triangle(a, b, c);
        return;
    }
    ++stats_.triangles;
    const bool flat = (regs_[kShadeMode] & 1u) == 0u;
    auto convert = [&](const Vertex &v) {
        const Vertex &col = flat ? c : v;
        return GlVertex{v.x, v.y, v.z, v.w, v.u, v.v,
                        static_cast<std::uint8_t>(clamp255(col.r)), static_cast<std::uint8_t>(clamp255(col.g)),
                        static_cast<std::uint8_t>(clamp255(col.b)), static_cast<std::uint8_t>(clamp255(col.a))};
    };
    gl_->triangle(convert(a), convert(b), convert(c));
}

void Ge::emit_sprite(const Vertex &a, const Vertex &b) {
    if (gl_ == nullptr) {
        draw_sprite(a, b);
        return;
    }
    ++stats_.sprites;
    const auto r = static_cast<std::uint8_t>(clamp255(b.r)), g = static_cast<std::uint8_t>(clamp255(b.g));
    const auto bl = static_cast<std::uint8_t>(clamp255(b.b)), al = static_cast<std::uint8_t>(clamp255(b.a));
    const GlVertex tl{a.x, a.y, b.z, 1.0f, a.u, a.v, r, g, bl, al};
    const GlVertex tr{b.x, a.y, b.z, 1.0f, b.u, a.v, r, g, bl, al};
    const GlVertex bl_{a.x, b.y, b.z, 1.0f, a.u, b.v, r, g, bl, al};
    const GlVertex br{b.x, b.y, b.z, 1.0f, b.u, b.v, r, g, bl, al};
    gl_->triangle(tl, tr, bl_);
    gl_->triangle(tr, br, bl_);
}

} // namespace pspweb
