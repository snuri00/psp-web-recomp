// sceAtrac3plus for the web profile: ATRAC3+ (and ATRAC3) playback, which
// games use for music and speech.  A game hands the library the start of an
// .at3 file and then keeps a ring buffer topped up from disc.  Every byte it
// adds is also copied into a host-side image of the file here, so decoding,
// looping and seeking never depend on what is still in the ring buffer; the
// free space reported back still follows playback, which paces the streaming
// the way the real library does.
//
// Sample positions follow the conventions PPSSPP documents: the fact chunk's
// second value plus a fixed codec delay is skipped at the start of the stream,
// and loop points from the smpl chunk are relative to that.

#include "kernel.hpp"

#include "at3_decoders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorNoId = 0x80630003u;
constexpr std::uint32_t kErrorBadId = 0x80630005u;
constexpr std::uint32_t kErrorUnknownFormat = 0x80630006u;
constexpr std::uint32_t kErrorBadSample = 0x80630015u;
constexpr std::uint32_t kErrorNoLoopInformation = 0x80630021u;
constexpr std::uint32_t kErrorSecondBufferNotNeeded = 0x80630022u;
constexpr std::uint32_t kErrorBufferIsEmpty = 0x80630023u;
constexpr std::uint32_t kErrorAllDataDecoded = 0x80630024u;
constexpr int kMaxIds = 6;

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

struct Track {
    bool plus{true};
    int channels{2};
    std::uint32_t block_align{};
    std::uint32_t data_offset{}, data_size{}, file_size{};
    std::int32_t end_sample{};    // index of the last sample
    std::int32_t first_offset{};  // stream samples before the track starts
    std::int32_t loop_start{-1}, loop_end{-1};
    std::vector<std::uint8_t> extra; // ATRAC3 codec parameters

    [[nodiscard]] std::int32_t frame_samples() const { return plus ? 2048 : 1024; }
    [[nodiscard]] std::int32_t delay() const { return first_offset + (plus ? 368 : 69); }
    [[nodiscard]] std::uint32_t frames() const { return block_align != 0u ? data_size / block_align : 0u; }
    [[nodiscard]] std::uint32_t frame_of(std::int32_t sample) const {
        return static_cast<std::uint32_t>((sample + delay()) / frame_samples());
    }
    [[nodiscard]] std::uint32_t offset_of(std::uint32_t frame) const { return data_offset + frame * block_align; }
};

// Parses the RIFF/WAVE header of an .at3 file.
bool parse_header(const std::uint8_t *p, std::uint32_t size, Track &t) {
    if (size < 12u || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return false;
    t.file_size = le32(p + 4) + 8u;
    std::uint32_t total = 0u;
    bool have_format = false;
    for (std::uint32_t at = 12u; at + 8u <= size;) {
        const std::uint8_t *chunk = p + at;
        const std::uint32_t length = le32(chunk + 4);
        const std::uint8_t *body = chunk + 8;
        const std::uint32_t available = size - at - 8u;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && available >= 16u) {
            const std::uint32_t tag = body[0] | (body[1] << 8u);
            t.channels = body[2] | (body[3] << 8u);
            t.block_align = body[12] | (body[13] << 8u);
            if (tag == 0xFFFEu) t.plus = true;
            else if (tag == 0x270u) t.plus = false;
            else return false;
            if (!t.plus && length >= 18u && available >= 18u) {
                const std::uint32_t extra = std::min<std::uint32_t>(body[16] | (body[17] << 8u), available - 18u);
                t.extra.assign(body + 18, body + 18 + extra);
            }
            have_format = true;
        } else if (std::memcmp(chunk, "fact", 4) == 0 && available >= 4u) {
            total = le32(body);
            t.first_offset = length >= 8u && available >= 8u ? static_cast<std::int32_t>(le32(body + 4)) : 0;
        } else if (std::memcmp(chunk, "smpl", 4) == 0 && available >= 36u && le32(body + 28) > 0u && available >= 60u) {
            t.loop_start = static_cast<std::int32_t>(le32(body + 36 + 8));
            t.loop_end = static_cast<std::int32_t>(le32(body + 36 + 12));
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            t.data_offset = at + 8u;
            t.data_size = length;
            break;
        }
        at += 8u + length + (length & 1u);
    }
    if (!have_format || t.block_align == 0u || t.data_offset == 0u || t.channels < 1 || t.channels > 2) return false;
    if (total == 0u) total = static_cast<std::uint32_t>(std::max(0, static_cast<std::int32_t>(t.frames()) * t.frame_samples() - t.delay()));
    t.end_sample = static_cast<std::int32_t>(total) - 1;
    if (t.loop_start >= 0) {
        t.loop_start -= t.first_offset;
        t.loop_end = std::min(t.loop_end - t.first_offset, t.end_sample);
        if (t.loop_start < 0 || t.loop_end < t.loop_start) t.loop_start = t.loop_end = -1;
    }
    return true;
}

struct Atrac {
    bool used{};
    Track track;
    std::vector<std::uint8_t> file;  // the file's bytes received so far, from offset 0
    std::uint32_t buffer{}, buffer_size{};
    std::int32_t sample{};           // next sample DecodeData returns
    std::int32_t loop_num{};
    ATRAC3PContext *plus{};
    ATRAC3Context *at3{};
    std::uint32_t frame{~0u};        // frame held in pcm
    std::array<std::vector<float>, 2> pcm;

    void release() {
        if (plus != nullptr) atrac3p_free(plus);
        if (at3 != nullptr) atrac3_free(at3);
        *this = Atrac{};
    }
    // Decodes `index` into pcm.  A frame that does not follow the previous one
    // restarts the decoder and primes it with the frame before.
    bool decode(std::uint32_t index) {
        if (index == frame) return true;
        if (index >= track.frames()) return false;
        const bool sequential = frame != ~0u && index == frame + 1u;
        if (!sequential) {
            if (plus != nullptr) atrac3p_flush_buffers(plus);
            if (at3 != nullptr) atrac3_flush_buffers(at3);
            frame = ~0u;
            if (index > 0u && !run(index - 1u)) return false;
        }
        return run(index);
    }
    bool run(std::uint32_t index) {
        const std::uint32_t offset = track.offset_of(index);
        if (static_cast<std::size_t>(offset) + track.block_align > file.size()) return false;
        for (auto &channel : pcm) channel.assign(static_cast<std::size_t>(track.frame_samples()), 0.0f);
        float *out[2] = {pcm[0].data(), pcm[1].data()};
        int samples = 0;
        const int result = plus != nullptr
            ? atrac3p_decode_frame(plus, out, &samples, file.data() + offset, static_cast<int>(track.block_align))
            : atrac3_decode_frame(at3, out, &samples, file.data() + offset, static_cast<int>(track.block_align));
        if (result < 0) {
            for (auto &channel : pcm) std::fill(channel.begin(), channel.end(), 0.0f);
        }
        frame = index;
        return true;
    }
    // Samples the next DecodeData call returns (0 once the track has ended).
    [[nodiscard]] std::int32_t next_count() const {
        if (sample > track.end_sample) return 0;
        const std::int32_t within = (sample + track.delay()) % track.frame_samples();
        std::int32_t count = std::min(track.frame_samples() - within, track.end_sample + 1 - sample);
        if (loop_num != 0 && track.loop_end >= 0 && sample <= track.loop_end) count = std::min(count, track.loop_end + 1 - sample);
        return count;
    }
    [[nodiscard]] bool all_on_memory() const { return buffer_size >= track.file_size; }
    [[nodiscard]] std::int32_t remain_frames() const {
        if (all_on_memory()) return -1;
        if (file.size() >= track.file_size) return track.loop_start >= 0 && loop_num != 0 ? -3 : -2;
        const std::uint32_t needed = track.offset_of(track.frame_of(std::max(sample, 0)));
        if (file.size() <= needed) return 0;
        return static_cast<std::int32_t>((file.size() - needed) / track.block_align);
    }
    // Where the game should write more of the file into the ring buffer.
    void stream_info(std::uint32_t &write_address, std::uint32_t &writable, std::uint32_t &file_offset) const {
        file_offset = static_cast<std::uint32_t>(file.size());
        if (file.size() >= track.file_size || buffer_size == 0u) {
            write_address = buffer;
            writable = 0u;
            return;
        }
        const std::uint32_t needed = std::min<std::uint32_t>(track.offset_of(track.frame_of(std::max(sample, 0))),
                                                             static_cast<std::uint32_t>(file.size()));
        const std::uint32_t held = static_cast<std::uint32_t>(file.size()) - needed;
        const std::uint32_t free_bytes = held < buffer_size ? buffer_size - held : 0u;
        const std::uint32_t write_offset = static_cast<std::uint32_t>(file.size() % buffer_size);
        write_address = buffer + write_offset;
        writable = std::min({free_bytes, buffer_size - write_offset, track.file_size - static_cast<std::uint32_t>(file.size())});
    }
    void add(const psprecomp::GuestMemory &memory, std::uint32_t bytes) {
        bytes = std::min<std::uint32_t>(bytes, track.file_size - std::min<std::uint32_t>(track.file_size, static_cast<std::uint32_t>(file.size())));
        for (std::uint32_t done = 0; done < bytes;) {
            const std::uint32_t write_offset = static_cast<std::uint32_t>(file.size() % buffer_size);
            const std::uint32_t piece = std::min(bytes - done, buffer_size - write_offset);
            const std::uint8_t *src = memory.raw_pointer(buffer + write_offset, piece);
            if (src == nullptr) return;
            file.insert(file.end(), src, src + piece);
            done += piece;
        }
    }
};

struct AtracState {
    std::array<Atrac, kMaxIds> ids{};
};

AtracState &state(std::shared_ptr<void> &slot) {
    if (!slot) slot = std::make_shared<AtracState>();
    return *static_cast<AtracState *>(slot.get());
}

} // namespace

void Kernel::install_atrac() {
    auto &m = rt_.memory();
    AtracState &st = state(atrac_);
    auto add = [this](std::uint32_t nid, const char *name, std::function<std::uint32_t(Ctx &)> body) {
        static const bool trace = std::getenv("PSPWEB_TRACE_HLE") != nullptr;
        rt_.register_hle("sceAtrac3plus", nid, [body, name](psprecomp::Runtime &, Ctx &ctx) {
            const std::uint32_t a0 = ctx.gpr[4], a1 = ctx.gpr[5], a2 = ctx.gpr[6];
            const std::uint32_t result = body(ctx);
            if (trace) std::fprintf(stderr, "[atrac] %s(0x%X, 0x%X, 0x%X) -> 0x%X\n", name, a0, a1, a2, result);
            finish(ctx, result);
        });
        rt_.nids().add("sceAtrac3plus", nid, name);
        char key[48];
        std::snprintf(key, sizeof key, "sceAtrac3plus:0x%08X", nid);
        registered_.insert(key);
    };
    auto get = [&st](std::uint32_t id) -> Atrac * {
        return id < static_cast<std::uint32_t>(kMaxIds) && st.ids[id].used ? &st.ids[id] : nullptr;
    };
    auto put = [&m](std::uint32_t address, std::uint32_t value) {
        if (address != 0u) m.store32(address, value);
    };

    add(0x7A20E7AFu, "sceAtracSetDataAndGetID", [&st, &m](Ctx &ctx) -> std::uint32_t {
        const std::uint32_t buffer = ctx.gpr[4], size = ctx.gpr[5];
        const std::uint8_t *header = m.raw_pointer(buffer, std::min<std::uint32_t>(size, 4096u));
        Track track;
        if (header == nullptr || !parse_header(header, std::min<std::uint32_t>(size, 4096u), track)) return kErrorUnknownFormat;
        auto slot = std::find_if(st.ids.begin(), st.ids.end(), [](const Atrac &a) { return !a.used; });
        if (slot == st.ids.end()) return kErrorNoId;
        Atrac &a = *slot;
        a = Atrac{};
        a.used = true;
        a.track = track;
        a.buffer = buffer;
        a.buffer_size = size;
        const std::uint32_t loaded = std::min(size, track.file_size);
        const std::uint8_t *data = m.raw_pointer(buffer, loaded);
        if (data != nullptr) a.file.assign(data, data + loaded);
        int align = static_cast<int>(track.block_align);
        if (track.plus) {
            a.plus = atrac3p_alloc(track.channels, &align);
        } else {
            a.at3 = atrac3_alloc(track.channels, &align, track.extra.data(), static_cast<int>(track.extra.size()));
        }
        if (a.plus == nullptr && a.at3 == nullptr) {
            a.release();
            return kErrorUnknownFormat;
        }
        return static_cast<std::uint32_t>(slot - st.ids.begin());
    });
    add(0x5622B7C1u, "sceAtracSetAA3DataAndGetID", [](Ctx &) { return kErrorUnknownFormat; });
    add(0x61EB33F5u, "sceAtracReleaseAtracID", [get](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        a->release();
        return 0u;
    });
    add(0x6A8C3CD5u, "sceAtracDecodeData", [get, put, &m](Ctx &ctx) -> std::uint32_t {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        const std::uint32_t out = ctx.gpr[5];
        const std::int32_t count = a->next_count();
        if (count <= 0) {
            put(ctx.gpr[6], 0u);
            put(ctx.gpr[7], 1u);
            put(ctx.gpr[8], static_cast<std::uint32_t>(a->remain_frames()));
            return kErrorAllDataDecoded;
        }
        const std::int32_t raw = a->sample + a->track.delay();
        if (!a->decode(static_cast<std::uint32_t>(raw / a->track.frame_samples()))) {
            put(ctx.gpr[6], 0u);
            put(ctx.gpr[7], 0u);
            put(ctx.gpr[8], static_cast<std::uint32_t>(a->remain_frames()));
            return kErrorBufferIsEmpty;
        }
        const std::int32_t skip = raw % a->track.frame_samples();
        if (std::uint8_t *dst = out != 0u ? m.raw_pointer(out, static_cast<std::uint32_t>(count) * 4u) : nullptr) {
            const bool stereo = a->track.channels == 2;
            for (std::int32_t i = 0; i < count; ++i) {
                const auto at = static_cast<std::size_t>(skip + i);
                const float left = a->pcm[0][at], right = stereo ? a->pcm[1][at] : left;
                const std::int16_t pair[2] = {
                    static_cast<std::int16_t>(std::clamp(std::lround(left * 32768.0f), -32768l, 32767l)),
                    static_cast<std::int16_t>(std::clamp(std::lround(right * 32768.0f), -32768l, 32767l))};
                std::memcpy(dst + static_cast<std::size_t>(i) * 4u, pair, 4u);
            }
        }
        a->sample += count;
        if (a->loop_num != 0 && a->track.loop_end >= 0 && a->sample > a->track.loop_end) {
            a->sample = a->track.loop_start;
            if (a->loop_num > 0) --a->loop_num;
        }
        put(ctx.gpr[6], static_cast<std::uint32_t>(count));
        put(ctx.gpr[7], a->sample > a->track.end_sample ? 1u : 0u);
        put(ctx.gpr[8], static_cast<std::uint32_t>(a->remain_frames()));
        return 0u;
    });
    add(0x9AE849A7u, "sceAtracGetRemainFrame", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        put(ctx.gpr[5], static_cast<std::uint32_t>(a->remain_frames()));
        return 0u;
    });
    add(0x5D268707u, "sceAtracGetStreamDataInfo", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        std::uint32_t address = 0u, writable = 0u, offset = 0u;
        a->stream_info(address, writable, offset);
        put(ctx.gpr[5], address);
        put(ctx.gpr[6], writable);
        put(ctx.gpr[7], offset);
        return 0u;
    });
    add(0x7DB31251u, "sceAtracAddStreamData", [get, &m](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        a->add(m, ctx.gpr[5]);
        return 0u;
    });
    add(0x83E85EA0u, "sceAtracGetSecondBufferInfo", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        put(ctx.gpr[5], 0u);
        put(ctx.gpr[6], 0u);
        // The whole file is kept on the host side, so a second buffer is never needed.
        return a == nullptr ? kErrorBadId : kErrorSecondBufferNotNeeded;
    });
    add(0x36FAABFBu, "sceAtracGetNextSample", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        put(ctx.gpr[5], static_cast<std::uint32_t>(a->next_count()));
        return 0u;
    });
    add(0xA2BBA8BEu, "sceAtracGetSoundSample", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        put(ctx.gpr[5], static_cast<std::uint32_t>(a->track.end_sample));
        put(ctx.gpr[6], static_cast<std::uint32_t>(a->track.loop_start));
        put(ctx.gpr[7], static_cast<std::uint32_t>(a->track.loop_end));
        return 0u;
    });
    add(0x868120B5u, "sceAtracSetLoopNum", [get](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        if (a->track.loop_start < 0) return kErrorNoLoopInformation;
        a->loop_num = static_cast<std::int32_t>(ctx.gpr[5]);
        return 0u;
    });
    add(0xFAA4F89Bu, "sceAtracGetLoopStatus", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        put(ctx.gpr[5], static_cast<std::uint32_t>(a->loop_num));
        put(ctx.gpr[6], a->track.loop_start >= 0 ? 1u : 0u);
        return 0u;
    });
    add(0x2DD3E298u, "sceAtracGetBufferInfoForResetting", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        const auto sample = static_cast<std::int32_t>(ctx.gpr[5]);
        if (sample < 0 || sample > a->track.end_sample) return kErrorBadSample;
        // Data already received is reused; otherwise continue the file where it stopped.
        std::uint32_t address = 0u, writable = 0u, offset = 0u;
        if (a->track.offset_of(a->track.frame_of(sample)) + a->track.block_align > a->file.size()) {
            a->stream_info(address, writable, offset);
        } else {
            address = a->buffer;
            offset = static_cast<std::uint32_t>(a->file.size());
        }
        const std::uint32_t info = ctx.gpr[6];
        put(info, address);
        put(info + 4u, writable);
        put(info + 8u, 0u);
        put(info + 12u, offset);
        for (std::uint32_t i = 16u; i < 32u; i += 4u) put(info + i, 0u);
        return 0u;
    });
    add(0x644E5607u, "sceAtracResetPlayPosition", [get, &m](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        const auto sample = static_cast<std::int32_t>(ctx.gpr[5]);
        if (sample < 0 || sample > a->track.end_sample) return kErrorBadSample;
        if (ctx.gpr[6] != 0u) a->add(m, ctx.gpr[6]);
        a->sample = sample;
        return 0u;
    });
    add(0xE88F759Bu, "sceAtracGetInternalErrorInfo", [get, put](Ctx &ctx) {
        if (get(ctx.gpr[4]) == nullptr) return kErrorBadId;
        put(ctx.gpr[5], 0u);
        return 0u;
    });
    add(0x31668BAAu, "sceAtracGetChannel", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        put(ctx.gpr[5], static_cast<std::uint32_t>(a->track.channels));
        return 0u;
    });
    add(0xD6A5F2F7u, "sceAtracGetMaxSample", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        put(ctx.gpr[5], static_cast<std::uint32_t>(a->track.frame_samples()));
        return 0u;
    });
    add(0xA554A158u, "sceAtracGetBitrate", [get, put](Ctx &ctx) {
        Atrac *a = get(ctx.gpr[4]);
        if (a == nullptr) return kErrorBadId;
        const std::uint64_t bits_per_second =
            static_cast<std::uint64_t>(a->track.block_align) * 8u * 44100u / static_cast<std::uint64_t>(a->track.frame_samples());
        put(ctx.gpr[5], static_cast<std::uint32_t>((bits_per_second + 500u) / 1000u)); // kbit/s
        return 0u;
    });
    add(0xB3B5D042u, "sceAtracGetOutputChannel", [get, put](Ctx &ctx) {
        if (get(ctx.gpr[4]) == nullptr) return kErrorBadId;
        put(ctx.gpr[5], 2u); // decoded output is always stereo here
        return 0u;
    });
}

} // namespace pspweb
