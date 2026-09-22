# Embedding artemis-compat

The repository builds both the official-shell Android `libartemis.so` and a
portable static core. An embedded host owns its window, GL context, frame loop,
input and UI; `EngineContext` owns packs, configuration, audio and the script and
rendering graph. Flutter is not a dependency.

## CMake consumer

Pin a complete checkout (for example a Git submodule). Do not fetch a moving
branch during configuration. An optional source path in the parent project is
useful when editing the engine and host together.

```cmake
add_subdirectory(third_party/artemis-compat)
target_link_libraries(my_runtime PRIVATE artemis::core)
```

As a subdirectory, CLI, JNI and Cocoa hosts default to off. A standalone macOS
configure still builds the CLI and Cocoa host. `ARTC_BUILD_JNI=ON` builds the
original Android shared library from the same core and unchanged JNI sources.
Lua and the core are position independent; platform libraries propagate to the
final consumer. `tests/embedded_consumer` demonstrates a shared-library consumer
and rejects accidentally enabled upstream hosts.

## Android host contract

The Android consumer includes [Tyranor-Next at `2773d8d`](https://github.com/Weiss-UltimateSavior/Tyranor-Next/tree/2773d8de72a9196b4360adf2bde0aa7b512ed345),
whose `com.ies_net.artemis.ArtemisActivity` is Kotlin source. Its
`ArtemisActivityClean` selects the clean-room engine in a separate process:

- The plugin packager places the built `libartemis.so` at
  `<filesDir>/engine_plugins/artemis/current/arm64-v8a/libartemis-clean.so`.
  Keep the upstream output name; the plugin filename belongs to the consumer.
- `artemis_loader` calls `dlopen` with `RTLD_NOW | RTLD_GLOBAL` and forwards
  `ANativeActivity_onCreate`. The Activity then calls `System.load` on the same
  path to make its JNI methods available to ART.
- The launcher's `getExternalFilesDir` supplies the game path. Validate startup,
  native key input, video completion, dialogs, pause/resume and exit through this
  actual host, in addition to checking the original shell contract.

This build integration leaves `src/jni` unchanged. Source and symbol inspection
found existing gaps that require a separate Android compatibility change:

- Tyranor declares `OnReadyPlayAssetDelivery(int, int, int)`, but the engine's
  short-name export accepts `jstring`. The existing `__III` export does not fix
  this: [JNI resolution tries the short name first](https://docs.oracle.com/en/java/javase/21/docs/specs/jni/design.html#resolving-native-method-names).
  Invoking that callback can therefore bind an incompatible native signature.
- `EmulateKeyEvent` and `OnFinishVideo` only log. `ExecuteTag` addresses the
  separate DebugBridge context, not the active NativeActivity context.
- Tyranor's bundled audio bridge looks up
  `_ZN7artemis12CSoundDevice16PauseAllInstanceEv` and
  `_ZN7artemis12CSoundDevice17ResumeAllInstanceEv`. The engine currently exports
  the unmangled `PauseAllInstance` / `ResumeAllInstance` names, so those bridge
  lookups cannot resolve them.

The Android library compiles and retains its entry points; Tyranor-Next runtime
compatibility is not established by those checks. No Android device run was
performed for this build integration.

## Backend selection

`ARTC_GRAPHICS_BACKEND` accepts AUTO, HEADLESS, GLES or DESKTOP_GL.
`ARTC_AUDIO_BACKEND` accepts AUTO, OPENSL, OHAUDIO or NULL.

| Platform | AUTO graphics | AUTO audio |
| --- | --- | --- |
| Android | GLES | OpenSL ES |
| OpenHarmony | GLES | OHAudio |
| iOS | GLES | Configuration error: Apple audio not implemented |
| Desktop core | Headless | Null |

For macOS GLES tests, enable `ARTC_TEST_GLES` and provide ANGLE. Desktop OpenGL
shader translation is selected by DESKTOP_GL, never merely by `__APPLE__`.
iOS compile experiments must explicitly select NULL audio; this is not a
production iOS backend. The Cocoa host is macOS-only.

If the parent already owns EGL/GLES (for example ANGLE), set `ARTC_GLES_TARGET`
to an existing interface target that supplies both matching headers and libraries.
Do not mix the host's ANGLE context with system EGL/GLES entry points.
Otherwise Android/OHOS use system EGL/GLES, and other GLES builds find
`unofficial-angle` plus EGL/GLES headers through CMake's search paths.

`ARTC_ENABLE_FFMPEG` retains upstream's optional decoder behavior. Supply
`FFMPEG_FOUND`, `FFMPEG_INCLUDE_DIRS` and `FFMPEG_LIBRARIES` when cross-compiling;
on a native host pkg-config can discover it. Without FFmpeg, movie decoding is
unavailable. Enabling the option alone does not prove a decoder was linked.

## Session ownership

1. Call `EngineContext::Open(pack_or_directory, os_id, key[, save_dir])` before
   creating the GL surface. Omit the fourth argument for the original sidecar
   save layout. An explicit writable directory keeps saves separate from assets.
2. Make the host's GL context current, then call `Start(true)` and
   `BootFramework(false)`. `Start(false)` is for a headless bootstrap.
3. Drive framework input, `RunEnterFrame`, queued tags, runner instructions,
   compositor `Update`/`Draw`, and `EndFrame` from the host thread. Queued tags
   already passed through the Lua filter; dispatch them with filtering disabled.
4. `ResetSession` drops Lua and runner but retains audio, packs and compositor.
   Hosts that stop audio during resets must explicitly stop voices and reset
   logical channels. Original Android reset/window policies are unchanged.
5. Call `Shutdown` while GL is still current, before destroying the host context.
   It releases the graph and restores configuration defaults. A subsequent
   `Open` may load a different game. Accessor references are then invalid.

All mutable engine calls require host serialization. `Current` is a non-owning
registry for legacy JNI hooks, not a concurrency or ownership API. Embedded hosts
need not register themselves. The existing dialog callback is synchronous; an
unset callback cancels the request. An asynchronous Flutter dialog requires a
separate host/API change.

## Reproducible checks

```sh
cmake -S . -B build-test -DARTC_BUILD_TESTS=ON -DARTC_ENABLE_FFMPEG=OFF
cmake --build build-test -j8
ctest --test-dir build-test --output-on-failure
cmake -S tests/embedded_consumer -B build-consumer -DARTC_ENABLE_FFMPEG=OFF
cmake --build build-consumer -j8
ctest --test-dir build-consumer --output-on-failure

cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DARTC_BUILD_JNI=ON -DARTC_BUILD_CLI=OFF -DARTC_ENABLE_FFMPEG=OFF
cmake --build build-android -j8
python3 tools/check_android_exports.py build-android/libartemis.so \
  --nm "$ANDROID_NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/llvm-nm"
cmake --install build-android --prefix build-android/install
```

Also configure `tests/embedded_consumer` with the Android/OHOS toolchain to verify
final linking without the JNI host. For shell-only OHOS regressions explicitly
select HEADLESS graphics; this configuration must not be shipped in the app.
A device may disallow unsigned shell executables; report that restriction and
validate with a signed application instead.

The export checker protects the six original JNI names plus bootstrap and existing
extra entry points. It does not prove Java descriptors or runtime behavior. Test
Tyranor-Next and the original shell on Android separately, including the gaps
listed above. Native iOS SDK builds, Apple audio, and platform texture/lifecycle
integration are separate requirements.
