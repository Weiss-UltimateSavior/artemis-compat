# ARCV v2 Animation Checkpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture and restore active tween/anime state and final transition scenes in ARCV v2 without changing the container version or weakening existing unsupported-state checks.

**Architecture:** Keep animation in the existing semantic scene stream. Capture `lytween` and `anime` commands after ordinary layer commands, validate them before mutation, and restore them after layer/text state exists. Use signed `elapsed = now_ms_ - start_ms` so future tween-set segments retain their schedule; drop only the visual transition overlay, not the runner's remaining wait.

**Tech Stack:** C++17, existing `artc::Compositor` and `artc::LuaEngine`, ARCV v2 `SavedCommand`, CTest, CMake, OHOS HAP/hdc.

## Global Constraints

- Work in `artemis-compat-checkpoint-v2`, based on upstream `origin/main@31099b5`; do not rewrite existing ARCV commits.
- Keep ARCV version `2` and the four existing sections. Old engines must reject unknown animation commands rather than silently dropping them.
- Do not add game names, game-specific IDs, commercial assets, or commercial fixtures to Git.
- Keep video, E-mote, mesh, pixel-only, custom affine, runtime masks, and an in-progress tween set explicitly unsupported.
- Every production change starts with a failing test. Watch the failure before implementing.
- Commit design/plan, capture, restore, transition integration, and downstream pin separately with Conventional Commit messages.
- Do not touch the user's pre-existing NextScene modifications in `apps/flutter_app/macos/Flutter/GeneratedPluginRegistrant.swift`, `apps/flutter_app/ohos/build-profile.json5`, `apps/flutter_app/pubspec.lock`, or deleted `build/build_{android,ios,macos}.sh`.
- Execute inline in this session; do not use subagents.

---

### Task 1: Capture tween and anime semantics

**Files:**
- Modify: `src/render/compositor.cpp:2596-2650`
- Test: `tests/compositor_regressions.cpp:1487-1529`

**Interfaces:**
- Consumes: existing `Compositor::Tween`, `Compositor::AnimeState`, `SavedCommand`, `now_ms_`, and `CheckpointFloat`.
- Produces: scene commands `lytween` with `id,param,from,to,time,delay,ease,repeat,yoyo,elapsed,started,from_current`; scene commands `anime` with `mode,id,file,time,loop,elapsed` plus each frame's props.

- [ ] **Step 1: Replace the unsupported-animation test with a failing capture test**

Add a tween and a looping anime before calling `CaptureScene`. Keep the mesh and pixel-only rejection tests unchanged.

```cpp
scene.AddTween("bg", {{"param","left"},{"from","12"},{"to","60"},
                      {"time","1000"},{"delay","100"},{"loop","0"},{"yoyo","-1"}}, 1000);
scene.SetAnimeFrame("cursor", "init", "small.tga", 0, -1,
                    {{"clip","0,0,4,4"}}, 1000);
scene.SetAnimeFrame("cursor", "add", "small.tga", 500, -1,
                    {{"clip","4,0,4,4"}}, 1000);
scene.SetAnimeFrame("cursor", "add", "small.tga", 1000, -1,
                    {{"clip","8,0,4,4"}}, 1000);
scene.SetAnimeFrame("cursor", "end", "", 1000, -1, {}, 1000);
scene.Update(1250);
Check(scene.CaptureScene(commands,error), "capture active tween and anime");
const auto tween = std::find_if(commands.begin(), commands.end(), [](const auto& c) {
    return c.name == "lytween";
});
Check(tween != commands.end() && tween->attrs.at("param") == "left" &&
      std::stod(tween->attrs.at("elapsed")) == 250 &&
      tween->attrs.at("repeat") == "-1" && tween->attrs.at("yoyo") == "1",
      "capture tween loop and signed phase");
const auto anime_end = std::find_if(commands.begin(), commands.end(), [](const auto& c) {
    return c.name == "anime" && c.attrs.at("mode") == "end";
});
Check(anime_end != commands.end() && anime_end->attrs.at("loop") == "-1" &&
      std::stod(anime_end->attrs.at("elapsed")) == 250,
      "capture anime loop and phase");
```

Also add a future tween-set segment and assert a negative `elapsed` is emitted, and call `BeginTweenSet()` without `EndTweenSet()` to assert capture still rejects an uncommitted set.

- [ ] **Step 2: Run the compositor test and verify RED**

Run:

```bash
cmake --build build-checkpoint --target compositor_regressions -j4
./build-checkpoint/tests/compositor_regressions
```

Expected: FAIL at `capture active tween and anime` because `CaptureScene` currently rejects any nonempty `tweens_` or `anime_`.

- [ ] **Step 3: Implement minimal capture emission**

Keep `collecting_tweens_` as an error. Remove only the blanket `tweens_`/`anime_` rejection. Append one tween command per entry:

```cpp
for (const auto& tw : tweens_) {
    next.push_back({"lytween", {
        {"id",tw.id},{"param",tw.param},
        {"from",CheckpointFloat(tw.from)},{"to",CheckpointFloat(tw.to)},
        {"time",std::to_string(tw.time_ms)},{"delay",std::to_string(tw.delay_ms)},
        {"ease",std::to_string(tw.ease)},{"repeat",std::to_string(tw.repeat)},
        {"yoyo",tw.yoyo ? "1" : "0"},
        {"elapsed",std::to_string(now_ms_ - tw.start_ms)},
        {"started",tw.started ? "1" : "0"},
        {"from_current",tw.from_current ? "1" : "0"}
    }});
}
```

For each `anime_` entry, emit `init`, all `add` frames, and `end`. Copy each frame's props, then overwrite `mode,id,file,time` with authoritative values. Put `loop` and `elapsed` only on `end`:

```cpp
for (const auto& [id, st] : anime_) {
    for (size_t i = 0; i < st.frames.size(); ++i) {
        SavedCommand command{"anime", st.frames[i].props};
        auto& a = command.attrs;
        a["mode"] = i == 0 ? "init" : "add";
        a["id"] = id;
        a["file"] = st.frames[i].file;
        a["time"] = std::to_string(st.frames[i].time_ms);
        next.push_back(std::move(command));
    }
    next.push_back({"anime", {{"mode","end"},{"id",id},{"file",""},
        {"time",std::to_string(st.total_ms)},{"loop",std::to_string(st.loop)},
        {"elapsed",std::to_string(now_ms_ - st.start_ms)}}});
}
```

Do not emit a command for `trans_active_`; final layer state already represents the new scene.

- [ ] **Step 4: Run capture and full tests**

Run:

```bash
./build-checkpoint/tests/compositor_regressions
ctest --test-dir build-checkpoint --output-on-failure -j4
```

Expected: compositor PASS and 32/32 PASS. Restoration tests added in Task 2 must be made to pass there, not by weakening capture assertions.

- [ ] **Step 5: Commit capture independently**

```bash
git add src/render/compositor.cpp tests/compositor_regressions.cpp
git commit -m "feat(render): capture semantic animation state"
```

---

### Task 2: Validate and restore animation phase

**Files:**
- Modify: `src/render/compositor.cpp:2551-2742`
- Test: `tests/compositor_regressions.cpp:1487-1535`

**Interfaces:**
- Consumes: Task 1 command schema and existing `Tween`, `AnimeState`, `QueueTween`, `SetAnimeFrame`, `AdvanceAnime`.
- Produces: restored tween/anime at `start_ms = now_ms_ - elapsed`; existing public signatures `ValidateScene` and `RestoreScene` remain unchanged.

- [ ] **Step 1: Add failing restoration and malformed-input tests**

After capturing in Task 1, delete the original layers, restore, and compare phase progression rather than only the initial frame.

```cpp
scene.DeleteLayer("bg");
scene.DeleteLayer("cursor");
Check(scene.RestoreScene(commands,error), "restore animated scene");
scene.Update(1500);
const auto& bg_restored = scene.Layers().at(0);
Check(bg_restored.x > 59.9f && bg_restored.x < 60.1f,
      "infinite yoyo resumes at the original cycle endpoint");
scene.Update(1750);
const auto cursor_it = std::find_if(scene.Layers().begin(), scene.Layers().end(),
                                    [](const auto& layer) { return layer.id == "cursor"; });
Check(cursor_it != scene.Layers().end(), "restored cursor exists");
const auto& cursor = *cursor_it;
Check(cursor.source_file == "small.tga" && cursor.u0 > 0.49f && cursor.u1 <= 1.0f,
      "looping anime advances from the restored phase");

auto before_animation = scene.Layers();
auto bad = commands;
for (auto& command : bad) {
    if (command.name == "lytween") command.attrs["elapsed"] = "nan";
}
Check(!scene.RestoreScene(bad,error), "non-finite tween phase rejected");
Check(scene.Layers().size() == before_animation.size(), "invalid animation preserves scene");
bad = commands;
for (auto& command : bad) {
    if (command.name == "anime" && command.attrs.at("mode") == "add")
        command.attrs["file"] = "missing.png";
}
Check(!scene.RestoreScene(bad,error), "missing anime frame rejected before mutation");
Check(scene.Layers().size() == before_animation.size(), "missing anime frame preserves scene");
```

Add a completed finite anime (`loop=1`, elapsed greater than total time) and assert capture succeeds and restoration holds the last frame.

- [ ] **Step 2: Run the test and verify RED**

Run `./build-checkpoint/tests/compositor_regressions`.

Expected: FAIL with `unsupported scene command: lytween` during validation.

- [ ] **Step 3: Implement strict command validation**

Extend `CheckpointAttrs` numeric checks to `from,to,time,delay,ease,repeat,yoyo,elapsed,started,from_current,loop`. Validate finite values, nonnegative duration/delay/frame time, `ease` in `[0,30]`, `repeat` and `loop` in `[-1,INT_MAX]`, and booleans exactly `0` or `1`. Require a known tween `param` accepted by `ReadParam`. Validate `anime` mode as `init/add/end`, require one terminal `end` per id, and call `CheckpointResource` for every nonempty frame file.

Do not reject negative `elapsed`; it represents a future tween-set segment. Reject an empty `anime` sequence and missing `end`.

- [ ] **Step 4: Implement restoration after layers/text**

Keep the existing `lyc/lyprop/chgmsg/font/print` path. In a second pass over scene commands, restore animations so they cannot overwrite captured base properties prematurely.

For `lytween`, construct `Tween` directly (the container is empty after `Init`) and push it:

```cpp
Tween tw;
tw.id = a.at("id");
tw.param = a.at("param");
tw.from = std::stof(a.at("from"));
tw.to = std::stof(a.at("to"));
tw.time_ms = std::stod(a.at("time"));
tw.delay_ms = std::stod(a.at("delay"));
tw.ease = std::stoi(a.at("ease"));
tw.repeat = std::stoi(a.at("repeat"));
tw.yoyo = a.at("yoyo") == "1";
tw.start_ms = now_ms_ - std::stod(a.at("elapsed"));
tw.started = a.at("started") == "1";
tw.from_current = a.at("from_current") == "1";
tweens_.push_back(std::move(tw));
```

For each `anime` group, rebuild frames with `SetAnimeFrame`. Call `end` with `now_ms_ - elapsed`, then `AdvanceAnime(now_ms_, nullptr)`. Completed finite states remain in `anime_` holding their final frame, as before.

- [ ] **Step 5: Run focused and full tests**

Run:

```bash
./build-checkpoint/tests/compositor_regressions
ctest --test-dir build-checkpoint --output-on-failure -j4
```

Expected: all animation phase, finite-anime, malformed-data, and 32/32 tests PASS.

- [ ] **Step 6: Commit restoration independently**

```bash
git add src/render/compositor.cpp tests/compositor_regressions.cpp
git commit -m "feat(render): restore animation phase"
```

---

### Task 3: Accept final transition scenes and verify ARCV end-to-end

**Files:**
- Modify: `src/render/compositor.cpp:2596-2742`
- Modify: `src/script/lua_engine.cpp:2115-2181`
- Test: `tests/compositor_regressions.cpp:1514-1535`
- Test: `tests/runtime_regressions.cpp:1771-1868`

**Interfaces:**
- Consumes: Task 2 animation restoration, existing `TransitionActive()`, `CheckpointWait.timed`, `remaining_ms`, and `LuaEngine::IsWaiting`.
- Produces: ARCV v2 files containing animation commands, final scene with `TransitionActive()==false`, and preserved runner wait deadlines.

- [ ] **Step 1: Replace the transition rejection test with a failing acceptance test**

```cpp
Check(scene.BeginTransition(0,100,{},0,0,0), "start transition");
Check(scene.CaptureScene(commands,error), "capture final scene during transition");
Check(scene.RestoreScene(commands,error), "restore final transition scene");
Check(!scene.TransitionActive(), "old-frame overlay is not restored");
```

Add an engine save/load fixture with a finite tween and a transition wait. Assert the generated header is `ARCV 02 00 00 00`, load restores the scene, the remaining timer releases, and runner execution continues.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```bash
./build-checkpoint/tests/compositor_regressions
./build-checkpoint/tests/runtime_regressions
```

Expected: compositor fails at `capture final scene during transition`; runtime save is rejected with `active scene animation is unsupported`.

- [ ] **Step 3: Remove only the transition blanket rejection and normalize wait meaning**

Delete `trans_active_` from the capture rejection. Keep `RestoreScene`'s existing `ReleaseGl(); Init(width,height);`, which clears visual transition state.

Do not alter `remaining_ms`. `WaitFromCheckpoint` restores the timed deadline independently of the overlay. `transition_wait_` may remain as a marker for the next `wt` only; it must not be used as proof that an overlay still exists. Do not synthesize a new transition or a pixel snapshot.

- [ ] **Step 4: Verify all engine boundaries**

Run:

```bash
./build-checkpoint/tests/compositor_regressions
./build-checkpoint/tests/runtime_regressions
ctest --test-dir build-checkpoint --output-on-failure -j4
cmake --build build-checkpoint --target embedded_consumer -j4
./build-checkpoint/tests/embedded_consumer/embedded_consumer
```

Expected: focused tests PASS, 32/32 PASS, embedded consumer PASS. Then rebuild Android `libartemis.so` and run `python3 tools/check_android_exports.py`; expected 14 required exports.

- [ ] **Step 5: Commit transition and ARCV integration**

```bash
git add src/render/compositor.cpp src/script/lua_engine.cpp \
  tests/compositor_regressions.cpp tests/runtime_regressions.cpp
git commit -m "feat(save): checkpoint animated scenes"
```

---

### Task 4: Pin downstream and perform real-game regression

**Files:**
- Modify: `NextScene-artemis-embed/cpp/artemis/upstream` (submodule pin only)
- Local diagnostics: `diagnostics/emulator-20261024/` (never commit)

**Interfaces:**
- Consumes: Task 3 upstream commit and existing `build/build_ohos.sh release` pipeline.
- Produces: an OHOS HAP containing the animated-checkpoint engine and device evidence of actual ARCV v2 saves.

- [ ] **Step 1: Update the pin without committing unrelated files**

```bash
git -C NextScene-artemis-embed/cpp/artemis/upstream fetch origin fix/checkpoint-runtime-v2
TASK3_COMMIT="$(git -C artemis-compat-checkpoint-v2 rev-parse HEAD)"
git -C NextScene-artemis-embed/cpp/artemis/upstream checkout "$TASK3_COMMIT"
git -C NextScene-artemis-embed add cpp/artemis/upstream
git -C NextScene-artemis-embed commit -m "chore(artemis): pin animation checkpoints"
```

- [ ] **Step 2: Build and install OHOS**

```bash
source nextscene-migration-20261024/env.sh
cd nextscene-migration-20261024/NextScene-artemis-embed
./build/build_ohos.sh release
"$HDC" install -r apps/flutter_app/build/ohos/hap/entry-default-signed.hap
```

Expected: new signed HAP; packaged native Build IDs match staged libraries. Install succeeds on `127.0.0.1:5555`.

- [ ] **Step 3: Verify the previously failing game and file format**

Enter the second Artemis game, start a scene with the looping cursor, save to a fresh slot, advance the story, and load it. Pull the exact new slot with `hdc file recv -b com.nextscene.app` and check:

```bash
xxd -l 16 diagnostics/emulator-20261024/<slot>.dat
```

Expected: `4152 4356 0200 0000` (`ARCV` version 2), `load: v2 checkpoint restored via onLoad`, cursor continues, scene/text return, and the next click advances the story. Repeat after a cold re-entry and Q-SAVE/Q-LOAD.

- [ ] **Step 4: Complete compatibility regression**

Verify the first Artemis game's ordinary and quick save/load, Artemis→KiriKiri2→Artemis switching, same-game re-entry, and Home/foreground. Do not claim KiriKiri2 rendering is fixed; record its existing composition artifacts separately.

Run final `ctest --test-dir build-checkpoint --output-on-failure -j4`, OHOS build, `git diff --check`, and both `git status --short`. Expected 32/32 PASS, no diagnostic source edits, only the user's original uncommitted NextScene files remain.

- [ ] **Step 5: Review and finish**

Invoke `superpowers:verification-before-completion`, review each commit with `git show --stat` and `git diff --check`, then invoke `superpowers:finishing-a-development-branch`. Do not push NextScene or open a PR until all actual checks pass and the user chooses integration.
