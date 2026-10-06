/*
 * mod-raid-reset: a reset timer (in days) for every raid.
 *
 * The core takes each raid's reset period from MapDifficulty.dbc (resetTime) times the global
 * Rate.InstanceResetTime. This module overwrites resetTime per raid, and pulls stored reset dates
 * that lie further out than the new period in to the next reset hour, so a shorter timer applies
 * right after a restart instead of after the old lockout runs out.
 *
 * Released under the MIT License.
 */

#include "CharacterDatabase.h"
#include "Config.h"
#include "DBCStores.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "World.h"
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct RaidDef
    {
        char const* key;            // RaidReset.<key>
        char const* name;
        uint32 mapId;
        std::vector<uint8> difficulties;
    };

    // Difficulty 2 of Onyxia's Lair and Naxxramas is the 40-man version that
    // mod-individual-progression adds; without that module it doesn't exist and is skipped.
    std::vector<RaidDef> const Raids =
    {
        // Vanilla
        { "ZulGurub",            "Zul'Gurub",                 309, { 0 } },
        { "RuinsOfAhnQiraj",     "Ruins of Ahn'Qiraj",        509, { 0 } },
        { "MoltenCore",          "Molten Core",               409, { 0 } },
        { "OnyxiasLair40",       "Onyxia's Lair (40)",        249, { 2 } },
        { "BlackwingLair",       "Blackwing Lair",            469, { 0 } },
        { "TempleOfAhnQiraj",    "Temple of Ahn'Qiraj",       531, { 0 } },
        { "Naxxramas40",         "Naxxramas (40)",            533, { 2 } },
        // The Burning Crusade
        { "Karazhan",            "Karazhan",                  532, { 0 } },
        { "ZulAman",             "Zul'Aman",                  568, { 0 } },
        { "GruulsLair",          "Gruul's Lair",              565, { 0 } },
        { "MagtheridonsLair",    "Magtheridon's Lair",        544, { 0 } },
        { "SerpentshrineCavern", "Serpentshrine Cavern",      548, { 0 } },
        { "TempestKeep",         "Tempest Keep",              550, { 0 } },
        { "MountHyjal",          "Hyjal Summit",              534, { 0 } },
        { "BlackTemple",         "Black Temple",              564, { 0 } },
        { "SunwellPlateau",      "Sunwell Plateau",           580, { 0 } },
        // Wrath of the Lich King
        { "Naxxramas",           "Naxxramas (10/25)",         533, { 0, 1 } },
        { "OnyxiasLair",         "Onyxia's Lair (10/25)",     249, { 0, 1 } },
        { "ObsidianSanctum",     "The Obsidian Sanctum",      615, { 0, 1 } },
        { "EyeOfEternity",       "The Eye of Eternity",       616, { 0, 1 } },
        { "VaultOfArchavon",     "Vault of Archavon",         624, { 0, 1 } },
        { "Ulduar",              "Ulduar",                    603, { 0, 1 } },
        { "TrialOfTheCrusader",  "Trial of the Crusader",     649, { 0, 1, 2, 3 } },
        { "IcecrownCitadel",     "Icecrown Citadel",          631, { 0, 1, 2, 3 } },
        { "RubySanctum",         "The Ruby Sanctum",          724, { 0, 1, 2, 3 } },
    };

    uint32 constexpr MaxDays = 30;

    bool sEnabled = true;
    std::vector<uint32> sDays(Raids.size(), 0);                 // 0 = keep the stock timer
    std::unordered_map<uint32, uint32> sStockResetTime;         // PAIR32(map, difficulty) -> DBC resetTime

    void LoadConfig()
    {
        sEnabled = sConfigMgr->GetOption<bool>("RaidReset.Enable", true);
        for (size_t i = 0; i < Raids.size(); ++i)
            sDays[i] = std::min(sConfigMgr->GetOption<uint32>(std::string("RaidReset.") + Raids[i].key, 0), MaxDays);
    }

    time_t NextResetHour(time_t now)
    {
        // Same day boundary as InstanceSaveMgr: UTC midnight plus Instance.ResetTimeHour.
        time_t t = (now / DAY) * DAY + sWorld->getIntConfig(CONFIG_INSTANCE_RESET_TIME_HOUR) * HOUR;
        while (t <= now)
            t += DAY;
        return t;
    }

    // Runs before InstanceSaveMgr::LoadResetTimes, which keeps any stored reset date that is still
    // in the future. Pull dates beyond the new period in to the next reset hour.
    void PullInStoredResets()
    {
        time_t const now = std::time(nullptr);
        time_t const next = NextResetHour(now);

        for (size_t i = 0; i < Raids.size(); ++i)
        {
            if (!sDays[i])
                continue;

            RaidDef const& raid = Raids[i];
            for (uint8 difficulty : raid.difficulties)
            {
                QueryResult result = CharacterDatabase.Query("SELECT resettime FROM instance_reset WHERE mapid = {} AND difficulty = {}", raid.mapId, difficulty);
                if (!result)
                    continue;

                time_t const stored = time_t(result->Fetch()[0].Get<uint32>());
                if (stored > now && stored <= now + time_t(sDays[i]) * DAY)
                    continue;

                CharacterDatabase.DirectExecute("UPDATE instance_reset SET resettime = {} WHERE mapid = {} AND difficulty = {}", uint32(next), raid.mapId, difficulty);
                // Existing lockouts show their own reset date in the raid info window.
                CharacterDatabase.DirectExecute("UPDATE instance SET resettime = {} WHERE map = {} AND difficulty = {} AND resettime > {}", uint32(next), raid.mapId, difficulty, uint32(next));

                LOG_INFO("server.loading", "mod-raid-reset: {} (difficulty {}) next reset moved from {} to {}", raid.name, difficulty,
                    Acore::Time::TimeToTimestampStr(Seconds(stored)), Acore::Time::TimeToTimestampStr(Seconds(next)));
            }
        }
    }

    // Sets each configured raid's period. At startup it also fixes the extended reset time (the
    // core initialises it to the reset time itself), which the calendar and lockout extension use.
    void ApplyPeriods(bool startup)
    {
        float rate = sWorld->getRate(RATE_INSTANCE_RESET_TIME);
        if (rate <= 0.0f)
            rate = 1.0f;

        for (size_t i = 0; i < Raids.size(); ++i)
        {
            RaidDef const& raid = Raids[i];
            for (uint8 difficulty : raid.difficulties)
            {
                uint32 const pair = MAKE_PAIR32(raid.mapId, difficulty);
                auto itr = sMapDifficultyMap.find(pair);
                if (itr == sMapDifficultyMap.end())
                    continue;

                uint32 const stock = sStockResetTime.emplace(pair, itr->second.resetTime).first->second;
                if (!stock)
                    continue;

                if (!sEnabled || !sDays[i])
                {
                    itr->second.resetTime = stock;
                    continue;
                }

                // The core computes the period as floor(resetTime * rate / DAY) days. The extra
                // hour keeps float rounding from losing a day; dividing by the rate cancels it.
                uint32 const period = sDays[i] * DAY;
                itr->second.resetTime = uint32((period + HOUR) / rate);

                if (startup)
                    if (time_t const reset = sInstanceSaveMgr->GetResetTimeFor(raid.mapId, Difficulty(difficulty)))
                        sInstanceSaveMgr->SetExtendedResetTimeFor(raid.mapId, Difficulty(difficulty), reset + period);
            }
        }
    }

    void LogSummary()
    {
        if (!sEnabled)
        {
            LOG_INFO("server.loading", "mod-raid-reset: disabled, stock raid reset timers");
            return;
        }

        std::string changed;
        for (size_t i = 0; i < Raids.size(); ++i)
        {
            if (!sDays[i])
                continue;

            auto stock = sStockResetTime.find(MAKE_PAIR32(Raids[i].mapId, Raids[i].difficulties.front()));
            if (stock == sStockResetTime.end() || !stock->second)
                continue; // raid not in this client's DBC (e.g. 40-man versions without individual progression)

            if (!changed.empty())
                changed += ", ";
            changed += Acore::StringFormat("{} {}d (stock {}d)", Raids[i].name, sDays[i], stock->second / DAY);
        }

        LOG_INFO("server.loading", "mod-raid-reset: {}", changed.empty() ? "no raid timers changed" : changed);
    }
}

class RaidResetWorldScript : public WorldScript
{
public:
    RaidResetWorldScript() : WorldScript("RaidResetWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE,
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        // A reload changes the period that follows the next reset; the reset already scheduled
        // stays where it is until a restart.
        if (reload && !sStockResetTime.empty())
        {
            ApplyPeriods(false);
            LogSummary();
        }
    }

    // Called right before the DBC stores load, so before InstanceSaveMgr reads instance_reset.
    void OnLoadCustomDatabaseTable() override
    {
        if (sEnabled)
            PullInStoredResets();
    }

    // Called after InstanceSaveMgr has loaded and before the first world update, so no reset has
    // fired with the stock period yet.
    void OnBeforeWorldInitialized() override
    {
        ApplyPeriods(true);
        LogSummary();
    }
};

void AddRaidResetScripts()
{
    new RaidResetWorldScript();
}
