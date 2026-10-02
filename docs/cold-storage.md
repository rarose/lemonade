# Model Cold Storage

## Context
Large models (50–70 GB vLLM/FP16 checkpoints) fill the fast main models directory. Users want to park downloaded models on a big, slow disk (NAS, 8 TB+ USB drive) and bring them back later without re-downloading. This adds an opt-in `cold_storage_dir`: a model can be **frozen** (moved to cold storage) and **thawed** (moved back). Loading a frozen model thaws it automatically.

User decisions:
- Loading a cold model auto-thaws it (with progress), then loads.
- Icons: snowflake = freeze, sun = thaw, icy-blue status for cold models. The flame is already used for the "Hot" label.
- Repos shared with another hot model are **copied** to cold; exclusive repos are **moved**.
- **Hard requirement:** with `cold_storage_dir` empty (the default), behavior is byte-for-byte what it is today.
- **Drive identity:** a random-ID marker on the drive must match an ID in config. If it's missing or mismatched, cold storage is unavailable and nothing is ever written there. This catches an unmounted mountpoint or the wrong drive at the same path.

Status: implemented on branch `feature/cold-storage`. User-facing docs: [API](api/lemonade.md#cold-storage), [CLI](guide/cli.md#options-for-cold-storage), [config](guide/configuration/README.md).

Differences from the plan below, decided during implementation:
- The adopt endpoint is `POST /internal/cold-storage/adopt`, so it gets the existing admin-key gate for `/internal/*` routes.
- Restoring a cold model through `/pull` or a load returns as soon as the files are back; it does not also run a remote update check. Pull again to update.
- An unavailable drive is HTTP 503 (`cold_storage_unavailable`) everywhere.
- A model whose main file is the *same file* another downloaded model uses can't be frozen (moving it would break the other model).
- Lemonade only moves `refs/` and `snapshots/` (symlinks dereferenced); `blobs/` is skipped.
- Desktop UI: the snowflake shows only on downloaded, unloaded rows; cold rows get a sun (move back), play (move back and load), and delete. The Download Manager hides pause for moves, and cancelling a move never deletes files.

Note: AGENTS.md says UI changes are handled by core maintainers. Phase 3 is scoped so it can be split into a separate PR.

## Design summary
- **Status model:** a cold model keeps `downloaded=false` and gains `"cold": true`, which is emitted only for cold models. Existing clients already handle `downloaded=false` by calling `/pull` (`ensureModelReady` backendInstaller.ts:855, CLI `run` cli/main.cpp:545), and `/pull` thaws instead of downloading. So progress UI and CLI SSE work unchanged.
- **Single thaw hook:** every load path goes through `ModelManager::download_registered_model` (model_manager.cpp:4271): auto-load, `/load`, collections, Ollama, upscale, `/pull`. Thaw happens there.
- **Cold index:** `<config_dir>/cold_storage.json` is the source of truth for which models are cold. Entries are keyed by canonical model name: `{drive_id, repos[], registry_source, frozen_at}`. `/models` and `build_cache` never touch the NAS, so a stale mount can't hang listing.
- **Drive identity:**
  - **Marker:** `<cold_storage_dir>/.lemonade-cold-storage.json` holds `{id: <128-bit random hex>, created, hostname}`. Config holds `cold_storage_id`.
  - **Writing the marker:** happens only when `cold_storage_dir` is set via `/internal/set` / `lemonade config set`. If a marker already exists, its ID is adopted (re-attaching an existing drive). An existing directory with no marker gets a fresh one. A **missing directory is never created**, because it may be an unmounted mountpoint; status reports `directory_missing`.
  - **Hand-edited config.json** (dir set, ID empty): reports `not_initialized`. No auto-adopt, no marker write; the user runs adopt.
  - **Persisting the ID:** `handle_config_set` only persists the keys in its own `updated` result, so factor `persist_config_changes(const json&)` out of it (server.cpp ~7150) and use it from the side effect.
  - **Checking:** before every freeze/thaw/cold delete, again right before each commit rename, and lazily for status (5 s cached TTL). Reasons: `ok | not_configured | not_initialized | directory_missing | marker_missing | id_mismatch | io_error | timeout`.
  - **Hung network mounts:** verification is `stat`/read only and runs on a worker thread joined with a 3 s timeout, so `/models` and status can't hang. A transfer itself can still block inside the kernel; that's a known limitation.
  - **When not `available`:**
    - Cold models report `cold: true, cold_available: false`.
    - Freeze, thaw and auto-thaw fail with 503 `cold_storage_unavailable` and a clear message.
    - It never re-downloads and never writes to the path.
  - **Re-pinning:** an explicit `POST /cold-storage/adopt` re-pins the ID to the marker currently present. It is never automatic.
  - **Index entries for other drives:** entries whose `drive_id` ≠ the current `cold_storage_id` are treated as unavailable rather than cold-here.
- **Safe transfer:**
  - **Same filesystem:** use `rename`.
  - **Otherwise, copy:** copy into `<dest>/.lemonade-staging/<op-id>/`, flushing each file to disk, then verify file counts and sizes (catches the FAT32 4 GB limit). Then rename into place and flush the directory. Only after the index is updated is the source removed.
  - **Cancel and recovery:** cancel removes the staging directory. A lazy recovery sweep on first use deletes leftover staging directories.
  - **"Hot wins" rule:** if the files are complete in hot storage, the model is hot. This makes crashes mid-thaw safe.
  - **Step order:** freeze is copy → verify → commit into cold → write index → delete hot. Thaw is copy → verify → commit into hot → remove index entry → delete/GC cold. Symlinks are dereferenced when copying (NAS/exFAT often can't hold them); only `refs/` and `snapshots/` are copied.

| Crash point | Result |
|---|---|
| During copy | Only staging debris; swept on the next transfer |
| Freeze: after commit, before index write | Hot still complete → hot. The orphan cold repo is reused by the next freeze |
| Freeze: after index write, during hot delete | Hot incomplete → cold. Correct |
| Thaw: after commit, before index removal | Hot wins; stale entry cleaned on next delete/freeze |

## Disabled-mode gate points (all short-circuit on `!cold_storage_enabled()`)
| Gate | Location | When disabled |
|---|---|---|
| G1 | `set_cold_storage_dir` / ctor | no directory creation, no index read, no marker I/O |
| G2 | `build_cache` cold pass | skipped |
| G3 | `update_model_in_cache` / collection recompute | skipped |
| G4 | `is_model_downloaded` in-transfer check | no-op |
| G5 | `download_registered_model` thaw hook | skipped |
| G6 | `get_downloaded_models` (`/models` without show_all, Ollama tags, CLI list) | unchanged |
| G7 | `model_info_to_json` | no `cold` key |
| G8 | `handle_pull` offline-mode bypass | unchanged |
| G9 | jobs `load_op` (server.cpp:480) | unchanged |
| G10 | `delete_model` cold cleanup | skipped |
| G11 | `/freeze`, `/thaw`, `/cold-storage/*` | 400 `cold_storage_disabled` |
| G12 | progress JSON `operation` field | emitted only when non-empty |
| G13 | UI | snowflake/sun/banner only render when config dir non-empty or `info.cold` |
| G14 | CLI | `freeze`/`thaw` get 400; `list` marker only when `cold` present |

## Phase 1 — Server core
**Config**
- `src/cpp/resources/defaults.json`: add `"cold_storage_dir": ""` and `"cold_storage_id": ""`. These are hand-maintained global keys; run `python docs/tools/gen_backend_boilerplate.py --check`.
- `runtime_config.{h,cpp}`:
  - Add getters.
  - Add `validate()` branches next to :863. Unknown keys are rejected at :1130.
  - Add `validate_cold_storage_dir_access`, modelled on `validate_extra_models_dir_access` (:81). It requires an absolute path and rejects a path nested in, or containing, the hot HF cache. A non-existent path is allowed.
- `server.cpp`:
  - At ModelManager construction (~:394), pass the dir and ID.
  - `apply_config_side_effects` (~:7368) gets a `cold_storage_dir` branch:
    - return 409 if transfers are active
    - otherwise write or adopt the marker, persist `cold_storage_id`, call the setter, then `invalidate_models_cache()`.
  - Keep the cold root as a mutex-guarded ModelManager member. Don't copy the racy `g_models_dir` global.

**`include/lemon/cold_storage.h` + `server/cold_storage.cpp` (new; add to the server-core sources in CMakeLists.txt ~:1011)**
- Keeps model_manager.cpp from growing further. ModelManager owns a `std::unique_ptr<ColdStorage>`.
- `ColdStorage` holds the dir, the pinned ID, the cached status, the index, transfer bookkeeping and the low-level transfer primitives (`transfer_repo`, `merge_tree_into`, staging cleanup).
- `ColdStorageUnavailableError`, and `ColdStorageStatus` with `to_json()`.
- ModelManager keeps everything model-aware: which repos a model uses, sharing decisions, cache updates.

**`model_manager.h` / `.cpp`**
- **State:**
  - `ModelInfo::cold`, `ModelInfo::cold_available`
  - `cold_mutex_` guarding `cold_root_`, `cold_id_`, `cold_index_`, `transfers_`
  - `DownloadProgress::operation`
- **API:**
  - `set_cold_storage(dir, id)`, `cold_storage_enabled()`, `cold_storage_status()`
  - `is_model_cold(name)`, `cold_freeze_eligibility(info)`
  - `freeze_model(name, cb)`, `ensure_model_hot(info, cb)`, `mark_transfer(name)`
- **Eligibility:** reject FLM (`backend_self_manages_downloads`), cloud/dynamic models, `extra.*`, `local_path`, collections (freeze components individually in v1), models that aren't downloaded, and models already in transfer. `local_upload` is also excluded in v1: its checkpoint is relative to the HF cache root (mm:1525), so it needs separate handling.
- **`freeze_model`:**
  1. Verify drive identity.
  2. Collect repos: main plus non-`npu_cache` checkpoints, via `repo_id_to_cache_dir_name` (mm:262).
  3. Take per-repo `download_locks_` in sorted order.
  4. Choose the mode per repo: COPY if a new `is_repo_used_by_other_hot_model` (a variant of `is_repo_shared`, mm:372) says another hot model uses it, else MOVE.
  5. Transfer the repos.
  6. For a shared main repo, remove only this variant from hot. Do this by extracting `remove_model_variant_from_repo` from `delete_model`'s shared branch (mm:6140-6153) and reusing it in both places.
  7. Write the index atomically, remove the MOVE sources, then call `update_model_in_cache`.
- **`ensure_model_hot`:**
  1. Return early if disabled, or if the model is neither in the index nor in transfer.
  2. Take the same sorted locks, so a load that arrives mid-freeze waits.
  3. Re-check status under the locks.
  4. Verify the drive. If unavailable, throw `ColdStorageUnavailableError` (503).
  5. If the marker is OK but the repos are gone, drop the index entry and fall through to a normal download.
  6. Otherwise transfer each repo back: skip repos already complete in hot; COPY when another index entry references the repo, else MOVE.
  7. Drop the index entry and call `update_model_in_cache(name, true)`.
- **Hooks:**
  - `download_registered_model`: call `ensure_model_hot` before taking `repo_lock`.
  - `is_model_downloaded`: return false while the model is in `transfers_`.
  - `get_downloaded_models`: include cold models.
  - `delete_model`: delete cold repos not referenced by other index entries, then the index entry. If the drive is unavailable, remove the entry and log the leftover path. Then run the existing hot logic.
  - `handle_delete`: if a `cold:<name>` job is running, cancel and join it first.
- **Cache updates after a transfer:** use `update_model_in_cache` and set `cold` under `models_cache_mutex_`. Don't use `remove_model_from_cache`, because it erases `user.*` models.
- **Lock order:** repo locks (sorted) → `cold_mutex_` → `models_cache_mutex_`. No I/O while holding the cache mutex.

**`utils/path_utils.{h,cpp}`** (exported):
- `copy_file_with_progress(src, dst, cb)`: 8 MiB chunks, `fsync` / `FlushFileBuffers` before close, returns false on cancel.
- `flush_directory(dir)`: no-op on Windows.
- `try_rename_dir(src, dst)`: EXDEV or `ERROR_NOT_SAME_DEVICE` falls back to copy.
- `move_tree_into` stays file-static because it has no progress, cancel or verification.
- Use `path_from_utf8` everywhere and `hf_cache::exists` for symlink checks on Windows.

## Phase 2 — API & CLI
- **`server.cpp`:**
  - Register with `register_post` (quad-prefix):
    - `/freeze` and `/thaw`: `{model_name, stream, subscribe}`, the same three modes as `/pull` (sync / SSE / `start_download_job`).
    - `/cold-storage/status`: dir, state, reason, `marker_id` vs `configured_id`.
    - `/cold-storage/adopt`: admin-gated, re-pins the ID.
  - Freeze job id is `cold:<name>`. Thaw reuses `model:<name>` so the UI's pull tracking matches.
  - `handle_freeze` takes `router_->begin_exclusive()` only long enough to `mark_transfer` and unload the model, then releases it.
  - `handle_pull`: skip the offline-mode rejection for cold models.
  - Jobs `load_op` (:480): allow cold models, thaw, then load.
  - `model_info_to_json` (:3188): add `cold` and `cold_available` only when cold.
  - `download_progress_to_json`: add `operation`.
  - Map `ColdStorageUnavailableError` to 503 `cold_storage_unavailable` in `get_http_status_from_error`.
- **`src/cpp/cli/main.cpp`** (subcommands :1317, dispatch :1611):
  - Add `freeze <model>`, `thaw <model>`, `cold-storage status|adopt`.
  - `list` shows a `cold` / `cold (unavailable)` marker.
  - `run` needs no change.
- **`src/cpp/cli/lemonade_client.cpp`:** add `freeze_model` and `thaw_model`, modelled on `delete_model` (:1124) and `pull_model` (SSE).

## Phase 3 — UI (`src/app/src/renderer`; may go in a separate maintainer PR)
- `utils/modelData.ts`: add `cold?` and `cold_available?` to `ModelInfo` (:14) and the mapping (:197).
- `components/Icons.tsx`: add lucide `Snowflake` and `Sun` SVGs, in the same style as `Flame` (:198).
- `utils/backendInstaller.ts`:
  - `freezeModel`: POST `/freeze` with `subscribe:false`, tracked as `cold:<name>`, dispatches `modelsUpdated`.
  - `thawModel`: reuses `pullModel`.
- `utils/downloadTracker.ts`: widen the type to include `'cold'`.
- `DownloadManager.tsx`: labels "Moving to cold storage" / "Restoring from cold storage"; cancel only.
- `ModelManager.tsx`:
  - `getModelStatus` (:1863): `cold` status class.
  - `renderActionButtonsContent` (:1962-2052):
    - snowflake on eligible downloaded, not-loaded models when cold storage is enabled
    - sun replaces download for cold models
    - play still works because it auto-thaws
    - an "unavailable" tooltip and disabled sun when `cold_available === false`.
  - The "Downloaded only" filter (:606) includes cold models.
  - Freeze confirm dialog, following the `handleDeleteModel` pattern (:1759).
  - Fetch `/internal/config` and `/cold-storage/status` like LogsWindow.tsx does, and show a warning banner when the state is missing or mismatch.
- `styles.css`: `.model-status-indicator.cold` (icy blue), plus button hover styles.
- A Settings field for `cold_storage_dir` is deferred (no UI edits directory settings today); use the CLI `config set` for now.

## Phase 4 — Docs
- `docs/guide/configuration/README.md`: add the keys to the defaults block, table rows, and the runtime-change note.
- `docs/api/lemonade.md`: `/freeze`, `/thaw`, `/cold-storage/*`, the `cold` fields, the `operation` field, the 503 error, and auto-thaw on pull/load.
- CLI guide: `freeze`, `thaw`, `cold-storage`.
- A user guide section covering:
  - the drive ID / adopt flow
  - the shared-repo copy rule
  - changing the dir doesn't migrate data
  - FAT32 4 GB limit and network-mount notes.

## Phase 5 — Tests
- **New `test/cpp/test_cold_storage.cpp`**, built from the `test_model_download_state.cpp` fake-HF-tree template. Register it with `add_cpp_ci_test` (CMakeLists.txt ~:2931) as a dependency of `cpp-ci-tests`. Cases:
  1. Disabled: no `cold` key, no files created, `get_downloaded_models` unchanged.
  2. Freeze an exclusive repo (rename path), plus a forced copy path through a test hook that fails `try_rename_dir`.
  3. Freeze with a shared mmproj: copied, stays hot.
  4. Freeze a shared main repo (two variants): only this variant leaves hot.
  5. Thaw: MOVE / COPY / reuse of repos already in hot.
  6. Cancel mid-copy: source intact, no staging left.
  7. Recovery: staging swept, hot wins, missing repos drop the index entry.
  8. Drive identity:
     - set dir → marker written and ID persisted
     - existing marker → adopted
     - marker removed → `missing`, no writes
     - different ID → `mismatch`, 503 on thaw
     - adopt → available.
  9. Delete a cold model: its cold repos are removed, repos shared with other cold models are kept.
  10. A concurrent load during a freeze waits, then thaws.
- **`test/server_endpoints.py`:** add the new routes to the test_000 list; `/freeze` returns 400 `cold_storage_disabled` by default.
- **`test/server_cli2.py`** (near tests 090–092): run the full cycle against a temp dir:
  - set config → freeze → `list` shows cold
  - `/models` includes it with `cold:true`
  - `pull` thaws offline
  - freeze → swap the marker ID → status `mismatch` and load fails with 503
  - adopt → `delete` removes the cold copy
  - unset the config → old behavior returns.

## Risks and open points
- **Lock ordering:** repo locks (sorted) → cold mutex → `models_cache_mutex_`. Downloads take only one repo lock, so there's no deadlock.
- **`begin_exclusive` stalls freeze:** it waits for all busy servers, so a freeze issued during a long generation waits for it to finish. A narrower per-model load block is a possible follow-up.
- **`refs/main` conflict on thaw:** if hot gained a newer snapshot while the model was cold, the thawed variant may not resolve, because resolution follows `refs/main` (mm:326). v1 keeps the hot ref, logs a warning and leaves the model for a re-pull. This needs a multi-snapshot test.
- **Shared repos are copied whole:** in v1, copying a shared multi-variant repo also copies its sibling variants to cold. Copying only one variant is a follow-up.
- **Disabling cold storage while models are frozen:** those models show as not downloaded, and a pull would re-download them. Document this, and warn in the side effect if the index is non-empty.
- **`huggingface_hub` scan warnings:** the temporary `.lemonade-staging` directory in the hot root may trigger a scan-cache warning while it exists.

## Verification
1. Build with `cmake --build --preset default`. Run `ctest -L cpp-ci` and `python docs/tools/gen_backend_boilerplate.py --check`.
2. Run `python test/server_endpoints.py` and `python test/server_cli2.py`, passing `--cli-binary build/lemonade`.
3. Manual check against the dev server (`build/lemond ~/lemonade-dev/cache ~/lemonade-dev/config --port 8123 --no-broadcast`):
   1. Confirm there are no `cold` fields and no new buttons by default.
   2. Set `cold_storage_dir` to a folder on a USB drive.
   3. Freeze a small model and check the Download Manager progress and that the files moved.
   4. Load it and confirm it auto-thaws, then loads.
   5. Unmount the drive and confirm the banner, the 503 error, and that nothing was written to the mountpoint.
   6. Remount, then delete a cold model.
