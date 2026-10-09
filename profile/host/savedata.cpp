#include "kernel.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pspweb {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kCanceled = 1u;
constexpr std::uint32_t kLoadNoData = 0x80110307u;
constexpr std::uint32_t kLoadFileNotFound = 0x80110309u;
constexpr std::uint32_t kRwNoData = 0x80110327u;
constexpr std::uint32_t kRwFileNotFound = 0x80110329u;
constexpr std::uint32_t kSaveAccessError = 0x80110385u;
constexpr std::uint32_t kDeleteNoData = 0x80110347u;
constexpr std::uint32_t kSizesNoData = 0x801103C7u;

enum Mode : std::uint32_t {
    kAutoLoad, kAutoSave, kLoad, kSave, kListLoad, kListSave, kListDelete, kListAllDelete, kSizes, kAutoDelete, kDelete, kList, kFiles,
    kMakeDataSecure, kMakeData, kReadDataSecure, kReadData, kWriteDataSecure, kWriteData, kEraseSecure, kErase, kDeleteData, kGetSize
};

enum Field : std::uint32_t {
    kResult = 0x1Cu, kMode = 0x30u, kGameName = 0x3Cu, kSaveName = 0x4Cu, kSaveNameList = 0x60u, kFileName = 0x64u, kDataBuf = 0x74u,
    kDataBufSize = 0x78u, kDataSize = 0x7Cu, kTitle = 0x80u, kSavedataTitle = 0x100u, kDetail = 0x180u, kParental = 0x580u,
    kIcon0 = 0x584u, kFocus = 0x5C8u, kMsFree = 0x5D0u, kMsData = 0x5D4u, kUtilityData = 0x5D8u, kIdList = 0x5F4u, kFileList = 0x5F8u
};

const char *const kSystemFiles[4] = {"ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3"};

struct Slot {
    std::string name;
    bool exists = false;
    std::string title, savedata_title, detail;
    std::time_t time = 0;
};

std::vector<std::uint8_t> read_all(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool write_all(const fs::path &path, const std::uint8_t *data, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (size != 0u) out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::time_t modified(const fs::path &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
}

void put16(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v) {
    out[at] = static_cast<std::uint8_t>(v);
    out[at + 1u] = static_cast<std::uint8_t>(v >> 8u);
}

void put32(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint32_t get32(const std::vector<std::uint8_t> &in, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

struct SfoEntry {
    std::string key;
    std::uint16_t format;
    std::string text;
    std::uint32_t number;
    std::uint32_t max;
};

std::vector<std::uint8_t> build_sfo(const std::vector<SfoEntry> &entries) {
    std::vector<std::uint8_t> keys, values, index(entries.size() * 16u);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const SfoEntry &e = entries[i];
        const std::size_t key_offset = keys.size();
        keys.insert(keys.end(), e.key.begin(), e.key.end());
        keys.push_back(0u);
        std::vector<std::uint8_t> bytes;
        std::uint32_t length = 4u, max = 4u;
        if (e.format == 0x0404u) {
            bytes.resize(4u);
            put32(bytes, 0u, e.number);
        } else {
            max = e.max;
            bytes.assign(e.text.begin(), e.text.end());
            if (bytes.size() > max - (e.format == 0x0204u ? 1u : 0u)) bytes.resize(max - (e.format == 0x0204u ? 1u : 0u));
            if (e.format == 0x0204u) bytes.push_back(0u);
            length = e.format == 0x0004u ? max : static_cast<std::uint32_t>(bytes.size());
            bytes.resize(max, 0u);
        }
        put16(index, i * 16u, static_cast<std::uint32_t>(key_offset));
        put16(index, i * 16u + 2u, e.format);
        put32(index, i * 16u + 4u, length);
        put32(index, i * 16u + 8u, max);
        put32(index, i * 16u + 12u, static_cast<std::uint32_t>(values.size()));
        values.insert(values.end(), bytes.begin(), bytes.end());
    }
    while (keys.size() % 4u != 0u) keys.push_back(0u);
    std::vector<std::uint8_t> out(20u);
    const auto key_table = static_cast<std::uint32_t>(20u + index.size());
    put32(out, 0u, 0x46535000u);
    put32(out, 4u, 0x00000101u);
    put32(out, 8u, key_table);
    put32(out, 12u, key_table + static_cast<std::uint32_t>(keys.size()));
    put32(out, 16u, static_cast<std::uint32_t>(entries.size()));
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), values.begin(), values.end());
    return out;
}

std::map<std::string, std::string> parse_sfo(const std::vector<std::uint8_t> &in, std::uint32_t &parental) {
    std::map<std::string, std::string> out;
    if (in.size() < 20u || get32(in, 0u) != 0x46535000u) return out;
    const std::uint32_t key_table = get32(in, 8u), data_table = get32(in, 12u), count = get32(in, 16u);
    for (std::uint32_t i = 0; i < count && 20u + i * 16u + 16u <= in.size(); ++i) {
        const std::size_t at = 20u + i * 16u;
        const std::size_t key_at = key_table + (in[at] | (in[at + 1u] << 8u));
        const std::uint32_t format = in[at + 2u] | (in[at + 3u] << 8u), length = get32(in, at + 4u);
        const std::size_t data_at = data_table + get32(in, at + 12u);
        if (key_at >= in.size() || data_at + length > in.size()) continue;
        std::string key;
        for (std::size_t k = key_at; k < in.size() && in[k] != 0u; ++k) key.push_back(static_cast<char>(in[k]));
        if (format == 0x0404u && length >= 4u) {
            if (key == "PARENTAL_LEVEL") parental = get32(in, data_at);
            continue;
        }
        std::string text(in.begin() + static_cast<std::ptrdiff_t>(data_at), in.begin() + static_cast<std::ptrdiff_t>(data_at + length));
        while (!text.empty() && text.back() == '\0') text.pop_back();
        out[key] = text;
    }
    return out;
}

bool matches(const std::string &pattern, const std::string &name) {
    if (pattern.empty() || pattern == "<>") return true;
    std::size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && (pattern[p] == '*' || pattern.compare(p, 2, "<>") == 0)) {
            p += pattern[p] == '*' ? 1u : 2u;
            star = p;
            mark = n;
        } else if (star != std::string::npos) {
            p = star;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && (pattern[p] == '*' || pattern.compare(p, 2, "<>") == 0)) p += pattern[p] == '*' ? 1u : 2u;
    return p == pattern.size();
}

std::string json_string(const std::string &s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c < 0x20u) {
            char code[8];
            std::snprintf(code, sizeof code, "\\u%04x", c);
            out += code;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out + "\"";
}

class Store {
public:
    Store(psprecomp::GuestMemory &memory, const fs::path &memstick) : m_(memory), memstick_(memstick) {}

    std::string text(std::uint32_t address, std::size_t limit) const {
        std::string out;
        for (std::size_t i = 0; address != 0u && i < limit; ++i) {
            const char c = static_cast<char>(m_.load8(address + static_cast<std::uint32_t>(i)));
            if (c == '\0') break;
            out.push_back(c);
        }
        return out;
    }

    void put_text(std::uint32_t address, std::size_t size, const std::string &value) {
        for (std::size_t i = 0; i < size; ++i)
            m_.store8(address + static_cast<std::uint32_t>(i), i + 1u < size && i < value.size() ? static_cast<std::uint8_t>(value[i]) : 0u);
    }

    fs::path root() const { return memstick_ / "PSP" / "SAVEDATA"; }
    std::string game(std::uint32_t p) const { return text(p + kGameName, 13); }
    fs::path dir(std::uint32_t p, const std::string &name) const { return root() / (game(p) + name); }

    Slot slot(const std::string &game, const std::string &name) const {
        Slot s;
        s.name = name;
        const fs::path d = root() / (game + name);
        std::error_code ec;
        s.exists = fs::is_directory(d, ec);
        if (!s.exists) return s;
        std::uint32_t parental = 0;
        auto sfo = parse_sfo(read_all(d / "PARAM.SFO"), parental);
        s.title = sfo["TITLE"];
        s.savedata_title = sfo["SAVEDATA_TITLE"];
        s.detail = sfo["SAVEDATA_DETAIL"];
        for (const auto &file : fs::directory_iterator(d, ec)) s.time = std::max(s.time, modified(file.path()));
        return s;
    }

    std::vector<std::string> saves(const std::string &game, const std::string &pattern) const {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(root(), ec)) {
            const std::string name = entry.path().filename().string();
            if (!entry.is_directory(ec) || name.size() < game.size() || name.compare(0, game.size(), game) != 0) continue;
            if (matches(pattern, name.substr(game.size()))) out.push_back(name.substr(game.size()));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::uint32_t load(std::uint32_t p, const std::string &name, bool rw_errors) {
        const fs::path d = dir(p, name);
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return rw_errors ? kRwNoData : kLoadNoData;
        const std::string file = text(p + kFileName, 13);
        if (!file.empty()) {
            if (!fs::exists(d / file, ec)) return rw_errors ? kRwFileNotFound : kLoadFileNotFound;
            copy_out(read_all(d / file), m_.load32(p + kDataBuf), m_.load32(p + kDataBufSize), p + kDataSize);
        }
        std::uint32_t parental = 0;
        auto sfo = parse_sfo(read_all(d / "PARAM.SFO"), parental);
        if (!sfo.empty()) {
            put_text(p + kTitle, 0x80u, sfo["TITLE"]);
            put_text(p + kSavedataTitle, 0x80u, sfo["SAVEDATA_TITLE"]);
            put_text(p + kDetail, 0x400u, sfo["SAVEDATA_DETAIL"]);
            m_.store8(p + kParental, static_cast<std::uint8_t>(parental));
        }
        for (std::uint32_t i = 0; i < 4u; ++i) {
            const std::uint32_t data = p + kIcon0 + i * 16u;
            if (m_.load32(data) == 0u || m_.load32(data + 4u) == 0u) continue;
            const auto bytes = read_all(d / kSystemFiles[i]);
            if (!bytes.empty()) copy_out(bytes, m_.load32(data), m_.load32(data + 4u), data + 8u);
        }
        return 0u;
    }

    std::uint32_t save(std::uint32_t p, const std::string &name, bool data_only) {
        const fs::path d = dir(p, name);
        std::error_code ec;
        fs::create_directories(d, ec);
        if (ec) return kSaveAccessError;
        const std::string file = text(p + kFileName, 13);
        const std::uint32_t buf = m_.load32(p + kDataBuf), size = m_.load32(p + kDataSize);
        if (!file.empty() && buf != 0u) {
            const std::uint8_t *data = m_.raw_pointer(buf, size);
            if ((data == nullptr && size != 0u) || !write_all(d / file, data, size)) return kSaveAccessError;
        }
        if (!data_only || !fs::exists(d / "PARAM.SFO", ec)) {
            const std::vector<SfoEntry> sfo = {
                {"CATEGORY", 0x0204u, "MS", 0u, 4u},
                {"PARENTAL_LEVEL", 0x0404u, {}, m_.load8(p + kParental), 4u},
                {"SAVEDATA_DETAIL", 0x0204u, text(p + kDetail, 0x400u), 0u, 0x400u},
                {"SAVEDATA_DIRECTORY", 0x0204u, game(p) + name, 0u, 0x40u},
                {"SAVEDATA_FILE_LIST", 0x0004u, {}, 0u, 0xC60u},
                {"SAVEDATA_PARAMS", 0x0004u, {}, 0u, 0x80u},
                {"SAVEDATA_TITLE", 0x0204u, text(p + kSavedataTitle, 0x80u), 0u, 0x80u},
                {"TITLE", 0x0204u, text(p + kTitle, 0x80u), 0u, 0x80u},
            };
            const auto bytes = build_sfo(sfo);
            if (!write_all(d / "PARAM.SFO", bytes.data(), bytes.size())) return kSaveAccessError;
        }
        for (std::uint32_t i = 0; i < 4u && !data_only; ++i) {
            const std::uint32_t data = p + kIcon0 + i * 16u, address = m_.load32(data), length = m_.load32(data + 8u);
            if (address == 0u || length == 0u) continue;
            if (const std::uint8_t *bytes = m_.raw_pointer(address, length)) write_all(d / kSystemFiles[i], bytes, length);
        }
        return 0u;
    }

    std::uint32_t remove(std::uint32_t p, const std::string &name) {
        const fs::path d = dir(p, name);
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return kDeleteNoData;
        fs::remove_all(d, ec);
        return 0u;
    }

    std::uint32_t erase(std::uint32_t p, const std::string &name) {
        const fs::path d = dir(p, name);
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return kRwNoData;
        const std::string file = text(p + kFileName, 13);
        if (file.empty() || !fs::remove(d / file, ec)) return kRwFileNotFound;
        return 0u;
    }

    std::uint32_t sizes(std::uint32_t p) {
        std::uint32_t result = 0u;
        if (const std::uint32_t free = m_.load32(p + kMsFree)) {
            m_.store32(free, 0x8000u);
            m_.store32(free + 4u, 0x8000u);
            m_.store32(free + 8u, 0x100000u);
            put_text(free + 12u, 8u, "1 GB");
        }
        if (const std::uint32_t data = m_.load32(p + kMsData)) {
            const fs::path d = root() / (text(data, 13) + text(data + 16u, 20));
            std::error_code ec;
            if (fs::is_directory(d, ec)) {
                std::uint64_t bytes = 0u;
                for (const auto &file : fs::directory_iterator(d, ec)) bytes += file.is_regular_file(ec) ? file.file_size(ec) : 0u;
                usage(data + 36u, bytes);
            } else {
                usage(data + 36u, 0u);
                result = kSizesNoData;
            }
        }
        if (const std::uint32_t needed = m_.load32(p + kUtilityData)) {
            std::uint64_t bytes = m_.load32(p + kDataSize) + 0x1330u;
            for (std::uint32_t i = 0; i < 4u; ++i) bytes += m_.load32(p + kIcon0 + i * 16u + 8u);
            usage(needed, bytes);
        }
        return result;
    }

    std::uint32_t list(std::uint32_t p) {
        const std::uint32_t ids = m_.load32(p + kIdList);
        if (ids == 0u) return 0u;
        const std::uint32_t max = m_.load32(ids), entries = m_.load32(ids + 8u);
        const auto names = saves(game(p), text(p + kSaveName, 20));
        std::uint32_t count = 0;
        for (const auto &name : names) {
            if (count >= max || entries == 0u) break;
            const std::uint32_t e = entries + count * 72u;
            const std::time_t when = slot(game(p), name).time;
            m_.store32(e, 0x11FFu);
            for (std::uint32_t t = 0; t < 3u; ++t) date(e + 4u + t * 16u, when);
            put_text(e + 52u, 20u, name);
            ++count;
        }
        m_.store32(ids + 4u, count);
        return 0u;
    }

    std::uint32_t files(std::uint32_t p) {
        const std::uint32_t list = m_.load32(p + kFileList);
        const fs::path d = dir(p, text(p + kSaveName, 20));
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return kRwNoData;
        if (list == 0u) return 0u;
        std::uint32_t counts[3] = {}, written[3] = {};
        for (const auto &file : fs::directory_iterator(d, ec)) {
            if (!file.is_regular_file(ec)) continue;
            const std::string name = file.path().filename().string();
            const bool system = name == "PARAM.SFO" ||
                                std::any_of(std::begin(kSystemFiles), std::end(kSystemFiles), [&](const char *s) { return name == s; });
            const int kind = system ? 2 : 1;
            ++counts[kind];
            const std::uint32_t max = m_.load32(list + static_cast<std::uint32_t>(kind) * 4u);
            const std::uint32_t entries = m_.load32(list + 24u + static_cast<std::uint32_t>(kind) * 4u);
            if (entries == 0u || written[kind] >= max) continue;
            const std::uint32_t e = entries + written[kind]++ * 80u;
            const std::uint64_t size = file.file_size(ec);
            m_.store32(e, 0x21FFu);
            m_.store32(e + 4u, 0u);
            m_.store32(e + 8u, static_cast<std::uint32_t>(size));
            m_.store32(e + 12u, static_cast<std::uint32_t>(size >> 32u));
            for (std::uint32_t t = 0; t < 3u; ++t) date(e + 16u + t * 16u, modified(file.path()));
            put_text(e + 64u, 16u, name);
        }
        for (int kind = 0; kind < 3; ++kind) m_.store32(list + 12u + static_cast<std::uint32_t>(kind) * 4u, written[kind]);
        return 0u;
    }

private:
    void copy_out(const std::vector<std::uint8_t> &bytes, std::uint32_t address, std::uint32_t capacity, std::uint32_t size_field) {
        const auto length = static_cast<std::uint32_t>(std::min<std::size_t>(bytes.size(), capacity));
        if (address != 0u && length != 0u)
            if (std::uint8_t *out = m_.raw_pointer(address, length)) std::memcpy(out, bytes.data(), length);
        m_.store32(size_field, length);
    }

    void usage(std::uint32_t at, std::uint64_t bytes) {
        const auto clusters = static_cast<std::uint32_t>((bytes + 0x7FFFu) / 0x8000u);
        const std::uint32_t kb = clusters * 32u;
        char label[8];
        std::snprintf(label, sizeof label, "%u KB", kb);
        m_.store32(at, clusters);
        m_.store32(at + 4u, kb);
        put_text(at + 8u, 8u, label);
        m_.store32(at + 16u, kb);
        put_text(at + 20u, 8u, label);
    }

    void date(std::uint32_t at, std::time_t when) {
        std::tm t{};
        localtime_r(&when, &t);
        m_.store16(at, static_cast<std::uint16_t>(t.tm_year + 1900));
        m_.store16(at + 2u, static_cast<std::uint16_t>(t.tm_mon + 1));
        m_.store16(at + 4u, static_cast<std::uint16_t>(t.tm_mday));
        m_.store16(at + 6u, static_cast<std::uint16_t>(t.tm_hour));
        m_.store16(at + 8u, static_cast<std::uint16_t>(t.tm_min));
        m_.store16(at + 10u, static_cast<std::uint16_t>(t.tm_sec));
        m_.store32(at + 12u, 0u);
    }

    psprecomp::GuestMemory &m_;
    const fs::path &memstick_;
};

struct Savedata {
    std::uint32_t status = 0, params = 0, mode = 0;
    bool waiting = false;
    std::vector<Slot> slots;
    std::unique_ptr<Store> store;
};

Savedata &state(std::shared_ptr<void> &slot) {
    if (!slot) slot = std::make_shared<Savedata>();
    return *static_cast<Savedata *>(slot.get());
}

void saves_changed() {
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (Module.pspSavesChanged) Module.pspSavesChanged(); });
#endif
}

int initial_choice(const std::vector<Slot> &slots, std::uint32_t focus, const std::string &current) {
    const auto n = static_cast<int>(slots.size());
    auto pick = [&](bool newest) {
        int best = -1;
        for (int i = 0; i < n; ++i)
            if (slots[static_cast<std::size_t>(i)].exists &&
                (best < 0 || (newest ? slots[static_cast<std::size_t>(i)].time > slots[static_cast<std::size_t>(best)].time
                                     : slots[static_cast<std::size_t>(i)].time < slots[static_cast<std::size_t>(best)].time)))
                best = i;
        return best;
    };
    auto empty = [&](bool first) {
        for (int k = 0; k < n; ++k) {
            const int i = first ? k : n - 1 - k;
            if (!slots[static_cast<std::size_t>(i)].exists) return i;
        }
        return -1;
    };
    int choice = -1;
    switch (focus) {
    case 1: choice = 0; break;
    case 2: choice = n - 1; break;
    case 3: choice = pick(true); break;
    case 4: choice = pick(false); break;
    case 7: choice = empty(true); break;
    case 8: choice = empty(false); break;
    default:
        for (int i = 0; i < n; ++i)
            if (slots[static_cast<std::size_t>(i)].name == current) choice = i;
        break;
    }
    return std::clamp(choice, 0, n - 1);
}

} // namespace

void Kernel::install_savedata() {
    Savedata &sd = state(savedata_);
    sd.store = std::make_unique<Store>(rt_.memory(), memstick_root_);
    auto add = [this](std::uint32_t nid, const char *name, std::function<std::uint32_t(Ctx &)> body) {
        rt_.register_hle("sceUtility", nid, [body](psprecomp::Runtime &, Ctx &ctx) { finish(ctx, body(ctx)); });
        rt_.nids().add("sceUtility", nid, name);
        char key[40];
        std::snprintf(key, sizeof key, "sceUtility:0x%08X", nid);
        registered_.insert(key);
    };

    add(0x50C4CD57u, "sceUtilitySavedataInitStart", [&sd, this](Ctx &ctx) {
        Store &s = *sd.store;
        auto &m = rt_.memory();
        const std::uint32_t p = ctx.gpr[4], mode = m.load32(p + kMode);
        const std::string name = s.text(p + kSaveName, 20);
        std::cerr << "[savedata] mode " << mode << " " << s.game(p) << name << " file " << s.text(p + kFileName, 13) << "\n";
        sd = Savedata{1u, p, mode, false, {}, std::move(sd.store)};
        std::uint32_t result = 0u;
        bool changed = false;
        switch (mode) {
        case kAutoLoad: case kLoad: result = s.load(p, name, false); break;
        case kReadData: case kReadDataSecure: result = s.load(p, name, true); break;
        case kAutoSave: case kSave: case kMakeData: case kMakeDataSecure:
            result = s.save(p, name, false);
            changed = true;
            break;
        case kWriteData: case kWriteDataSecure:
            result = s.save(p, name, true);
            changed = true;
            break;
        case kAutoDelete: case kDelete: case kDeleteData:
            result = s.remove(p, name);
            changed = true;
            break;
        case kErase: case kEraseSecure:
            result = s.erase(p, name);
            changed = true;
            break;
        case kSizes: case kGetSize: result = s.sizes(p); break;
        case kList: result = s.list(p); break;
        case kFiles: result = s.files(p); break;
        case kListLoad: case kListSave: case kListDelete: case kListAllDelete: {
            std::vector<std::string> names;
            if (const std::uint32_t list = m.load32(p + kSaveNameList); list != 0u && mode != kListAllDelete)
                for (std::uint32_t i = 0; i < 256u; ++i) {
                    const std::string n = s.text(list + i * 20u, 20);
                    if (n.empty()) break;
                    names.push_back(n);
                }
            if (names.empty()) names = mode == kListSave ? std::vector<std::string>{name} : s.saves(s.game(p), "");
            for (const auto &n : names) {
                Slot slot = s.slot(s.game(p), n);
                if (mode == kListSave || slot.exists) sd.slots.push_back(std::move(slot));
            }
            if (sd.slots.empty()) {
                result = mode == kListLoad ? kLoadNoData : kDeleteNoData;
                break;
            }
            const int focus = initial_choice(sd.slots, m.load32(p + kFocus), name);
            sd.waiting = true;
#ifdef __EMSCRIPTEN__
            std::string json = std::string("{\"mode\":\"") + (mode == kListLoad ? "load" : mode == kListSave ? "save" : "delete") +
                               "\",\"focus\":" + std::to_string(focus) + ",\"root\":" + json_string(s.root().string()) + ",\"game\":" +
                               json_string(s.game(p)) + ",\"slots\":[";
            for (std::size_t i = 0; i < sd.slots.size(); ++i) {
                const Slot &slot = sd.slots[i];
                json += std::string(i ? "," : "") + "{\"name\":" + json_string(slot.name) + ",\"exists\":" + (slot.exists ? "true" : "false") +
                        ",\"title\":" + json_string(slot.title) + ",\"subtitle\":" + json_string(slot.savedata_title) +
                        ",\"detail\":" + json_string(slot.detail) + ",\"time\":" + std::to_string(static_cast<long long>(slot.time)) + "}";
            }
            json += "]}";
            if (EM_ASM_INT({ return Module.pspSaveDialog ? (Module.pspSaveDialog(JSON.parse(UTF8ToString($0))), 1) : 0; }, json.c_str()))
                return 0u;
            savedata_choose(focus);
#else
            const char *forced = std::getenv("PSPWEB_SAVE_CHOICE");
            savedata_choose(forced != nullptr ? std::atoi(forced) : focus);
#endif
            return 0u;
        }
        default:
            std::cerr << "[savedata] unsupported mode " << mode << "\n";
            break;
        }
        m.store32(p + kResult, result);
        if (changed && result == 0u) saves_changed();
        return 0u;
    });
    add(0x8874DBE0u, "sceUtilitySavedataGetStatus", [&sd](Ctx &) {
        const std::uint32_t status = sd.status;
        if (sd.status == 1u) sd.status = 2u;
        else if (sd.status == 2u && !sd.waiting) sd.status = 3u;
        else if (sd.status == 4u) sd.status = 0u;
        return status;
    });
    add(0xD4B95FFBu, "sceUtilitySavedataUpdate", [](Ctx &) { return 0u; });
    add(0x9790B33Cu, "sceUtilitySavedataShutdownStart", [&sd](Ctx &) {
        sd.status = 4u;
        return 0u;
    });
}

void Kernel::savedata_choose(int index) {
    Savedata &sd = state(savedata_);
    if (!sd.waiting) return;
    sd.waiting = false;
    Store &s = *sd.store;
    const std::uint32_t p = sd.params;
    std::uint32_t result = kCanceled;
    if (index >= 0 && static_cast<std::size_t>(index) < sd.slots.size()) {
        const std::string name = sd.slots[static_cast<std::size_t>(index)].name;
        s.put_text(p + kSaveName, 20u, name);
        if (sd.mode == kListLoad) {
            result = s.load(p, name, false);
        } else if (sd.mode == kListSave) {
            result = s.save(p, name, false);
        } else {
            result = s.remove(p, name);
        }
        std::cerr << "[savedata] chose " << name << " -> 0x" << std::hex << result << std::dec << "\n";
        if (sd.mode != kListLoad && result == 0u) saves_changed();
    }
    rt_.memory().store32(p + kResult, result);
}

} // namespace pspweb
