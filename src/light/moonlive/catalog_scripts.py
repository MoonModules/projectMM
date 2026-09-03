"""Generate the MoonLive script CATALOG: the name and role of every factory script.

Called by catalog_scripts.cmake, never by hand. It takes a file listing the script paths (one per
line) and writes a header holding their NAMES, not their contents: a name costs ~12 bytes where a
script costs ~800, so the catalog stays a few KB however large the library grows. The device fetches
a script's text the first time someone picks it (the UI fetches it from GitHub and posts it to /api/file).

Python rather than pure CMake so the role can be derived from the extension in one place and the
name collision below can be reported properly.
"""

import re
import sys
from pathlib import Path

# The role a script plays, from its extension. This mirrors MoonLiveScriptFile.h's kEffectExt /
# kLayoutExt / kModifierExt, and it is what a picker filters on: the DEVICE keeps one flat
# directory, so the extension is the only role signal once a file lands there.
ROLE_BY_EXT = {".mle": "Effect", ".mll": "Layout", ".mlm": "Modifier", ".mls": "Service",
               ".mlp": "Palette"}

# Where each role lives in the repo. The device keeps one flat directory, so this is only ever part
# of the download URL.
FOLDER_BY_ROLE = {"Effect": "effects", "Layout": "layouts", "Modifier": "modifiers", "Service": "services",
                  "Palette": "palettes"}


# What a script DECLARES about itself, read from its source. The script is the one home for this
# (a user-written script never appears in a catalog at all), so what a build extracts here is a
# COPY for the picker to show before a script is downloaded. Once a script is on the device the
# compiled program is the truth: nothing reads these values to decide behavior.
#
# Two fixed forms, deliberately not a parser. Anything more would be a second reader of the
# language drifting away from the real one; this recognizes exactly what the scripts write and
# FAILS THE BUILD on anything else, so the two cannot disagree quietly.
DIM_RE  = re.compile(r'^\s*int\s+dimensions\(\)\s*\{\s*return\s+([123])\s*;\s*\}', re.M)
TAGS_RE = re.compile(r'^\s*string\s+tags\(\)\s*\{\s*return\s+"([^"]*)"\s*;\s*\}', re.M)


def declared(path: Path) -> tuple[int, str]:
    """The dimension and tags a script declares: (0, "") when it declares neither.

    0 rather than 2 for "unsaid": the DEVICE decides what a silent script defaults to, and baking
    that default in here would freeze today's answer into every catalog ever generated.
    """
    text = path.read_text(encoding="utf-8")
    dim = DIM_RE.search(text)
    tags = TAGS_RE.search(text)
    # A declaration the regex cannot read is a BUILD failure rather than a silent 0: a script that
    # says `int dimensions() { return someExpression; }` is legal to the real compiler, and quietly
    # cataloguing it as "unsaid" is how the two readers drift apart.
    if "dimensions()" in text and not dim:
        raise ValueError(f"{path}: dimensions() is not a plain `int dimensions() {{ return 1|2|3; }}`")
    if "tags()" in text and not tags:
        raise ValueError(f"{path}: tags() is not a plain `string tags() {{ return \"…\"; }}`")
    return (int(dim.group(1)) if dim else 0, tags.group(1) if tags else "")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: catalog_scripts.py <filelist> <out.h>", file=sys.stderr)
        return 2

    paths = [Path(p) for p in Path(sys.argv[1]).read_text(encoding="utf-8").split("\n") if p.strip()]
    out = Path(sys.argv[2])

    # A name must be unique on the device: the repo's subfolders vanish when scripts land in one
    # flat directory, so two roles sharing a base name would collide there. Catching it here turns a
    # silent overwrite on a user's device into a build failure.
    seen: dict[str, Path] = {}
    for p in paths:
        if p.suffix not in ROLE_BY_EXT:
            print(f"catalog_scripts: {p} has no known script extension "
                  f"({', '.join(ROLE_BY_EXT)})", file=sys.stderr)
            return 1
        if p.name in seen:
            print(f"catalog_scripts: duplicate script name {p.name} "
                  f"({seen[p.name]} and {p}); the device keeps one flat directory, "
                  f"so these would collide", file=sys.stderr)
            return 1
        seen[p.name] = p

    # One array per role rather than one array of {name, folder, role}. The folder is implied by
    # the role ("effects" holds the effects) and the role by the extension, so storing either per
    # entry would be the same value repeated once per script. It also makes the picker's job a
    # range rather than a scan: it wants "every effect", which is now an array, not a filter.
    by_role: dict[str, list[str]] = {r: [] for r in ROLE_BY_EXT.values()}
    # What each script says it is, in the same order, so the picker can show a row the way the
    # module picker shows a type: its dimension and its emoji, before the script is downloaded.
    decl_by_role: dict[str, list[tuple[int, str]]] = {r: [] for r in ROLE_BY_EXT.values()}
    for p in paths:
        role = ROLE_BY_EXT[p.suffix]
        by_role[role].append(p.name)
        try:
            decl_by_role[role].append(declared(p))
        except ValueError as e:
            print(f"catalog_scripts: {e}", file=sys.stderr)
            return 1

    parts = [
        "// Auto-generated from moonlive/ by catalog_scripts.cmake. Do not edit; rebuild to update.\n",
        "//\n",
        "// The CATALOG, not the library: names only. A device carries this list and the UI fetches a\n",
        "// script's text from GitHub the first time someone picks it, so flash scales with how many\n",
        "// scripts exist rather than how large they are, and the filesystem holds only what is used.\n",
        "//\n",
        "// One array per role: the folder a script lives in is implied by its role and the role by its\n",
        "// extension, so neither is stored per entry.\n",
        "#pragma once\n",
        "#include <cstddef>\n\n",
        "namespace mm::moonlive {\n\n",
    ]

    for role, names in by_role.items():
        lower = role.lower()
        folder = FOLDER_BY_ROLE[role]
        parts.append(f"/// Every factory {lower}, by file name. They live in `moonlive/{folder}/`\n")
        parts.append("/// upstream and in the factory script directory on the device.\n")
        parts.append(f"constexpr const char* k{role}Catalog[] = {{\n")
        parts.append("".join(f'    "{n}",\n' for n in names))
        parts.append("};\n")
        parts.append(f"constexpr size_t k{role}CatalogCount = {len(names)};\n")
        # Parallel arrays rather than a struct: the name array is what every existing caller walks,
        # and a struct would rewrite each of them to reach a field they do not use.
        decls = decl_by_role[role]
        parts.append(f"/// What each {lower} above declares about itself, in the same order.\n")
        parts.append("/// A dimension of 0 means the script says nothing, so the DEVICE decides the default.\n")
        parts.append(f"constexpr unsigned char k{role}CatalogDim[] = {{\n")
        parts.append("".join(f"    {d},\n" for d, _ in decls))
        parts.append("};\n")
        parts.append(f"/// The emoji each declares, \"\" when it declares none.\n")
        parts.append(f"constexpr const char* k{role}CatalogTags[] = {{\n")
        parts.append("".join(f'    "{g}",\n' for _, g in decls))
        parts.append("};\n")
        parts.append(f'constexpr const char* k{role}Folder = "{folder}";   ///< its directory upstream\n\n')

    total = sum(len(v) for v in by_role.values())
    parts.append(f"constexpr size_t kCatalogCount = {total};   ///< every factory script, all roles\n\n")
    parts.append("} // namespace mm::moonlive\n")

    # UTF-8 EXPLICITLY, on every read and write in this file: a script's tags() is emoji, and
    # Python falls back to the platform encoding when none is named, which on Windows is
    # cp1252 and cannot encode them. The build failed there with UnicodeEncodeError while
    # every POSIX host passed, because their default already is UTF-8.
    out.write_text("".join(parts), encoding="utf-8")
    print(f"catalog: {len(paths)} scripts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
