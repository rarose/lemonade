#include "lemon/cold_storage.h"
#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using lemon::ColdStorage;
using lemon::ColdStorageRequestError;
using lemon::ColdStorageUnavailableError;
using lemon::DownloadProgress;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::json;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static std::string gguf(size_t size = 64 * 1024) {
    std::string data = "GGUF";
    data.resize(size, 'x');
    return data;
}

static void write_repo(const fs::path& hf, const std::string& repo,
                       const std::vector<std::string>& files) {
    const fs::path dir = hf / ("models--org--" + repo);
    write_file(dir / "refs" / "main", "snap");
    for (const auto& f : files) {
        write_file(dir / "snapshots" / "snap" / f, gguf());
    }
}

static bool has_staging(const fs::path& root) {
    return fs::exists(root / ColdStorage::kStagingDir);
}

template <typename E, typename F>
static bool throws(F&& fn) {
    try {
        fn();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

struct Env {
    fs::path root;
    fs::path hf;
    fs::path cold;
};

static void test_disabled(ModelManager& manager, const Env& env) {
    check("disabled: not enabled", !manager.cold_storage_enabled());
    check("disabled: model is not cold", !manager.get_model_info("user.alpha").cold);
    check("disabled: freeze is rejected",
          throws<ColdStorageRequestError>([&] { manager.freeze_model("user.alpha"); }));
    check("disabled: thaw is a no-op",
          !manager.thaw_model(manager.get_model_info("user.alpha")));
    check("disabled: no index file written", !fs::exists(env.root / "cold_storage.json"));
    check("disabled: hot files untouched", fs::exists(env.hf / "models--org--alpha"));
}

static void enable(ModelManager& manager, const Env& env) {
    fs::create_directories(env.cold);
    const std::string id = ColdStorage::attach(path_to_utf8(env.cold), true);
    manager.configure_cold_storage(path_to_utf8(env.cold), id);
}

static void test_attach(const Env& env) {
    const std::string first = ColdStorage::attach(path_to_utf8(env.cold), true);
    const std::string second = ColdStorage::attach(path_to_utf8(env.cold), true);
    check("attach: marker written", fs::exists(env.cold / ColdStorage::kMarkerFile));
    check("attach: existing marker is adopted", first == second && first.size() == 32);
    check("attach: missing directory is not created",
          throws<ColdStorageUnavailableError>([&] {
              ColdStorage::attach(path_to_utf8(env.root / "not-mounted"), true);
          }) && !fs::exists(env.root / "not-mounted"));
}

static void test_freeze_and_thaw(ModelManager& manager, const Env& env, const char* label) {
    std::string name = std::string(label) + ": ";
    manager.freeze_model("user.alpha");
    ModelInfo info = manager.get_model_info("user.alpha");
    check((name + "frozen model is cold and not downloaded").c_str(), info.cold && !info.downloaded);
    check((name + "hot repo removed").c_str(), !fs::exists(env.hf / "models--org--alpha"));
    check((name + "cold repo present").c_str(),
          fs::exists(env.cold / "models--org--alpha" / "snapshots" / "snap" / "model.gguf"));
    bool listed = false;
    for (const auto& [public_name, model] : manager.get_downloaded_models()) {
        listed = listed || (model.cold && public_name.find("alpha") != std::string::npos);
    }
    check((name + "cold model listed with downloaded models").c_str(), listed);
    check((name + "no staging left").c_str(), !has_staging(env.cold) && !has_staging(env.hf));

    manager.download_registered_model(info, true);
    info = manager.get_model_info("user.alpha");
    check((name + "thawed model is downloaded").c_str(), info.downloaded && !info.cold);
    check((name + "hot repo restored").c_str(),
          fs::exists(env.hf / "models--org--alpha" / "snapshots" / "snap" / "model.gguf"));
    check((name + "cold repo removed").c_str(), !fs::exists(env.cold / "models--org--alpha"));
}

static void test_shared_main_repo(ModelManager& manager, const Env& env) {
    manager.freeze_model("user.beta-q4");
    const fs::path hot_snap = env.hf / "models--org--beta" / "snapshots" / "snap";
    check("shared main: frozen variant removed from hot", !fs::exists(hot_snap / "q4.gguf"));
    check("shared main: sibling variant stays hot", fs::exists(hot_snap / "q8.gguf"));
    check("shared main: sibling still downloaded", manager.get_model_info("user.beta-q8").downloaded);
    check("shared main: frozen variant is cold", manager.get_model_info("user.beta-q4").cold);

    manager.thaw_model(manager.get_model_info("user.beta-q4"));
    check("shared main: thaw restores variant", fs::exists(hot_snap / "q4.gguf"));
    check("shared main: thawed model downloaded", manager.get_model_info("user.beta-q4").downloaded);
    check("shared main: cold copy removed", !fs::exists(env.cold / "models--org--beta"));
}

static void test_shared_aux_repo(ModelManager& manager, const Env& env) {
    manager.freeze_model("user.gamma");
    check("shared aux: own repo moved", !fs::exists(env.hf / "models--org--gamma"));
    check("shared aux: shared repo stays hot", fs::exists(env.hf / "models--org--proj"));
    check("shared aux: shared repo copied to cold", fs::exists(env.cold / "models--org--proj"));
    check("shared aux: other model still downloaded", manager.get_model_info("user.delta").downloaded);
    check("shared aux: frozen model is cold", manager.get_model_info("user.gamma").cold);

    manager.thaw_model(manager.get_model_info("user.gamma"));
    check("shared aux: thawed model downloaded", manager.get_model_info("user.gamma").downloaded);
    check("shared aux: cold copies removed",
          !fs::exists(env.cold / "models--org--proj") && !fs::exists(env.cold / "models--org--gamma"));
}

static void test_cancel(ModelManager& manager, const Env& env) {
    lemon::set_cold_storage_force_copy_for_test(true);
    int calls = 0;
    bool cancelled = throws<lemon::TransferCancelledError>([&] {
        manager.freeze_model("user.alpha", [&](const DownloadProgress&) { return ++calls < 2; });
    });
    lemon::set_cold_storage_force_copy_for_test(false);
    ModelInfo info = manager.get_model_info("user.alpha");
    check("cancel: freeze reports cancellation", cancelled);
    check("cancel: source intact", fs::exists(env.hf / "models--org--alpha" / "snapshots" / "snap" / "model.gguf"));
    check("cancel: model still downloaded", info.downloaded && !info.cold);
    check("cancel: nothing left in cold storage",
          !fs::exists(env.cold / "models--org--alpha") && !has_staging(env.cold));
}

static void test_identity(ModelManager& manager, const Env& env) {
    const fs::path marker = env.cold / ColdStorage::kMarkerFile;
    const std::string id = ColdStorage::attach(path_to_utf8(env.cold), false);

    manager.freeze_model("user.alpha");
    fs::rename(marker, env.root / "marker.bak");
    check("identity: missing marker reported",
          manager.cold_storage_status(true).reason == "marker_missing");
    check("identity: thaw refused when marker missing",
          throws<ColdStorageUnavailableError>([&] {
              manager.download_registered_model(manager.get_model_info("user.alpha"), true);
          }));
    check("identity: hot copy not recreated", !fs::exists(env.hf / "models--org--alpha"));
    check("identity: freeze refused when marker missing",
          throws<ColdStorageUnavailableError>([&] { manager.freeze_model("user.delta"); }) &&
          fs::exists(env.hf / "models--org--delta"));

    write_file(marker, json{{"id", std::string(32, 'a')}}.dump());
    check("identity: different drive reported",
          manager.cold_storage_status(true).reason == "id_mismatch");
    check("identity: model reports unavailable drive via status",
          manager.get_model_info("user.alpha").cold && !manager.cold_storage_status().available);

    const std::string adopted = ColdStorage::attach(path_to_utf8(env.cold), false);
    manager.configure_cold_storage(path_to_utf8(env.cold), adopted);
    check("identity: adopting re-pins the id", manager.cold_storage_status(true).available);
    check("identity: models frozen to the old drive are no longer cold here",
          !manager.get_model_info("user.alpha").cold);

    fs::remove(marker);
    fs::rename(env.root / "marker.bak", marker);
    manager.configure_cold_storage(path_to_utf8(env.cold), id);
    check("identity: original drive back", manager.cold_storage_status(true).available &&
          manager.get_model_info("user.alpha").cold);

    manager.configure_cold_storage(path_to_utf8(env.root / "not-mounted"), id);
    check("identity: missing directory reported",
          manager.cold_storage_status(true).reason == "directory_missing");
    manager.configure_cold_storage(path_to_utf8(env.cold), id);
    manager.thaw_model(manager.get_model_info("user.alpha"));
}

static void test_delete(ModelManager& manager, const Env& env) {
    manager.freeze_model("user.delta");
    manager.delete_model("user.delta");
    check("delete: cold repo removed", !fs::exists(env.cold / "models--org--delta"));
    check("delete: shared repo kept hot for other models", fs::exists(env.hf / "models--org--proj"));
    bool registered = true;
    try {
        manager.get_model_info("user.delta");
    } catch (const std::exception&) {
        registered = false;
    }
    check("delete: user model unregistered", !registered);
}

static void test_concurrent_load(ModelManager& manager, const Env& env) {
    lemon::set_cold_storage_force_copy_for_test(true);
    std::atomic<bool> started{false};
    std::thread freezer([&] {
        manager.freeze_model("user.alpha", [&](const DownloadProgress&) {
            if (!started.exchange(true)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            return true;
        });
    });
    while (!started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    manager.download_registered_model(manager.get_model_info("user.alpha"), true);
    freezer.join();
    lemon::set_cold_storage_force_copy_for_test(false);
    ModelInfo info = manager.get_model_info("user.alpha");
    check("concurrent: load during freeze ends with the model hot", info.downloaded && !info.cold);
    check("concurrent: files restored",
          fs::exists(env.hf / "models--org--alpha" / "snapshots" / "snap" / "model.gguf"));
}

int main() {
    Env env;
    env.root = fs::temp_directory_path() /
        ("cold_storage_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    env.hf = env.root / "hf";
    env.cold = env.root / "cold";
    fs::create_directories(env.hf);

    lemon::utils::set_cache_dir(path_to_utf8(env.root));
    lemon::utils::set_config_dir(path_to_utf8(env.root));
    lemon::utils::set_models_dir(path_to_utf8(env.hf));

    write_repo(env.hf, "alpha", {"model.gguf"});
    write_repo(env.hf, "beta", {"q4.gguf", "q8.gguf"});
    write_repo(env.hf, "gamma", {"model.gguf"});
    write_repo(env.hf, "delta", {"model.gguf"});
    write_repo(env.hf, "proj", {"mmproj.gguf"});

    json user_models = {
        {"alpha", {{"checkpoint", "org/alpha:model.gguf"}, {"recipe", "llamacpp"}}},
        {"beta-q4", {{"checkpoint", "org/beta:q4.gguf"}, {"recipe", "llamacpp"}}},
        {"beta-q8", {{"checkpoint", "org/beta:q8.gguf"}, {"recipe", "llamacpp"}}},
        {"gamma", {{"checkpoints", {{"main", "org/gamma:model.gguf"}, {"mmproj", "org/proj:mmproj.gguf"}}},
                   {"recipe", "llamacpp"}}},
        {"delta", {{"checkpoints", {{"main", "org/delta:model.gguf"}, {"mmproj", "org/proj:mmproj.gguf"}}},
                   {"recipe", "llamacpp"}}},
    };
    write_file(env.root / "user_models.json", user_models.dump(2));

    {
        ModelManager manager;
        check("fixture: models resolve as downloaded",
              manager.get_model_info("user.alpha").downloaded &&
              manager.get_model_info("user.gamma").downloaded);
        test_disabled(manager, env);
        enable(manager, env);
        test_attach(env);
        check("enabled: drive available", manager.cold_storage_status(true).available);
        test_freeze_and_thaw(manager, env, "rename");
        lemon::set_cold_storage_force_copy_for_test(true);
        test_freeze_and_thaw(manager, env, "copy");
        lemon::set_cold_storage_force_copy_for_test(false);
        test_shared_main_repo(manager, env);
        test_shared_aux_repo(manager, env);
        test_cancel(manager, env);
        test_identity(manager, env);
        test_concurrent_load(manager, env);
        test_delete(manager, env);
    }

    fs::remove_all(env.root);

    if (g_failures == 0) {
        std::printf("All cold storage tests passed.\n");
    } else {
        std::printf("%d cold storage test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
