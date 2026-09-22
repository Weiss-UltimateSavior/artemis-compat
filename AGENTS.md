# Contributor instructions

Read `AGENT.md` before changing this repository. It is the canonical engine
behavior, layering and validation guide. Embedded consumers are additional hosts;
the official-shell-compatible Android `libartemis.so` remains a required product.

Keep Android JNI method signatures, NativeActivity entry points and existing host
defaults intact when adding embedded build options. Do not introduce Flutter or
downstream product dependencies into the engine. See the embedded-host section in
`AGENT.md` for build and regression requirements.
