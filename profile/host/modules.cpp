// Modules a game loads at run time with sceKernelLoadModule.
//
// Firmware libraries ship encrypted on the disc and are emulated, so loading
// one only hands out an id. A game's own modules are plain ELF PRX files: they
// are loaded and relocated as on the PSP, at the address their recompiled code
// was generated for, and that code is registered while the module is loaded.
// A module without recompiled code is still loaded, and where it went is
// written to modules.txt next to the disc, which scripts/modules.sh reads to
// translate it.

#include "kernel.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace pspweb {

namespace {

constexpr std::uint32_t kErrorUnknownModule = 0x8002012Eu;
constexpr std::uint32_t kErrorModuleLoad = 0x80020146u;
constexpr std::uint32_t kNidModuleStart = 0xD632ACDBu;
constexpr std::uint32_t kNidModuleStop = 0xCEE8593Cu;
constexpr std::uint32_t kModuleStartStack = 0x40000u;

std::string hex32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

// Disc paths compare without case, leading slash or doubled slashes.
std::string path_key(const std::string &path) {
    std::string key;
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c == '/' && (key.empty() || key.back() == '/')) continue;
        key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return key;
}

bool is_plain_elf(std::span<const std::uint8_t> bytes) {
    return bytes.size() > 4u && bytes[0] == 0x7Fu && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F';
}

} // namespace

void Kernel::register_exports(const psprecomp::PspModuleInfo &info) {
    auto &m = rt_.memory();
    for (std::uint32_t cursor = info.ent_top; cursor + 16u <= info.ent_end;) {
        const std::uint32_t libname = m.load32(cursor);
        const std::uint32_t words = m.load8(cursor + 8u);
        const std::uint32_t variables = m.load8(cursor + 9u);
        const std::uint32_t functions = m.load16(cursor + 10u);
        const std::uint32_t table = m.load32(cursor + 12u);
        if (words == 0u) break;
        cursor += words * 4u;
        if (libname == 0u) continue; // module_start and friends; read separately
        const std::string library = m.read_c_string(libname, 64u);
        for (std::uint32_t i = 0; i < functions; ++i) {
            const std::uint32_t nid = m.load32(table + i * 4u);
            const std::uint32_t address = m.load32(table + (functions + variables + i) * 4u);
            // A call through the importer's stub continues in the exporting
            // module, exactly like the jump the PSP's loader patches in.
            rt_.register_hle(library, nid, [address](psprecomp::Runtime &, Ctx &ctx) { ctx.pc = address; });
            rt_.nids().add(library, nid, library + "_" + hex32(nid));
            registered_.insert(library + ":" + hex32(nid));
        }
    }
}

void Kernel::add_main_module(const psprecomp::PspModuleInfo &info, std::uint32_t base, std::uint32_t size) {
    LoadedModule module;
    module.name = info.name;
    module.base = base;
    module.size = size;
    module.gp = info.gp;
    module.has_code = true;
    modules_[new_uid()] = std::move(module);
    register_exports(info);
}

std::int32_t Kernel::module_at(std::uint32_t address) const {
    for (const auto &[uid, module] : modules_)
        if (module.size != 0u && address >= module.base && address < module.base + module.size) return uid;
    return -1;
}

std::uint32_t Kernel::load_module_image(const std::string &relative, std::span<const std::uint8_t> bytes) {
    const auto elf = psprecomp::Elf32Image::from_bytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), relative);
    if (!elf.is_psp_prx()) {
        std::cerr << "[module] " << relative << " is not a relocatable PRX\n";
        return kErrorModuleLoad;
    }
    std::uint32_t size = 0u;
    for (std::size_t i = 0; i < elf.segments().size(); ++i)
        if (elf.segments()[i].type == 1u)
            size = std::max(size, elf.segment_runtime_address(i, 0u) + elf.segments()[i].memory_size);

    const GeneratedModule *generated = nullptr;
    for (const GeneratedModule &candidate : generated_modules())
        if (path_key(candidate.path) == path_key(relative)) generated = &candidate;
    const std::int32_t block = generated != nullptr ? allocate("module:" + relative, 2u, size, generated->base)
                                                    : allocate("module:" + relative, 0u, size, 0u);
    if (block < 0) {
        std::cerr << "[module] no room for " << relative << " (" << size << " bytes"
                  << (generated != nullptr ? " at " + hex32(generated->base) : std::string()) << ")\n";
        return kErrorModuleLoad;
    }
    const std::uint32_t base = blocks_.at(block).address;
    (void)elf.load_and_relocate(rt_.memory(), base);
    const auto info = elf.find_module_info(rt_.memory(), base);
    if (!info) {
        blocks_.erase(block);
        std::cerr << "[module] no module info in " << relative << "\n";
        return kErrorModuleLoad;
    }
    for (const auto &import : elf.scan_imports(rt_.memory(), *info)) install_fallback(import);

    rt_.unregister_functions(base, base + size);
    if (generated != nullptr) {
        generated->register_functions(rt_);
    } else {
        std::cerr << "[module] " << relative << " loaded at " << hex32(base)
                  << " has no recompiled code; translate it with scripts/modules.sh\n";
#ifndef __EMSCRIPTEN__
        // Note the address for scripts/modules.sh (games/<name>/root/modules.txt).
        const auto list = disc_root_.parent_path() / "modules.txt";
        std::string line = relative.substr(relative.find_first_not_of('/')) + " " + hex32(base);
        std::ifstream existing(list);
        std::string seen;
        bool known = false;
        while (std::getline(existing, seen)) known = known || path_key(seen.substr(0, seen.find(' '))) == path_key(relative);
        if (!known) std::ofstream(list, std::ios::app) << line << "\n";
#endif
    }
    register_exports(*info);

    LoadedModule module;
    module.path = relative;
    module.name = info->name;
    module.base = base;
    module.size = size;
    module.gp = info->gp;
    module.block = block;
    module.has_code = generated != nullptr;
    module.start = elf.runtime_entry(base);
    // The unnamed (system) export table holds module_start and module_stop.
    auto &m = rt_.memory();
    for (std::uint32_t cursor = info->ent_top; cursor + 16u <= info->ent_end;) {
        const std::uint32_t words = m.load8(cursor + 8u);
        if (words == 0u) break;
        if (m.load32(cursor) == 0u) {
            const std::uint32_t variables = m.load8(cursor + 9u), functions = m.load16(cursor + 10u);
            const std::uint32_t table = m.load32(cursor + 12u);
            for (std::uint32_t i = 0; i < functions + variables; ++i) {
                const std::uint32_t nid = m.load32(table + i * 4u);
                const std::uint32_t address = m.load32(table + (functions + variables + i) * 4u);
                if (nid == kNidModuleStart) module.start = address;
                if (nid == kNidModuleStop) module.stop = address;
            }
        }
        cursor += words * 4u;
    }
    const std::int32_t uid = new_uid();
    std::cerr << "[module] " << info->name << " (" << relative << ") at " << hex32(base) << "-" << hex32(base + size)
              << (module.has_code ? "" : ", no code") << "\n";
    modules_[uid] = std::move(module);
    return static_cast<std::uint32_t>(uid);
}

void Kernel::unload_module(std::int32_t uid) {
    const auto it = modules_.find(uid);
    if (it == modules_.end()) return;
    const LoadedModule &module = it->second;
    if (module.size != 0u) rt_.unregister_functions(module.base, module.base + module.size);
    if (module.block >= 0) blocks_.erase(module.block);
    if (!module.name.empty()) std::cerr << "[module] unloaded " << module.name << "\n";
    modules_.erase(it);
}

void Kernel::sceKernelLoadModule(Ctx &ctx) {
    const std::string psp_path = resolve_lbn_path(read_string(ctx.gpr[4]));
    std::string relative;
    (void)disc_relative(psp_path, relative);
    with_path_contents(ctx, psp_path, [this, psp_path, relative](std::span<const std::uint8_t> bytes) -> std::uint32_t {
        if (is_plain_elf(bytes)) {
            try {
                return load_module_image(relative, bytes);
            } catch (const std::exception &e) {
                std::cerr << "[module] loading " << psp_path << " failed: " << e.what() << "\n";
                return kErrorModuleLoad;
            }
        }
        // Encrypted firmware library (or a file that is missing): emulated.
        const std::int32_t uid = new_uid();
        modules_[uid] = LoadedModule{relative, {}, 0u, 0u, 0u, 0u, 0u, -1, false};
        std::cerr << "[kernel] sceKernelLoadModule(" << psp_path << ") -> HLE module " << uid << "\n";
        return static_cast<std::uint32_t>(uid);
    });
}

void Kernel::sceKernelStartModule(Ctx &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = modules_.find(uid);
    if (it == modules_.end()) {
        finish(ctx, kErrorUnknownModule);
        return;
    }
    const std::uint32_t status = ctx.gpr[7];
    const LoadedModule &module = it->second;
    if (module.size == 0u || module.start == 0u) { // emulated library
        if (status != 0u) rt_.memory().store32(status, 0u);
        finish(ctx, static_cast<std::uint32_t>(uid));
        return;
    }
    if (!module.has_code) {
        halted_ = true;
        halt_reason_ = "module " + module.path + " has no recompiled code (see modules.txt)";
        std::cerr << "[module] " << halt_reason_ << "\n";
        rt_.stop(halt_reason_);
        return;
    }
    // module_start(args, argp) runs on a thread of its own with the module's gp.
    call_guest(ctx, module.start, {ctx.gpr[5], ctx.gpr[6]},
               [this, uid, status](std::uint32_t result) {
                   if (status != 0u) rt_.memory().store32(status, result);
                   return static_cast<std::uint32_t>(uid);
               },
               module.gp, kModuleStartStack);
}

void Kernel::sceKernelStopModule(Ctx &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = modules_.find(uid);
    if (it == modules_.end()) {
        finish(ctx, kErrorUnknownModule);
        return;
    }
    const std::uint32_t status = ctx.gpr[7];
    if (!it->second.has_code || it->second.stop == 0u) {
        if (status != 0u) rt_.memory().store32(status, 0u);
        finish(ctx, static_cast<std::uint32_t>(uid));
        return;
    }
    call_guest(ctx, it->second.stop, {ctx.gpr[5], ctx.gpr[6]},
               [this, uid, status](std::uint32_t result) {
                   if (status != 0u) rt_.memory().store32(status, result);
                   return static_cast<std::uint32_t>(uid);
               },
               it->second.gp, kModuleStartStack);
}

void Kernel::sceKernelUnloadModule(Ctx &ctx) {
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (!modules_.contains(uid)) {
        finish(ctx, kErrorUnknownModule);
        return;
    }
    unload_module(uid);
    finish(ctx, static_cast<std::uint32_t>(uid));
}

// Called from inside a module: unloads it and ends the calling thread.
void Kernel::sceKernelStopUnloadSelfModuleWithStatus(Ctx &ctx) {
    const std::int32_t uid = module_at(ctx.gpr[31]);
    if (uid >= 0 && modules_.at(uid).block >= 0) unload_module(uid);
    exit_thread(ctx, static_cast<std::int32_t>(ctx.gpr[4]), true);
}

void Kernel::sceKernelSelfStopUnloadModule(Ctx &ctx) {
    const std::int32_t uid = module_at(ctx.gpr[31]);
    if (uid >= 0 && modules_.at(uid).block >= 0) unload_module(uid);
    exit_thread(ctx, 0, true);
}

void Kernel::sceKernelGetModuleIdByAddress(Ctx &ctx) {
    const std::int32_t uid = module_at(ctx.gpr[4]);
    finish(ctx, uid >= 0 ? static_cast<std::uint32_t>(uid) : kErrorUnknownModule);
}

void Kernel::sceKernelGetModuleId(Ctx &ctx) {
    const std::int32_t uid = module_at(ctx.gpr[31]);
    finish(ctx, uid >= 0 ? static_cast<std::uint32_t>(uid) : kErrorUnknownModule);
}

// SceKernelModuleInfo: one segment covering the whole image.
void Kernel::sceKernelQueryModuleInfo(Ctx &ctx) {
    const auto it = modules_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == modules_.end()) {
        finish(ctx, kErrorUnknownModule);
        return;
    }
    const LoadedModule &module = it->second;
    auto &m = rt_.memory();
    const std::uint32_t info = ctx.gpr[5];
    for (std::uint32_t i = 4u; i < 0x60u; i += 4u) m.store32(info + i, 0u);
    m.store8(info + 4u, 1u);
    m.store32(info + 8u, module.base);
    m.store32(info + 24u, module.size);
    m.store32(info + 40u, module.start);
    m.store32(info + 44u, module.gp);
    m.store32(info + 48u, module.base);
    m.store32(info + 52u, module.size);
    for (std::size_t i = 0; i < 27u && i < module.name.size(); ++i)
        m.store8(info + 68u + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(module.name[i]));
    finish(ctx, 0u);
}

void Kernel::sceKernelGetModuleIdList(Ctx &ctx) {
    const std::uint32_t buffer = ctx.gpr[4], capacity = ctx.gpr[5] / 4u;
    std::uint32_t count = 0u;
    for (const auto &[uid, module] : modules_) {
        (void)module;
        if (count < capacity) rt_.memory().store32(buffer + count * 4u, static_cast<std::uint32_t>(uid));
        ++count;
    }
    if (ctx.gpr[6] != 0u) rt_.memory().store32(ctx.gpr[6], count);
    finish(ctx, 0u);
}

} // namespace pspweb
