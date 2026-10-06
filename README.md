# Raid Reset

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module. It gives every raid its own
reset timer, in days, set in the config.

The core only has one knob for raid lockouts: `Rate.InstanceResetTime`, a multiplier on top of the
timers in `MapDifficulty.dbc` (3 days for Zul'Gurub and Ruins of Ahn'Qiraj, 7 days for most other
raids). With this module you set each raid directly, for example Molten Core every day and
Naxxramas every 3 days.

- **One setting per raid**, from Zul'Gurub to the Ruby Sanctum. `0` keeps the raid's own timer.
- **The 40-man Onyxia and Naxxramas** from
  [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) have
  their own settings, separate from the 10/25-man versions.
- **Takes effect right away.** After a restart, a raid whose next reset is further away than its
  new timer resets at the next reset hour, and existing lockouts are shortened to match.
- **The calendar and raid info window** show the new reset times.

## Patch Notes: Raid Reset

Category: Raids

- Raid lockouts now reset every day at 04:00 UTC (midnight Eastern, 11 PM once daylight saving
  time ends). That includes Zul'Gurub, Ruins of Ahn'Qiraj, Molten Core, Onyxia's Lair, Blackwing
  Lair, Temple of Ahn'Qiraj and Naxxramas.

> Small groups and bot raids shouldn't have to wait a week to try a raid again.

## Installation

Clone it into your AzerothCore `modules` folder, **as `mod-raid-reset`**. AzerothCore derives the
module's loader name from the folder name:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-raid-reset.git mod-raid-reset
```

Re-run CMake, rebuild the worldserver, and copy `conf/mod_raid_reset.conf.dist` to
`mod_raid_reset.conf` in your config directory. The module needs no SQL.

To check that it's loaded, look for a line like this in the worldserver log at startup:

```
mod-raid-reset: Molten Core 1d (stock 7d), Blackwing Lair 1d (stock 7d)
```

## Configuration

Every raid setting takes a number of days from 1 to 30. `0` (the default) keeps the raid's own
timer.

| Setting | Raid |
| --- | --- |
| `RaidReset.Enable` | Master switch (`1`). `0` restores every stock timer. |
| `RaidReset.ZulGurub` | Zul'Gurub |
| `RaidReset.RuinsOfAhnQiraj` | Ruins of Ahn'Qiraj |
| `RaidReset.MoltenCore` | Molten Core |
| `RaidReset.OnyxiasLair40` | Onyxia's Lair, 40-man (mod-individual-progression) |
| `RaidReset.BlackwingLair` | Blackwing Lair |
| `RaidReset.TempleOfAhnQiraj` | Temple of Ahn'Qiraj |
| `RaidReset.Naxxramas40` | Naxxramas, 40-man (mod-individual-progression) |
| `RaidReset.Karazhan` | Karazhan |
| `RaidReset.ZulAman` | Zul'Aman |
| `RaidReset.GruulsLair` | Gruul's Lair |
| `RaidReset.MagtheridonsLair` | Magtheridon's Lair |
| `RaidReset.SerpentshrineCavern` | Serpentshrine Cavern |
| `RaidReset.TempestKeep` | Tempest Keep (The Eye) |
| `RaidReset.MountHyjal` | Hyjal Summit |
| `RaidReset.BlackTemple` | Black Temple |
| `RaidReset.SunwellPlateau` | Sunwell Plateau |
| `RaidReset.Naxxramas` | Naxxramas 10/25 |
| `RaidReset.OnyxiasLair` | Onyxia's Lair 10/25 |
| `RaidReset.ObsidianSanctum` | The Obsidian Sanctum |
| `RaidReset.EyeOfEternity` | The Eye of Eternity |
| `RaidReset.VaultOfArchavon` | Vault of Archavon |
| `RaidReset.Ulduar` | Ulduar |
| `RaidReset.TrialOfTheCrusader` | Trial of the Crusader |
| `RaidReset.IcecrownCitadel` | Icecrown Citadel |
| `RaidReset.RubySanctum` | The Ruby Sanctum |

A WotLK raid's setting covers all its sizes and heroic modes.

Raids reset at `Instance.ResetTimeHour` (worldserver.conf, default `4`). The core counts that hour
from midnight **UTC**, so the default is 04:00 UTC. `Rate.InstanceResetTime` doesn't affect raids
you set here, so you can leave it at `1`.

Restart the worldserver after changing a timer. `.reload config` also applies new values, but only
from the reset after the one already scheduled.

## How it works

The core reads each raid's reset period from `MapDifficulty.dbc` at startup, keeps the next reset
date per raid in the `instance_reset` table, and schedules resets from it.

- **Before the DBC stores load** (`OnLoadCustomDatabaseTable`), the module looks at
  `instance_reset`. Any configured raid whose stored date is further out than its new timer, or
  already past, is moved to the next reset hour. Lockouts in `instance` that run past that date
  are shortened to it.
- **Before the first world update** (`OnBeforeWorldInitialized`), it overwrites each configured
  raid's `resetTime` in the core's map-difficulty table, so every reset after that schedules the
  next one with the new period. It also sets the extended reset time, which the calendar and
  lockout extension use.

A raid that has never reset on your server (no `instance_reset` row yet) gets its first reset date
from its stock timer. Every reset after that uses the configured one.

## License

MIT
