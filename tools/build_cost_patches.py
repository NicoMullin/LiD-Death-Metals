"""Write the Death Metals mod's cost patches for BrgGame.upk (.PackagePatch).

The offline release moved things the online game (4.18) sold for Death Metals
onto Kill Coins: the database prices became Kill Coins, and the screens that
sell them were changed to read, check and show Kill Coins. The Death Metals mod
switches each one back on its own setting; for each, the SQL in mod.json sets
the Death Metal price and one of these patches puts the screen back:

- tfc/vip: Uncle Death's "Buy Express Pass" (VIP pass and 1-day pass).
  - GetVIPPassPrice, GetOneDayPassPrice: the Death Metal price (mMedal), as
    the online game read it, not the Kill Coin one (mPrice).
  - Tick_BuyVIPPass: checks your Death Metals, not your Kill Coins; when they
    are short, says "Death Metal Required" (BUY_VIP.TXT_USE_DM) instead of
    "You don't have enough Kill Coins".
  - RenderState_BuyVIPPass_BuyPrice: the price shows the Death Metal sign, not
    the Kill Coin one.
- tfc/coinlocker: Uncle Death's "Expand Coin Locker". The online game sold it
  for Kill Coins too; this is an extra choice.
  - GetExpandCoinLockerPrice: the Death Metal price (mMedal).
  - Tick_ExpandCoinLocker: checks your Death Metals; when they are short, says
    "Death Metal Required". The confirm text's sign is the mod's SQL.
- tfc/stew: the Death Metal stew at the Mushroom Stew shop. The menu still
  has all the online game's Death Metal stew code (the price check against
  your Death Metals, its own pull animation); the offline release only added a
  filter that leaves every Death Metal stew out of the list.
  - MakeGachaPropertyArray: the filter's jump goes to the next statement, so
    every stew in masters.db is listed again.

The game's server needs no change for any of them: it charges what masters.db
says, Kill Coins and Death Metals alike.

Reads the game's stock BrgGame.upk and never changes it. Needs a LiD DB Editor
checkout for its package reader:

    py -3 tools/build_cost_patches.py --manager PATH/TO/LiD-DB-Editor --upk PATH/TO/BrgGame.upk
"""
from __future__ import annotations

import argparse
from pathlib import Path

from upkpatch import Kit, cstr, o

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TFC = REPO / "mods" / "Death Metals" / "tfc"
PATCH_PATH = Path("Game") / "BrgGame" / "CookedPCConsole" / "BrgGame.upk.PackagePatch"
INS = "BrgUIMenu_Insurance"
PRICE = "BrgGameBase.BrgNetworkDeclStruct.BrgDbProductPrice"
ICONS = "BrgGameBase.BrgUIResource_Icon_Info_v00.stx_ui_icon_info_v00__info_item_all"

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("--manager", required=True, type=Path, help="a LiD DB Editor checkout")
parser.add_argument("--upk", required=True, type=Path, help="the game's stock BrgGame.upk")
parser.add_argument("--tfc", type=Path, default=TFC, help="where the tfc/<feature> folders go")
parser.add_argument("--listing", type=Path, help="also write the changed code, disassembled, here")
args = parser.parse_args()

kit = Kit(args.manager, args.upk)


def death_metal_price(fn_name: str, sizes, at: int):
    """return dbPrdPrice.mPrice -> return dbPrdPrice.mMedal"""
    fn = kit.fn(f"{INS}.{fn_name}", sizes)
    struct_ref = o(kit.ref_of(PRICE))
    fn.poke(at, "return (", b"\x35" + o(kit.ref_of(f"{PRICE}.mPrice")) + struct_ref,
            b"\x35" + o(kit.ref_of(f"{PRICE}.mMedal")) + struct_ref)
    return fn


def vip() -> list:
    price = death_metal_price("GetVIPPassPrice", (4, 115, 79), 0x4B)
    oneday = death_metal_price("GetOneDayPassPrice", (4, 118, 82), 0x4E)

    tick = kit.fn(f"{INS}.Tick_BuyVIPPass", (57, 1088, 880))
    tick.poke(0x216, "jumpifnot 0x2b6 (N132(N130(N154(",
              kit.virtual_call("GetKillCoinNumber"), kit.virtual_call("GetDeathMetalNumber"), count=2)
    short = tick.stmt_bytes(0x278, f"StartTransition(I:{INS}.mCurrentMenuState, "
                                   "GetNextMenuState(4, 'MSHR_KAIKAN.TXT_NO_KC'")
    tick.stmt(0x2B3, "jump 0x33d")
    tick.divert(0x278, "StartTransition(", kit.asm()
                .code(short.replace(cstr("MSHR_KAIKAN.TXT_NO_KC"), cstr("BUY_VIP.TXT_USE_DM")))
                .jump(0x2B3))

    shown = kit.fn(f"{INS}.RenderState_BuyVIPPass_BuyPrice", (16, 1223, 803))
    icons = o(kit.ref_of(ICONS))
    shown.poke(0x27B, "DrawRotImage_Guide(",
               b"\x35" + o(kit.ref_of(f"{ICONS}.l_menu_money_flip")) + icons,
               b"\x35" + o(kit.ref_of(f"{ICONS}.l_menu_deathmetal_flip")) + icons, count=2)
    return [price, oneday, tick, shown]


def coinlocker() -> list:
    price = death_metal_price("GetExpandCoinLockerPrice", (4, 124, 88), 0x54)

    tick = kit.fn(f"{INS}.Tick_ExpandCoinLocker", (45, 797, 621))
    tick.poke(0xEC, "jumpifnot 0x14f (N151(GetExpandCoinLockerPrice(), GetKillCoinNumber()))",
              kit.virtual_call("GetKillCoinNumber"), kit.virtual_call("GetDeathMetalNumber"))
    short = tick.stmt_bytes(0x111, f"StartTransition(I:{INS}.mCurrentMenuState, "
                                   "GetNextMenuState(3, 'MSHR_KAIKAN.TXT_NO_KC'")
    tick.stmt(0x14C, "jump 0x231")
    tick.divert(0x111, "StartTransition(", kit.asm()
                .code(short.replace(cstr("MSHR_KAIKAN.TXT_NO_KC"), cstr("BUY_VIP.TXT_USE_DM")))
                .jump(0x14C))
    return [price, tick]


def stew() -> list:
    listed = kit.fn("BrgUIMenu_SkillExchange.MakeGachaPropertyArray", (9, 132, 84))
    listed.stmt(0x57, "L:BrgUIMenu_SkillExchange.MakeGachaPropertyArray.gachaPropertyArray.additem(")
    listed.poke(0x34, "jumpifnot 0x6d (N242(bool((L:BrgUIMenu_SkillExchange.MakeGachaPropertyArray"
                ".gachaProperty).BrgUIMenu_SkillExchange.SkillExchange_GachaProperty.mIsDeathMetalGacha",
                b"\x07\x6d\x00", b"\x07\x57\x00")

    return [listed]


FEATURES = {"vip": vip, "coinlocker": coinlocker, "stew": stew}
listing = []
for name, make in FEATURES.items():
    print(f"--- {name}")
    functions = make()
    kit.write_patch(functions, args.tfc / name / PATCH_PATH)
    listing.append(f"=== {name}\n" + kit.listing(functions))
if args.listing:
    args.listing.write_text("\n".join(listing), encoding="utf-8")
