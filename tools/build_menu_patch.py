"""Write the Death Metal menu entries for BrgGame.upk as a .PackagePatch.

Uncle Death's Charging menu (BrgUIMenu_Insurance) still holds two screens the
online game (4.18) had in its menu, but the offline release took their menu
entries out:

    Convert Death Metals   CHARGING.TXT_DM_TO_KC     menu state 6
    Currency Details       CHARGING.TXT_DM_TIMELEFT  menu state 5

This puts them back as entries 4 and 5, after the offline menu's own four:

- Initialize: two more AddTopMenuItem calls, built from the game's own call
  for the entry before them. The offline package no longer refers to the
  icons 4.18 used for them (l_icon_v02, l_icon_v01), and a patch cannot add
  references, so both use l_icon_v00 - 4.18's Death Metal purchase icon.
- Tick_TopMenu: the cursor wraps at 6 instead of 4, and picking entry 4 or 5
  opens state 6 or 5.
- RenderCaptionAndDescription: each entry's own description text.

The exchange itself is answered by lid-deathmetals.dll 0.2.0 or later; without
it the game shows a communication error.

A .PackagePatch holds only the changed functions, not the game's package. LiD
DB Editor applies it to the player's own BrgGame.upk (the format is FCH823's,
from TFC Installer). It names every object and name the new code relies on, so
it is refused, not forced, on a package where any of them moved.

Reads the game's stock BrgGame.upk and never changes it. Needs a LiD DB Editor
checkout for its package reader:

    py -3 tools/build_menu_patch.py --manager PATH/TO/LiD-DB-Editor --upk PATH/TO/BrgGame.upk
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
OUT = REPO / "mods" / "Death Metals" / "tfc" / "menu" / "Game" / "BrgGame" / "CookedPCConsole" / \
    "BrgGame.upk.PackagePatch"
HEADER = 48                     # a function's data before its script
REFERENCE_VERSION = 12          # the reference layout TFC Installer writes
CLS = "BrgUIMenu_Insurance"
ICON = "BrgGameBase.BrgUIResource_InsuranceMenu.stx_ui_insurancemenu_v00__flip_all__icon_all"

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("--manager", required=True, type=Path, help="a LiD DB Editor checkout")
parser.add_argument("--upk", required=True, type=Path, help="the game's stock BrgGame.upk")
parser.add_argument("--out", type=Path, default=OUT)
parser.add_argument("--listing", type=Path, help="also write the changed code, disassembled, to this file")
args = parser.parse_args()

sys.path.insert(0, str(args.manager.resolve()))
sys.path.insert(0, str(HERE))
from lid_db_manager.upk import apply as A                   # noqa: E402
from lid_db_manager.upk import package as P                 # noqa: E402
from lid_db_manager.upk import packagepatch as PP           # noqa: E402
from ue3dis import Dis                                      # noqa: E402

raw = args.upk.read_bytes()
print(f"{args.upk.name}: SHA-1 {hashlib.sha1(raw).hexdigest()}")
summary, flat = P.decompressed(raw)
pkg = P.Package(summary, flat, P.read_names(flat, summary), P.read_imports(flat, summary),
                P.read_exports(flat, summary), source=args.upk)
names = lambda i: pkg.names[i].text      # noqa: E731
objs = lambda i: pkg.full_path(i)        # noqa: E731
o = lambda ref: struct.pack("<i", ref)   # noqa: E731
u16 = lambda v: struct.pack("<H", v)     # noqa: E731


def ref_of(path: str) -> int:
    """The object reference (export > 0, import < 0) with this full path."""
    hits = [i + 1 for i in range(len(pkg.exports)) if pkg.full_path(i + 1) == path]
    hits += [-(i + 1) for i in range(len(pkg.imports)) if pkg.full_path(-(i + 1)) == path]
    if len(hits) != 1:
        sys.exit(f"{path}: {len(hits)} matches - is this BrgGame.upk?")
    return hits[0]


def mem_len(chunk: bytes) -> int:
    d = Dis(chunk, names, objs)
    while d.pos < len(chunk):
        d.expr()
    return d.mem


# ------------------------------------------------------------ expression bits
def inst(ref): return b"\x01" + o(ref)
def local(ref): return b"\x00" + o(ref)
def intb(v): return b"\x2c" + bytes([v])
def byte(v): return b"\x24" + bytes([v])
def native(n, *a): return bytes([n]) + b"".join(a) + b"\x16"
def let(a, b): return b"\x0f" + a + b
def eq(a, b): return native(0x9A, a, b)
def cstr(text): return b"\x1f" + text.encode("latin-1") + b"\0"


class Asm:
    """Statements with forward labels; jump targets are memory offsets."""

    def __init__(self):
        self.parts = []

    def code(self, b): self.parts.append(("code", b)); return self
    def label(self, name): self.parts.append(("label", name)); return self
    def jump(self, to): self.parts.append(("jump", to, None)); return self
    def jumpifnot(self, to, cond): self.parts.append(("jumpifnot", to, cond)); return self

    def assemble(self, at: int) -> bytes:
        labels, pos = {}, at
        for p in self.parts:
            if p[0] == "label":
                labels[p[1]] = pos
            elif p[0] == "code":
                pos += mem_len(p[1])
            else:
                pos += 3 + (mem_len(p[2]) if p[2] else 0)
        out = b""
        for p in self.parts:
            if p[0] == "code":
                out += p[1]
            elif p[0] in ("jump", "jumpifnot"):
                to = labels[p[1]] if isinstance(p[1], str) else p[1]
                out += (b"\x06" if p[0] == "jump" else b"\x07") + u16(to) + (p[2] or b"")
        return out


class Fn:
    """One function's script, with code added before its END token."""

    def __init__(self, path, stock_sizes):
        self.ref = ref_of(path)
        self.path = path
        self.data = pkg.export_data(self.ref - 1)
        self.mem, self.disk = struct.unpack_from("<ii", self.data, HEADER - 8)
        self.code = bytearray(self.data[HEADER:HEADER + self.disk])
        self.tail = self.data[HEADER + self.disk:]
        d = Dis(bytes(self.code), names, objs)
        self.lines = d.run()
        if (len(self.lines), self.mem, self.disk) != stock_sizes or self.lines[-1][4] != "END":
            sys.exit(f"{path} is not the one this was written for: {len(self.lines)} statements, "
                     f"{self.mem}/{self.disk} bytes. Is this the stock BrgGame.upk of game 1.89?")
        self.targets = {t for _, _, t in d.targets}
        self.by_mem = {line[1]: line for line in self.lines}
        self.added = b""
        self.added_at = self.mem - 1            # the END token's place
        self.replaced = []

    def stmt(self, at, starts_with):
        line = self.by_mem.get(at)
        if not line or not line[4].startswith(starts_with):
            sys.exit(f"{self.path} @{at:#x}: expected `{starts_with}`, found `{line and line[4]}`")
        return line

    def stmt_bytes(self, at, starts_with):
        d, m, dl, ml, _ = self.stmt(at, starts_with)
        return bytes(self.code[d:d + dl])

    def after(self, at, starts_with):
        _, m, _, ml, _ = self.stmt(at, starts_with)
        return m + ml

    def add(self, asm: Asm) -> int:
        at = self.added_at + mem_len(self.added)
        self.added += asm.assemble(at)
        return at

    def divert(self, at, starts_with, asm: Asm):
        """Replace the statement at `at` with a jump to `asm`, added on the end."""
        d, m, dl, ml, _ = self.stmt(at, starts_with)
        to = self.add(asm)
        refs, extra = divmod(ml - dl, 4)
        pad = dl - 3 - 5 * refs
        if pad < 0 or extra:
            sys.exit(f"{self.path} @{at:#x}: statement too short to replace")
        # an object constant is 4 bytes on disk and 8 in memory, so the filler
        # keeps both sizes of the statement it replaces
        self.code[d:d + dl] = b"\x06" + u16(to) + (b"\x20" + o(0)) * refs + b"\x0b" * pad
        self.replaced.append((m, m + ml))

    def poke_once(self, at, starts_with, old: bytes, new: bytes):
        """Change the one occurrence of `old` in the statement at `at`."""
        d, m, dl, ml, _ = self.stmt(at, starts_with)
        body = bytes(self.code[d:d + dl])
        if body.count(old) != 1 or len(old) != len(new):
            sys.exit(f"{self.path} @{at:#x}: cannot change {old.hex()} here")
        i = d + body.index(old)
        self.code[i:i + len(old)] = new

    def finish(self):
        for lo, hi in self.replaced:
            if any(lo < t < hi for t in self.targets):
                sys.exit(f"{self.path}: a jump lands inside the replaced statement at {lo:#x}")
        code = bytes(self.code[:-1]) + self.added + b"\x53"
        d = Dis(code, names, objs)
        lines = d.run()
        if lines[-1][4] != "END" or d.pos != len(code):
            sys.exit(f"{self.path}: the new script does not read back cleanly")
        starts = {line[1] for line in lines}
        for _, kind, t in d.targets:
            if t not in starts:
                sys.exit(f"{self.path}: a {kind} to {t:#x} does not land on a statement")
        if d.mem > 0xFFFF:
            sys.exit(f"{self.path}: too long for 16-bit jumps")
        self.new_lines = lines
        self.used_names = {i for _, i in d.names}
        self.used_objects = {r for _, r in d.objs if r}
        self.new_data = self.data[:HEADER - 8] + struct.pack("<ii", d.mem, len(code)) + code + self.tail
        print(f"{self.path}: {self.mem}/{self.disk} -> {d.mem}/{len(code)} script bytes (memory/disk)")
        return self.new_data


TOP_INDEX = f"I:{CLS}.mTopMenuSelectIndex"
top_index = inst(ref_of(f"{CLS}.mTopMenuSelectIndex"))
ENTRIES = [  # (top menu entry, its text, its description, the menu state it opens)
    (4, "CHARGING.TXT_DM_TO_KC", "CHARGING.TXT_DM_TO_KC_DES", 6),
    (5, "CHARGING.TXT_DM_TIMELEFT", "CHARGING.TXT_DM_TIMELEFT_DES", 5),
]

# ----------------------------------------------------------------- Initialize
init = Fn(f"{CLS}.Initialize", (28, 1739, 1187))
last_item = init.stmt_bytes(0x255, "AddTopMenuItem('CHARGING.TXT_VIP_SETTING'")
icon_from, icon_to = o(ref_of(f"{ICON}.l_icon_v05")), o(ref_of(f"{ICON}.l_icon_v00"))
if last_item.count(icon_from) != 1:
    sys.exit("the VIP setting entry is not built as expected")
new_items = b""
for _, text, _, _ in ENTRIES:
    item = last_item.replace(cstr("CHARGING.TXT_VIP_SETTING"), cstr(text)).replace(icon_from, icon_to)
    new_items += item
COMMERCE = f"I:{CLS}.mCommerceInterface = new("
init.divert(0x2BD, COMMERCE, Asm()
            .code(new_items)
            .code(init.stmt_bytes(0x2BD, COMMERCE))
            .jump(init.after(0x2BD, COMMERCE)))

# --------------------------------------------------------------- Tick_TopMenu
tick = Fn(f"{CLS}.Tick_TopMenu", (61, 985, 757))
# the cursor wraps at 6 entries, as in 4.18
tick.poke_once(0x19D, f"N161({TOP_INDEX}, 4)", intb(4), intb(6))
tick.poke_once(0x1AD, f"jumpifnot 0x1ca (N153({TOP_INDEX}, 4))", intb(4), intb(6))
tick.poke_once(0x1BD, f"N162({TOP_INDEX}, 4)", intb(4), intb(6))
# picking entry 4 or 5: set the state it opens before the switch, which has no
# case for them and leaves it alone
CONFIRM = "jumpifnot 0x39a (IsConfirmButtonPressed())"
next_state = tick.stmt_bytes(0x1FD, f"L:{CLS}.Tick_TopMenu.NextState = 7b")
if not next_state.endswith(byte(7)):
    sys.exit("Tick_TopMenu: the NextState assignment is not built as expected")
set_state = lambda s: next_state[:-2] + byte(s)     # noqa: E731
asm = Asm().code(tick.stmt_bytes(0x1D9, CONFIRM))
for entry, _, _, state in ENTRIES:
    asm.jumpifnot(f"not{entry}", eq(top_index, intb(entry))).code(set_state(state)).label(f"not{entry}")
tick.divert(0x1D9, CONFIRM, asm.jump(tick.after(0x1D9, CONFIRM)))

# ------------------------------------------------- RenderCaptionAndDescription
cap = Fn(f"{CLS}.RenderCaptionAndDescription", (103, 3156, 2296))
buy_vip = cap.stmt_bytes(0xA0, f"L:{CLS}.RenderCaptionAndDescription.Description = 'CHARGING.TXT_BUY_VIP_DES'")
description = buy_vip[:-len(cstr("CHARGING.TXT_BUY_VIP_DES"))]
# after the top menu's description switch, whichever case ran
asm = Asm()
for entry, _, des, _ in ENTRIES:
    asm.jumpifnot(f"not{entry}", eq(top_index, intb(entry))).code(description + cstr(des)).label(f"not{entry}")
cap.stmt(0x89, f"switch<{CLS}.mTopMenuSelectIndex")
cap.divert(0x156, "jump 0x4c5", asm.jump(0x4C5))

functions = [init, tick, cap]
for fn in functions:
    fn.finish()


# -------------------------------------------------------------------- the patch
def fstring(text: str) -> bytes:
    encoded = text.encode("latin-1") + b"\0"
    return struct.pack("<i", len(encoded)) + encoded


def class_of(ref: int) -> tuple[str, str]:
    if ref < 0:
        imp = pkg.imports[-ref - 1]
        return pkg.name(imp.class_name, imp.class_name_number), \
            pkg.name(imp.class_package, imp.class_package_number)
    class_index = pkg.exports[ref - 1].class_index
    return (pkg.object_name(class_index) if class_index else "Class"), ""


def path_of(ref: int) -> str:
    return pkg.full_path(ref).replace(".", "\\")


used_objects = {fn.ref for fn in functions}
used_names = set()
for fn in functions:
    used_objects |= fn.used_objects
    used_names |= fn.used_names
object_refs = sorted(used_objects, key=lambda r: (r < 0, abs(r)))
name_refs = sorted(used_names)

out = bytearray()
out += struct.pack("<i", 2)
for table in (pkg.names, pkg.imports, pkg.exports):
    out += struct.pack("<ii", len(table), 0)
out += struct.pack("<i", len(functions))
for fn in functions:
    out += struct.pack("<ii", fn.ref - 1, 0)
    out += struct.pack("<i", len(fn.new_data)) + fn.new_data
out += struct.pack("<i", REFERENCE_VERSION)
out += struct.pack("<i", len(name_refs))
for i in name_refs:
    out += struct.pack("<i", i) + fstring(pkg.names[i].text)
out += struct.pack("<i", len(object_refs))
for ref in object_refs:
    class_name, class_package = class_of(ref)
    out += struct.pack("<i", ref) + fstring(path_of(ref))
    if ref < 0:
        out += fstring(path_of(ref))
    out += fstring(class_name)
    if ref < 0:
        out += fstring(class_package)
patch_bytes = bytes(out)

# ----------------------------------------------------- check it before writing it
patch = PP.read(patch_bytes)
fresh = P.Package(summary, flat, pkg.names, pkg.imports, P.read_exports(flat, summary), source=args.upk)
if A.problems(fresh, patch):
    sys.exit("the patch does not fit: " + "; ".join(A.problems(fresh, patch)))
applied, _ = A.apply(fresh, patch)
expected = bytearray(flat)
entries = P.read_exports(flat, summary)
for fn in functions:
    e = entries[fn.ref - 1]
    expected[e.serial_offset:e.serial_offset + e.serial_size] = bytes(e.serial_size)
for fn in functions:
    at = len(expected)
    expected += fn.new_data
    A._place_entry(expected, entries[fn.ref - 1], at, len(fn.new_data))
if applied != bytes(expected):
    sys.exit("applying the patch does not give the expected package")
re_exports = P.read_exports(applied, summary)
for fn in functions:
    e = re_exports[fn.ref - 1]
    if bytes(applied[e.serial_offset:e.serial_offset + e.serial_size]) != fn.new_data:
        sys.exit(f"{fn.path}: not where its export entry says")

args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_bytes(patch_bytes)
print(f"checks {len(name_refs)} name(s), {len(object_refs)} object(s); wrote {args.out} "
      f"({len(patch_bytes):,} bytes), SHA-256 {hashlib.sha256(patch_bytes).hexdigest()}")

if args.listing:
    with open(args.listing, "w", encoding="utf-8") as f:
        for fn in functions:
            f.write(f"# {fn.path}\n")
            changed = {m for m, _ in fn.replaced}
            for d, m, dl, ml, t in fn.new_lines:
                if m >= fn.added_at or m in changed or m in (0x19D, 0x1AD, 0x1BD):
                    f.write(f"{m:04x} {t}\n")
