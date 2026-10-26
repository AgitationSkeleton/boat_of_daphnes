// See anchor_content.h.
#include <cstdio>
#undef rename // bw_posix_compat.h; <filesystem> declares rename()
#include <filesystem>

#include "anchor_content.h"

#include "chat_portrait.h"
#include "content_validate.h"
#include "model_colors.h"
#include "mods.h"
#include "rarc.h"
#include "voice_bank.h"

#define PICOJSON_USE_INT64
#include "picojson.h"

#include "platform/bod_platform.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace content {
namespace {

namespace fs = std::filesystem;
namespace cv = content_validate;

constexpr size_t kMaxFile = cv::kMaxFile;
constexpr size_t kMaxPack = cv::kMaxPack;
constexpr size_t kMaxManifest = cv::kMaxManifest;
constexpr uintmax_t kCacheLimit = 500ull * 1024 * 1024;

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<std::function<void()>> g_jobs;
std::thread g_worker;
bool g_stop = false;

std::string g_base;
std::string g_local_hash;
bool g_local_body = false; // the advertised content has the player's own body (parts.link)
bool g_local_changed = false;
std::string g_published_model = "\x01"; // never a model's name: forces the first check
std::string g_published_base;
bool g_publishing = false;
bool g_was_sharing = false;
bool g_off_handled = false; // the cache was purged since sharing was last on (once per OFF, also at start)
unsigned long long g_last_check = 0;
// Counts the times sharing was turned off. A job that began in an earlier epoch is stale: whatever it downloaded is
// thrown away, and it writes nothing to the cache.
unsigned long long g_epoch = 1;

struct Peer {
    std::string hash;
    bool fetching = false, done = false, failed = false;
};
std::map<int, Peer> g_peers;
std::vector<Ready> g_ready;
std::vector<int> g_cleared;

std::mutex g_cache_mutex; // the cache folder: stores and the purge do not interleave

// Voice banks by voice id (the hash of the init and wave files), shared between the players that have the same one. A bank
// lives as long as a peer holds it. It holds the compressed waves its voices use and decodes a voice when first played
// (voice_bank.h), so 16 distinct banks come to a few MB; a new one is refused (the puppet keeps the local bank) when the live
// ones already hold kMaxBankMemory, and a voice is refused when all banks' PCM reaches voice_bank::kMaxDecodedTotal.
constexpr size_t kMaxBankMemory = 128u * 1024 * 1024;
std::mutex g_bank_mutex;
std::map<std::string, std::weak_ptr<const voice_bank::Bank>> g_banks;

std::shared_ptr<const voice_bank::Bank> bank_for(const std::string& voice_id, const std::vector<uint8_t>& aaf, const std::vector<uint8_t>& aw, size_t* live_bytes,
                                                 bool* shared, int* live_banks) {
    std::lock_guard<std::mutex> lock(g_bank_mutex);
    size_t live = 0;
    int banks = 0;
    if (shared != nullptr)
        *shared = false;
    for (auto it = g_banks.begin(); it != g_banks.end();) {
        if (const auto b = it->second.lock()) {
            if (it->first == voice_id) {
                if (shared != nullptr)
                    *shared = true;
                if (live_bytes != nullptr)
                    *live_bytes = b->Bytes();
                return b;
            }
            live += b->Bytes();
            ++banks;
            ++it;
        } else
            it = g_banks.erase(it);
    }
    if (live_bytes != nullptr)
        *live_bytes = live;
    if (live_banks != nullptr)
        *live_banks = banks;
    if (live >= kMaxBankMemory)
        return nullptr;
    auto bank = std::make_shared<voice_bank::Bank>();
    std::string why;
    if (!voice_bank::Build(aaf, aw, *bank, &why)) {
        std::fprintf(stderr, "[content] the voice bank %.12s could not be decoded: %s\n", voice_id.c_str(), why.c_str());
        return nullptr;
    }
    g_banks[voice_id] = bank;
    return bank;
}

fs::path cache_dir() {
    static const fs::path dir = [] {
        fs::path p = bodplat::DataFolder();
        if (p.empty())
            p = fs::current_path();
        return p / "cache" / "anchor-content";
    }();
    return dir;
}

void worker_main() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return g_stop || !g_jobs.empty(); });
            if (g_stop)
                return;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
    }
}

} // namespace

void Shutdown() {
    // The worker is a std::thread still joinable at exit, whose destructor calls std::terminate (a crash report at every
    // quit). It is told to stop and let go: a download in progress ends with the process.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        g_wake.notify_all();
    }
    if (g_worker.joinable())
        g_worker.detach();
}

namespace {

void enqueue(std::function<void()> job) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_worker.joinable())
        g_worker = std::thread(worker_main);
    g_jobs.push_back(std::move(job));
    g_wake.notify_one();
}

bool still_on(unsigned long long epoch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return epoch == g_epoch && g_was_sharing;
}

// ----------------------------------------------------------------- HTTP ----

using Http = bodplat::HttpResult;

// BLUEWAKE_TEST_CONTENT_DELAY=<ms> (testing): every download takes that much longer, so a scenario can turn sharing
// off while one is in flight.
unsigned test_delay_ms() {
    static const unsigned ms = [] {
        const char* v = std::getenv("BLUEWAKE_TEST_CONTENT_DELAY");
        return v != nullptr ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return ms;
}

Http request(const std::string& base, const char* method, const std::string& path, const std::vector<uint8_t>* body,
             size_t max_response) {
    std::fprintf(stderr, "[content] %s %.60s\n", method, path.c_str());
    Http h = bodplat::HttpRequest(base, method, path, body, max_response);
    if (test_delay_ms() != 0 && std::strcmp(method, "GET") == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(test_delay_ms()));
    return h;
}

// ---------------------------------------------------------------- cache ----
// Files here are named by the SHA-256 of their bytes (64 hex digits, checked) and nothing else: nothing a peer says
// is ever used as a file name.

bool is_hex64(const std::string& s) { return cv::IsHex64(s); }

bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void touch(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
}

// Oldest files go first until the cache fits.
void trim_cache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uintmax_t total = 0;
    for (fs::directory_iterator it(cache_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2))
            continue;
        total += it->file_size(e2);
        files.emplace_back(it->last_write_time(e2), it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& [time, path] : files) {
        if (total <= kCacheLimit)
            break;
        std::error_code e2;
        const uintmax_t size = fs::file_size(path, e2);
        if (fs::remove(path, e2))
            total -= std::min(total, size);
    }
}

// Writes a downloaded blob, unless sharing was turned off since its download began (the cache was purged then).
void store(const std::string& hash, const std::vector<uint8_t>& bytes, unsigned long long epoch) {
    if (!is_hex64(hash))
        return;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (!still_on(epoch)) // the epoch was raised before the purge, which waits for this lock: a stale store never lands
        return;
    std::error_code ec;
    fs::create_directories(cache_dir(), ec);
    const fs::path tmp = cache_dir() / (hash + ".part");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const fs::path dest = cache_dir() / hash;
    fs::rename(tmp, dest, ec); // replaces an existing file, on Windows too (MoveFileEx with REPLACE_EXISTING)
    if (ec)
        fs::remove(tmp, ec);
}

// A file by hash: the cache's, or downloaded and checked against the hash and size.
bool fetch_blob(const std::string& base, const std::string& hash, size_t size, std::vector<uint8_t>& out, unsigned long long epoch) {
    if (!is_hex64(hash) || size > kMaxFile || !still_on(epoch))
        return false;
    const fs::path cached = cache_dir() / hash;
    if (read_file(cached, out) && out.size() == size && Sha256Hex(out.data(), out.size()) == hash) {
        touch(cached);
        return true;
    }
    const Http h = request(base, "GET", "/content/" + hash, nullptr, kMaxFile + 1024);
    if (!still_on(epoch)) // turned off while it downloaded: thrown away
        return false;
    if (!h.ok || h.status != 200 || h.body.size() != size || Sha256Hex(h.body.data(), h.body.size()) != hash)
        return false;
    out = h.body;
    store(hash, out, epoch);
    trim_cache();
    return true;
}

// ------------------------------------------------------------ publishing ----

// What the player's choice is made of, as a manifest.
void publish(std::string base, std::string model) {
    std::string name;
    std::vector<mods::PackFile> files;
    mods::SharedParts parts;
    std::string hash;
    bool body_shared = false;
    if (!model.empty() && mods::ActivePlayerPack(name, files, parts)) {
        // Our own files are checked as a peer would check them: a damaged mod is not sent (the server would refuse it).
        std::vector<mods::PackFile> kept;
        for (auto& f : files) {
            std::string why;
            bool ok = false;
            if (f.role == "model" || f.role == "equipment") {
                std::vector<uint8_t> copy = f.bytes;
                ok = cv::ExpandArchive(copy, &why);
            } else if (f.path == "sound/JaiInit.aaf") {
                ok = cv::ValidateAaf(f.bytes, &why);
            } else {
                ok = cv::ValidateWave(f.bytes, &why);
            }
            if (ok)
                kept.push_back(std::move(f));
            else
                std::fprintf(stderr, "[content] %s %s is not shared: %s\n", name.c_str(), f.path.c_str(), why.c_str());
        }
        files = std::move(kept);
        const auto has = [&](const char* role) {
            return std::any_of(files.begin(), files.end(), [&](const mods::PackFile& f) { return f.role == role; });
        };
        if (!has("model")) {
            parts.link = false;
            if (parts.equipment == "model")
                parts.equipment.clear();
        }
        if (parts.equipment == "equipment" && !has("equipment"))
            parts.equipment.clear();
        const size_t voice_files = static_cast<size_t>(std::count_if(files.begin(), files.end(), [](const mods::PackFile& f) { return f.role == "voice"; }));
        if (voice_files < 2) {
            files.erase(std::remove_if(files.begin(), files.end(), [](const mods::PackFile& f) { return f.role == "voice"; }), files.end());
            parts.voice = false;
        }
        // Over the pack limit: the voice goes first, then the separate equipment.
        auto total = [&] {
            size_t t = 0;
            for (const auto& f : files)
                t += f.bytes.size();
            return t;
        };
        for (const char* drop : {"voice", "equipment"})
            if (total() > kMaxPack) {
                files.erase(std::remove_if(files.begin(), files.end(), [&](const mods::PackFile& f) { return f.role == drop; }), files.end());
                if (std::string(drop) == "voice")
                    parts.voice = false;
                else if (parts.equipment == "equipment")
                    parts.equipment.clear();
            }
        if (total() <= kMaxPack && !files.empty()) {
            std::sort(files.begin(), files.end(), [](const mods::PackFile& a, const mods::PackFile& b) { return a.path < b.path; });
            // Only what the server lacks goes up. The voice is optional: a server that does not take it (an older one)
            // still gets the body and the equipment.
            const auto upload = [&](const std::string& h, const std::vector<uint8_t>& bytes, const char* kind,
                                     const std::string& what) {
                const Http head = request(base, "HEAD", "/content/" + h, nullptr, 0);
                if (head.ok && head.status == 200)
                    return true;
                const Http put = request(base, "PUT", "/content/" + h + (kind ? std::string("?kind=") + kind : ""), &bytes, 4096);
                if (!put.ok || (put.status != 201 && put.status != 204)) {
                    // The server's refusal names the part and why (its error body, JSON { error }); a transport failure has no status.
                    std::string why(put.body.begin(), put.body.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(put.body.size(), 200)));
                    for (char& c : why)
                        if (c < 32 || c > 126)
                            c = ' ';
                    std::fprintf(stderr, "[content] upload of %.12s (%s, %s, %zu bytes) failed: status %d%s, server said \"%s\"\n", h.c_str(),
                                 what.c_str(), kind ? kind : "archive", bytes.size(), put.status, put.ok ? "" : " (no response)", why.c_str());
                    return false;
                }
                std::fprintf(stderr, "[content] uploaded %.12s (%s, %s, %zu bytes): status %d\n", h.c_str(), what.c_str(), kind ? kind : "archive",
                             bytes.size(), put.status);
                return true;
            };
            bool ok = true;
            picojson::array list;
            for (auto it = files.begin(); it != files.end();) {
                const std::string h = Sha256Hex(it->bytes.data(), it->bytes.size());
                const char* kind = it->role == "voice" ? (it->path == "sound/JaiInit.aaf" ? "aaf" : "wave") : nullptr;
                if (!upload(h, it->bytes, kind, it->path)) {
                    if (it->role == "voice") { // the voice is dropped altogether
                        std::fprintf(stderr, "[content] the voice is not shared: %s was refused by the server\n", it->path.c_str());
                        parts.voice = false;
                        for (auto v = list.begin(); v != list.end();)
                            v = v->get("role").to_str() == "voice" ? list.erase(v) : v + 1;
                        it = files.erase(std::remove_if(files.begin(), files.end(), [](const mods::PackFile& f) { return f.role == "voice"; }), files.end());
                        continue;
                    }
                    ok = false;
                    break;
                }
                picojson::object o;
                o["path"] = picojson::value(it->path);
                o["role"] = picojson::value(it->role);
                o["sha256"] = picojson::value(h);
                o["size"] = picojson::value(static_cast<int64_t>(it->bytes.size()));
                list.push_back(picojson::value(o));
                ++it;
            }
            // The colours of the body's model: its metadata.txt and colour masks, as a section of their own beside "files"
            // (older clients read "files" alone, and limit it to 64 entries; they never see this). Optional all through: a
            // server that does not know the kinds (an older one) refuses the first upload, and the pack goes without.
            picojson::object colors;
            if (ok && parts.link) {
                model_colors::Files mf;
                if (mods::ActiveModelFiles(mf) && mf.name == name && !mf.metadata.empty()) {
                    std::vector<uint8_t> meta(mf.metadata.begin(), mf.metadata.end());
                    std::string why;
                    picojson::object section;
                    picojson::array masks;
                    size_t bytes = 0;
                    bool good = cv::ValidateMetadata(meta, &why);
                    if (!good)
                        std::fprintf(stderr, "[content] %s: metadata.txt is not shared: %s\n", name.c_str(), why.c_str());
                    if (good) {
                        const std::string h = Sha256Hex(meta.data(), meta.size());
                        good = upload(h, meta, "metadata", "metadata.txt");
                        picojson::object e;
                        e["sha256"] = picojson::value(h);
                        e["size"] = picojson::value(static_cast<int64_t>(meta.size()));
                        section["metadata"] = picojson::value(e);
                    }
                    int skipped = 0;
                    for (auto& [path, png] : mf.masks) {
                        if (!good || masks.size() >= cv::kMaxMasks)
                            break;
                        std::vector<uint8_t> bytes_png = png;
                        std::string why2;
                        if (!cv::MaskPathOk(path) || !cv::SanitizeMask(bytes_png, &why2) || !cv::ValidateMask(bytes_png, &why2) ||
                            bytes + bytes_png.size() > cv::kMaxMasksTotal) {
                            ++skipped;
                            std::fprintf(stderr, "[content] %s: mask %s is not shared (%s)\n", name.c_str(), path.c_str(), why2.c_str());
                            continue;
                        }
                        const std::string h = Sha256Hex(bytes_png.data(), bytes_png.size());
                        if (!upload(h, bytes_png, "mask", path)) {
                            good = false;
                            break;
                        }
                        bytes += bytes_png.size();
                        picojson::object e;
                        e["path"] = picojson::value(path);
                        e["sha256"] = picojson::value(h);
                        e["size"] = picojson::value(static_cast<int64_t>(bytes_png.size()));
                        masks.push_back(picojson::value(e));
                    }
                    if (good && !masks.empty()) {
                        section["masks"] = picojson::value(masks);
                        colors = std::move(section);
                        std::fprintf(stderr, "[content] %s: colour pack of %zu masks (%zu bytes) and metadata.txt, %d masks left out\n", name.c_str(),
                                     masks.size(), bytes, skipped);
                    } else if (!good)
                        std::fprintf(stderr, "[content] %s: the colour pack is not shared (the server refused a part)\n", name.c_str());
                }
            }
            if (ok && !list.empty()) {
                picojson::object p;
                p["link"] = picojson::value(parts.link);
                p["equipment"] = picojson::value(parts.equipment);
                p["voice"] = picojson::value(parts.voice);
                picojson::object m;
                m["v"] = picojson::value(int64_t{1});
                m["name"] = picojson::value(name.substr(0, 60));
                m["files"] = picojson::value(list);
                m["parts"] = picojson::value(p);
                if (!colors.empty())
                    m["colors"] = picojson::value(colors);
                std::string text = picojson::value(m).serialize();
                if (text.size() > kMaxManifest && m.erase("colors")) // the masks' list does not fit: the pack goes without
                    text = picojson::value(m).serialize();
                const std::vector<uint8_t> manifest(text.begin(), text.end());
                const std::string manifest_hash = Sha256Hex(manifest.data(), manifest.size());
                if (manifest.size() <= kMaxManifest && upload(manifest_hash, manifest, "manifest", "manifest")) {
                    hash = manifest_hash;
                    body_shared = parts.link;
                    std::fprintf(stderr, "[content] sharing %s: link=%d equipment=%s voice=%d, %zu files, %zu bytes, manifest %.12s\n", name.c_str(),
                                 parts.link ? 1 : 0, parts.equipment.empty() ? "game" : parts.equipment.c_str(), parts.voice ? 1 : 0, files.size(),
                                 total(), hash.c_str());
                }
            }
        } else
            std::fprintf(stderr, "[content] %s is over %zu MB: not shared\n", name.c_str(), kMaxPack >> 20);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_local_body = body_shared;
    if (g_local_hash != hash) {
        g_local_hash = hash;
        g_local_changed = true;
    }
    g_publishing = false;
}

// -------------------------------------------------------------- fetching ----

void fetch_peer(std::string base, int id, std::string hash, unsigned long long epoch) {
    Ready ready;
    ready.id = id;
    ready.hash = hash;
    bool any = false;
    std::vector<uint8_t> manifest;
    std::fprintf(stderr, "[content] fetching manifest %.12s for player %d\n", hash.c_str(), id);
    const bool have_manifest = [&] {
        if (!still_on(epoch))
            return false;
        // The manifest's size is not known: it is checked by its hash alone.
        const fs::path cached = cache_dir() / hash;
        if (read_file(cached, manifest) && manifest.size() <= kMaxManifest && Sha256Hex(manifest.data(), manifest.size()) == hash) {
            touch(cached);
            return true;
        }
        const Http h = request(base, "GET", "/content/" + hash, nullptr, kMaxManifest + 1);
        if (!still_on(epoch) || !h.ok || h.status != 200 || h.body.empty() || Sha256Hex(h.body.data(), h.body.size()) != hash)
            return false;
        manifest = h.body;
        store(hash, manifest, epoch);
        return true;
    }();
    if (have_manifest) {
        cv::Manifest m;
        std::string why;
        if (!cv::ParseManifest(std::string(manifest.begin(), manifest.end()), m, &why)) {
            std::fprintf(stderr, "[content] manifest %.12s is refused: %s\n", hash.c_str(), why.c_str());
        } else {
            ready.name = m.name;
            const auto find = [&](const char* path) -> const cv::File* {
                for (const auto& f : m.files)
                    if (f.path == path)
                        return &f;
                return nullptr;
            };
            // An older client's manifest has no parts: its Link.arc is the body, and only the body (as before).
            const bool want_body = !m.has_parts || m.link;
            const std::string equip_from = m.has_parts ? m.equipment : "";
            // Archives are fetched once and used for both parts when they are the same file.
            std::map<std::string, std::vector<uint8_t>> archives; // path -> validated, expanded
            const auto archive = [&](const char* path) -> const std::vector<uint8_t>* {
                const auto have = archives.find(path);
                if (have != archives.end())
                    return &have->second;
                const cv::File* f = find(path);
                std::vector<uint8_t> bytes;
                std::string why2;
                if (f == nullptr || !fetch_blob(base, f->sha256, f->size, bytes, epoch))
                    return nullptr;
                if (!cv::ExpandArchive(bytes, &why2)) {
                    std::fprintf(stderr, "[content] %s %.12s from player %d is refused: %s\n", path, f->sha256.c_str(), id, why2.c_str());
                    return nullptr;
                }
                return &(archives[path] = std::move(bytes));
            };
            if (want_body) {
                if (const auto* arc = archive("Link.arc")) {
                    if (cv::ExtractBody(*arc, ready.body, &why)) {
                        cv::ExtractBodyParts(*arc, ready.hands, ready.hair);
                        ready.has_body = true;
                        any = true;
                    }
                }
            }
            if (ready.has_body && m.metadata.size != 0) {
                // The model's colours (a newer client's manifest): metadata.txt and the masks, each checked as it arrives; a
                // part that fails is left out, and the puppet keeps the colours it has.
                const auto* arc = archive("Link.arc");
                model_colors::Files files;
                files.name = m.name;
                std::vector<uint8_t> meta;
                std::string r1;
                if (arc != nullptr && fetch_blob(base, m.metadata.sha256, m.metadata.size, meta, epoch) && cv::ValidateMetadata(meta, &r1)) {
                    files.metadata.assign(meta.begin(), meta.end());
                    files.link_arc = *arc;
                    for (const auto& f : m.masks) {
                        std::vector<uint8_t> png;
                        std::string r2;
                        if (!fetch_blob(base, f.sha256, f.size, png, epoch))
                            continue;
                        if (!cv::ValidateMask(png, &r2)) {
                            std::fprintf(stderr, "[content] mask %s from player %d is refused: %s\n", f.path.c_str(), id, r2.c_str());
                            continue;
                        }
                        files.masks[f.path] = std::move(png);
                    }
                    auto set = std::make_shared<model_colors::Set>();
                    std::string r3;
                    if (still_on(epoch) && model_colors::Build(files, *set, &r3)) {
                        std::fprintf(stderr, "[content] player %d's model colours: %zu Hero's and %zu Outset colors, %zu textures, %zu mask regions from %zu mask files\n", id,
                                     set->outfits[0].colors.size(), set->outfits[1].colors.size(), set->textures.size(), set->masks.size(), files.masks.size());
                        if (!set->outfits[0].colors.empty() || !set->outfits[1].colors.empty())
                            ready.colors = std::move(set);
                    } else
                        std::fprintf(stderr, "[content] player %d's model colours could not be built: %s\n", id, r3.c_str());
                } else
                    std::fprintf(stderr, "[content] player %d's metadata.txt could not be used (%s)\n", id, r1.c_str());
            }
            if (!equip_from.empty()) {
                const auto* arc = archive(equip_from == "model" ? "Link.arc" : "equipment/Link.arc");
                if (arc != nullptr && cv::ExtractEquipment(*arc, ready.equipment, &why)) {
                    ready.has_equipment = true;
                    any = true;
                }
            }
            if (m.has_parts && m.voice) {
                const cv::File *aaf = find("sound/JaiInit.aaf"), *wave = find("sound/voice_0.aw"), *ganon = find("sound/GanonT_0.aw");
                std::vector<uint8_t> a, w, g;
                std::string r1, r2;
                if (aaf && wave && fetch_blob(base, aaf->sha256, aaf->size, a, epoch) && fetch_blob(base, wave->sha256, wave->size, w, epoch) &&
                    cv::ValidateAaf(a, &r1) && cv::ValidateWave(w, &r2) &&
                    (ganon == nullptr || (fetch_blob(base, ganon->sha256, ganon->size, g, epoch) && cv::ValidateWave(g, &r2)))) {
                    ready.voice = aaf->sha256.substr(0, 16) + wave->sha256.substr(0, 16);
                    size_t live = 0;
                    bool shared = false;
                    int banks = 0;
                    ready.bank = bank_for(ready.voice, a, w, &live, &shared, &banks);
                    if (ready.bank)
                        std::fprintf(stderr, "[content] player %d's voice %s: bank %s (%d voices playable, %zu KB held; %d other banks hold %zu KB, PCM decoded in all banks %zu KB of %zu KB)\n", id,
                                     ready.voice.c_str(), shared ? "shared with another player" : "ready", ready.bank->Playable(), ready.bank->Bytes() >> 10, banks,
                                     live >> 10, voice_bank::DecodedTotal() >> 10, voice_bank::kMaxDecodedTotal >> 10);
                    else
                        std::fprintf(stderr, "[content] player %d's voice %s: bank not made, the local bank is used (%d banks hold %zu KB of %zu KB)\n", id, ready.voice.c_str(), banks,
                                     live >> 10, kMaxBankMemory >> 10);
                    any = true;
                } else
                    std::fprintf(stderr, "[content] the voice of player %d could not be used (%s %s)\n", id, r1.c_str(), r2.c_str());
            }
            std::fprintf(stderr, "[content] manifest %.12s (%s): body %d, equipment %d (%zu models), voice %s\n", hash.c_str(), m.name.c_str(),
                         ready.has_body ? 1 : 0, ready.has_equipment ? 1 : 0, ready.equipment.size(), ready.voice.empty() ? "game's" : ready.voice.c_str());
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (epoch != g_epoch || !g_was_sharing) { // sharing was turned off (or off and on) while this ran: nothing is applied
        std::fprintf(stderr, "[content] content %.12s for player %d is thrown away: sharing was turned off while it downloaded\n", hash.c_str(), id);
        return;
    }
    auto it = g_peers.find(id);
    if (it != g_peers.end() && it->second.hash == hash) {
        it->second.fetching = false;
        it->second.done = any;
        it->second.failed = !any;
        if (any) {
            g_ready.push_back(std::move(ready));
            std::fprintf(stderr, "[content] player %d's content %.12s is ready\n", id, hash.c_str());
        } else
            std::fprintf(stderr, "[content] player %d's content %.12s could not be used: their puppet is stock\n", id, hash.c_str());
    }
}

} // namespace

std::string Sha256Hex(const uint8_t* data, size_t size) {
    uint8_t digest[32] = {0};
    if (!bodplat::Sha256(data, size, digest))
        return "";
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : digest) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

int PurgeCache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    std::error_code ec;
    int removed = 0;
    // Only regular files directly in the cache folder (named by hash, or .part): never a path from anywhere else.
    for (fs::directory_iterator it(cache_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && fs::remove(it->path(), e2))
            ++removed;
    }
    return removed;
}

void Tick(const std::string& http_base, bool share, bool connected) {
    const unsigned long long now = bodplat::TickMs();
    std::string model;
    bool purge = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_base = http_base;
        if (!share || !connected) {
            // Not sharing: nothing of ours is advertised, and nobody's is used.
            if (g_was_sharing) {
                if (!g_local_hash.empty())
                    g_local_changed = true;
                g_local_hash.clear();
                g_published_model = "\x01";
                for (auto& [id, p] : g_peers) {
                    if (p.done)
                        g_cleared.push_back(id);
                    p = Peer{};
                }
                g_ready.clear();
                ++g_epoch; // downloads and uploads under way are stale
            }
            g_was_sharing = false;
            if (!share && !g_off_handled) {
                g_off_handled = true;
                purge = true;
            }
        } else {
            g_off_handled = false;
            g_was_sharing = true;
            if (g_publishing || now - g_last_check < 1500)
                return;
            g_last_check = now;
            model = mods::ActiveModel();
            if (model == g_published_model && http_base == g_published_base)
                return;
            g_published_model = model;
            g_published_base = http_base;
            g_publishing = true;
        }
    }
    if (purge) {
        const int n = PurgeCache() + chat_portrait::PurgeDisk();
        std::fprintf(stderr, "[content] sharing is off: %d cached files of other players removed\n", n);
        return;
    }
    if (!share || !connected)
        return;
    enqueue([http_base, model] { publish(http_base, model); });
}

std::string LocalHash() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_local_hash;
}

std::string LocalVoiceId() { return mods::LoadedVoiceId(); }

std::shared_ptr<const voice_bank::Bank> StockVoiceBank(std::string* id, bool* pending) {
    static std::shared_ptr<const voice_bank::Bank> bank; // held for the session: every stock player's puppet shares it
    static std::string bank_id;
    static bool tried;
    if (pending != nullptr)
        *pending = false;
    if (!tried) {
        std::vector<unsigned char> aaf, aw;
        if (!mods::VanillaVoiceFiles(aaf, aw)) {
            if (pending != nullptr)
                *pending = true;
            return nullptr;
        }
        tried = true;
        std::string hex = Sha256Hex(aaf.data(), aaf.size()).substr(0, 16) + Sha256Hex(aw.data(), aw.size()).substr(0, 16);
        bank_id = "disc-" + hex;
        bank = bank_for(bank_id, std::vector<uint8_t>(aaf.begin(), aaf.end()), std::vector<uint8_t>(aw.begin(), aw.end()), nullptr, nullptr, nullptr);
        if (bank)
            std::fprintf(stderr, "[content] the game's own voice bank %s decoded from the disc (%d voices playable, %zu KB held) for stock players' puppets\n",
                         bank_id.c_str(), bank->Playable(), bank->Bytes() >> 10);
        else
            std::fprintf(stderr, "[content] the game's own voice bank could not be made from the disc: stock players' puppets use the local bank\n");
    }
    if (id != nullptr)
        *id = bank_id;
    return bank;
}

bool LocalSharesBody() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_local_body && !g_local_hash.empty();
}

bool LocalChanged() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool c = g_local_changed;
    g_local_changed = false;
    return c;
}

void Want(int id, const std::string& hash) {
    std::string base;
    unsigned long long epoch = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_was_sharing)
            return;
        Peer& p = g_peers[id];
        if (p.hash == hash)
            return;
        if (p.done)
            g_cleared.push_back(id);
        p = Peer{};
        p.hash = hash;
        if (hash.empty() || !is_hex64(hash))
            return;
        p.fetching = true;
        base = g_base;
        epoch = g_epoch;
    }
    enqueue([base, id, hash, epoch] { fetch_peer(base, id, hash, epoch); });
}

void Forget(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_peers.find(id);
    if (it != g_peers.end()) {
        if (it->second.done)
            g_cleared.push_back(id);
        g_peers.erase(it);
    }
}

std::vector<Ready> TakeReady() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::exchange(g_ready, {});
}

std::vector<int> TakeCleared() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::exchange(g_cleared, {});
}

} // namespace content
