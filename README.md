# Raid Lockout

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module. It sets how long each raid's
lockout lasts, in days, per raid in the config. (Formerly mod-raid-reset; the config keys moved
from `RaidReset.*` to `RaidLockout.*`.)

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

## Patch Notes: Raid Lockout

Category: Raids

- Raid lockouts now reset every day at 04:00 UTC (midnight Eastern, 11 PM once daylight saving
  time ends). That includes Zul'Gurub, Ruins of Ahn'Qiraj, Molten Core, Onyxia's Lair, Blackwing
  Lair, Temple of Ahn'Qiraj and Naxxramas.

> Small groups and bot raids shouldn't have to wait a week to try a raid again.

## Requirements

- An AzerothCore WotLK server (`azerothcore-wotlk`, master). Stock core only; the module needs no
  SQL and no client patch.
- Optional: [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  for the `RaidLockout.OnyxiasLair40` and `RaidLockout.Naxxramas40` settings. Without it the 40-man
  versions don't exist and those two settings are skipped.
- WoW 3.3.5a (12340) client.

## Installation

Clone it into your AzerothCore `modules` folder, **as `mod-raid-lockout`**. AzerothCore derives the
module's loader name from the folder name:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-raid-lockout.git mod-raid-lockout
```

Re-run CMake, rebuild the worldserver, and copy `conf/mod_raid_lockout.conf.dist` to
`mod_raid_lockout.conf` in your config directory. The module needs no SQL.

To check that it's loaded, look for a line like this in the worldserver log at startup:

```
mod-raid-lockout: Molten Core 1d (stock 7d), Blackwing Lair 1d (stock 7d)
```

## Configuration

Every raid setting takes a number of days from 1 to 30. `0` (the default) keeps the raid's own
timer.

| Setting | Raid |
| --- | --- |
| `RaidLockout.Enable` | Master switch (`1`). `0` restores every stock timer. |
| `RaidLockout.ZulGurub` | Zul'Gurub |
| `RaidLockout.RuinsOfAhnQiraj` | Ruins of Ahn'Qiraj |
| `RaidLockout.MoltenCore` | Molten Core |
| `RaidLockout.OnyxiasLair40` | Onyxia's Lair, 40-man (mod-individual-progression) |
| `RaidLockout.BlackwingLair` | Blackwing Lair |
| `RaidLockout.TempleOfAhnQiraj` | Temple of Ahn'Qiraj |
| `RaidLockout.Naxxramas40` | Naxxramas, 40-man (mod-individual-progression) |
| `RaidLockout.Karazhan` | Karazhan |
| `RaidLockout.ZulAman` | Zul'Aman |
| `RaidLockout.GruulsLair` | Gruul's Lair |
| `RaidLockout.MagtheridonsLair` | Magtheridon's Lair |
| `RaidLockout.SerpentshrineCavern` | Serpentshrine Cavern |
| `RaidLockout.TempestKeep` | Tempest Keep (The Eye) |
| `RaidLockout.MountHyjal` | Hyjal Summit |
| `RaidLockout.BlackTemple` | Black Temple |
| `RaidLockout.SunwellPlateau` | Sunwell Plateau |
| `RaidLockout.Naxxramas` | Naxxramas 10/25 |
| `RaidLockout.OnyxiasLair` | Onyxia's Lair 10/25 |
| `RaidLockout.ObsidianSanctum` | The Obsidian Sanctum |
| `RaidLockout.EyeOfEternity` | The Eye of Eternity |
| `RaidLockout.VaultOfArchavon` | Vault of Archavon |
| `RaidLockout.Ulduar` | Ulduar |
| `RaidLockout.TrialOfTheCrusader` | Trial of the Crusader |
| `RaidLockout.IcecrownCitadel` | Icecrown Citadel |
| `RaidLockout.RubySanctum` | The Ruby Sanctum |

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

## Troubleshooting

- **A new timer doesn't apply after `.reload config`.** Restart the worldserver. A reload only
  applies new values from the reset after the one already scheduled.
- **Raids reset at the wrong time of day.** The core counts `Instance.ResetTimeHour`
  (worldserver.conf, default `4`) from midnight UTC, so the default is 04:00 UTC.
- **A raid still uses its stock timer.** Its setting is `0` (the default), which keeps the raid's
  own timer, or `RaidLockout.Enable` is `0`.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT. See [LICENSE](LICENSE).
