#pragma once

// Minimal PSP kernel for the web profile: cooperative threads, wait objects,
// the user memory partition, file I/O and the display registers.  Anything a
// title imports that is not implemented here gets a logging stub returning 0,
// so bring-up shows which services a game actually needs.

#include "ge.hpp"
#include "ge_worker.hpp"
#include "webfs.hpp"

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace pspweb {

// Host trampoline a guest thread returns into when its entry function ends.
// Placed in the 16 KiB gap below the module load base so the runtime's direct
// PC table does not grow to cover the kernel partition.
inline constexpr std::uint32_t kThreadReturnAddress = 0x08800100u;
inline constexpr std::uint32_t kUserPartitionStart = 0x08800000u;
inline constexpr std::uint32_t kUserPartitionEnd = 0x0A000000u;
inline constexpr std::uint32_t kVramAddress = 0x04000000u;
inline constexpr std::uint32_t kVramSize = 0x00200000u;

enum class ThreadState { Dormant, Ready, Running, Waiting, Dead };
enum class WaitType { None, Sleep, Delay, Vblank, ThreadEnd, Sema, EventFlag, Fpl, Io, Callback, Ge };

// PSP pad button bits (SceCtrlData::Buttons).
namespace pad {
inline constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u,
    kDown = 0x0040u, kLeft = 0x0080u, kL = 0x0100u, kR = 0x0200u, kTriangle = 0x1000u,
    kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;
}

struct Thread {
    std::int32_t uid{};
    std::string name;
    std::uint32_t entry{};
    std::int32_t priority{};
    std::uint32_t attr{};
    std::uint32_t stack_size{};
    std::uint32_t stack_top{};
    std::int32_t stack_block{-1};
    std::uint32_t gp{};
    ThreadState state{ThreadState::Dormant};
    WaitType wait{WaitType::None};
    std::int32_t wait_uid{};
    std::uint32_t wait_count{};
    std::uint32_t wait_mode{};
    std::uint32_t wait_out_address{};
    std::uint32_t timeout_address{};
    std::uint64_t wake_time_us{};
    std::uint32_t wakeup_count{};
    std::int32_t exit_status{};
    std::uint64_t ready_sequence{};
    psprecomp::AllegrexContext ctx{};
};

struct Semaphore {
    std::string name;
    std::int32_t count{};
    std::int32_t max{};
};

struct EventFlag {
    std::string name;
    std::uint32_t attr{};
    std::uint32_t pattern{};
};

struct MemoryBlock {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
};

struct OpenFile {
    std::FILE *file{};          // native host file
    const WebFile *web{};       // streamed disc file (browser build)
    std::uint64_t position{};   // web and decrypted files track their own position
    std::string path;
    std::int64_t async_result{};
    bool async_pending{};
    bool closing{};
    // Set once a PGD (DRM) file has been decrypted; reads come from here then.
    std::shared_ptr<const std::vector<std::uint8_t>> plain;
    std::uint32_t pgd_offset{}; // where the PGD container starts in the file
};

struct DirectoryEntry {
    std::string name;
    std::uint64_t size{};
    bool directory{};
};

struct OpenDirectory {
    std::vector<DirectoryEntry> entries;
    std::size_t next{};
};

struct FixedPool {
    std::string name;
    std::uint32_t block_size{};
    std::uint32_t base{};
    std::int32_t memory_block{-1};
    std::vector<bool> used;
};

struct AudioChannel {
    bool reserved{};
    std::uint32_t samples{};
    std::uint32_t format{}; // 0 stereo, 0x10 mono
    std::uint64_t busy_until_us{};
    std::uint64_t next_sample{};  // 44.1 kHz sample index where the next buffer starts
};

struct UtilityDialog {
    std::uint32_t status{};   // 0 none, 1 init, 2 visible, 3 quit, 4 finished
    std::uint32_t params{};
};

struct DisplayState {
    std::uint32_t framebuffer{};
    std::uint32_t stride{512};
    std::uint32_t format{3};
    std::uint32_t vcount{};
    std::uint64_t flips{}; // sceDisplaySetFrameBuf calls
    std::uint32_t flip_vcount{~0u}; // vblank the newest buffer change belongs to
};

class Kernel {
public:
    explicit Kernel(psprecomp::Runtime &runtime);

    // Host directories that disc0:/umd0: and ms0: resolve to.
    void set_roots(std::filesystem::path disc_root, std::filesystem::path memstick_root);
    // Marks a guest range (the loaded module image) as allocated.
    void reserve(std::uint32_t address, std::uint32_t size, std::string name);
    // Registers HLE handlers, logging fallbacks for every other import, and the
    // thread-return trampoline.
    void install(const std::vector<psprecomp::PspImport> &imports);
    // Creates and readies the loader thread that runs module_start.
    void boot(std::uint32_t entry, std::uint32_t gp, const std::string &module_path);

    // Runs one host frame: vblank, timers, then guest code for up to budget_ms.
    // Returns false once the guest exited, crashed or has no live threads.
    bool frame(double budget_ms);

    [[nodiscard]] const DisplayState &display() const noexcept { return display_; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] const std::string &halt_reason() const noexcept { return halt_reason_; }
    [[nodiscard]] std::uint64_t now_us() const;
    [[nodiscard]] Ge &ge() noexcept { return *ge_; }
    // Runs display lists (on a GE thread once started) and other GE-side work.
    [[nodiscard]] GeWorker &ge_worker() noexcept { return *ge_worker_; }
    // Total milliseconds the guest has spent waiting for the GE.
    [[nodiscard]] double ge_wait_ms() const noexcept { return ge_wait_ms_; }
    // Serves disc0:/umd0: from a streamed manifest instead of host files.
    bool enable_webfs(const std::string &manifest_path, const std::string &url_prefix);
    // Native builds complete simulated streaming reads here, once per frame.
    void poll_io();
    [[nodiscard]] const WebFs *webfs() const noexcept { return webfs_.get(); }

    // Copies the visible 480x272 framebuffer into `rgba` (480*272*4 bytes).
    void read_framebuffer(std::uint8_t *rgba) const;

    // Entry point of the thread-return trampoline.
    void thread_returned(psprecomp::AllegrexContext &ctx);
    // Prints every thread with its state and what it waits on.
    void dump_threads() const;

    // Host input: PSP button bits and analog stick (128 = centre).
    void set_pad(std::uint32_t buttons, std::uint8_t lx = 128u, std::uint8_t ly = 128u);
    // Receives interleaved stereo 16-bit PCM at 44.1 kHz as the guest outputs it.
    // Receives the mixed 44.1 kHz stereo output once per frame.
    using AudioSink = void (*)(const std::int16_t *samples, std::uint32_t frames);
    void set_audio_sink(AudioSink sink) { audio_sink_ = sink; }
    void savedata_choose(int index);

private:
    using Ctx = psprecomp::AllegrexContext;

    void start_thread(Thread &t, std::uint32_t arglen, const std::vector<std::uint8_t> &args);
    [[nodiscard]] std::uint64_t earliest_timed_wake() const;
    [[nodiscard]] bool waiting_for_ge() const;

    void hle(const char *library, std::uint32_t nid, const char *name,
             void (Kernel::*handler)(Ctx &));
    void install_fallback(const psprecomp::PspImport &import);

    // Scheduling.
    Thread *current();
    Thread *thread(std::int32_t uid);
    void make_ready(Thread &t, bool front = false);
    Thread *pick_ready();
    void load_thread(Thread &t);
    // Saves the running thread (already advanced past its import) and switches.
    void block(Ctx &ctx, WaitType wait, std::uint32_t result);
    void reschedule(Ctx &ctx);
    void maybe_preempt(Ctx &ctx);
    void exit_thread(Ctx &ctx, std::int32_t status, bool remove);
    // Runs guest function `entry` on a temporary thread while the calling
    // thread waits; `done` maps the function's return value to the caller's.
    // Works even when the guest function blocks (for example on streamed I/O).
    void call_guest(Ctx &ctx, std::uint32_t entry, std::initializer_list<std::uint32_t> args,
                    std::function<std::uint32_t(std::uint32_t)> done);
    void wake(Thread &t, std::uint32_t result);
    void poll_waits();
    bool try_satisfy_sema(Thread &t);
    bool try_satisfy_event(Thread &t);
    void begin_timeout(Thread &t, std::uint32_t timeout_address);
    void reset_context(Ctx &ctx) const;

    // Memory.
    std::int32_t allocate(std::string name, std::uint32_t type, std::uint32_t size, std::uint32_t address);
    [[nodiscard]] std::uint32_t largest_free() const;
    [[nodiscard]] std::uint32_t total_free() const;

    // Helpers.
    [[nodiscard]] std::string read_string(std::uint32_t address, std::size_t limit = 256) const;
    [[nodiscard]] std::filesystem::path host_path(const std::string &psp_path) const;
    // Path relative to the disc root, or nullopt-like empty flag for other devices.
    [[nodiscard]] bool disc_relative(const std::string &psp_path, std::string &relative) const;
    [[nodiscard]] bool stat_path(const std::string &psp_path, DirectoryEntry &entry) const;
    void write_stat(std::uint32_t address, const DirectoryEntry &entry);
    void web_read(Ctx &ctx, std::int32_t fd, std::uint32_t buffer, std::uint32_t length, bool async);
    std::uint32_t plain_read(OpenFile &open, std::uint32_t buffer, std::uint32_t length);
    void with_file_contents(Ctx &ctx, std::int32_t fd,
                            std::function<std::uint32_t(OpenFile &, std::span<const std::uint8_t>)> use);
    void finish_async(std::int32_t fd);
    static void finish(Ctx &ctx, std::uint32_t result);
    static void finish64(Ctx &ctx, std::uint64_t result);
    std::int32_t new_uid() { return next_uid_++; }

    // ThreadManForUser.
    void sceKernelCreateThread(Ctx &ctx);
    void sceKernelStartThread(Ctx &ctx);
    void sceKernelExitThread(Ctx &ctx);
    void sceKernelExitDeleteThread(Ctx &ctx);
    void sceKernelDeleteThread(Ctx &ctx);
    void sceKernelTerminateDeleteThread(Ctx &ctx);
    void sceKernelSleepThread(Ctx &ctx);
    void sceKernelWakeupThread(Ctx &ctx);
    void sceKernelDelayThread(Ctx &ctx);
    void sceKernelGetThreadId(Ctx &ctx);
    void sceKernelGetThreadCurrentPriority(Ctx &ctx);
    void sceKernelChangeThreadPriority(Ctx &ctx);
    void sceKernelWaitThreadEnd(Ctx &ctx);
    void sceKernelCreateCallback(Ctx &ctx);
    void sceKernelCheckCallback(Ctx &ctx);
    void sceKernelGetSystemTimeLow(Ctx &ctx);
    void sceKernelGetSystemTimeWide(Ctx &ctx);
    void sceKernelGetSystemTime(Ctx &ctx);
    void sceKernelCreateSema(Ctx &ctx);
    void sceKernelDeleteSema(Ctx &ctx);
    void sceKernelSignalSema(Ctx &ctx);
    void sceKernelWaitSema(Ctx &ctx);
    void sceKernelPollSema(Ctx &ctx);
    void sceKernelCreateEventFlag(Ctx &ctx);
    void sceKernelDeleteEventFlag(Ctx &ctx);
    void sceKernelSetEventFlag(Ctx &ctx);
    void sceKernelClearEventFlag(Ctx &ctx);
    void sceKernelWaitEventFlag(Ctx &ctx);
    void sceKernelPollEventFlag(Ctx &ctx);
    void sceKernelRotateThreadReadyQueue(Ctx &ctx);

    // SysMemUserForUser.
    void sceKernelMaxFreeMemSize(Ctx &ctx);
    void sceKernelTotalFreeMemSize(Ctx &ctx);
    void sceKernelAllocPartitionMemory(Ctx &ctx);
    void sceKernelFreePartitionMemory(Ctx &ctx);
    void sceKernelGetBlockHeadAddr(Ctx &ctx);
    void sceKernelDevkitVersion(Ctx &ctx);
    void sceKernelPrintf(Ctx &ctx);

    // LoadExecForUser / ModuleMgrForUser.
    void sceKernelExitGame(Ctx &ctx);
    void sceKernelSelfStopUnloadModule(Ctx &ctx);
    void sceKernelLoadModule(Ctx &ctx);
    void sceKernelStartModule(Ctx &ctx);

    // StdioForUser.
    void sceKernelStdin(Ctx &ctx);
    void sceKernelStdout(Ctx &ctx);
    void sceKernelStderr(Ctx &ctx);

    // IoFileMgrForUser.
    void sceIoOpen(Ctx &ctx);
    void sceIoClose(Ctx &ctx);
    void sceIoRead(Ctx &ctx);
    void sceIoWrite(Ctx &ctx);
    void sceIoLseek(Ctx &ctx);
    void sceIoLseek32(Ctx &ctx);
    void sceIoGetstat(Ctx &ctx);
    void sceIoChdir(Ctx &ctx);
    void sceIoDopen(Ctx &ctx);
    void sceIoDread(Ctx &ctx);
    void sceIoDclose(Ctx &ctx);
    void sceIoOpenAsync(Ctx &ctx);
    void sceIoReadAsync(Ctx &ctx);
    void sceIoLseekAsync(Ctx &ctx);
    void sceIoCloseAsync(Ctx &ctx);
    void sceIoWaitAsync(Ctx &ctx);
    void sceIoPollAsync(Ctx &ctx);
    void sceIoIoctl(Ctx &ctx);

    // sceDisplay / sceGe_user.
    void sceDisplaySetMode(Ctx &ctx);
    void sceDisplaySetFrameBuf(Ctx &ctx);
    void sceDisplayGetFrameBuf(Ctx &ctx);
    void sceDisplayWaitVblankStart(Ctx &ctx);
    void sceDisplayGetVcount(Ctx &ctx);
    void sceGeEdramGetAddr(Ctx &ctx);
    void sceGeEdramGetSize(Ctx &ctx);
    void sceGeListEnQueue(Ctx &ctx);
    void sceGeListUpdateStallAddr(Ctx &ctx);
    void sceGeGetCmd(Ctx &ctx);
    void sceGeListSync(Ctx &ctx);
    void sceGeDrawSync(Ctx &ctx);

    // UtilsForUser / Kernel_Library.
    void sceKernelLibcTime(Ctx &ctx);
    void sceKernelLibcGettimeofday(Ctx &ctx);
    void sceKernelCpuSuspendIntr(Ctx &ctx);
    void sceKernelMemcpy(Ctx &ctx);
    void sceKernelMemset(Ctx &ctx);
    void return_zero(Ctx &ctx);
    void return_one(Ctx &ctx);

    // devices.cpp: controller, audio, UMD, utility dialogs, misc kernel queries.
    void install_devices();
    void audio_output(Ctx &ctx, AudioChannel &channel, std::uint32_t buffer, std::uint32_t lvol, std::uint32_t rvol);
    void dialog_get_status(Ctx &ctx, UtilityDialog &dialog);
    void sceCtrlPeekBufferPositive(Ctx &ctx);
    void sceAudioChReserve(Ctx &ctx);
    void sceAudioChRelease(Ctx &ctx);
    void sceAudioOutputBlocking(Ctx &ctx);
    void sceAudioOutputPannedBlocking(Ctx &ctx);
    void sceAudioSetChannelDataLen(Ctx &ctx);
    void sceAudioGetChannelRestLength(Ctx &ctx);
    void sceAudioOutput2Reserve(Ctx &ctx);
    void sceAudioOutput2OutputBlocking(Ctx &ctx);
    void sceAudioOutput2ChangeLength(Ctx &ctx);
    void sceUmdCheckMedium(Ctx &ctx);
    void sceImposeGetLanguageMode(Ctx &ctx);
    void sceDisplayGetFramePerSec(Ctx &ctx);
    void sceUtilityMsgDialogInitStart(Ctx &ctx);
    void sceUtilityMsgDialogGetStatus(Ctx &ctx);
    void sceUtilityMsgDialogShutdownStart(Ctx &ctx);
    void sceKernelCreateFpl(Ctx &ctx);
    void sceKernelDeleteFpl(Ctx &ctx);
    void sceKernelAllocateFpl(Ctx &ctx);
    void sceKernelFreeFpl(Ctx &ctx);
    void sceKernelReferThreadStatus(Ctx &ctx);
    void sceKernelReferEventFlagStatus(Ctx &ctx);
    void sceKernelLibcClock(Ctx &ctx);
    void sceRtcGetCurrentClockLocalTime(Ctx &ctx);
    void sceKernelVolatileMemLock(Ctx &ctx);

    psprecomp::Runtime &rt_;
    std::unique_ptr<Ge> ge_;
    std::unique_ptr<GeWorker> ge_worker_;
    double ge_wait_ms_{};
    std::unique_ptr<WebFs> webfs_;
    std::map<std::int32_t, Thread> threads_;
    std::map<std::int32_t, Semaphore> semaphores_;
    std::map<std::int32_t, EventFlag> event_flags_;
    std::map<std::int32_t, MemoryBlock> blocks_;
    std::map<std::int32_t, OpenFile> files_;
    std::map<std::int32_t, OpenDirectory> directories_;
    std::set<std::int32_t> modules_;
    std::map<std::int32_t, FixedPool> fpls_;
    struct GuestCall {
        std::int32_t caller{};
        std::function<std::uint32_t(std::uint32_t)> done;
    };
    std::map<std::int32_t, GuestCall> guest_calls_;   // callback thread uid -> waiting caller

    // mpeg.cpp: sceMpeg (movies are consumed without decoding for now).
    void install_mpeg();
    void install_sas();      // sas.cpp: the SAS voice synthesizer (sound effects)
    // Mixes a buffer that starts playing at `start_sample` into the output.
    void mix_audio(std::uint64_t start_sample, const std::int16_t *stereo, std::uint32_t frames,
                   std::int32_t left_volume, std::int32_t right_volume);
    void drain_audio(std::uint64_t until_us);
    std::vector<std::int32_t> mix_ring_;    // stereo, indexed by sample % ring size
    std::uint64_t mix_read_{};              // next sample handed to the sink
    std::vector<std::int16_t> mix_out_;
    std::shared_ptr<void> sas_;             // sas.cpp state
    void install_atrac();    // atrac.cpp: ATRAC3/ATRAC3+ streams (music, speech)
    void install_savedata();
    std::shared_ptr<void> savedata_;
    std::shared_ptr<void> atrac_;           // atrac.cpp state
    std::array<AudioChannel, 8> audio_channels_{};
    AudioChannel audio_output2_{};
    AudioSink audio_sink_{};
    UtilityDialog msg_dialog_{};
    std::uint32_t pad_buttons_{};
    std::uint8_t pad_lx_{128u};
    std::uint8_t pad_ly_{128u};
    std::int32_t current_uid_{-1};
    std::int32_t next_uid_{0x100};
    std::int32_t next_fd_{3};
    std::uint64_t ready_sequence_{};
    // Guest time is frame-based: each host frame is one vblank period, real
    // time advances it within the frame, and an idle guest warps forward to its
    // next timer instead of stalling the browser.
    std::uint64_t frame_start_guest_us_{};
    std::uint64_t warp_us_{};
    std::int64_t frame_real_start_ns_{};
    DisplayState display_{};
    bool throttle_flips_{std::getenv("PSPWEB_NO_FLIP_THROTTLE") == nullptr};
    std::filesystem::path disc_root_{"."};
    std::filesystem::path memstick_root_{"."};
    std::string cwd_{"disc0:/"};
    std::set<std::string> registered_;
    std::set<std::string> fallback_logged_;
    bool halted_{};
    std::string halt_reason_;
};

} // namespace pspweb
