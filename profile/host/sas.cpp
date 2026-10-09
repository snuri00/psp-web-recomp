// sceSasCore for the web profile: the PSP's software voice synthesizer, which
// games use for sound effects.  Each of the 32 voices plays PSP ADPCM ("VAG")
// data at a pitch, shaped by an ADSR envelope and a left/right volume; every
// __sceSasCore call mixes one grain of samples into a stereo buffer that the
// game then sends to sceAudio.  Reverb is accepted but not applied.
//
// Envelope rates and curves follow the behaviour documented by PPSSPP and JPCSP.

#include "kernel.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pspweb {
namespace {

constexpr int kVoices = 32;
constexpr std::int64_t kEnvelopeMax = 0x40000000;
constexpr std::int32_t kPitchBase = 0x1000;
constexpr std::uint32_t kErrorInvalidVoice = 0x80420010u;

enum Curve { kLinearIncrease, kLinearDecrease, kLinearBent, kExponentDecrease, kExponentIncrease, kDirect };
enum class EnvState { Off, Attack, Decay, Sustain, Release };

int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int rate = ((7 - (n & 3)) << 26) >> (n >> 2);
    return rate == 0 ? 1 : rate;
}

int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int rate = ((7 - (n & 3)) << 24) >> (n >> 2);
    return rate == 0 ? 1 : rate;
}

struct Envelope {
    std::int64_t attack_rate{}, decay_rate{}, sustain_rate{}, release_rate{};
    int attack_type{kLinearIncrease}, decay_type{kLinearDecrease}, sustain_type{kLinearDecrease},
        release_type{kLinearDecrease};
    std::int64_t sustain_level{};
    std::int64_t height{};
    EnvState state{EnvState::Off};

    void walk(int type, std::int64_t rate) {
        switch (type) {
        case kLinearIncrease: height += rate; break;
        case kLinearDecrease: height -= rate; break;
        case kLinearBent: height += height <= kEnvelopeMax * 3 / 4 ? rate : rate / 4; break;
        case kExponentDecrease: height -= ((height * rate) >> 32) + 1; break;
        case kExponentIncrease: height += (((kEnvelopeMax - height) * rate) >> 32) + 1; break;
        default: height = rate; break; // direct
        }
    }
    void set(EnvState next) {
        height = std::clamp<std::int64_t>(height, 0, kEnvelopeMax);
        state = next;
    }
    void step() {
        switch (state) {
        case EnvState::Attack:
            walk(attack_type, attack_rate);
            if (height >= kEnvelopeMax || height < 0) set(EnvState::Decay);
            break;
        case EnvState::Decay:
            walk(decay_type, decay_rate);
            if (height < sustain_level) set(EnvState::Sustain);
            break;
        case EnvState::Sustain:
            walk(sustain_type, sustain_rate);
            if (height <= 0) set(EnvState::Release);
            break;
        case EnvState::Release:
            walk(release_type, release_rate);
            // Below this the scaled envelope is zero, so the voice is silent.
            if (height < 0x8000) {
                height = 0;
                state = EnvState::Off;
            }
            break;
        case EnvState::Off: break;
        }
    }
};

// PSP ADPCM: 16-byte blocks of 28 samples (shift/filter byte, flags byte, 14 data bytes).
struct VagDecoder {
    std::uint32_t address{}, blocks{};
    bool loop{};
    std::uint32_t block{};
    int loop_start{-1};
    bool loop_after_block{}, ended{true};
    std::int32_t s1{}, s2{};
    std::array<std::int16_t, 28> samples{};
    int cursor{28};

    void start(std::uint32_t addr, std::uint32_t size, bool looping) {
        address = addr;
        blocks = size / 16u;
        loop = looping;
        block = 0u;
        loop_start = -1;
        loop_after_block = false;
        ended = blocks == 0u;
        s1 = s2 = 0;
        cursor = 28;
    }
    void decode_block(const psprecomp::GuestMemory &memory) {
        if (loop_after_block && loop_start >= 0) block = static_cast<std::uint32_t>(loop_start);
        loop_after_block = false;
        if (block >= blocks) {
            ended = true;
            return;
        }
        const std::uint8_t *p = memory.raw_pointer(address + block * 16u, 16u);
        if (p == nullptr) {
            ended = true;
            return;
        }
        const int shift = p[0] & 0xF, filter = std::min(p[0] >> 4, 4);
        const int flags = p[1];
        if (flags == 7) {
            ended = true;
            return;
        }
        if (flags == 6) loop_start = static_cast<int>(block);
        if (flags == 3 && loop) loop_after_block = true;
        static constexpr int kCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        for (int i = 0; i < 28; ++i) {
            const int nibble = (p[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
            std::int32_t s = static_cast<std::int16_t>(static_cast<std::uint16_t>(nibble << 12)) >> shift;
            s += (s1 * kCoef[filter][0] + s2 * kCoef[filter][1]) >> 6;
            s = std::clamp(s, -32768, 32767);
            s2 = s1;
            s1 = s;
            samples[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(s);
        }
        cursor = 0;
        ++block;
    }
    std::int16_t next(const psprecomp::GuestMemory &memory) {
        if (ended) return 0;
        if (cursor >= 28) {
            decode_block(memory);
            if (ended) return 0;
        }
        return samples[static_cast<std::size_t>(cursor++)];
    }
};

struct Voice {
    bool playing{}, on{}, paused{};
    std::uint32_t vag_address{}, vag_size{};
    bool loop{};
    std::int32_t pitch{kPitchBase};
    std::int32_t left{}, right{}, effect_left{}, effect_right{};
    Envelope envelope;
    VagDecoder vag;
    std::uint32_t frac{};
    std::int16_t current{}, following{};

    void key_on(const psprecomp::GuestMemory &memory) {
        vag.start(vag_address, vag_size, loop);
        envelope.height = 0;
        envelope.state = EnvState::Attack;
        playing = on = true;
        paused = false;
        frac = 0u;
        current = vag.next(memory);
        following = vag.next(memory);
    }
};

struct Sas {
    std::uint32_t grain{256}, output_mode{};
    std::array<Voice, kVoices> voices{};
    std::vector<std::int32_t> mix;
};

Sas &state(std::shared_ptr<void> &slot) {
    if (!slot) slot = std::make_shared<Sas>();
    return *static_cast<Sas *>(slot.get());
}

} // namespace

void Kernel::install_sas() {
    auto &m = rt_.memory();
    Sas &sas = state(sas_);
    auto add = [this](std::uint32_t nid, const char *name, std::function<std::uint32_t(Ctx &)> body) {
        static const bool trace = std::getenv("PSPWEB_TRACE_HLE") != nullptr;
        rt_.register_hle("sceSasCore", nid, [body, name](psprecomp::Runtime &, Ctx &ctx) {
            const std::uint32_t a0 = ctx.gpr[4], a1 = ctx.gpr[5], a2 = ctx.gpr[6];
            const std::uint32_t result = body(ctx);
            if (trace) std::fprintf(stderr, "[sas] %s(0x%X, 0x%X, 0x%X) -> 0x%X\n", name, a0, a1, a2, result);
            finish(ctx, result);
        });
        rt_.nids().add("sceSasCore", nid, name);
        char key[40];
        std::snprintf(key, sizeof key, "sceSasCore:0x%08X", nid);
        registered_.insert(key);
    };
    auto voice = [&sas](Ctx &ctx) -> Voice * {
        const auto index = static_cast<std::int32_t>(ctx.gpr[5]);
        return index >= 0 && index < kVoices ? &sas.voices[static_cast<std::size_t>(index)] : nullptr;
    };

    add(0x42778A9Fu, "__sceSasInit", [&sas](Ctx &ctx) {
        sas = Sas{};
        sas.grain = std::clamp(ctx.gpr[5], 64u, 2048u);
        sas.output_mode = ctx.gpr[7];
        return 0u;
    });
    add(0xD1E0A01Eu, "__sceSasSetGrain", [&sas](Ctx &ctx) {
        sas.grain = std::clamp(ctx.gpr[5], 64u, 2048u);
        return 0u;
    });
    add(0xBD11B7C2u, "__sceSasGetGrain", [&sas](Ctx &) { return sas.grain; });
    add(0xE855BF76u, "__sceSasSetOutputmode", [&sas](Ctx &ctx) {
        sas.output_mode = ctx.gpr[5];
        return 0u;
    });
    add(0xE175EF66u, "__sceSasGetOutputmode", [&sas](Ctx &) { return sas.output_mode; });
    add(0x99944089u, "__sceSasSetVoice", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        v->vag_address = ctx.gpr[6];
        v->vag_size = ctx.gpr[7];
        v->loop = ctx.gpr[8] != 0u;
        return 0u;
    });
    add(0xAD84D37Fu, "__sceSasSetPitch", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        v->pitch = std::clamp(static_cast<std::int32_t>(ctx.gpr[6]), 0, 0x4000);
        return 0u;
    });
    add(0x440CA7D8u, "__sceSasSetVolume", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        v->left = static_cast<std::int32_t>(ctx.gpr[6]);
        v->right = static_cast<std::int32_t>(ctx.gpr[7]);
        v->effect_left = static_cast<std::int32_t>(ctx.gpr[8]);
        v->effect_right = static_cast<std::int32_t>(ctx.gpr[9]);
        return 0u;
    });
    add(0x019B25EBu, "__sceSasSetADSR", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        const std::uint32_t flags = ctx.gpr[6];
        if ((flags & 1u) != 0u) v->envelope.attack_rate = static_cast<std::int32_t>(ctx.gpr[7]);
        if ((flags & 2u) != 0u) v->envelope.decay_rate = static_cast<std::int32_t>(ctx.gpr[8]);
        if ((flags & 4u) != 0u) v->envelope.sustain_rate = static_cast<std::int32_t>(ctx.gpr[9]);
        if ((flags & 8u) != 0u) v->envelope.release_rate = static_cast<std::int32_t>(ctx.gpr[10]);
        return 0u;
    });
    add(0x9EC3676Au, "__sceSasSetADSRmode", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        const std::uint32_t flags = ctx.gpr[6];
        if ((flags & 1u) != 0u) v->envelope.attack_type = static_cast<int>(ctx.gpr[7]);
        if ((flags & 2u) != 0u) v->envelope.decay_type = static_cast<int>(ctx.gpr[8]);
        if ((flags & 4u) != 0u) v->envelope.sustain_type = static_cast<int>(ctx.gpr[9]);
        if ((flags & 8u) != 0u) v->envelope.release_type = static_cast<int>(ctx.gpr[10]);
        return 0u;
    });
    add(0x5F9529F6u, "__sceSasSetSL", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        v->envelope.sustain_level = static_cast<std::int32_t>(ctx.gpr[6]);
        return 0u;
    });
    add(0xCBCD4F79u, "__sceSasSetSimpleADSR", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        const auto a = static_cast<int>(ctx.gpr[6] & 0xFFFFu), b = static_cast<int>(ctx.gpr[7] & 0xFFFFu);
        Envelope &e = v->envelope;
        e.attack_rate = simple_rate(a >> 8);
        e.attack_type = (a & 0x8000) == 0 ? kLinearIncrease : kLinearBent;
        const int decay = (a >> 4) & 0xF;
        e.decay_rate = decay == 0 ? 0x7FFFFFFF : (0x80000000ll >> decay);
        e.decay_type = kExponentDecrease;
        static constexpr int kSustainTypes[4] = {kLinearIncrease, kLinearDecrease, kLinearBent, kExponentDecrease};
        e.sustain_type = kSustainTypes[(b >> 14) & 3];
        e.sustain_rate = e.sustain_type == kExponentDecrease ? exponent_rate(b >> 6) : simple_rate(b >> 6);
        e.release_type = (b & 0x20) == 0 ? kLinearDecrease : kExponentDecrease;
        const int release = b & 0x1F;
        if (release == 31) e.release_rate = 0;
        else if (e.release_type == kLinearDecrease) e.release_rate = release == 30 ? 0x40000000 : release == 29 ? 1 : (0x10000000 >> release);
        else e.release_rate = release == 0 ? 0x7FFFFFFF : (0x80000000ll >> release);
        e.sustain_level = static_cast<std::int64_t>((a & 0xF) + 1) << 26;
        return 0u;
    });
    add(0x76F01ACAu, "__sceSasSetKeyOn", [voice, &m](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        v->key_on(m);
        return 0u;
    });
    add(0xA0CF2FA4u, "__sceSasSetKeyOff", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        v->on = false;
        if (v->envelope.state != EnvState::Off) v->envelope.state = EnvState::Release;
        return 0u;
    });
    add(0x68A46B95u, "__sceSasGetEndFlag", [&sas](Ctx &) {
        std::uint32_t ended = 0u;
        for (int i = 0; i < kVoices; ++i)
            if (!sas.voices[static_cast<std::size_t>(i)].playing) ended |= 1u << i;
        return ended;
    });
    add(0x74AE582Au, "__sceSasGetEnvelopeHeight", [voice](Ctx &ctx) {
        Voice *v = voice(ctx);
        if (v == nullptr) return kErrorInvalidVoice;
        return static_cast<std::uint32_t>(v->envelope.height);
    });
    add(0x07F58C24u, "__sceSasGetAllEnvelopeHeights", [&sas, &m](Ctx &ctx) {
        for (int i = 0; i < kVoices; ++i)
            m.store32(ctx.gpr[5] + static_cast<std::uint32_t>(i) * 4u,
                      static_cast<std::uint32_t>(sas.voices[static_cast<std::size_t>(i)].envelope.height));
        return 0u;
    });
    add(0x787D04D5u, "__sceSasSetPause", [&sas](Ctx &ctx) {
        for (int i = 0; i < kVoices; ++i)
            if ((ctx.gpr[5] >> i) & 1u) sas.voices[static_cast<std::size_t>(i)].paused = ctx.gpr[6] != 0u;
        return 0u;
    });
    add(0x2C8E6AB3u, "__sceSasGetPauseFlag", [&sas](Ctx &) {
        std::uint32_t paused = 0u;
        for (int i = 0; i < kVoices; ++i)
            if (sas.voices[static_cast<std::size_t>(i)].paused) paused |= 1u << i;
        return paused;
    });
    // Reverb settings are accepted; the effect itself is not rendered.
    add(0x33D4AB37u, "__sceSasRevType", [](Ctx &) { return 0u; });
    add(0x267A6DD2u, "__sceSasRevParam", [](Ctx &) { return 0u; });
    add(0xD5A229C9u, "__sceSasRevEVOL", [](Ctx &) { return 0u; });
    add(0xF983B186u, "__sceSasRevVON", [](Ctx &) { return 0u; });

    // Mixes one grain of every playing voice; `accumulate` adds to the buffer
    // (scaled by the given volumes) instead of overwriting it.
    auto render = [&sas, &m](std::uint32_t out, bool accumulate, std::int32_t out_left, std::int32_t out_right) {
        const std::uint32_t frames = sas.grain;
        sas.mix.assign(static_cast<std::size_t>(frames) * 2u, 0);
        for (Voice &v : sas.voices) {
            if (!v.playing || v.paused) continue;
            for (std::uint32_t i = 0; i < frames; ++i) {
                // Linear interpolation between the two source samples around the read position.
                const std::int32_t sample =
                    v.current + (((v.following - v.current) * static_cast<std::int32_t>(v.frac)) >> 12);
                v.frac += static_cast<std::uint32_t>(v.pitch);
                while (v.frac >= static_cast<std::uint32_t>(kPitchBase)) {
                    v.frac -= static_cast<std::uint32_t>(kPitchBase);
                    v.current = v.following;
                    v.following = v.vag.next(m);
                }
                const std::int64_t height = v.envelope.height;
                v.envelope.step();
                const auto envelope = static_cast<std::int32_t>((height + (1 << 14)) >> 15);
                const std::int32_t shaped = (sample * envelope + (1 << 14)) >> 15;
                sas.mix[i * 2u] += (shaped * v.left) >> 12;
                sas.mix[i * 2u + 1u] += (shaped * v.right) >> 12;
            }
            if (v.vag.ended && v.current == 0 && v.following == 0) v.envelope.state = EnvState::Off;
            if (v.envelope.state == EnvState::Off) {
                v.playing = false;
                v.on = false;
            }
        }
        std::uint8_t *dst = m.raw_pointer(out, frames * 4u);
        if (dst == nullptr) return;
        for (std::uint32_t i = 0; i < frames * 2u; ++i) {
            std::int32_t value = sas.mix[i];
            if (accumulate) {
                std::int16_t existing;
                std::memcpy(&existing, dst + i * 2u, 2u);
                value = existing + ((value * ((i & 1u) == 0u ? out_left : out_right)) >> 12);
            }
            const auto clamped = static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
            std::memcpy(dst + i * 2u, &clamped, 2u);
        }
    };
    add(0xA3589D81u, "__sceSasCore", [render](Ctx &ctx) {
        render(ctx.gpr[5], false, 0, 0);
        return 0u;
    });
    add(0x50A14DFCu, "__sceSasCoreWithMix", [render](Ctx &ctx) {
        render(ctx.gpr[5], true, static_cast<std::int32_t>(ctx.gpr[6]), static_cast<std::int32_t>(ctx.gpr[7]));
        return 0u;
    });
}

} // namespace pspweb
