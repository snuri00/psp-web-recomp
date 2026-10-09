// IoFileMgrForUser for the web profile.  Disc paths are served either from
// host files (native) or from the streamed WebFs; a streamed read that misses
// the cache parks the calling thread until its chunks arrive.

#include "kernel.hpp"
#include "pgd.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace pspweb {
namespace {

constexpr std::uint32_t kErrorFileNotFound = 0x80010002u;
constexpr std::uint32_t kErrorBadFile = 0x80020323u;
constexpr std::uint32_t kErrorIo = 0x80010005u;
constexpr std::uint32_t kErrorPgdInvalidHeader = 0x80510204u;
constexpr std::uint32_t kErrorTooManyFiles = 0x80020320u;
constexpr std::int32_t kMaxFds = 64;
constexpr std::uint64_t kMaxWholeFileRead = 256u << 20u;

#if !defined(__EMSCRIPTEN__)
const char *fopen_mode(std::uint32_t flags) {
    const bool write = (flags & 0x2u) != 0u;
    const bool read = (flags & 0x1u) != 0u;
    if (!write) return "rb";
    if ((flags & 0x100u) != 0u) return read ? "a+b" : "ab";
    if ((flags & 0x400u) != 0u || (flags & 0x200u) != 0u) return read ? "w+b" : "wb";
    return "r+b";
}
#else
const char *fopen_mode(std::uint32_t flags) {
    const bool write = (flags & 0x2u) != 0u;
    if (!write) return "rb";
    if ((flags & 0x100u) != 0u) return "ab";
    if ((flags & 0x400u) != 0u || (flags & 0x200u) != 0u) return "wb";
    return "r+b";
}
#endif

std::uint64_t seek_target(std::uint64_t position, std::uint64_t size, std::int64_t offset, std::uint32_t whence) {
    std::int64_t base = 0;
    if (whence == 1u) base = static_cast<std::int64_t>(position);
    else if (whence == 2u) base = static_cast<std::int64_t>(size);
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, base + offset));
}

} // namespace

void webfs_native_poll(WebFs &fs);

bool Kernel::enable_webfs(const std::string &manifest_path, const std::string &url_prefix) {
    auto fs = std::make_unique<WebFs>();
    if (!fs->load_manifest(manifest_path, url_prefix)) return false;
    webfs_ = std::move(fs);
    return true;
}

void Kernel::poll_io() {
#if !defined(__EMSCRIPTEN__)
    if (webfs_) webfs_native_poll(*webfs_);
#endif
}

bool Kernel::disc_relative(const std::string &psp_path, std::string &relative) const {
    std::string path = psp_path;
    if (path.find(':') == std::string::npos) {
        std::string base = cwd_;
        if (!base.empty() && base.back() != '/') base.push_back('/');
        path = base + path;
    }
    const auto colon = path.find(':');
    std::string device = path.substr(0, colon);
    std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (device.rfind("disc", 0) != 0 && device.rfind("umd", 0) != 0) return false;
    relative = path.substr(colon + 1u);
    return true;
}

namespace {
std::string lbn_key(std::string relative) {
    std::string key;
    for (char c : relative) {
        if (c == '\\') c = '/';
        if (c == '/' && !key.empty() && key.back() == '/') continue;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (key.empty() || key.front() != '/') key.insert(key.begin(), '/');
    while (key.size() > 1u && key.back() == '/') key.pop_back();
    return key;
}
} // namespace

std::uint32_t Kernel::disc_lbn(const std::string &relative, std::uint64_t size) {
    const std::string key = lbn_key(relative);
    if (const auto it = lbn_by_path_.find(key); it != lbn_by_path_.end()) return it->second;
    const std::uint32_t lbn = next_lbn_;
    next_lbn_ += static_cast<std::uint32_t>((size + 2047u) / 2048u) + 1u; // keep every file's range apart
    lbn_by_path_[key] = lbn;
    path_by_lbn_[lbn] = {relative, size};
    return lbn;
}

std::string Kernel::resolve_lbn_path(const std::string &psp_path) const {
    std::string relative;
    if (!disc_relative(psp_path, relative)) return psp_path;
    const std::string key = lbn_key(relative);
    if (key.rfind("/sce_lbn", 0) != 0u) return psp_path;
    char *end = nullptr;
    const auto lbn = static_cast<std::uint32_t>(std::strtoul(key.c_str() + 8, &end, 16));
    const auto it = path_by_lbn_.find(lbn);
    if (it == path_by_lbn_.end()) {
        std::cerr << "[io] no disc file starts at sector " << lbn << " (" << psp_path << ")\n";
        return psp_path;
    }
    return psp_path.substr(0, psp_path.find(':') + 1u) + it->second.first;
}

std::int32_t Kernel::allocate_fd() const {
    for (std::int32_t fd = 3; fd < kMaxFds; ++fd)
        if (!files_.contains(fd) && !directories_.contains(fd)) return fd;
    return -1;
}

bool Kernel::stat_path(const std::string &psp_path, DirectoryEntry &entry) const {
    std::string relative;
    if (webfs_ && disc_relative(psp_path, relative)) {
        const WebFile *file = webfs_->find(relative);
        if (file == nullptr) return false;
        entry = DirectoryEntry{file->path, file->size, file->directory};
        return true;
    }
    const auto path = host_path(psp_path);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    entry.directory = std::filesystem::is_directory(path, ec);
    entry.size = entry.directory ? 0u : std::filesystem::file_size(path, ec);
    entry.name = path.filename().string();
    return true;
}

void Kernel::write_stat(std::uint32_t address, const DirectoryEntry &entry) {
    for (std::uint32_t i = 0; i < 0x58u; i += 4u) rt_.memory().store32(address + i, 0u);
    rt_.memory().store32(address + 0u, entry.directory ? 0x11FFu : 0x21FFu);
    rt_.memory().store32(address + 4u, entry.directory ? 0x10u : 0x20u);
    rt_.memory().store32(address + 8u, static_cast<std::uint32_t>(entry.size));
    rt_.memory().store32(address + 12u, static_cast<std::uint32_t>(entry.size >> 32u));
    rt_.memory().store32(address + 0x40u, entry.lbn); // st_private[0]
}

void Kernel::sceIoOpen(Ctx &ctx) {
    std::string psp_path = resolve_lbn_path(read_string(ctx.gpr[4]));
    static const bool skip_movies = std::getenv("PSPWEB_SKIP_MOVIES") != nullptr;
    if (skip_movies && psp_path.size() > 4u && lbn_key(psp_path.substr(psp_path.size() - 4u)) == "/.pmf") psp_path += ".skipped";
    static const bool trace = std::getenv("PSPWEB_TRACE_IO") != nullptr;
    if (trace) std::cerr << "[io] open " << read_string(ctx.gpr[4]) << " -> " << psp_path << "\n";
    std::string relative;
    if (webfs_ && disc_relative(psp_path, relative)) {
        const WebFile *file = webfs_->find(relative);
        if (file == nullptr || file->directory) {
            std::cerr << "[io] open failed: " << psp_path << "\n";
            finish(ctx, kErrorFileNotFound);
            return;
        }
        const std::int32_t fd = allocate_fd();
        if (fd < 0) {
            finish(ctx, kErrorTooManyFiles);
            return;
        }
        files_[fd] = OpenFile{nullptr, file, 0u, psp_path};
        finish(ctx, static_cast<std::uint32_t>(fd));
        return;
    }
    const std::int32_t fd = allocate_fd();
    if (fd < 0) {
        finish(ctx, kErrorTooManyFiles);
        return;
    }
    std::FILE *file = std::fopen(host_path(psp_path).string().c_str(), fopen_mode(ctx.gpr[5]));
    if (file == nullptr) {
        std::cerr << "[io] open failed: " << psp_path << "\n";
        finish(ctx, kErrorFileNotFound);
        return;
    }
    files_[fd] = OpenFile{file, nullptr, 0u, psp_path};
    finish(ctx, static_cast<std::uint32_t>(fd));
}

void Kernel::sceIoClose(Ctx &ctx) {
    const auto it = files_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (it->second.file != nullptr) std::fclose(it->second.file);
    files_.erase(it);
    finish(ctx, 0u);
}

void Kernel::web_read(Ctx &ctx, std::int32_t fd, std::uint32_t buffer, std::uint32_t length, bool async) {
    OpenFile &open = files_.at(fd);
    const WebFile &file = *open.web;
    const std::uint64_t position = open.position;
    const std::uint32_t count = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(length, file.size > position ? file.size - position : 0u));
    std::uint8_t *dst = rt_.memory().raw_pointer(buffer, count);
    if (count == 0u || (dst != nullptr && webfs_->read_cached(file, position, count, dst))) {
        open.position += count;
        if (async) {
            open.async_result = count;
            open.async_pending = false;
            finish(ctx, 0u);
        } else {
            finish(ctx, count);
        }
        return;
    }
    const std::int32_t waiting_thread = current_uid_;
    if (async) open.async_pending = true;
    webfs_->fetch(file, position, count, [this, fd, buffer, count, position, async, waiting_thread](bool ok) {
        const auto it = files_.find(fd);
        if (it == files_.end()) return;
        std::uint8_t *out = rt_.memory().raw_pointer(buffer, count);
        const bool copied = ok && out != nullptr && webfs_->read_cached(*it->second.web, position, count, out);
        const std::int64_t result = copied ? static_cast<std::int64_t>(count) : static_cast<std::int64_t>(static_cast<std::int32_t>(kErrorIo));
        if (copied) it->second.position = position + count;
        if (async) {
            it->second.async_result = result;
            it->second.async_pending = false;
            finish_async(fd);
        } else if (Thread *t = thread(waiting_thread); t != nullptr && t->uid == waiting_thread &&
                   t->state == ThreadState::Waiting && t->wait == WaitType::Io) {
            wake(*t, static_cast<std::uint32_t>(result));
        }
    });
    if (async) {
        finish(ctx, 0u);
        return;
    }
    if (Thread *self = current(); self != nullptr) {
        self->wait_uid = fd;
        self->wait_mode = 0u;
    }
    block(ctx, WaitType::Io, 0u);
}

// Wakes threads blocked in sceIoWaitAsync on `fd` and hands them the result.
void Kernel::finish_async(std::int32_t fd) {
    const auto it = files_.find(fd);
    if (it == files_.end()) return;
    for (auto &[uid, t] : threads_) {
        (void)uid;
        if (t.state != ThreadState::Waiting || t.wait != WaitType::Io || t.wait_uid != fd || t.wait_mode != 1u)
            continue;
        const auto value = static_cast<std::uint64_t>(it->second.async_result);
        if (t.wait_out_address != 0u) {
            rt_.memory().store32(t.wait_out_address, static_cast<std::uint32_t>(value));
            rt_.memory().store32(t.wait_out_address + 4u, static_cast<std::uint32_t>(value >> 32u));
        }
        wake(t, 0u);
    }
    if (it->second.closing) files_.erase(it);
}

void Kernel::sceIoRead(Ctx &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = files_.find(fd);
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (it->second.plain) {
        finish(ctx, plain_read(it->second, ctx.gpr[5], ctx.gpr[6]));
        return;
    }
    if (it->second.web != nullptr) {
        web_read(ctx, fd, ctx.gpr[5], ctx.gpr[6], false);
        return;
    }
    std::uint8_t *dst = rt_.memory().raw_pointer(ctx.gpr[5], ctx.gpr[6]);
    if (it->second.file == nullptr || dst == nullptr) {
        finish(ctx, kErrorBadFile);
        return;
    }
    finish(ctx, static_cast<std::uint32_t>(std::fread(dst, 1u, ctx.gpr[6], it->second.file)));
}

void Kernel::sceIoWrite(Ctx &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint8_t *src = rt_.memory().raw_pointer(ctx.gpr[5], ctx.gpr[6]);
    if (src == nullptr) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (fd == 1 || fd == 2) {
        std::ostream &out = fd == 1 ? std::cout : std::cerr;
        out << "[guest] " << std::string(reinterpret_cast<const char *>(src), ctx.gpr[6]);
        out.flush();
        finish(ctx, ctx.gpr[6]);
        return;
    }
    const auto it = files_.find(fd);
    if (it == files_.end() || it->second.file == nullptr) {
        finish(ctx, kErrorBadFile);
        return;
    }
    finish(ctx, static_cast<std::uint32_t>(std::fwrite(src, 1u, ctx.gpr[6], it->second.file)));
}

void Kernel::sceIoLseek(Ctx &ctx) {
    const auto it = files_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    const std::int64_t offset = static_cast<std::int64_t>(ctx.gpr[6] | (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u));
    if (it != files_.end() && (it->second.plain || it->second.web != nullptr)) {
        const std::uint64_t size = it->second.plain ? it->second.plain->size() : it->second.web->size;
        it->second.position = seek_target(it->second.position, size, offset, ctx.gpr[8]);
        finish64(ctx, it->second.position);
        return;
    }
    if (it == files_.end() || it->second.file == nullptr) {
        finish64(ctx, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(kErrorBadFile))));
        return;
    }
    std::fseek(it->second.file, static_cast<long>(offset), static_cast<int>(ctx.gpr[8]));
    finish64(ctx, static_cast<std::uint64_t>(std::ftell(it->second.file)));
}

void Kernel::sceIoLseek32(Ctx &ctx) {
    const auto it = files_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    const std::int64_t offset = static_cast<std::int32_t>(ctx.gpr[5]);
    if (it != files_.end() && (it->second.plain || it->second.web != nullptr)) {
        const std::uint64_t size = it->second.plain ? it->second.plain->size() : it->second.web->size;
        it->second.position = seek_target(it->second.position, size, offset, ctx.gpr[6]);
        finish(ctx, static_cast<std::uint32_t>(it->second.position));
        return;
    }
    if (it == files_.end() || it->second.file == nullptr) {
        finish(ctx, kErrorBadFile);
        return;
    }
    std::fseek(it->second.file, static_cast<long>(offset), static_cast<int>(ctx.gpr[6]));
    finish(ctx, static_cast<std::uint32_t>(std::ftell(it->second.file)));
}

void Kernel::sceIoGetstat(Ctx &ctx) {
    DirectoryEntry entry;
    const std::string psp_path = resolve_lbn_path(read_string(ctx.gpr[4]));
    if (!stat_path(psp_path, entry)) {
        finish(ctx, kErrorFileNotFound);
        return;
    }
    if (std::string relative; disc_relative(psp_path, relative)) entry.lbn = disc_lbn(relative, entry.size);
    write_stat(ctx.gpr[5], entry);
    finish(ctx, 0u);
}

void Kernel::sceIoChdir(Ctx &ctx) {
    cwd_ = read_string(ctx.gpr[4]);
    finish(ctx, 0u);
}

void Kernel::sceIoDopen(Ctx &ctx) {
    const std::string psp_path = read_string(ctx.gpr[4]);
    OpenDirectory dir;
    std::string disc_dir;
    const bool on_disc = disc_relative(psp_path, disc_dir);
    std::string relative;
    if (webfs_ && disc_relative(psp_path, relative)) {
        const WebFile *folder = webfs_->find(relative);
        if (folder == nullptr || !folder->directory) {
            finish(ctx, kErrorFileNotFound);
            return;
        }
        for (const WebFile *file : webfs_->list(folder->path)) {
            const auto slash = file->path.rfind('/');
            dir.entries.push_back({slash == std::string::npos ? file->path : file->path.substr(slash + 1u),
                                   file->size, file->directory});
        }
    } else {
        const auto path = host_path(psp_path);
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) {
            finish(ctx, kErrorFileNotFound);
            return;
        }
        for (const auto &entry : std::filesystem::directory_iterator(path, ec)) {
            const bool directory = entry.is_directory(ec);
            dir.entries.push_back({entry.path().filename().string(), directory ? 0u : entry.file_size(ec), directory});
        }
    }
    if (on_disc)
        for (DirectoryEntry &entry : dir.entries)
            if (entry.name != "." && entry.name != "..")
                entry.lbn = disc_lbn(disc_dir + (disc_dir.ends_with('/') ? "" : "/") + entry.name, entry.size);
    const std::int32_t fd = allocate_fd();
    if (fd < 0) {
        finish(ctx, kErrorTooManyFiles);
        return;
    }
    directories_[fd] = std::move(dir);
    finish(ctx, static_cast<std::uint32_t>(fd));
}

void Kernel::sceIoDread(Ctx &ctx) {
    const auto it = directories_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == directories_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (it->second.next >= it->second.entries.size()) {
        finish(ctx, 0u);
        return;
    }
    const DirectoryEntry &entry = it->second.entries[it->second.next++];
    const std::uint32_t dirent = ctx.gpr[5];
    write_stat(dirent, entry);
    for (std::size_t i = 0; i < 256u; ++i)
        rt_.memory().store8(dirent + 0x58u + static_cast<std::uint32_t>(i),
                            i < entry.name.size() ? static_cast<std::uint8_t>(entry.name[i]) : 0u);
    finish(ctx, 1u);
}

void Kernel::sceIoDclose(Ctx &ctx) {
    finish(ctx, directories_.erase(static_cast<std::int32_t>(ctx.gpr[4])) != 0u ? 0u : kErrorBadFile);
}

// Async calls return immediately; results are collected with WaitAsync/PollAsync.
void Kernel::sceIoOpenAsync(Ctx &ctx) {
    std::string psp_path = resolve_lbn_path(read_string(ctx.gpr[4]));
    // Debugging aid: pretend movies are missing, which makes some games skip them.
    static const bool skip_movies = std::getenv("PSPWEB_SKIP_MOVIES") != nullptr;
    if (skip_movies && psp_path.size() > 4u && lbn_key(psp_path.substr(psp_path.size() - 4u)) == "/.pmf") psp_path += ".skipped";
    const std::int32_t fd = allocate_fd();
    if (fd < 0) {
        finish(ctx, kErrorTooManyFiles);
        return;
    }
    std::string relative;
    OpenFile open{nullptr, nullptr, 0u, psp_path};
    if (webfs_ && disc_relative(psp_path, relative)) {
        const WebFile *file = webfs_->find(relative);
        open.web = file != nullptr && !file->directory ? file : nullptr;
    } else {
        open.file = std::fopen(host_path(psp_path).string().c_str(), fopen_mode(ctx.gpr[5]));
    }
    const bool ok = open.web != nullptr || open.file != nullptr;
    if (!ok) std::cerr << "[io] async open failed: " << psp_path << "\n";
    if (std::getenv("PSPWEB_TRACE_IO") != nullptr) std::cerr << "[io] open async " << psp_path << " -> " << fd << "\n";
    open.async_result = ok ? fd : static_cast<std::int32_t>(kErrorFileNotFound);
    files_[fd] = std::move(open);
    finish(ctx, static_cast<std::uint32_t>(fd));
}

void Kernel::sceIoReadAsync(Ctx &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = files_.find(fd);
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (it->second.plain) {
        it->second.async_result = static_cast<std::int32_t>(plain_read(it->second, ctx.gpr[5], ctx.gpr[6]));
        it->second.async_pending = false;
        finish(ctx, 0u);
        return;
    }
    if (it->second.web != nullptr) {
        web_read(ctx, fd, ctx.gpr[5], ctx.gpr[6], true);
        return;
    }
    std::uint8_t *dst = rt_.memory().raw_pointer(ctx.gpr[5], ctx.gpr[6]);
    it->second.async_result = it->second.file != nullptr && dst != nullptr
        ? static_cast<std::int64_t>(std::fread(dst, 1u, ctx.gpr[6], it->second.file)) : 0;
    if (std::getenv("PSPWEB_TRACE_IO") != nullptr)
        std::cerr << "[io] read async fd " << fd << " " << ctx.gpr[6] << " bytes at " << std::ftell(it->second.file)
                  << " -> " << it->second.async_result << "\n";
    it->second.async_pending = false;
    finish(ctx, 0u);
}

void Kernel::sceIoLseekAsync(Ctx &ctx) {
    const auto it = files_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    const std::int64_t offset = static_cast<std::int64_t>(ctx.gpr[6] | (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u));
    if (it->second.plain || it->second.web != nullptr) {
        const std::uint64_t size = it->second.plain ? it->second.plain->size() : it->second.web->size;
        it->second.position = seek_target(it->second.position, size, offset, ctx.gpr[8]);
        it->second.async_result = static_cast<std::int64_t>(it->second.position);
    } else if (it->second.file != nullptr) {
        std::fseek(it->second.file, static_cast<long>(offset), static_cast<int>(ctx.gpr[8]));
        it->second.async_result = std::ftell(it->second.file);
    }
    finish(ctx, 0u);
}

void Kernel::sceIoCloseAsync(Ctx &ctx) {
    const auto it = files_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (it->second.file != nullptr) std::fclose(it->second.file);
    it->second.file = nullptr;
    it->second.closing = true;
    if (!it->second.async_pending) it->second.async_result = 0;
    finish(ctx, 0u);
}

void Kernel::sceIoWaitAsync(Ctx &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = files_.find(fd);
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (it->second.async_pending) {
        if (Thread *self = current(); self != nullptr) {
            self->wait_uid = fd;
            self->wait_mode = 1u;
            self->wait_out_address = ctx.gpr[5];
        }
        block(ctx, WaitType::Io, 0u);
        return;
    }
    if (ctx.gpr[5] != 0u) {
        const auto value = static_cast<std::uint64_t>(it->second.async_result);
        rt_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(value));
        rt_.memory().store32(ctx.gpr[5] + 4u, static_cast<std::uint32_t>(value >> 32u));
    }
    if (it->second.closing) files_.erase(it);
    finish(ctx, 0u);
}

void Kernel::sceIoPollAsync(Ctx &ctx) {
    const auto it = files_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it != files_.end() && it->second.async_pending) {
        finish(ctx, 1u);
        return;
    }
    sceIoWaitAsync(ctx);
}

std::uint32_t Kernel::plain_read(OpenFile &open, std::uint32_t buffer, std::uint32_t length) {
    const std::vector<std::uint8_t> &plain = *open.plain;
    const std::uint64_t position = std::min<std::uint64_t>(open.position, plain.size());
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(length, plain.size() - position));
    std::uint8_t *dst = rt_.memory().raw_pointer(buffer, count);
    if (count != 0u && dst == nullptr) return kErrorBadFile;
    if (count != 0u) std::memcpy(dst, plain.data() + position, count);
    open.position = position + count;
    return count;
}

// Hands the whole of an open file to `use` and finishes the call with its
// result.  A streamed file that is not cached yet parks the thread until its
// data has arrived.
void Kernel::with_file_contents(Ctx &ctx, std::int32_t fd,
                                std::function<std::uint32_t(OpenFile &, std::span<const std::uint8_t>)> use) {
    OpenFile &open = files_.at(fd);
    if (open.file != nullptr) {
        const long position = std::ftell(open.file);
        std::fseek(open.file, 0, SEEK_END);
        const long size = std::ftell(open.file);
        if (size < 0 || static_cast<std::uint64_t>(size) > kMaxWholeFileRead) {
            std::fseek(open.file, position, SEEK_SET);
            finish(ctx, kErrorIo);
            return;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        std::fseek(open.file, 0, SEEK_SET);
        bytes.resize(std::fread(bytes.data(), 1u, bytes.size(), open.file));
        std::fseek(open.file, position, SEEK_SET);
        finish(ctx, use(open, bytes));
        return;
    }
    if (open.web == nullptr || open.web->size > kMaxWholeFileRead) {
        finish(ctx, open.web == nullptr ? kErrorBadFile : kErrorIo);
        return;
    }
    const auto size = static_cast<std::uint32_t>(open.web->size);
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
    if (webfs_->read_cached(*open.web, 0u, size, bytes->data())) {
        finish(ctx, use(open, *bytes));
        return;
    }
    const std::int32_t waiting_thread = current_uid_;
    webfs_->fetch(*open.web, 0u, size, [this, fd, size, bytes, use = std::move(use), waiting_thread](bool ok) {
        const auto it = files_.find(fd);
        std::uint32_t result = kErrorIo;
        if (it != files_.end() && ok && webfs_->read_cached(*it->second.web, 0u, size, bytes->data()))
            result = use(it->second, *bytes);
        if (Thread *t = thread(waiting_thread); t != nullptr && t->uid == waiting_thread &&
            t->state == ThreadState::Waiting && t->wait == WaitType::Io)
            wake(*t, result);
    });
    if (Thread *self = current(); self != nullptr) {
        self->wait_uid = fd;
        self->wait_mode = 0u;
    }
    block(ctx, WaitType::Io, 0u);
}

// Like with_file_contents, for a file that is not open (a module to load).
void Kernel::with_path_contents(Ctx &ctx, const std::string &psp_path,
                                std::function<std::uint32_t(std::span<const std::uint8_t>)> use) {
    std::string relative;
    if (webfs_ && disc_relative(psp_path, relative)) {
        const WebFile *file = webfs_->find(relative);
        if (file == nullptr || file->directory || file->size > kMaxWholeFileRead) {
            finish(ctx, use({}));
            return;
        }
        const auto size = static_cast<std::uint32_t>(file->size);
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
        if (webfs_->read_cached(*file, 0u, size, bytes->data())) {
            finish(ctx, use(*bytes));
            return;
        }
        const std::int32_t waiting_thread = current_uid_;
        webfs_->fetch(*file, 0u, size, [this, file, size, bytes, use = std::move(use), waiting_thread](bool ok) {
            const bool read = ok && webfs_->read_cached(*file, 0u, size, bytes->data());
            const std::uint32_t result = use(read ? std::span<const std::uint8_t>(*bytes) : std::span<const std::uint8_t>());
            if (Thread *t = thread(waiting_thread); t != nullptr && t->uid == waiting_thread &&
                t->state == ThreadState::Waiting && t->wait == WaitType::Io)
                wake(*t, result);
        });
        if (Thread *self = current(); self != nullptr) {
            self->wait_uid = -1;
            self->wait_mode = 0u;
        }
        block(ctx, WaitType::Io, 0u);
        return;
    }
    std::ifstream in(host_path(psp_path), std::ios::binary);
    std::vector<std::uint8_t> bytes;
    if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    finish(ctx, use(bytes));
}

// Games use ioctls on disc files to set up PGD (DRM) decryption; other
// commands are accepted without doing anything.
void Kernel::sceIoIoctl(Ctx &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t command = ctx.gpr[5];
    const auto it = files_.find(fd);
    if (it == files_.end()) {
        finish(ctx, kErrorBadFile);
        return;
    }
    if (command == 0x04100002u) { // where the PGD container starts inside the file
        if (ctx.gpr[7] >= 4u) it->second.pgd_offset = rt_.memory().load32(ctx.gpr[6]);
        finish(ctx, 0u);
        return;
    }
    if (command != 0x04100001u) {
        std::ostringstream name;
        name << "sceIoIoctl 0x" << std::hex << std::setw(8) << std::setfill('0') << command;
        if (fallback_logged_.insert(name.str()).second) std::cerr << "[io] ignoring " << name.str() << "\n";
        finish(ctx, 0u);
        return;
    }
    // The game's version key; files that turn out not to be PGD are read as they are.
    std::optional<std::array<std::uint8_t, 16>> key;
    if (ctx.gpr[7] == 16u) {
        key.emplace();
        for (std::uint32_t i = 0; i < 16u; ++i) (*key)[i] = rt_.memory().load8(ctx.gpr[6] + i);
    }
    with_file_contents(ctx, fd, [key](OpenFile &open, std::span<const std::uint8_t> bytes) -> std::uint32_t {
        if (open.pgd_offset >= bytes.size() || !pgd::is_pgd(bytes.subspan(open.pgd_offset))) return 0u;
        auto plain = std::make_shared<std::vector<std::uint8_t>>();
        if (!pgd::decrypt(bytes.subspan(open.pgd_offset), key ? key->data() : nullptr, *plain)) {
            std::cerr << "[io] PGD decryption failed: " << open.path << "\n";
            return kErrorPgdInvalidHeader;
        }
        open.plain = std::move(plain);
        open.position = 0u;
        return 0u;
    });
}

} // namespace pspweb
