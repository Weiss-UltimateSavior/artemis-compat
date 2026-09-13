#!/usr/bin/env python3
"""tag_matrix.py — tag coverage census for artemis-compat.

Scans the engine's tag dispatch surface and prints a markdown matrix. Optionally
diffs it against a reference tag list (e.g. a sibling clean-room engine's tag
registry), so coverage gaps are visible without hand-maintaining a table.

Usage:
    python3 tools/tag_matrix.py                       # engine tags only
    python3 tools/tag_matrix.py --reference PATH      # + reference diff
    python3 tools/tag_matrix.py --json                # machine-readable

Reference extraction is deliberately permissive: any `"tagname"` string literal
that looks like a tag identifier is collected.
"""
import argparse
import json
import os
import re
import sys

ENGINE_SRC = os.path.join(os.path.dirname(__file__), "..", "src", "script", "lua_engine.cpp")
ASB_SRC = os.path.join(os.path.dirname(__file__), "..", "src", "script", "asb_parser.cpp")

# `tagname == "foo"` / `c.name != "foo"` / `{"foo", l_handler}` method entries.
DISPATCH_RE = re.compile(r'\b(?:tagname|c\.name|kCommand)\s*==\s*"([^"]+)"')
# Native control flow in the .asb runner (if/elseif/else/loop/goto).
ASB_RE = re.compile(r'\bline\.command\s*==\s*"([^"]+)"')
METHODS_RE = re.compile(r'\{"([a-zA-Z_][a-zA-Z0-9_]*)"\s*,\s*l_[a-zA-Z0-9_]+\}')
REFERENCE_RE = re.compile(r'"([a-z_][a-z0-9_/@]*)"')


def engine_tags():
    tags = set()
    for path, pattern in ((ENGINE_SRC, DISPATCH_RE), (ASB_SRC, ASB_RE)):
        with open(path, encoding="utf-8", errors="replace") as f:
            tags |= set(pattern.findall(f.read()))
    return tags


def reference_tags(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    return set(REFERENCE_RE.findall(text))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", default=None, help="reference source file to diff against")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of markdown")
    args = ap.parse_args()

    ours = sorted(engine_tags())
    result = {"engine_count": len(ours), "engine": ours}

    if args.reference:
        ref = sorted(reference_tags(args.reference))
        missing = sorted(set(ref) - set(ours))
        extra = sorted(set(ours) - set(ref))
        result.update({"reference_count": len(ref), "missing": missing, "extra": extra})

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0

    print("# artemis-compat tag coverage\n")
    print(f"Engine dispatch tags: **{len(ours)}**\n")
    for i in range(0, len(ours), 6):
        print("`" + "` `".join(ours[i:i + 6]) + "`")
    if args.reference:
        print(f"\n## vs reference ({result['reference_count']} tags)\n")
        print(f"- missing ({len(result['missing'])}): " +
              ", ".join(f"`{t}`" for t in result["missing"]) or "- missing: none")
        print(f"- extra ({len(result['extra'])}): " +
              ", ".join(f"`{t}`" for t in result["extra"]) or "- extra: none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
