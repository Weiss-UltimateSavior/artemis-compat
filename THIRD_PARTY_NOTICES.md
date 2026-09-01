# Third-party notices

The `third_party/` components are distributed under their **own** licenses,
which are independent of the GPL-3.0 license applied to this project's
own source code under `src/`, `CMakeLists.txt`, and the documentation.

## lua-5.1.5 (`third_party/lua-5.1.5/`)
- License: MIT
- Copyright (C) 1994-2012 Lua.org, PUC-Rio
- See `third_party/lua-5.1.5/doc/*.html` for the full license text.
- Vendored unmodified source (the `lua.c`/`luac.c` standalone interpreters are
  excluded from the build).

## stb_vorbis (`third_party/stb_vorbis/stb_vorbis.c`)
- Author: Sean Barrett (nothings.org)
- License: dual public-domain / MIT (see the file header comment).
- Vendored as-is; used for Ogg Vorbis decoding.