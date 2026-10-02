#include <lemon/cold_storage.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {

using utils::path_from_utf8;
using utils::path_to_utf8;

namespace {

constexpr auto kStatusTtl = std::chrono::seconds(5);
constexpr auto kVerifyTimeout = std::chrono::seconds(3);
constexpr std::size_t kCopyChunkBytes = 8 * 1024 * 1024;
constexpr std::uint64_t kFreeSpaceMargin = 256ull * 1024 * 1024;

std::atomic<bool> g_force_copy{false};

bool is_valid_id(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string generate_id() {
    std::random_device rd;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i) {
        out << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(rd());
    }
    return out.str();
}

std::string utc_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string host_name() {
#ifdef _WIN32
    const char* name = std::getenv("COMPUTERNAME");
    return name ? name : "";
#else
    char buf[256] = {0};
    return gethostname(buf, sizeof(buf) - 1) == 0 ? std::string(buf) : std::string();
#endif
}

void flush_file_to_disk(const fs::path& path) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

void flush_directory(const fs::path& dir) {
#ifndef _WIN32
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

void write_file_durably(const fs::path& path, const std::string& content) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Failed to write " + path_to_utf8(tmp));
        }
        out << content;
        out.close();
        if (!out) {
            throw std::runtime_error("Failed to write " + path_to_utf8(tmp));
        }
    }
    flush_file_to_disk(tmp);
    std::error_code ec;
    if (!utils::atomic_replace_file(tmp, path, ec)) {
        throw std::runtime_error("Failed to replace " + path_to_utf8(path) + ": " + ec.message());
    }
    flush_directory(path.parent_path());
}

std::string read_marker_id(const fs::path& marker) {
    std::ifstream in(marker, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read " + path_to_utf8(marker));
    }
    json j = json::parse(in);
    std::string id = j.value("id", std::string());
    if (!is_valid_id(id)) {
        throw std::runtime_error("Malformed cold storage marker " + path_to_utf8(marker));
    }
    return id;
}

ColdStorageStatus compute_status(const std::string& dir, const std::string& id) {
    ColdStorageStatus s;
    s.dir = dir;
    s.id = id;
    s.enabled = !dir.empty();
    if (!s.enabled) {
        s.reason = "not_configured";
        return s;
    }
    if (id.empty()) {
        s.reason = "not_initialized";
        return s;
    }
    try {
        const fs::path root = path_from_utf8(dir);
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            s.reason = "directory_missing";
            return s;
        }
        const fs::path marker = root / ColdStorage::kMarkerFile;
        if (!fs::exists(marker, ec)) {
            s.reason = "marker_missing";
            return s;
        }
        s.marker_id = read_marker_id(marker);
        if (s.marker_id != id) {
            s.reason = "id_mismatch";
            return s;
        }
        auto space = fs::space(root, ec);
        if (!ec) {
            s.free_bytes = space.available;
        }
        s.available = true;
        s.reason = "ok";
    } catch (const std::exception& e) {
        LOG(WARNING, "ColdStorage") << "Cold storage check failed: " << e.what() << std::endl;
        s.reason = "io_error";
    }
    return s;
}

bool is_skipped_transfer_file(const fs::path& p) {
    const std::string name = p.filename().string();
    return name == ".download_manifest.json" ||
           (name.size() > 8 && name.compare(name.size() - 8, 8, ".partial") == 0);
}

struct TransferFile {
    fs::path rel;
    std::uint64_t size = 0;
};

std::vector<TransferFile> list_transfer_files(const fs::path& repo) {
    std::vector<TransferFile> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(repo, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        throw std::runtime_error("Cannot read " + path_to_utf8(repo) + ": " + ec.message());
    }
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            throw std::runtime_error("Cannot read " + path_to_utf8(repo) + ": " + ec.message());
        }
        const fs::path rel = it->path().lexically_relative(repo);
        // Snapshot entries are dereferenced while copying, so the blob store is redundant.
        if (it.depth() == 0 && rel == "blobs") {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code sec;
        if (!fs::is_regular_file(it->path(), sec) || is_skipped_transfer_file(rel)) {
            continue;
        }
        files.push_back({rel, static_cast<std::uint64_t>(fs::file_size(it->path(), sec))});
        if (sec) {
            throw std::runtime_error("Cannot stat " + path_to_utf8(it->path()) + ": " + sec.message());
        }
    }
    return files;
}

bool report(const TransferProgressFn& progress, const TransferProgress& state) {
    return !progress || progress(state);
}

void copy_file_with_progress(const fs::path& src, const fs::path& dst,
                             const TransferProgressFn& progress, TransferProgress& state) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open " + path_to_utf8(src));
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Cannot create " + path_to_utf8(dst));
    }
    std::vector<char> buffer(kCopyChunkBytes);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        out.write(buffer.data(), got);
        if (!out) {
            throw std::runtime_error("Failed writing " + path_to_utf8(dst) +
                " (destination full, or file too large for the destination filesystem?)");
        }
        state.bytes_done += static_cast<std::uint64_t>(got);
        state.file_bytes_done += static_cast<std::uint64_t>(got);
        if (!report(progress, state)) {
            throw TransferCancelledError();
        }
    }
    if (in.bad()) {
        throw std::runtime_error("Failed reading " + path_to_utf8(src));
    }
    out.close();
    if (!out) {
        throw std::runtime_error("Failed writing " + path_to_utf8(dst));
    }
    flush_file_to_disk(dst);
}

void remove_staging(const fs::path& staging_root) {
    std::error_code ec;
    fs::remove_all(staging_root, ec);
    const fs::path parent = staging_root.parent_path();
    if (fs::is_directory(parent, ec) && fs::is_empty(parent, ec)) {
        fs::remove(parent, ec);
    }
}

std::string read_small_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void merge_tree_into(const fs::path& staged_repo, const fs::path& dst_repo) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staged_repo, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) {
            continue;
        }
        const fs::path rel = it->path().lexically_relative(staged_repo);
        const fs::path target = dst_repo / rel;
        std::error_code tec;
        if (fs::exists(target, tec)) {
            if (rel == fs::path("refs") / "main") {
                if (read_small_file(target) != read_small_file(it->path())) {
                    LOG(WARNING, "ColdStorage") << "Keeping existing " << path_to_utf8(target)
                        << "; the transferred snapshot differs from the active one" << std::endl;
                }
                continue;
            }
            if (fs::file_size(target, tec) == fs::file_size(it->path(), tec) && !tec) {
                continue;
            }
        }
        fs::create_directories(target.parent_path(), tec);
        fs::rename(it->path(), target, tec);
        if (tec) {
            throw std::runtime_error("Failed to move " + path_to_utf8(it->path()) + " to " +
                                     path_to_utf8(target) + ": " + tec.message());
        }
    }
    if (ec) {
        throw std::runtime_error("Failed to merge into " + path_to_utf8(dst_repo) + ": " + ec.message());
    }
}

}  // namespace

json ColdStorageStatus::to_json() const {
    json j = {
        {"enabled", enabled},
        {"available", available},
        {"reason", reason},
        {"dir", dir},
        {"id", id},
    };
    if (!marker_id.empty()) {
        j["marker_id"] = marker_id;
    }
    if (available) {
        j["free_bytes"] = free_bytes;
    }
    return j;
}

std::string ColdStorageStatus::describe() const {
    if (reason == "directory_missing") {
        return "Cold storage directory '" + dir + "' does not exist (is the drive mounted?)";
    }
    if (reason == "marker_missing") {
        return "Cold storage directory '" + dir + "' has no Lemonade marker file "
               "(wrong drive, or not mounted?). Run `lemonade cold-storage adopt` if this is intended";
    }
    if (reason == "id_mismatch") {
        return "Cold storage directory '" + dir + "' belongs to a different cold storage drive. "
               "Run `lemonade cold-storage adopt` to use this drive instead";
    }
    if (reason == "not_initialized") {
        return "Cold storage directory '" + dir + "' has not been initialized. "
               "Run `lemonade cold-storage adopt` to use it";
    }
    if (reason == "timeout") {
        return "Cold storage directory '" + dir + "' did not respond (stale network mount?)";
    }
    if (reason == "io_error") {
        return "Cold storage directory '" + dir + "' could not be read";
    }
    if (reason == "not_configured") {
        return "Cold storage is not configured";
    }
    return "Cold storage is available";
}

ColdStorage::ColdStorage(fs::path index_path) : index_path_(std::move(index_path)) {}

void ColdStorage::configure(const std::string& dir, const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir_ = dir;
        id_ = id;
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    cached_status_.reset();
}

bool ColdStorage::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !dir_.empty();
}

std::string ColdStorage::dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dir_;
}

std::string ColdStorage::id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id_;
}

ColdStorageStatus ColdStorage::status(bool force) {
    std::string dir;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = dir_;
        id = id_;
    }
    if (dir.empty()) {
        return compute_status(dir, id);
    }

    std::lock_guard<std::mutex> lock(status_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!force && cached_status_ && now - cached_at_ < kStatusTtl) {
        return *cached_status_;
    }

    auto promise = std::make_shared<std::promise<ColdStorageStatus>>();
    auto future = promise->get_future();
    std::thread([promise, dir, id]() {
        promise->set_value(compute_status(dir, id));
    }).detach();

    ColdStorageStatus s;
    if (future.wait_for(kVerifyTimeout) == std::future_status::ready) {
        s = future.get();
    } else {
        s.enabled = true;
        s.dir = dir;
        s.id = id;
        s.reason = "timeout";
    }
    cached_status_ = s;
    cached_at_ = now;
    return s;
}

void ColdStorage::verify_or_throw() {
    ColdStorageStatus s = status(true);
    if (!s.available) {
        throw ColdStorageUnavailableError(s.describe());
    }
}

std::string ColdStorage::attach(const std::string& dir, bool create_if_missing) {
    const fs::path root = path_from_utf8(dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        throw ColdStorageUnavailableError(
            "Cold storage directory '" + dir + "' does not exist (is the drive mounted?)");
    }
    const fs::path marker = root / kMarkerFile;
    if (fs::exists(marker, ec)) {
        return read_marker_id(marker);
    }
    if (!create_if_missing) {
        throw ColdStorageUnavailableError(
            "Cold storage directory '" + dir + "' has no Lemonade marker file");
    }
    const std::string id = generate_id();
    json j = {
        {"id", id},
        {"created", utc_timestamp()},
        {"hostname", host_name()},
    };
    write_file_durably(marker, j.dump(2));
    LOG(INFO, "ColdStorage") << "Initialized cold storage at " << dir << std::endl;
    return id;
}

void ColdStorage::ensure_index_loaded_locked() const {
    if (index_loaded_) {
        return;
    }
    index_loaded_ = true;
    std::error_code ec;
    if (!fs::exists(index_path_, ec)) {
        return;
    }
    try {
        json j = utils::JsonUtils::load_from_file(path_to_utf8(index_path_));
        if (!j.contains("models") || !j["models"].is_object()) {
            return;
        }
        for (auto& [name, e] : j["models"].items()) {
            ColdIndexEntry entry;
            entry.storage_id = e.value("storage_id", std::string());
            entry.registry_source = e.value("registry_source", std::string());
            entry.repos = e.value("repos", std::vector<std::string>());
            entry.bytes = e.value("bytes", static_cast<std::uint64_t>(0));
            entry.frozen_at = e.value("frozen_at", std::string());
            index_[name] = std::move(entry);
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "ColdStorage") << "Ignoring unreadable " << path_to_utf8(index_path_)
                                    << ": " << e.what() << std::endl;
    }
}

void ColdStorage::save_index_locked() const {
    json models = json::object();
    for (const auto& [name, e] : index_) {
        models[name] = {
            {"storage_id", e.storage_id},
            {"registry_source", e.registry_source},
            {"repos", e.repos},
            {"bytes", e.bytes},
            {"frozen_at", e.frozen_at},
        };
    }
    json j = {{"version", 1}, {"models", models}};
    std::error_code ec;
    fs::create_directories(index_path_.parent_path(), ec);
    write_file_durably(index_path_, j.dump(2));
}

bool ColdStorage::has_entry(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dir_.empty()) {
        return false;
    }
    ensure_index_loaded_locked();
    return index_.count(model) > 0;
}

bool ColdStorage::is_cold(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dir_.empty()) {
        return false;
    }
    ensure_index_loaded_locked();
    auto it = index_.find(model);
    return it != index_.end() && it->second.storage_id == id_;
}

std::optional<ColdIndexEntry> ColdStorage::entry(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dir_.empty()) {
        return std::nullopt;
    }
    ensure_index_loaded_locked();
    auto it = index_.find(model);
    if (it == index_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void ColdStorage::put(const std::string& model, const ColdIndexEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_index_loaded_locked();
    index_[model] = entry;
    save_index_locked();
}

void ColdStorage::erase(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_index_loaded_locked();
    if (index_.erase(model) > 0) {
        save_index_locked();
    }
}

bool ColdStorage::repo_referenced_by_others(const std::string& repo_dir,
                                            const std::string& exclude_model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_index_loaded_locked();
    for (const auto& [name, e] : index_) {
        if (name == exclude_model || e.storage_id != id_) {
            continue;
        }
        if (std::find(e.repos.begin(), e.repos.end(), repo_dir) != e.repos.end()) {
            return true;
        }
    }
    return false;
}

bool ColdStorage::index_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_index_loaded_locked();
    return index_.empty();
}

bool ColdStorage::begin_transfer(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    return transfers_.insert(model).second;
}

void ColdStorage::end_transfer(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    transfers_.erase(model);
}

bool ColdStorage::is_transferring(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transfers_.count(model) > 0;
}

bool ColdStorage::any_transfers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !transfers_.empty();
}

std::size_t ColdStorage::transfer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transfers_.size();
}

std::uint64_t cold_transfer_bytes(const fs::path& repo_dir, int* file_count) {
    const auto files = list_transfer_files(repo_dir);
    std::uint64_t total = 0;
    for (const auto& f : files) {
        total += f.size;
    }
    if (file_count) {
        *file_count += static_cast<int>(files.size());
    }
    return total;
}

void transfer_repo(const fs::path& src_repo,
                   const fs::path& dst_root,
                   TransferMode mode,
                   const TransferProgressFn& progress,
                   TransferProgress& state,
                   const std::function<void()>& before_commit) {
    const fs::path name = src_repo.filename();
    const fs::path dst = dst_root / name;
    std::error_code ec;

    const std::vector<TransferFile> files = list_transfer_files(src_repo);
    std::uint64_t repo_bytes = 0;
    for (const auto& f : files) {
        repo_bytes += f.size;
    }

    if (mode == TransferMode::Move && !g_force_copy && !fs::exists(dst, ec)) {
        if (before_commit) {
            before_commit();
        }
        fs::rename(src_repo, dst, ec);
        if (!ec) {
            state.bytes_done += repo_bytes;
            state.file_index += static_cast<int>(files.size());
            report(progress, state);
            return;
        }
        LOG(INFO, "ColdStorage") << "Rename across devices not possible (" << ec.message()
                                 << "), copying " << path_to_utf8(name) << std::endl;
    }

    fs::create_directories(dst_root, ec);
    auto space = fs::space(dst_root, ec);
    if (!ec && space.available < repo_bytes + kFreeSpaceMargin) {
        throw std::runtime_error(
            "Not enough free space in " + path_to_utf8(dst_root) + " (need " +
            std::to_string(repo_bytes / (1024 * 1024)) + " MiB, " +
            std::to_string(space.available / (1024 * 1024)) + " MiB free)");
    }

    const fs::path staging_root = dst_root / ColdStorage::kStagingDir / generate_id();
    const fs::path staged_repo = staging_root / name;
    try {
        fs::create_directories(staged_repo);
        const bool merging = fs::exists(dst, ec);
        for (const auto& f : files) {
            const fs::path from = src_repo / f.rel;
            if (merging) {
                std::error_code xec;
                const fs::path existing = dst / f.rel;
                if (fs::exists(existing, xec) &&
                    (f.rel == fs::path("refs") / "main" || fs::file_size(existing, xec) == f.size)) {
                    state.bytes_done += f.size;
                    state.file_index += 1;
                    continue;
                }
            }
            const fs::path to = staged_repo / f.rel;
            fs::create_directories(to.parent_path());
            state.file = path_to_utf8(f.rel.filename());
            state.file_index += 1;
            state.file_bytes_done = 0;
            state.file_bytes_total = f.size;
            if (!report(progress, state)) {
                throw TransferCancelledError();
            }
            copy_file_with_progress(from, to, progress, state);
            std::error_code sec;
            const auto copied = fs::file_size(to, sec);
            if (sec || copied != f.size) {
                throw std::runtime_error("Size mismatch after copying " + path_to_utf8(from) +
                    " (destination filesystem may not support files this large)");
            }
        }
        flush_directory(staged_repo);
        if (before_commit) {
            before_commit();
        }
        if (!fs::exists(dst, ec)) {
            fs::rename(staged_repo, dst, ec);
            if (ec) {
                throw std::runtime_error("Failed to move " + path_to_utf8(staged_repo) + " to " +
                                         path_to_utf8(dst) + ": " + ec.message());
            }
        } else {
            merge_tree_into(staged_repo, dst);
        }
        flush_directory(dst_root);
    } catch (...) {
        remove_staging(staging_root);
        throw;
    }
    remove_staging(staging_root);

    if (mode == TransferMode::Move) {
        fs::remove_all(src_repo, ec);
        if (ec) {
            LOG(WARNING, "ColdStorage") << "Transferred " << path_to_utf8(name)
                << " but could not remove the source: " << ec.message() << std::endl;
        }
    }
}

void cleanup_cold_staging(const fs::path& root) {
    std::error_code ec;
    const fs::path staging = root / ColdStorage::kStagingDir;
    if (fs::exists(staging, ec)) {
        LOG(INFO, "ColdStorage") << "Removing leftover staging directory " << path_to_utf8(staging) << std::endl;
        fs::remove_all(staging, ec);
    }
}

void set_cold_storage_force_copy_for_test(bool force) {
    g_force_copy = force;
}

} // namespace lemon
