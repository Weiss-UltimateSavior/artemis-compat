# ARCV v2 Runtime Checkpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a validated ARCV v2 checkpoint that restores script flow and ordinary image/text scenes while rejecting incomplete or unsupported saves before mutation.

**Architecture:** A new `save/checkpoint` container carries variables, runner flow, macro scopes, and semantic scene commands. `AsbRunner` and `Compositor` own their respective capture/validate/restore operations; `LuaEngine` performs two-phase checkpoint commit without changing BOWS/BOWG behavior.

**Tech Stack:** C++17, Lua 5.1, CMake/CTest, OpenGL/GLES, Android NDK, OHOS SDK 20, Flutter OHOS.

## Global Constraints

- Base is `origin/main` at `31099b581a2f2355d0a32ac2d840a3a387dec62e`; preserve `56896f8` post-discard return-frame behavior.
- All production save writes use the existing O_EXCL + fsync + rename pipeline.
- Decode and prevalidate everything before `ResetEphemeral`; failed loads do not mutate runner, variables, compositor, audio, or queues.
- ARCV v1 is rejected explicitly. BOWS/BOWG code paths and expectations remain unchanged.
- No title-specific branches, commercial assets in tests, commercial names in commits, Flutter dependencies in upstream, or signing/generated files in commits.
- Video/E-mote/active tween/transition states fail explicitly at capture or validation; they are never silently dropped.
- Every implementation task uses red-green tests and a small Conventional Commit.

---

### Task 1: Validated ARCV v2 Container

**Files:**
- Create: `src/save/checkpoint.h`
- Create: `src/save/checkpoint.cpp`
- Create: `tests/checkpoint_regressions.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `VariableBank`, `SavedCommand`, `ReadSaveFile`/`WriteSaveFile` conventions from existing save code.
- Produces:
  ```cpp
  struct CheckpointWait {
      bool waiting = false, timed = false, accept_input = false;
      bool announced = false, sound = false, transition = false;
      std::string sound_key, video_key;
      uint64_t remaining_ms = 0;
      double auto_elapsed = -1;
  };
  struct CheckpointFrame {
      std::string file;
      size_t pc = 0;
      bool halted = false, macro_scope = false;
      CheckpointWait wait;
  };
  struct CheckpointFlow {
      std::string current_file;
      size_t pc = 0;
      bool loaded = false, halted = false, pc_pending = false;
      std::vector<CheckpointFrame> frames;
      std::map<std::string, std::string> global_labels;
  };
  using CheckpointScopes =
      std::vector<std::vector<std::pair<std::string, std::pair<bool, std::string>>>>;
  struct CheckpointV2 {
      VariableBank variables;
      CheckpointFlow flow;
      CheckpointScopes scopes;
      std::vector<SavedCommand> scene;
  };
  bool EncodeCheckpointV2(const CheckpointV2&, std::vector<uint8_t>&, std::string&);
  bool DecodeCheckpointV2(const std::vector<uint8_t>&, CheckpointV2&, std::string&);
  ```

- [ ] **Step 1: Register and write the failing container test**

Add `checkpoint_regressions` using the same `Check(condition, message)` pattern as other suites. Its main round-trip input is:

```cpp
artc::CheckpointV2 input;
input.variables["lua.scr"] = std::string("a\0b", 3);
input.flow.current_file = "scenario/main.iet";
input.flow.pc = 7;
input.flow.loaded = true;
input.flow.frames.push_back({"system/ui.iet", 3, false, true,
                             {true, true, false, true, false, false,
                              "bgm", "", 125, 40}});
input.flow.global_labels["game_start"] = "scenario/main.iet";
input.scopes.push_back({{"time", {true, "1000"}}, {"name", {false, ""}}});
input.scene.push_back({"lyc", {{"id", "bg"}, {"file", "image/bg"}}});
input.scene.push_back({"print", {{"data", "restored"}}});
artc::CheckpointV2 output;
std::vector<uint8_t> bytes;
std::string error;
Check(artc::EncodeCheckpointV2(input, bytes, error), error);
Check(artc::DecodeCheckpointV2(bytes, output, error), error);
Check(output.variables == input.variables, "binary variable round trip");
Check(output.flow.frames == input.flow.frames, "runner frame round trip");
Check(output.scopes == input.scopes, "macro scope round trip");
Check(output.scene.size() == 2 && output.scene[1].attrs.at("data") == "restored",
      "scene command round trip");
```

Add comparisons for `CheckpointWait`, `CheckpointFrame`, and `CheckpointFlow` so these assertions compile. Add corruption cases: bad magic/version/CRC, truncated sections, trailing bytes, invalid PC (`SIZE_MAX`), invalid remaining wait, and non-finite numeric attributes.

- [ ] **Step 2: Run the test and confirm red**

Run:
```bash
cmake -S . -B build-checkpoint -DCMAKE_BUILD_TYPE=Release -DARTC_BUILD_TESTS=ON
cmake --build build-checkpoint -j8 --target checkpoint_regressions
```
Expected: FAIL because `src/save/checkpoint.h` and the test target do not exist.

- [ ] **Step 3: Implement the bounded container**

Create the types exactly as above. Encode fixed-width little-endian integers and length-prefixed strings with sections for variables, flow, scopes, and scene. Reject more than 65,536 variables/commands/frames/scopes, strings over 16 MiB, and total documents over the existing save-size ceiling. Put version `2` and a final CRC in the container. Decode into a temporary `CheckpointV2 next` and assign `out` only after full validation.

- [ ] **Step 4: Run focused and full tests**

Run:
```bash
cmake --build build-checkpoint -j8
ctest --test-dir build-checkpoint --output-on-failure
```
Expected: `checkpoint_regressions` PASS and all existing tests PASS (30 baseline plus the new suite).

- [ ] **Step 5: Commit the container only**

```bash
git add src/save/checkpoint.h src/save/checkpoint.cpp tests/checkpoint_regressions.cpp tests/CMakeLists.txt
git commit -m "feat(save): add validated ARCV v2 container"
```

---

### Task 2: Runner Flow Capture and Restore

**Files:**
- Modify: `src/script/asb_parser.h`
- Modify: `src/script/asb_parser.cpp`
- Modify: `tests/asb_runner_regressions.cpp`

**Interfaces:**
- Consumes: `CheckpointFlow`, `CheckpointFrame`, `CheckpointWait` from Task 1.
- Produces:
  ```cpp
  CheckpointFlow CaptureFlow() const;
  bool ValidateFlow(const CheckpointFlow&, std::string&) const;
  bool RestoreFlow(const CheckpointFlow&, std::string&);
  ```
  `ValidateFlow` is side-effect free. `RestoreFlow` installs a previously validated flow and re-bases wait deadlines.

- [ ] **Step 1: Add failing nested/cross-file and post-discard tests**

Use generated `post.iet` and `sub.iet`. Drive the runner into a nested call, capture it, discard the flow, then validate and restore. Assert:

```cpp
const auto saved = runner.CaptureFlow();
Check(saved.current_file == "post.iet" && saved.frames.size() == 1,
      "capture nested script position");
runner.DiscardFlow();
std::string error;
Check(runner.ValidateFlow(saved, error), error);
Check(runner.RestoreFlow(saved, error), error);
Check(runner.CurrentFile() == saved.current_file &&
      runner.CurrentIndex() == saved.pc &&
      runner.StackFiles().size() == saved.frames.size() + 1,
      "restore exact nested position");
Check(runner.StackFiles()[0] == "post.iet", "callee remains active");
```

Add a separate quickjump case whose callee ends in `[return]`; assert the `56896f8` behavior continues. Add invalid file, invalid current PC, invalid frame PC, and `remaining_ms == UINT64_MAX` cases that must fail without changing `StackFiles()`.

- [ ] **Step 2: Run the focused test and confirm red**

Run:
```bash
cmake --build build-checkpoint -j8 --target asb_runner_regressions
./build-checkpoint/tests/asb_runner_regressions
```
Expected: FAIL because `CaptureFlow` is not defined.

- [ ] **Step 3: Implement side-effect-free validation and exact restoration**

`CaptureFlow` copies current cursor, flags, global labels, and frames. Convert each `WaitState` to `CheckpointWait` using nonnegative remaining milliseconds.

`ValidateFlow` reads and parses every referenced script without `RunLoadTimeLuaChunks`, and checks `pc < lines.size()` for every active position. Do not modify `script_`, labels, stack, Lua, or log output beyond an error string.

`RestoreFlow` restores the outer caller chain before the active script, restores macro-scope markers and waits, restores global labels, and finally restores the active cursor. It uses existing script parsing/indexing and preserves load-time chunk semantics. Convert portable waits back with `RestoreWait` and `AutoReadTimer::Restore` semantics rather than serializing absolute deadlines.

- [ ] **Step 4: Run focused and full tests**

Run:
```bash
cmake --build build-checkpoint -j8
ctest --test-dir build-checkpoint --output-on-failure
```
Expected: all tests PASS, including existing post-discard and new invalid-flow cases.

- [ ] **Step 5: Commit runner support only**

```bash
git add src/script/asb_parser.h src/script/asb_parser.cpp tests/asb_runner_regressions.cpp
git commit -m "feat(script): capture and restore runner flow"
```

---

### Task 3: Semantic Scene Capture and Restore

**Files:**
- Modify: `src/render/compositor.h`
- Modify: `src/render/compositor.cpp`
- Modify: `tests/compositor_regressions.cpp`

**Interfaces:**
- Consumes: `SavedCommand` from `save/native_save.h`.
- Produces:
  ```cpp
  bool CaptureScene(std::vector<SavedCommand>&, std::string&) const;
  bool ValidateScene(const std::vector<SavedCommand>&, std::string&) const;
  bool RestoreScene(const std::vector<SavedCommand>&, std::string&);
  ```
  New `Layer` metadata:
  ```cpp
  std::string source_file;
  float text_size = 0;
  uint32_t text_color = 0;
  float text_wrap_width = 0;
  std::map<std::string, std::string> text_style;
  std::vector<TextRuby> text_ruby;
  ```

- [ ] **Step 1: Add failing semantic scene tests**

Create an image layer and a text layer with the existing synthetic PNG/font helpers. Set nondefault geometry, visibility, alpha, clip, drag bounds, message overlay, and text style. Capture, mutate to a different scene, and restore. Assert:

```cpp
std::vector<artc::SavedCommand> scene;
std::string error;
Check(c.CaptureScene(scene, error), error);
Check(!scene.empty(), "scene emits semantic commands");
Check(c.RestoreScene(scene, error), error);
const auto& bg = c.Layers().at(0);
Check(bg.source_file == "image/bg" && bg.x == 12 && bg.y == 34 &&
      bg.alpha == 0.5f && bg.visible && bg.message_overlay,
      "restore image semantics");
Check(c.Layers().at(1).text == "restored text" &&
      c.Layers().at(1).text_size == 25 &&
      c.Layers().at(1).text_wrap_width == 720,
      "restore text semantics");
```

Add unsupported `mesh`/active tween/active transition cases and a missing image/font case. Assert failure does not change `Layers()` or remove existing `lyevent` registrations.

- [ ] **Step 2: Run the focused test and confirm red**

Run:
```bash
cmake --build build-checkpoint -j8 --target compositor_regressions
./build-checkpoint/tests/compositor_regressions
```
Expected: FAIL because `CaptureScene` is not defined.

- [ ] **Step 3: Implement semantic capture, validation, and restoration**

`LoadImage` records the resolved source in both GL and headless implementations. `SetText` records all style arguments in both implementations. Capture emits `lyc`, `lyprop`, `font`, and `print` commands from existing layer/font state. Reject mesh layers, pixel-only/video layers, active anime/tweens/transitions, and runtime stage masks.

`ValidateScene` checks command names, required IDs, finite numeric values, and source/font existence without mutating the compositor. `RestoreScene` recreates semantic state through `LoadImage`, `SetProps`, `LoadFont`, and `SetText`, invalidates indices, and increments revision. Do not serialize GL names, atlas pages, stage-mask FBOs, or shader handles.

- [ ] **Step 4: Run focused and full tests**

Run:
```bash
cmake --build build-checkpoint -j8
ctest --test-dir build-checkpoint --output-on-failure
```
Expected: all tests PASS, including existing real desktop CGL compositor tests.

- [ ] **Step 5: Commit compositor support only**

```bash
git add src/render/compositor.h src/render/compositor.cpp tests/compositor_regressions.cpp
git commit -m "feat(render): capture and restore semantic scene state"
```

---

### Task 4: Atomic LuaEngine Checkpoint Integration

**Files:**
- Modify: `src/script/lua_engine.h`
- Modify: `src/script/lua_engine.cpp`
- Modify: `src/script/lua_tags_nav.cpp`
- Modify: `tests/runtime_regressions.cpp`

**Interfaces:**
- Consumes: all Task 1–3 interfaces and existing `LyeventEntry`/`StoreLyevent`.
- Produces: ARCV v2 `SaveSnapshot`/`LoadSnapshot`; `TagLoad` returns the boolean produced by `LoadSnapshot` when a file is present. Existing no-file `TagLoad` behavior is unchanged.

- [ ] **Step 1: Add failing end-to-end checkpoint tests**

Extend the generated `runtime_regressions` fixture with an `onSave`/`onLoad` callback, an active nested runner, ordinary image/text layers, macro scopes, and click/drag `lyevent` registrations. Save to two slots, mutate, and load the first. Assert variable separation, exact flow, scene text, macro scopes, and enabled event types. Advance restored dialogue once and assert the next text is produced.

Add a small legacy ARCV v1 document using `EncodeVariableBank(items, true, bytes)`. Record the current runner and scene, call `LoadSnapshot`, and require `false` plus unchanged state. Add bad CRC, missing script, missing image, and unsupported video cases with the same nonmutation assertion.

- [ ] **Step 2: Run the focused test and confirm red**

Run:
```bash
cmake --build build-checkpoint -j8 --target runtime_regressions
./build-checkpoint/tests/runtime_regressions
```
Expected: FAIL because v1 loads or clears state and v2 integration is absent.

- [ ] **Step 3: Implement two-phase save/load and event capture**

`SaveSnapshot` captures variables, flow, macro scopes, compositor commands, and per-type `lyevent` commands after `onSave`. Encode and atomically write v2 before calling `SaveSystemData`.

`LoadSnapshot` branches BOWS into the existing importer path. Recognize ARCV v1 and return `false` before mutation. For v2, decode, call `ValidateBankGraphs`, `ValidateFlow`, and `ValidateScene`, and validate every event before `ResetEphemeral`. Restore variables/scopes, scene, runner, then call `onLoad` once. Restore `lyevent` entries through `StoreLyevent`, retaining handler presence, enable state, and event type.

`TagLoad` changes only the file branch:

```cpp
bool LuaEngine::TagLoad(const std::string &tag, TagAttrs &m) {
    if (m.count("file")) return LoadSnapshot(m.at("file"));
    return true;
}
```

- [ ] **Step 4: Run focused and full tests**

Run:
```bash
cmake --build build-checkpoint -j8
ctest --test-dir build-checkpoint --output-on-failure
```
Expected: all tests PASS, including existing BOWS/BOWG and new v1 nonmutation tests.

- [ ] **Step 5: Commit integration only**

```bash
git add src/script/lua_engine.h src/script/lua_engine.cpp src/script/lua_tags_nav.cpp tests/runtime_regressions.cpp
git commit -m "fix(save): restore v2 checkpoints atomically"
```

---

### Task 5: Cross-Platform and Real-Game Validation

**Files:**
- Modify only if a discovered defect requires a separately reviewed fix: relevant source and focused tests.
- Modify after upstream publication: `NextScene-artemis-embed/cpp/artemis/upstream` submodule pointer.

**Interfaces:**
- Consumes: the complete upstream `fix/checkpoint-runtime-v2` branch and existing NextScene `env.sh`.
- Produces: validated upstream commits, then one downstream submodule-pin commit.

- [ ] **Step 1: Run upstream and embedded validation**

Run:
```bash
ctest --test-dir build-checkpoint --output-on-failure
cmake --build build-checkpoint -j8 --target artemis-mac
python3 tools/check_android_exports.py build-android/libartemis.so \
  --nm "$KRROOT/.devtools/android-ndk-r30/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"
```
Expected: all tests PASS, desktop GL target builds, and every required Android export is present. Install the documented Android NDK r30 at `$KRROOT/.devtools/android-ndk-r30` if it is absent, configure `build-android` with that NDK, and build `libartemis.so` before running the export check. Build the embedded consumer with JNI/CLI/macOS disabled and run its test.

- [ ] **Step 2: Build OHOS and check artifact identity**

Run from NextScene:
```bash
source ../env.sh
./build/build_ohos.sh release
```
Expected: exit 0; signed HAP contains this run's `libengine_api.so` and `libfile_archive.so` with matching Build IDs.

- [ ] **Step 3: Validate real checkpoint flows on the OHOS emulator**

Install the new HAP on `NextSceneAll`. For at least two different Artemis frameworks, run ordinary save -> mutate/advance -> load -> background and text restored -> click advances. Repeat quicksave/quickload and title-load. Verify a second game does not read the first game's system bank. Use screenshots and engine logs as evidence.

- [ ] **Step 4: Run lifecycle and cross-engine smoke tests**

Check same-game re-entry, background/resume, Artemis -> KiriKiri2 -> Artemis switching, and one KiriKiri2 title. Confirm no new Lua nil/Artemis E errors and no texture geometry regression. Keep known unrelated plugin warnings separate from new failures.

- [ ] **Step 5: Publish upstream and pin downstream**

Review every commit and test result. Publish the upstream branch, then update the downstream submodule to the published commit and run `git submodule status`. Commit only the pin:

```bash
git add cpp/artemis/upstream
git commit -m "chore(artemis): pin runtime checkpoint fixes"
```
