"""Per-game defaults for the RE tools in tools/re (mcla only).

    from game_config import add_game_argument, get_game
    add_game_argument(ap)                  # --game {mcla}, --work DIR
    g = get_game(args.game, args.work)     # g.pe, g.register_cpp, g.exempt, ...

Paths (nothing here is ever inside the repo except the codegen TOMLs):
    $WORK  work root: SDK, generated code, RE output, logs.
           --work DIR, else env MCLA_WORK, else C:/Users/Ali/Development/mcla-x86-work
    $GAME  game files (disc image, default.xex, extracted game/).
           --game DIR (xex_image.py, codegen.py), else env MCLA_GAME, else
           C:/Users/Ali/Development/dumps/mcla-x360

Ported from the owner's iOS port (tools/core/game_config.py). Nothing here is a code range: those come
from the PE section table (pe_sections.py).
"""
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

_LEGACY_WORK = "C:/Users/Ali/Development/mcla-x86-work"
_LEGACY_GAME = "C:/Users/Ali/Development/dumps/mcla-x360"
# the owner's folders when they exist; elsewhere the folder mcla-x86-work next to the repo (tools/setup.py's layout)
DEFAULT_WORK = _LEGACY_WORK if os.path.isdir(_LEGACY_WORK) else (REPO.parent / "mcla-x86-work").as_posix()
DEFAULT_GAME = _LEGACY_GAME if os.path.isdir(_LEGACY_GAME) else DEFAULT_WORK + "/game"

# Pinned identity of the supported build (Complete Edition, title 545407F8, media 5940C9DB, v0.0.0.8).
XEX_SHA256 = "c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432"
PE_SHA256 = "5a9de5cd05f32ae66d2a7fd3b09a569e18239ffa258bb1a0efb9126de1cff319"   # decrypted image, 10,354,688 bytes
PE_SIZE = 0x9E0000

# [project] name of the manifest. It prefixes the generated files (nfsmw_register.cpp, nfsmw_init.h, ...).
PROJECT = "nfsmw"


def work_root(arg=None):
    """$WORK: the flag, else MCLA_WORK, else the default."""
    return Path(arg or os.environ.get("MCLA_WORK") or DEFAULT_WORK)


def game_dir(arg=None):
    """$GAME: the flag, else MCLA_GAME, else the default."""
    return Path(arg or os.environ.get("MCLA_GAME") or DEFAULT_GAME)


@dataclass(frozen=True)
class Game:
    name: str
    work_root: Path                  # $WORK
    register_cpp: Path               # generated function table (exists only after codegen)
    codegen_dir: Path                # manifest + include TOMLs (huecos.toml, ...)
    manifest_name: str
    exempt: frozenset = frozenset()  # addresses the indirect-target scan must not report (see _mcla)
    prior_lists: dict = field(default_factory=dict)   # name -> (path, regex) of other projects' address lists
    switch_toml: Path = None         # harvest_switch_tables.toml (jump-table labels), optional

    @property
    def pe(self):
        return self.work_root / "re" / "default.pe"

    @property
    def prior(self):
        return self.work_root / "prior"

    @property
    def scratch(self):
        return self.work_root / "scratch"

    @property
    def huecos_toml(self):
        return self.codegen_dir / "huecos.toml"


def _mcla(work=None):
    root = work_root(work)
    return Game(
        name="mcla",
        work_root=root,
        register_cpp=root / "generated" / "default" / f"{PROJECT}_register.cpp",   # project name stays nfsmw
        codegen_dir=REPO / "mcla" / "codegen",
        manifest_name="mcla_manifest.toml",
        # Reviewed in the iOS port (task 2.4): the indirect-target scan keeps reporting these, none is a function.
        #   7 SEH scope-table fields in .rdata (__except JumpTargets / TryEnd) that the SDK labels itself:
        #     0x82134350 0x82135AF4 0x82135C6C 0x823DEF0C 0x823E2B88 0x823E6130 0x823E6384
        #   8 first cases of computed switches (`lis/addi; add; mtctr; bctr`), emitted as a C `switch`:
        #     0x823E1890 0x823E1AE0 0x823E43C0 0x823E52E0 0x8242079C 0x82421B00 0x82428B4C 0x8245F2E8
        # memcpy 0x823DA950 / memset 0x823D9890 join this set only if [rexcrt] is enabled (overrides.toml).
        exempt=frozenset({0x82134350, 0x82135AF4, 0x82135C6C, 0x823DEF0C, 0x823E2B88, 0x823E6130, 0x823E6384,
                          0x823E1890, 0x823E1AE0, 0x823E43C0, 0x823E52E0, 0x8242079C, 0x82421B00, 0x82428B4C,
                          0x8245F2E8}),
        prior_lists={},              # LARecomp / Renan lists: pass --extra-list FILE (see find_indirect_targets.py)
        switch_toml=root / "re" / "harvest_switch_tables.toml",
    )


GAME_NAMES = ("mcla",)


def add_game_argument(ap):
    ap.add_argument("--game", default="mcla", choices=GAME_NAMES, metavar="{%s}" % ",".join(GAME_NAMES),
                    help="which game's defaults to use (only mcla exists; default mcla)")
    ap.add_argument("--work", metavar="DIR",
                    help="work root $WORK (default: env MCLA_WORK, else %s)" % DEFAULT_WORK)


def get_game(name=None, work=None):
    name = name or "mcla"
    if name not in GAME_NAMES:
        print("error: unknown game %r (choose from %s)" % (name, ", ".join(GAME_NAMES)), file=sys.stderr)
        raise SystemExit(2)
    return _mcla(work)


def parse_addresses(values):
    """['0x82000000,0x82000004', '0x82000008'] -> {0x82000000, ...} (for repeatable --exempt style options)."""
    out = set()
    for v in values or []:
        for tok in str(v).replace(",", " ").split():
            out.add(int(tok, 16))
    return out


# ---- the generated register table -------------------------------------------------------------------------
# <project>_register.cpp comes in two forms (the tools below accept both):
#   stock SDK v0.10.0:  one call per function, in the registration function
#       registrar->SetFunction(0x821BDA90, sub_821BDA90);
#   with the owner's register-table patch (iOS carbon 0100): one constant table and a loop
#       const RexRegisterEntry kRexRegisterTable[] = { { 0x821BDA90, sub_821BDA90 }, ..., { 0, nullptr }, };
_SET_FUNCTION = re.compile(r"registrar->SetFunction\(\s*0x([0-9A-Fa-f]+)\s*,\s*([A-Za-z_]\w*)\s*\)")
_TABLE_ROW = re.compile(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*([A-Za-z_]\w*)\s*\}")


def parse_register_table(text):
    """Entries of a generated <project>_register.cpp as [(address, symbol)], in file order.
    Returns ([], None) when no table is recognised, else (entries, form) with form 'table' or 'calls'."""
    at = text.find("kRexRegisterTable[]")
    if at >= 0:                                   # the patched form: only the rows of that table
        end = text.find("};", at)
        body = text[at:end if end >= 0 else len(text)]
        return [(int(a, 16), n) for a, n in _TABLE_ROW.findall(body)], "table"
    calls = [(int(a, 16), n) for a, n in _SET_FUNCTION.findall(text)]
    return calls, ("calls" if calls else None)
