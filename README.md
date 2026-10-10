# LiD Death Metals

Brings Death Metals back to the offline version of Let It Die (Steam).

The offline release kept the Death Metal code but switched it off in several
places at once: the rewards that gave them were turned into Kill Coins, the
currency bar stopped drawing them, and the menus and shop code that spent them
were changed to Kill Coins. This project undoes that, one piece at a time,
using the online game (client 4.18) as the reference for how it used to work.

It comes in two parts that go together:

- **lid-deathmetals.dll** (`dll/`): a plugin for the
  [LiD Plugin Loader](https://github.com/NicoMullin/LiD-Plugin-Loader). It
  changes the game's code, in memory.
- **The Death Metals mod** (`mods/Death Metals`), for
  [LiD DB Editor](https://github.com/NicoMullin/LiD-DB-Editor). It changes
  `masters.db` and adds a patch to `BrgGame.upk`. See its
  [readme](mods/Death%20Metals/readme.md).

## What it does so far

- **Death Metals on the currency bar** (plugin, 0.1.0). The bar at the top of
  the screen has six slots, and slot 0 is Death Metals. The game still fills
  it with your Death Metal count, but the offline release added a check that
  skips drawing it, and deleted the one call that drew its icon. The plugin
  removes the check and puts the icon draw back, using the online game's exact
  instructions.
- **The Death Metal sign in text** (plugin, 0.3.0). Texts such as a quest's
  Death Metal reward show the amount with a small picture (`#MNEY_DM` in
  `masters.db`). The HUD sets up those pictures when it starts, and the
  offline release gave the Death Metal one the Kill Coin picture. The plugin
  points it back at the Death Metal picture, as the online game had it.
- **Death Metal rewards** (mod). The 20 Death Metal rewards pay Death Metals
  again instead of 5,000 Kill Coins per metal; mystery bags roll them.
- **Uncle Death: Convert Death Metals and Currency Details** (mod and plugin,
  0.2.0). The mod puts the two menu entries back; their screens were never
  removed. The offline game's built-in server has no code for the exchange
  request, so the plugin adds it: it checks you hold the Death Metals, pays
  the Kill Coins (`EXCHANGE_RATE` in `masters.db`, 5,000 each) with the game's
  own function, takes the Death Metals (free ones first) and saves.
- **The Death Metal stew** (mod 0.6.0). A second stew at the Mushroom Stew
  shop, for Death Metals: one random ★★★ or better decal a pull. The stew menu
  still has all the online game's code for it; the offline release only added
  a filter that leaves Death Metal stews out of the list, and the mod's
  `BrgGame.upk` patch takes it out. Which decals it gives is a Configuration
  setting (LiD DB Editor 0.12.1's decal picker): every ★★★ and up from the
  normal stew until the player picks. The offline server offers only the
  first normal stew and draws every normal stew from one list kept in the
  save, so the stew also needs the plugin from
  [LiD Stew Multi Pull](https://github.com/NicoMullin/LiD-Stew-Multi-Pull),
  `lid-stewpull.dll`: it has the shop offer every stew and gives this one a
  fresh pick of its own every pull.

Up to 0.5.x, `lid-deathmetals.dll` held that stew code too, and the one-request
multi pull. In 0.6.0 all of it moved to `lid-stewpull.dll`, so the multi pull
no longer needs Death Metals. Update both together: Death Metals 0.5.x and
`lid-stewpull.dll` would change the same server code.
- **Prices in Death Metals** (mod, 0.2.0 and 0.3.0). Each one is switched on
  and priced in the mod's Configuration tab: Express Passes (on at first, the
  online game's 33 and 6 Death Metals) and the Coin Locker expansion (off at
  first; the online game sold it for Kill Coins too). The game's server
  charges whatever `masters.db` says, so only the screens that sell them
  needed changing, back to reading, checking and showing Death Metals.

Still to come: the rest of what the online game sold for Death Metals. Time skips for Death Metals
are the separate [LiD Timers](https://github.com/NicoMullin/LiD-Timers) mod.

Safety rules for the plugin, all in [dll/lid_deathmetals.c](dll/lid_deathmetals.c):

- It only does anything inside `BrgGame-Steam.exe`. Loaded by any other
  program, it changes nothing.
- Every piece of game code is found by its exact bytes and must be found
  exactly once. If a game update moves the code, the plugin changes nothing
  and says so in its log.
- Changes are made **in memory only**. The exe on disk is never touched.
- No network, no registry. The only file it writes is `lid-deathmetals.log`
  next to itself.
- Delete `lid-deathmetals.dll` and the game is exactly stock again.

## Why the DLL is built by GitHub

A DLL can do anything, so you should not have to take one on trust. Every
release on this page is built by GitHub Actions from the source in this
repository, on GitHub's own machines, and the tests run before anything is
published. You can check the DLL in two ways:

1. **Signed build record.** Each release DLL has an attestation: a signed
   record, kept by GitHub, of the exact workflow run and commit that built it.
   With the [GitHub CLI](https://cli.github.com/):

   ```
   gh attestation verify lid-deathmetals.dll --repo NicoMullin/LiD-Death-Metals
   ```

2. **Rebuild it yourself.** The build is reproducible: the same source and the
   same compiler give a byte-identical file. See
   [Building it yourself](#building-it-yourself) and compare the SHA-256 with
   the one on the release page.

## Installing

**The easy way.** In LiD DB Editor (the LET IT DIE DB Mod Manager) 0.12.6 or
newer, close the game, click **Get mods...** and then **Get** next to **Death
Metals**. It downloads this repository's newest release, checks it against the
SHA-256 GitHub lists for it, and once you say yes puts the Death Metals mod in
the manager and `lid-deathmetals.dll` in the game's
`Binaries\Win64\lid-plugins` - with the [LiD Plugin
Loader](https://github.com/NicoMullin/LiD-Plugin-Loader) too, if you do not
have it yet. Then switch the game's file check off for `BrgGame.upk` (Tools >
Hash Patcher) if it is not already, tick **Death Metals** and click **Save Mod
List**.

**By hand.**

1. Install the [LiD Plugin Loader](https://github.com/NicoMullin/LiD-Plugin-Loader)
   once, if you have not already. It puts a `dinput8.dll` and a `lid-plugins`
   folder in the game's `Binaries\Win64` folder.
2. Download `LiD-Death-Metals.zip` from the
   [latest release](https://github.com/NicoMullin/LiD-Death-Metals/releases/latest).
   Put `lid-plugins\lid-deathmetals.dll` from it in the game's `Binaries\Win64\lid-plugins`.
3. In the manager, switch the game's file check off for `BrgGame.upk` (Tools >
   Hash Patcher) if it is not already, drop the `mods\Death Metals` folder from
   the zip onto it, tick it and click **Save Mod List**.

The release also has `lid-deathmetals.dll` on its own, for anyone who only wants the plugin.

It works alongside other plugins, such as LiD Timers, LiD Login Bonus and
Fighter Class Passives: they change different parts of the game.

## Building it yourself

You need Windows and Python 3. The compiler is Zig, installed with pip; no
Visual Studio needed.

```
py -3 -m pip install ziglang==0.16.0
py -3 build.py --test
```

The DLL lands in `out\lid-deathmetals.dll` and the SHA-256 is printed at the
end. Use Zig 0.16.0 exactly: another version builds a working DLL, but not a
byte-identical one.

`--test` builds stand-ins for the game exe and runs them against the DLL:

- `tests/harness.in.c` holds the currency bar's code shapes (the draw before
  the Death Metal icon, the draw after it and the slot loop) and checks that
  slot 0 is drawn again, that the icon is drawn with the right layout values
  and the game's own register values, and that the draws around it are
  unchanged. It also holds the HUD's set-up of the Death Metal text sign and
  checks it takes the Death Metal pictures instead of the Kill Coin ones.
- `tests/exchange.c` holds the offline server's request lookup in the game's
  byte shapes, puts the plugin's real hooks into it and sends requests through
  it: an exchange (Kill Coins paid, free Death Metals used first, totals in
  the reply, saved), asking for more than you hold, the Kill Coin limit, zero
  and negative amounts, and that real Vipconfig requests and other unknown
  requests are untouched.
- `tests/nocode.c` holds none of them, as after a game update, and checks the
  DLL changes nothing and says so in its log.

`--exe PATH\BrgGame-Steam.exe` also runs `tests/realexe.c`: the plugin's own
search on the real game file (mapped, not run), which must find every piece
exactly once. The game is not in this repository, so GitHub does not run it.

## Rebuilding the BrgGame.upk patches

Each `mods/Death Metals/tfc/<part>/.../BrgGame.upk.PackagePatch` is written
from the game's stock `BrgGame.upk`, using the package reader from a
[LiD DB Editor](https://github.com/NicoMullin/LiD-DB-Editor) checkout:

```
py -3 tools/build_menu_patch.py --manager PATH\TO\LiD-DB-Editor --upk PATH\TO\BrgGame.upk
py -3 tools/build_cost_patches.py --manager PATH\TO\LiD-DB-Editor --upk PATH\TO\BrgGame.upk
```

[tools/build_menu_patch.py](tools/build_menu_patch.py) writes `tfc/menu`, the
three changed functions of Uncle Death's menu.
[tools/build_cost_patches.py](tools/build_cost_patches.py) writes one folder
per part the Configuration tab can switch (`tfc/vip`, `tfc/coinlocker`,
`tfc/stew`).
Each holds only the changed functions, never the game's package, and checks
itself by applying the patch before it writes it.

## Credit

The `.PackagePatch` format is FCH823's, from
[TFC Installer](https://www.nexusmods.com/site/mods/587); LiD DB Editor applies
it with their permission.

## Making a release

1. Set the new version in [dll/version.h](dll/version.h).
2. Push a tag with the same version, for example `v0.2.1`.

GitHub builds, tests and publishes the release. The build stops if the tag and
`version.h` don't match.

## License

MIT - see [LICENSE](LICENSE). Made by KSFA.
