# Death Metals

Brings Death Metals back to the offline game. The offline release kept the
Death Metal code but switched it off; this mod and its plugin switch it back
on, following how the online game (4.18) worked.

## What this mod changes

- **Rewards (`masters.db`).** The game still has its 20 Death Metal rewards,
  `RWD_MEDAL_1` to `RWD_MEDAL_20`, named "Death Metal xN", but the offline
  release made each one pay 5,000 Kill Coins per metal instead. They pay N
  Death Metals again. Mystery bags roll them: a Rainbow bag about 1 time in 4
  (5 Death Metals), a Platinum bag about 1 in 125, Copper, Silver and Gold
  about 1 in 1,000.
- **Uncle Death's menu (`BrgGame.upk`).** Two entries the online game had are
  back, after the offline menu's own four:
  - **Convert Death Metals**: change Death Metals into Kill Coins, at the rate
    in `masters.db` (`EXCHANGE_RATE`, 5,000 Kill Coins each).
  - **Currency Details**: how many free and paid Death Metals you hold.

  The screens behind them were never removed, only their menu entries. Both
  entries use the Death Metal icon; the offline game no longer has the icons
  the online game used for them.

## What things cost (Configuration)

The offline release moved things the online game sold for Death Metals onto
Kill Coins. Each one can be put back on its own, with its price, in the mod's
**Configuration** tab. Switched off, it keeps the offline Kill Coin price.

| Option                                   | On at first | Death Metals (default) | Kill Coins (off) |
|------------------------------------------|-------------|------------------------|------------------|
| Express Pass (30 days), from Uncle Death | Yes         | 33 (online price)      | 165,000          |
| 1-Day Pass, from Uncle Death             | Yes         | 6 (online price)       | 30,000           |
| Coin Locker expansion, from Uncle Death  | No          | 2                      | 10,000           |
| Death Metal stew, a pull                 | Yes         | 10                     | (not sold)       |

With **Express Passes cost Death Metals** on, Uncle Death's Buy Express Pass
screen checks and shows your Death Metals, says "Death Metal Required" when
you are short, and its description says Death Metals (in all ten languages).

The online game sold the Coin Locker expansion for Kill Coins as well, so it
stays on Kill Coins unless you switch **Coin Locker expansion costs Death
Metals** on. Then Uncle Death checks your Death Metals, says "Death Metal
Required" when you are short, and the confirm message shows the Death Metal
sign. The **Storage Limit** mod's optional price part sets the same price;
the manager reports the two as a conflict while both are on, and you would
pay both prices, so keep only one of them on.

The **Death Metal stew** is a second stew at the Mushroom Stew shop, "Buy
decals with Death Metal", as the online game had. Each pull gives one random
★★★ or better decal: every ★★★, ★★★★ and ★★★★★ decal of the normal stew,
with the normal stew's odds between them. Every pull is a fresh roll, so the
same decal can come again. The normal stew is not affected: its pulls still
come from its own list in your save. With **Stew Multi Pull** (0.4.0 or
later), the Death Metal stew gets its Purchase xN entry too, and a multi pull
gives no decal twice: each pull skips the decals the earlier ones gave, until
every decal on the list has come once.

**Choosing the stew's decals** (LiD DB Editor 0.12.1 or newer): in the mod's
Configuration tab, *Decals in the Death Metal stew* has a **Choose decals...**
button. It lists every decal in your `masters.db` - the game's own and any a
mod added, so switch those mods on first - with a search box and columns to
sort by name, stars and category. Tick the ones you want and save. Each keeps
the odds the normal stew gives it; a decal the normal stew never gives gets
the normal stew's odds for its stars (★ 74, ★★ 72, ★★★ 40, ★★★★ 4, ★★★★★ 2).
Until you pick, it is every ★★★ and up from the normal stew, worked out
afresh each time the mod applies.

## You also need the plugin

The Death Metal count at the top of the screen, and the exchange itself, need
`lid-deathmetals.dll` 0.2.0 or later in `Binaries\Win64\lid-plugins` (see the
[LiD Death Metals](https://github.com/NicoMullin/LiD-Death-Metals) page). The
offline game has no server code for the exchange; the plugin adds it. Without
the plugin, Convert Death Metals ends in a communication error and nothing is
exchanged.

The Death Metal sign in messages (the quest window, the Coin Locker confirm)
needs `lid-deathmetals.dll` 0.3.0 or later. Without it those messages show the
Kill Coin sign next to a Death Metal amount.

The Death Metal stew needs `lid-deathmetals.dll` 0.4.3 or later. The offline
game offers only one normal stew at a time, and draws every normal stew from
one list kept in your save; the plugin has the shop offer both stews and gives
the Death Metal stew a fresh roll of its own. Without it the Death Metal stew
does not appear. With plugin 0.5.0, a Stew Multi Pull purchase is one request
- paid, drawn and saved once - instead of one per pull, for both stews. The
Kill Coin stew still gives the decals lined up in your save, repeats and all;
only the Death Metal stew never repeats within a multi pull.

## Installing

**Needs LiD DB Editor 0.12.0 or newer.** Older versions do not know how to
switch a part off, so they would apply every part whatever the Configuration
tab says.

1. Switch the game's file check off for `BrgGame.upk` (Tools > Hash Patcher)
   if it is not already.
2. Drop this mod onto LiD DB Editor, tick it and click **Save Mod List**.
3. If you used the **Death Metal Rewards (test)** mod, untick it: this mod
   replaces it.

The `BrgGame.upk` part stacks with other mods that change that file, such as
Stew Multi Pull, Instant Drops and Fighter Class Passives.

## Undo

Untick the mod and save. The rewards pay Kill Coins again, the menu entries
go and everything costs what the offline game charges. Death Metals you
already have stay in your save.
