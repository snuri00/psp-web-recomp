// Device-side HLE for the web profile: controller, audio channels, UMD,
// system utility dialogs and the remaining kernel queries titles poll during
// boot.  Dialogs complete immediately (no on-screen UI yet).

#include "kernel.hpp"

#include <cstring>

#include <algorithm>
#include <bit>
#include <ctime>
#include <iostream>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorNoMemory = 0x80020190u;
constexpr std::uint32_t kErrorUnknownUid = 0x800200CBu;
constexpr std::uint32_t kErrorUnknownThread = 0x80020198u;
constexpr std::uint32_t kErrorUnknownEventFlag = 0x8002019Au;
constexpr std::uint32_t kErrorAudioChannelNotReserved = 0x80260008u;
constexpr std::uint32_t kErrorAudioNoChannel = 0x80260002u;
constexpr std::uint32_t kAudioRate = 44100u;

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

void Kernel::set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly) {
    pad_buttons_ = buttons;
    pad_lx_ = lx;
    pad_ly_ = ly;
}

void Kernel::return_one(Ctx &ctx) { finish(ctx, 1u); }

void Kernel::install_devices() {
    const char *ctrl = "sceCtrl";
    hle(ctrl, 0x3A622550u, "sceCtrlPeekBufferPositive", &Kernel::sceCtrlPeekBufferPositive);
    hle(ctrl, 0x1F803938u, "sceCtrlReadBufferPositive", &Kernel::sceCtrlPeekBufferPositive);
    hle(ctrl, 0x1F4011E6u, "sceCtrlSetSamplingMode", &Kernel::return_zero);
    hle(ctrl, 0x6A2774F3u, "sceCtrlSetSamplingCycle", &Kernel::return_zero);
    hle(ctrl, 0xA7144800u, "sceCtrlSetIdleCancelThreshold", &Kernel::return_zero);

    const char *audio = "sceAudio";
    hle(audio, 0x5EC81C55u, "sceAudioChReserve", &Kernel::sceAudioChReserve);
    hle(audio, 0x6FC46853u, "sceAudioChRelease", &Kernel::sceAudioChRelease);
    hle(audio, 0x136CAF51u, "sceAudioOutputBlocking", &Kernel::sceAudioOutputBlocking);
    hle(audio, 0x13F592BCu, "sceAudioOutputPannedBlocking", &Kernel::sceAudioOutputPannedBlocking);
    hle(audio, 0xCB2E439Eu, "sceAudioSetChannelDataLen", &Kernel::sceAudioSetChannelDataLen);
    hle(audio, 0xB011922Fu, "sceAudioGetChannelRestLength", &Kernel::sceAudioGetChannelRestLength);
    hle(audio, 0x95FD0C2Du, "sceAudioChangeChannelConfig", &Kernel::return_zero);
    hle(audio, 0xB7E1D8E7u, "sceAudioChangeChannelVolume", &Kernel::return_zero);
    hle(audio, 0x01562BA3u, "sceAudioOutput2Reserve", &Kernel::sceAudioOutput2Reserve);
    hle(audio, 0x2D53F36Eu, "sceAudioOutput2OutputBlocking", &Kernel::sceAudioOutput2OutputBlocking);
    hle(audio, 0x43196845u, "sceAudioOutput2Release", &Kernel::return_zero);
    hle(audio, 0x63F2889Cu, "sceAudioOutput2ChangeLength", &Kernel::sceAudioOutput2ChangeLength);

    hle("sceUmdUser", 0x46EBB729u, "sceUmdCheckMedium", &Kernel::sceUmdCheckMedium);
    hle("sceUmdUser", 0xC6183D47u, "sceUmdActivate", &Kernel::return_zero);
    hle("sceUmdUser", 0x56202973u, "sceUmdWaitDriveStatWithTimer", &Kernel::return_zero);
    hle("sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat", &Kernel::return_zero);
    hle("sceImpose", 0x24FD7BCFu, "sceImposeGetLanguageMode", &Kernel::sceImposeGetLanguageMode);
    hle("sceImpose", 0x36AA6E91u, "sceImposeSetLanguageMode", &Kernel::return_zero);
    hle("sceDisplay", 0xDBA6C4C4u, "sceDisplayGetFramePerSec", &Kernel::sceDisplayGetFramePerSec);
    hle("sceRtc", 0xE7C27D1Bu, "sceRtcGetCurrentClockLocalTime", &Kernel::sceRtcGetCurrentClockLocalTime);
    hle("UtilsForUser", 0x91E4F6A7u, "sceKernelLibcClock", &Kernel::sceKernelLibcClock);
    hle("Kernel_Library", 0xB55249D2u, "sceKernelIsCpuIntrEnable", &Kernel::return_one);

    const char *util = "sceUtility";
    hle(util, 0x2AD8E239u, "sceUtilityMsgDialogInitStart", &Kernel::sceUtilityMsgDialogInitStart);
    hle(util, 0x9A1C91D7u, "sceUtilityMsgDialogGetStatus", &Kernel::sceUtilityMsgDialogGetStatus);
    hle(util, 0x95FC253Bu, "sceUtilityMsgDialogUpdate", &Kernel::return_zero);
    hle(util, 0x67AF3428u, "sceUtilityMsgDialogShutdownStart", &Kernel::sceUtilityMsgDialogShutdownStart);

    const char *tm = "ThreadManForUser";
    hle(tm, 0xC07BB470u, "sceKernelCreateFpl", &Kernel::sceKernelCreateFpl);
    hle(tm, 0xED1410E0u, "sceKernelDeleteFpl", &Kernel::sceKernelDeleteFpl);
    hle(tm, 0xD979E9BFu, "sceKernelAllocateFpl", &Kernel::sceKernelAllocateFpl);
    hle(tm, 0xF6414A71u, "sceKernelFreeFpl", &Kernel::sceKernelFreeFpl);
    hle(tm, 0x17C1684Eu, "sceKernelReferThreadStatus", &Kernel::sceKernelReferThreadStatus);
    hle(tm, 0xA66B0120u, "sceKernelReferEventFlagStatus", &Kernel::sceKernelReferEventFlagStatus);
    hle(tm, 0x3AD58B8Cu, "sceKernelSuspendDispatchThread", &Kernel::return_one);
    hle(tm, 0x27E22EC2u, "sceKernelResumeDispatchThread", &Kernel::return_zero);
    hle(tm, 0xEDBA5844u, "sceKernelDeleteCallback", &Kernel::return_zero);

    const char *suspend = "sceSuspendForUser";
    hle(suspend, 0x3E0271D3u, "sceKernelVolatileMemLock", &Kernel::sceKernelVolatileMemLock);
    hle(suspend, 0xA14F40B2u, "sceKernelVolatileMemTryLock", &Kernel::sceKernelVolatileMemLock);
    hle(suspend, 0xA569E425u, "sceKernelVolatileMemUnlock", &Kernel::return_zero);
    hle(suspend, 0xEADB1BD7u, "sceKernelPowerLock", &Kernel::return_zero);
    hle(suspend, 0x3AEE7261u, "sceKernelPowerUnlock", &Kernel::return_zero);
    hle(suspend, 0x090CCB3Fu, "sceKernelPowerTick", &Kernel::return_zero);
    hle("SysMemUserForUser", 0x342061E5u, "sceKernelSetCompiledSdkVersion370", &Kernel::return_zero);
    hle("SysMemUserForUser", 0x1B4217BCu, "sceKernelSetCompiledSdkVersion603_605", &Kernel::return_zero);
    hle("scePower", 0xEBD177D6u, "scePowerSetClockFrequency", &Kernel::return_zero);
    hle("scePower", 0x469989ADu, "scePowerSetClockFrequency2", &Kernel::return_zero);
    // Firmware modules are all high-level emulated, so loading one is a no-op.
    hle("sceUtility", 0x2A2B3DE0u, "sceUtilityLoadModule", &Kernel::return_zero);
    hle("sceUtility", 0xE49BFE92u, "sceUtilityUnloadModule", &Kernel::return_zero);
    // Keys for downloadable content; disc files use sceIoIoctl instead.
    hle("scePspNpDrm_user", 0xA1336091u, "sceNpDrmSetLicenseeKey", &Kernel::return_zero);
    hle("scePspNpDrm_user", 0x08D98894u, "sceNpDrmEdataSetupKey", &Kernel::return_zero);
    hle("scePower", 0x04B7766Eu, "scePowerRegisterCallback", &Kernel::return_zero);
    hle("scePower", 0xDFA8BAF8u, "scePowerUnregisterCallback", &Kernel::return_zero);
}

// The 4 MiB "volatile" block in the kernel partition, lent to games that ask.
void Kernel::sceKernelVolatileMemLock(Ctx &ctx) {
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], 0x08400000u);
    if (ctx.gpr[6] != 0u) rt_.memory().store32(ctx.gpr[6], 0x00400000u);
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// sceCtrl

void Kernel::sceCtrlPeekBufferPositive(Ctx &ctx) {
    const std::uint32_t data = ctx.gpr[4];
    const std::uint32_t count = std::max(ctx.gpr[5], 1u);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = data + i * 16u;
        rt_.memory().store32(entry + 0u, static_cast<std::uint32_t>(now_us()));
        rt_.memory().store32(entry + 4u, pad_buttons_);
        rt_.memory().store8(entry + 8u, pad_lx_);
        rt_.memory().store8(entry + 9u, pad_ly_);
        for (std::uint32_t j = 10u; j < 16u; ++j) rt_.memory().store8(entry + j, 0u);
    }
    finish(ctx, count);
}

// ---------------------------------------------------------------------------
// sceAudio.  Blocking output waits until the previous buffer of the channel
// has played, which paces audio threads at the real 44.1 kHz rate.

void Kernel::audio_output(Ctx &ctx, AudioChannel &channel, std::uint32_t buffer,
                          std::uint32_t lvol, std::uint32_t rvol) {
    const std::uint32_t frames = channel.samples;
    // A buffer starts when the channel's previous one ends, or now if it is idle.
    const std::uint64_t now = now_us();
    const std::uint64_t now_sample = now * kAudioRate / 1000000u;
    const std::uint64_t start = std::max(now_sample, channel.next_sample);
    channel.next_sample = start + frames;
    channel.busy_until_us = channel.next_sample * 1000000u / kAudioRate;
    if (frames != 0u && buffer != 0u) {
        const bool mono = channel.format == 0x10u;
        const std::uint8_t *raw = rt_.memory().raw_pointer(buffer, frames * (mono ? 2u : 4u));
        if (raw != nullptr) {
            mix_out_.resize(static_cast<std::size_t>(frames) * 2u);
            for (std::uint32_t i = 0; i < frames; ++i) {
                std::int16_t left, right;
                std::memcpy(&left, raw + (mono ? i * 2u : i * 4u), 2u);
                if (mono) right = left;
                else std::memcpy(&right, raw + i * 4u + 2u, 2u);
                mix_out_[i * 2u] = left;
                mix_out_[i * 2u + 1u] = right;
            }
            mix_audio(start, mix_out_.data(), frames, static_cast<std::int32_t>(lvol & 0xFFFFu),
                      static_cast<std::int32_t>(rvol & 0xFFFFu));
        }
    }
    // Blocking output returns once the previous buffer has finished playing.
    const std::uint64_t start_us = start * 1000000u / kAudioRate;
    Thread *self = current();
    if (start_us <= now || self == nullptr) {
        finish(ctx, frames);
        return;
    }
    self->wake_time_us = start_us;
    block(ctx, WaitType::Delay, frames);
}

namespace {
constexpr std::uint64_t kMixRingFrames = 1u << 17; // ~3 s
} // namespace

void Kernel::mix_audio(std::uint64_t start_sample, const std::int16_t *stereo, std::uint32_t frames,
                       std::int32_t left_volume, std::int32_t right_volume) {
    if (mix_ring_.empty()) mix_ring_.assign(kMixRingFrames * 2u, 0);
    if (mix_read_ == 0u) mix_read_ = start_sample;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint64_t at = start_sample + i;
        if (at < mix_read_ || at >= mix_read_ + kMixRingFrames) continue; // already played, or too far ahead
        const std::size_t slot = static_cast<std::size_t>(at % kMixRingFrames) * 2u;
        mix_ring_[slot] += stereo[i * 2u] * left_volume / 0x8000;
        mix_ring_[slot + 1u] += stereo[i * 2u + 1u] * right_volume / 0x8000;
    }
}

void Kernel::drain_audio(std::uint64_t until_us) {
    const std::uint64_t until = until_us * kAudioRate / 1000000u;
    if (mix_ring_.empty() || mix_read_ == 0u || until <= mix_read_) return;
    const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(until - mix_read_, kMixRingFrames));
    mix_out_.resize(static_cast<std::size_t>(frames) * 2u);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::size_t slot = static_cast<std::size_t>((mix_read_ + i) % kMixRingFrames) * 2u;
        mix_out_[i * 2u] = static_cast<std::int16_t>(std::clamp(mix_ring_[slot], -32768, 32767));
        mix_out_[i * 2u + 1u] = static_cast<std::int16_t>(std::clamp(mix_ring_[slot + 1u], -32768, 32767));
        mix_ring_[slot] = 0;
        mix_ring_[slot + 1u] = 0;
    }
    mix_read_ = until;
    if (audio_sink_ != nullptr) audio_sink_(mix_out_.data(), frames);
}

void Kernel::sceAudioChReserve(Ctx &ctx) {
    std::int32_t channel = static_cast<std::int32_t>(ctx.gpr[4]);
    if (channel < 0) {
        for (std::size_t i = 0; i < audio_channels_.size(); ++i)
            if (!audio_channels_[i].reserved) {
                channel = static_cast<std::int32_t>(i);
                break;
            }
    }
    if (channel < 0 || channel >= static_cast<std::int32_t>(audio_channels_.size()) ||
        audio_channels_[static_cast<std::size_t>(channel)].reserved) {
        finish(ctx, kErrorAudioNoChannel);
        return;
    }
    auto &c = audio_channels_[static_cast<std::size_t>(channel)];
    c = AudioChannel{true, ctx.gpr[5], ctx.gpr[6], 0u};
    finish(ctx, static_cast<std::uint32_t>(channel));
}

void Kernel::sceAudioChRelease(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel < audio_channels_.size()) audio_channels_[channel].reserved = false;
    finish(ctx, 0u);
}

void Kernel::sceAudioOutputBlocking(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size() || !audio_channels_[channel].reserved) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    audio_output(ctx, audio_channels_[channel], ctx.gpr[6], ctx.gpr[5], ctx.gpr[5]);
}

void Kernel::sceAudioOutputPannedBlocking(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size() || !audio_channels_[channel].reserved) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    audio_output(ctx, audio_channels_[channel], ctx.gpr[7], ctx.gpr[5], ctx.gpr[6]);
}

void Kernel::sceAudioSetChannelDataLen(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel < audio_channels_.size()) audio_channels_[channel].samples = ctx.gpr[5];
    finish(ctx, 0u);
}

void Kernel::sceAudioGetChannelRestLength(Ctx &ctx) {
    const std::uint32_t channel = ctx.gpr[4];
    if (channel >= audio_channels_.size()) {
        finish(ctx, kErrorAudioChannelNotReserved);
        return;
    }
    const std::uint64_t now_sample = now_us() * kAudioRate / 1000000u, next = audio_channels_[channel].next_sample;
    finish(ctx, next > now_sample ? static_cast<std::uint32_t>(next - now_sample) : 0u);
}

void Kernel::sceAudioOutput2Reserve(Ctx &ctx) {
    audio_output2_ = AudioChannel{true, ctx.gpr[4], 0u, 0u, 0u};
    finish(ctx, 0u);
}

void Kernel::sceAudioOutput2OutputBlocking(Ctx &ctx) {
    audio_output(ctx, audio_output2_, ctx.gpr[5], ctx.gpr[4], ctx.gpr[4]);
}

void Kernel::sceAudioOutput2ChangeLength(Ctx &ctx) {
    audio_output2_.samples = ctx.gpr[4];
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// UMD, impose, display, clocks

void Kernel::sceUmdCheckMedium(Ctx &ctx) { finish(ctx, 1u); }

void Kernel::sceImposeGetLanguageMode(Ctx &ctx) {
    if (ctx.gpr[4] != 0u) rt_.memory().store32(ctx.gpr[4], 1u); // English
    if (ctx.gpr[5] != 0u) rt_.memory().store32(ctx.gpr[5], 1u); // Cross confirms
    finish(ctx, 0u);
}

void Kernel::sceDisplayGetFramePerSec(Ctx &ctx) {
    ctx.fpr[0] = 59.9400599f;
    finish(ctx, std::bit_cast<std::uint32_t>(ctx.fpr[0]));
}

void Kernel::sceKernelLibcClock(Ctx &ctx) { finish(ctx, static_cast<std::uint32_t>(now_us())); }

void Kernel::sceRtcGetCurrentClockLocalTime(Ctx &ctx) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const std::uint32_t out = ctx.gpr[4];
    if (out != 0u) {
        rt_.memory().store16(out + 0u, static_cast<std::uint16_t>(local.tm_year + 1900));
        rt_.memory().store16(out + 2u, static_cast<std::uint16_t>(local.tm_mon + 1));
        rt_.memory().store16(out + 4u, static_cast<std::uint16_t>(local.tm_mday));
        rt_.memory().store16(out + 6u, static_cast<std::uint16_t>(local.tm_hour));
        rt_.memory().store16(out + 8u, static_cast<std::uint16_t>(local.tm_min));
        rt_.memory().store16(out + 10u, static_cast<std::uint16_t>(local.tm_sec));
        rt_.memory().store32(out + 12u, 0u);
    }
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// sceUtility dialogs: report success without UI.

void Kernel::dialog_get_status(Ctx &ctx, UtilityDialog &dialog) {
    const std::uint32_t status = dialog.status;
    if (dialog.status == 1u) dialog.status = 2u;
    else if (dialog.status == 2u) dialog.status = 3u;
    else if (dialog.status == 4u) dialog.status = 0u;
    finish(ctx, status);
}

void Kernel::sceUtilityMsgDialogInitStart(Ctx &ctx) {
    const std::uint32_t params = ctx.gpr[4];
    std::cerr << "[utility] message dialog: \"" << read_string(params + 0x3Cu, 512) << "\"\n";
    rt_.memory().store32(params + 0x1Cu, 0u);   // result
    rt_.memory().store32(params + 0x240u, 1u);  // buttonPressed = YES
    msg_dialog_ = UtilityDialog{1u, params};
    finish(ctx, 0u);
}

void Kernel::sceUtilityMsgDialogGetStatus(Ctx &ctx) { dialog_get_status(ctx, msg_dialog_); }

void Kernel::sceUtilityMsgDialogShutdownStart(Ctx &ctx) {
    msg_dialog_.status = 4u;
    finish(ctx, 0u);
}

// ---------------------------------------------------------------------------
// Fixed-size memory pools

void Kernel::sceKernelCreateFpl(Ctx &ctx) {
    const std::uint32_t block_size = align_up(std::max(ctx.gpr[7], 1u), 4u);
    const std::uint32_t count = ctx.gpr[8];
    FixedPool pool;
    pool.name = read_string(ctx.gpr[4], 32);
    pool.block_size = block_size;
    pool.memory_block = allocate("fpl:" + pool.name, (ctx.gpr[6] & 0x4000u) != 0u ? 1u : 0u, block_size * count, 0u);
    if (pool.memory_block < 0) {
        finish(ctx, kErrorNoMemory);
        return;
    }
    pool.base = blocks_.at(pool.memory_block).address;
    pool.used.assign(count, false);
    const std::int32_t uid = new_uid();
    fpls_[uid] = std::move(pool);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

void Kernel::sceKernelDeleteFpl(Ctx &ctx) {
    const auto it = fpls_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == fpls_.end()) {
        finish(ctx, kErrorUnknownUid);
        return;
    }
    blocks_.erase(it->second.memory_block);
    fpls_.erase(it);
    finish(ctx, 0u);
}

void Kernel::sceKernelAllocateFpl(Ctx &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) {
        finish(ctx, kErrorUnknownUid);
        return;
    }
    auto &pool = it->second;
    for (std::size_t i = 0; i < pool.used.size(); ++i) {
        if (pool.used[i]) continue;
        pool.used[i] = true;
        rt_.memory().store32(ctx.gpr[5], pool.base + static_cast<std::uint32_t>(i) * pool.block_size);
        finish(ctx, 0u);
        return;
    }
    Thread *self = current();
    if (self != nullptr) {
        self->wait_uid = uid;
        self->wait_out_address = ctx.gpr[5];
        begin_timeout(*self, ctx.gpr[6]);
    }
    block(ctx, WaitType::Fpl, 0u);
}

void Kernel::sceKernelFreeFpl(Ctx &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = fpls_.find(uid);
    if (it == fpls_.end()) {
        finish(ctx, kErrorUnknownUid);
        return;
    }
    auto &pool = it->second;
    const std::uint32_t index = (ctx.gpr[5] - pool.base) / pool.block_size;
    if (ctx.gpr[5] < pool.base || index >= pool.used.size()) {
        finish(ctx, kErrorUnknownUid);
        return;
    }
    pool.used[index] = false;
    for (auto &[tid, t] : threads_) {
        (void)tid;
        if (t.state != ThreadState::Waiting || t.wait != WaitType::Fpl || t.wait_uid != uid) continue;
        pool.used[index] = true;
        rt_.memory().store32(t.wait_out_address, ctx.gpr[5]);
        wake(t, 0u);
        break;
    }
    finish(ctx, 0u);
    maybe_preempt(ctx);
}

// ---------------------------------------------------------------------------
// Status queries

void Kernel::sceKernelReferThreadStatus(Ctx &ctx) {
    Thread *t = thread(static_cast<std::int32_t>(ctx.gpr[4]));
    if (t == nullptr) {
        finish(ctx, kErrorUnknownThread);
        return;
    }
    const std::uint32_t info = ctx.gpr[5];
    for (std::uint32_t i = 4u; i < 0x68u; i += 4u) rt_.memory().store32(info + i, 0u);
    for (std::size_t i = 0; i < 32u; ++i)
        rt_.memory().store8(info + 4u + static_cast<std::uint32_t>(i),
                            i < t->name.size() ? static_cast<std::uint8_t>(t->name[i]) : 0u);
    std::uint32_t status = 0x10u; // dormant
    if (t->state == ThreadState::Running) status = 0x01u;
    else if (t->state == ThreadState::Ready) status = 0x02u;
    else if (t->state == ThreadState::Waiting) status = 0x04u;
    else if (t->state == ThreadState::Dead) status = 0x20u;
    rt_.memory().store32(info + 0x24u, t->attr);
    rt_.memory().store32(info + 0x28u, status);
    rt_.memory().store32(info + 0x2Cu, t->entry);
    rt_.memory().store32(info + 0x30u, t->stack_top - t->stack_size);
    rt_.memory().store32(info + 0x34u, t->stack_size);
    rt_.memory().store32(info + 0x38u, t->gp);
    rt_.memory().store32(info + 0x3Cu, static_cast<std::uint32_t>(t->priority));
    rt_.memory().store32(info + 0x40u, static_cast<std::uint32_t>(t->priority));
    // PSP wait types: 1 sleep, 2 delay, 3 sema, 4 event flag, 7 fpl, 9 thread end.
    std::uint32_t wait_type = 0u;
    if (t->state == ThreadState::Waiting) {
        switch (t->wait) {
        case WaitType::Sleep: wait_type = 1u; break;
        case WaitType::Delay: case WaitType::Vblank: case WaitType::Io: wait_type = 2u; break;
        case WaitType::Sema: wait_type = 3u; break;
        case WaitType::EventFlag: wait_type = 4u; break;
        case WaitType::Fpl: wait_type = 7u; break;
        case WaitType::ThreadEnd: wait_type = 9u; break;
        default: break;
        }
    }
    const bool waits_on_object = wait_type == 3u || wait_type == 4u || wait_type == 7u || wait_type == 9u;
    rt_.memory().store32(info + 0x44u, wait_type);
    rt_.memory().store32(info + 0x48u, waits_on_object ? static_cast<std::uint32_t>(t->wait_uid) : 0u);
    rt_.memory().store32(info + 0x4Cu, t->wakeup_count);
    rt_.memory().store32(info + 0x50u, static_cast<std::uint32_t>(t->exit_status));
    finish(ctx, 0u);
}

void Kernel::sceKernelReferEventFlagStatus(Ctx &ctx) {
    const auto it = event_flags_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == event_flags_.end()) {
        finish(ctx, kErrorUnknownEventFlag);
        return;
    }
    const std::uint32_t info = ctx.gpr[5];
    for (std::size_t i = 0; i < 32u; ++i)
        rt_.memory().store8(info + 4u + static_cast<std::uint32_t>(i),
                            i < it->second.name.size() ? static_cast<std::uint8_t>(it->second.name[i]) : 0u);
    rt_.memory().store32(info + 0x24u, it->second.attr);
    rt_.memory().store32(info + 0x28u, it->second.pattern);
    rt_.memory().store32(info + 0x2Cu, it->second.pattern);
    std::uint32_t waiting = 0u;
    for (const auto &[tid, t] : threads_) {
        (void)tid;
        if (t.state == ThreadState::Waiting && t.wait == WaitType::EventFlag && t.wait_uid == it->first) ++waiting;
    }
    rt_.memory().store32(info + 0x30u, waiting);
    finish(ctx, 0u);
}

} // namespace pspweb
