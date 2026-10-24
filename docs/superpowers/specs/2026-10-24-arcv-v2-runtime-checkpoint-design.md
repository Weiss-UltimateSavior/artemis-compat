# ARCV v2 Runtime Checkpoint Design

Date: 2026-10-24
Base: `origin/main` at `31099b581a2f2355d0a32ac2d840a3a387dec62e`
Status: Approved direction; pending implementation plan

## 1. Problem and evidence

ARCV v1 stores only a variable bank. During `LoadSnapshot`, the engine discards
script flow and GL-backed scene state, then relies on the game's `onLoad`
callback to rebuild derived state. A framework callback may restore message
window furniture without restoring either the scenario cursor or scene images.
The load then reports success with no loaded runner and no scene layers.

This is a general checkpoint-format and restoration-contract defect, not a
NextScene host issue and not a title-specific script issue. The upstream commit
`56896f8` already fixes the separate post-discard quickjump return-frame bug;
that behavior and its regression must be preserved.

## 2. Goals

1. Define an ARCV v2 checkpoint that restores the variable bank, script flow,
   and supported scene state without title-specific branches.
2. Validate the complete checkpoint before changing the running engine.
3. Reject ARCV v1 and unsupported v2 content explicitly, without erasing the
   current runner or scene and without reporting a successful restore.
4. Preserve the existing BOWS/BOWG importer behavior and its byte-level
   compatibility boundaries.
5. Keep all production writes on the existing O_EXCL + fsync + rename atomic
   pipeline, with size limits and CRC validation.
6. Add synthetic, platform-independent regressions before implementation and
   validate the same engine on desktop GL, Android, embedded hosts, and OHOS.

## 3. Non-goals

- Exporting ARCV as BOWS or claiming original-engine save compatibility.
- Serializing Lua closures, VM pointers, GL handles, native file descriptors,
  threads, or whole process memory.
- Changing NextScene UI, storage permissions, bundle signing, or game scripts.
- Adding title-specific script, path, resource, or bundle-name exceptions.
- Weakening existing assertions or silently selecting headless/audio-null
  backends to make production validation pass.

## 4. Alternatives considered

### 4.1 Runner-only v2

Rejected. Restoring a cursor without the scene still leaves frameworks whose
`onLoad` only rebuilds message furniture with an empty background. It would make
success depend on a particular framework's replay behavior.

### 4.2 Whole Lua VM and native runtime snapshot

Rejected. Closures, GL resources, media objects, and native pointers cannot be
serialized safely or portably. The design instead persists semantic state and
rebuilds native resources.

### 4.3 Runner and scene v2 (selected)

Persist enough semantic state to resume execution and reconstruct the visible
scene. Keep resource reconstruction in the existing engine ownership graph.

## 5. Format and compatibility

Introduce a separate `CheckpointV2` model rather than extending `NativeSave` or
changing BOWS meanings. The ARCV container has:

1. Magic `ARCV` and version `2`.
2. A variable-bank section using the existing scalar/Pluto string conventions.
3. A runner-flow section.
4. A scene section.
5. Explicit section lengths, total-size limits, and a final CRC.

Readers accept only a complete, structurally valid document. Unknown versions,
trailing bytes, duplicate critical fields, invalid UTF-8 where text is required,
non-finite geometry, and out-of-range indices are errors. No partial output is
installed into the live checkpoint object.

ARCV v1 is recognized before `ResetEphemeral` and rejected with a specific
"runtime state is missing" diagnostic. It must not be decoded as v2 or treated
as a successful compatibility fallback. Existing v1 files remain untouched.

## 6. State ownership

### 6.1 `AsbRunner`

Add narrow capture/validate/restore interfaces for a serializable flow model:

- current file and instruction index;
- loaded/halted and pending-instruction flags;
- caller frames in original order, including file, return PC, halted state,
  macro-scope marker, and portable wait data;
- any cross-file label context required for identical subsequent resolution.

Restore must preserve the post-`DiscardFlow` quickjump behavior introduced by
`56896f8`. Loading parses scripts through the existing pack source and must not
rerun load-time Lua chunks during prevalidation. Restoration runs load-time
chunks only when the existing script-loading contract requires it, never twice.

Absolute `steady_clock` values are not written. Portable waits store remaining
durations and are re-based at restore time. Active input events, pressed
pointers, drags, and event interruption frames are ephemeral and are not saved.

### 6.2 `LuaEngine`

Own the checkpoint orchestration and script-side state not contained in the
runner, including macro variable scopes required by saved frames. Save calls
the registered `onSave` callback first, then captures a consistent engine view.
Load restores macro scopes only with their associated runner flow.

### 6.3 `Compositor`

Add semantic capture/restore support using the existing layer table and font
state, not a second runtime scene graph. Add only the source/style metadata
needed for reconstruction:

- image source paths, layer geometry, clipping, visibility, alpha, anchors,
  transforms, drag bounds, message-overlay ordering, and effect parameters;
- text content, font source, size, color, wrapping, ruby and line-break style;
- per-type `lyevent` registrations associated with the saved layers.

Do not serialize GL names, atlas pages, uploaded textures, mask FBOs, or shader
program handles. Those are recreated through existing compositor/resource
paths. Restoration invalidates lazy indices and bumps the revision so the host
presents the restored scene.

### 6.4 Explicit support boundary

The initial v2 implementation must handle ordinary image/text layers and their
input registrations used by dialogue and menu scenes. Video/E-mote instances,
active transitions, and active tweens require their own semantic models and
are outside this change. If any are present, checkpoint creation reports an
unsupported-state error before replacing the destination slot; restoration
never silently drops them. This is a deliberate safe limitation, not a claim
that all visual features are covered.

## 7. Restore protocol

`LoadSnapshot` becomes a two-phase operation.

### Phase 1: decode and prevalidate

- Read within existing size limits and verify magic, version, sections, and CRC.
- Validate variable graphs without closures or native pointers.
- Resolve every saved script and validate all cursor/return positions.
- Validate scene commands, finite geometry, resource paths, text styles, and
  supported object types.
- Complete all checks before clearing variables, runner state, compositor
  state, audio, or queues.

Any failure returns `false` before mutation and leaves the current game running.

### Phase 2: commit and rebuild

Follow the existing monotone load phases:

1. `ResetEphemeral`: clear queued input, active waits/events, media, and active
   animation state after validation has succeeded.
2. `RestoreData`: install variables and macro scopes.
3. `SnapshotScene`: recreate compositor semantic state and native resources.
4. Restore the validated runner flow without executing queued old events.
5. `RebuildDerived`: call the game's `onLoad` once to rebuild message pages,
   audio replay, and framework-specific derived state.

A successful load logs the v2 section counts. A failed callback returns `false`;
it must not print a restored-success message.

## 8. Save protocol and failure behavior

- Keep reserved-name rejection for `system.dat` and `saveg.dat`, slot path
  confinement, and the reentrancy guard.
- Do not replace an existing slot if capture, encoding, CRC, or atomic writing
  fails.
- Do not update the system bank as though a checkpoint succeeded when the v2
  checkpoint itself was not committed.
- Keep `SaveSystemData` behavior separate from scenario checkpoint commits.
- Preserve current slot metadata and thumbnails; they are derived artifacts,
  not substitutes for runtime state.

## 9. Test strategy

### 9.1 New failing-first synthetic regressions

Use only generated packs, scripts, fonts, and colors:

1. v2 round trip preserves variables, current file/PC, halted/pending state,
   nested/cross-file returns, macro scopes, and remaining waits.
2. Post-load quickjump and its final `[return]` continue instead of halting,
   explicitly covering the `56896f8` contract.
3. Image/text layers, fonts, geometry, visibility, message overlays, and
   per-type input registrations are reconstructed.
4. A later click advances restored dialogue and produces the next text.
5. v1, bad CRC, truncated sections, invalid PCs, missing scripts/resources,
   non-finite geometry, and unsupported visual objects fail before mutation.
6. Failed saves preserve an existing slot byte-for-byte; failed loads preserve
   the current runner, variables, and scene.
7. BOWS/BOWG existing regression expectations remain unchanged.

### 9.2 Platform and integration validation

- Full upstream ctest suite, including the existing 30-test baseline and new
  cases.
- Real desktop GL compositor checks; headless tests are not presented as GL
  proof.
- Android `libartemis.so` build and required export checks, with actual shell
  behavior separately identified if a device is unavailable.
- Embedded-consumer final link test.
- OHOS SDK 20 arm64 build and signed HAP Build-ID consistency.
- On the OHOS emulator: new save -> load -> restored background/text -> click
  advances, plus title-load and quicksave/quickload paths.
- Cross-engine and multi-title smoke tests to detect global-bank pollution,
  framework-specific assumptions, and NextScene lifecycle regressions.

## 10. Commit layering

Use small Conventional Commits with no commercial title names:

1. `docs(save): design ARCV v2 runtime checkpoints`
2. `feat(save): add validated ARCV v2 container`
3. `feat(script): capture and restore runner flow`
4. `feat(render): capture and restore semantic scene state`
5. `fix(save): restore v2 checkpoints atomically`
6. `test(save): cover runtime checkpoint regressions` (test-only additions may
   instead be included in the corresponding small implementation commit when
   required by red-green evidence)
7. `chore(artemis): pin checkpoint runtime fixes` in the downstream repository.

Public engine commits are published before the downstream submodule pointer is
changed. Existing unrelated working-tree changes and signing files are never
included.
