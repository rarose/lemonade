#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

// CONTRACT: HTTP handlers map this to 503 with {"code": kColdStorageUnavailableCode}.
// Load paths only see the exception message, so they detect it by the prefix.
constexpr const char* kColdStorageUnavailableCode = "cold_storage_unavailable";
constexpr const char* kColdStorageUnavailablePrefix = "Cold storage unavailable: ";
class ColdStorageUnavailableError : public std::runtime_error {
public:
    explicit ColdStorageUnavailableError(const std::string& message)
        : std::runtime_error(kColdStorageUnavailablePrefix + message) {}
};

// Thrown when a model cannot be frozen (wrong kind of model, not downloaded,
// feature disabled). HTTP handlers map this to 400.
class ColdStorageRequestError : public std::invalid_argument {
public:
    ColdStorageRequestError(const std::string& code, const std::string& message)
        : std::invalid_argument(message), code_(code) {}
    const std::string& code() const { return code_; }
private:
    std::string code_;
};

class TransferCancelledError : public std::runtime_error {
public:
    TransferCancelledError() : std::runtime_error("Transfer cancelled") {}
};

struct ColdStorageStatus {
    bool enabled = false;
    bool available = false;
    std::string reason;  // ok, not_configured, not_initialized, directory_missing,
                         // marker_missing, id_mismatch, io_error, timeout
    std::string dir;
    std::string id;
    std::string marker_id;
    std::uint64_t free_bytes = 0;

    nlohmann::json to_json() const;
    std::string describe() const;
};

struct ColdIndexEntry {
    std::string storage_id;
    std::string registry_source;
    std::vector<std::string> repos;  // cache directory names, e.g. models--org--name
    std::uint64_t bytes = 0;
    std::string frozen_at;
};

enum class TransferMode { Move, Copy };

struct TransferProgress {
    std::string file;
    std::uint64_t file_bytes_done = 0;
    std::uint64_t file_bytes_total = 0;
    std::uint64_t bytes_done = 0;   // across the whole operation
    std::uint64_t bytes_total = 0;
    int file_index = 0;             // 1-based, across the whole operation
    int total_files = 0;
};

// Returns false to cancel. Cancellation is only honored before commit.
using TransferProgressFn = std::function<bool(const TransferProgress&)>;

class ColdStorage {
public:
    explicit ColdStorage(std::filesystem::path index_path);

    void configure(const std::string& dir, const std::string& id);
    bool enabled() const;
    std::string dir() const;
    std::string id() const;

    // Cached for a few seconds; verification runs off-thread with a timeout so
    // a hung network mount cannot block callers indefinitely.
    ColdStorageStatus status(bool force = false);
    void verify_or_throw();

    // Returns the id in dir's marker, writing a new marker when there is none
    // and create_if_missing is set. Never creates dir itself: a missing
    // directory is usually an unmounted drive.
    static std::string attach(const std::string& dir, bool create_if_missing);

    bool has_entry(const std::string& model) const;
    bool is_cold(const std::string& model) const;
    std::optional<ColdIndexEntry> entry(const std::string& model) const;
    void put(const std::string& model, const ColdIndexEntry& entry);
    void erase(const std::string& model);
    bool repo_referenced_by_others(const std::string& repo_dir, const std::string& exclude_model) const;
    bool index_empty() const;

    bool begin_transfer(const std::string& model);
    void end_transfer(const std::string& model);
    bool is_transferring(const std::string& model) const;
    bool any_transfers() const;
    std::size_t transfer_count() const;

    static constexpr const char* kMarkerFile = ".lemonade-cold-storage.json";
    static constexpr const char* kStagingDir = ".lemonade-staging";

private:
    void ensure_index_loaded_locked() const;
    void save_index_locked() const;

    std::filesystem::path index_path_;
    mutable std::mutex mutex_;
    std::string dir_;
    std::string id_;
    mutable bool index_loaded_ = false;
    mutable std::map<std::string, ColdIndexEntry> index_;
    std::set<std::string> transfers_;

    std::mutex status_mutex_;
    std::optional<ColdStorageStatus> cached_status_;
    std::chrono::steady_clock::time_point cached_at_;
};

// Bytes that transfer_repo would copy for this repo directory.
std::uint64_t cold_transfer_bytes(const std::filesystem::path& repo_dir, int* file_count = nullptr);

// Moves or copies one cache repo directory into dst_root. Copies go through a
// staging directory and are size-verified before being renamed into place, so
// an interrupted transfer never leaves a half-written repo at the destination.
// before_commit runs after verification and before anything at the
// destination is modified; it may throw to abort.
void transfer_repo(const std::filesystem::path& src_repo,
                   const std::filesystem::path& dst_root,
                   TransferMode mode,
                   const TransferProgressFn& progress,
                   TransferProgress& state,
                   const std::function<void()>& before_commit = nullptr);

void cleanup_cold_staging(const std::filesystem::path& root);

void set_cold_storage_force_copy_for_test(bool force);

} // namespace lemon
