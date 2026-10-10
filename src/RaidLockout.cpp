/*
 * mod-raid-lockout: a reset timer (in days) for every raid.
 *
 * The core takes each raid's reset period from MapDifficulty.dbc (resetTime) times the global
 * Rate.InstanceResetTime. This module overwrites resetTime per raid, and pulls stored reset dates
 * that lie further out than the new period in to the next reset hour, so a shorter timer applies
 * right after a restart instead of after the old lockout runs out.
 *
 * With shorter lockouts a boss dies far more often than its trophy can be handed in. A Head of
 * Onyxia starts a one-time quest, and the core stops dropping a quest starter for anyone who has
 * done its quest. So when a dungeon or raid boss (or a chest inside a dungeon or raid) drops an
 * item that starts a quest, the module forgets that quest and its follow-ups for every group
 * member who has finished them. The item drops for them again and they can hand it in for another
 * reward, or just for the city buff (RaidLockout.RepeatableBossQuests).
 *
 * Released under the MIT License.
 */

#include "CharacterDatabase.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "World.h"
#include <algorithm>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    struct RaidDef
    {
        char const* key;            // RaidLockout.<key>
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

    // Quest starter item -> the quest it starts and the follow-ups handed out after it. Filled
    // once the world is loaded and only read after that, so map threads can use it.
    bool sRepeatableBossQuests = true;
    uint32 sBossQuestChain = 3;
    std::unordered_map<uint32, std::vector<uint32>> sBossQuestChains;

    void LoadConfig()
    {
        sEnabled = sConfigMgr->GetOption<bool>("RaidLockout.Enable", true);
        sRepeatableBossQuests = sConfigMgr->GetOption<bool>("RaidLockout.RepeatableBossQuests", true);
        sBossQuestChain = sConfigMgr->GetOption<uint32>("RaidLockout.RepeatableBossQuestChain", 3);
        for (size_t i = 0; i < Raids.size(); ++i)
            sDays[i] = std::min(sConfigMgr->GetOption<uint32>(std::string("RaidLockout.") + Raids[i].key, 0), MaxDays);
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

                LOG_INFO("server.loading", "mod-raid-lockout: {} (difficulty {}) next reset moved from {} to {}", raid.name, difficulty,
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
            LOG_INFO("server.loading", "mod-raid-lockout: disabled, stock raid reset timers");
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

        LOG_INFO("server.loading", "mod-raid-lockout: {}", changed.empty() ? "no raid timers changed" : changed);
    }

    // The quests a starter item's turn-in is made of: the quest it starts, then each quest's
    // follow-up for as long as that follow-up needs the quest before it. Empty when the turn-in
    // can't safely be done again:
    // - a quest in it is already repeatable, daily, weekly or seasonal;
    // - an NPC or object also gives the first quest, so forgetting it would hand it out without
    //   the boss;
    // - it is longer than RaidLockout.RepeatableBossQuestChain. Those are story chains (An Unsent
    //   Letter), not trophies.
    // A follow-up that doesn't need the quest before it ends the chain and is left alone: forgotten,
    // it could be taken again and again without a new item.
    std::vector<uint32> BuildChain(uint32 firstQuest, std::unordered_set<uint32> const& givenQuests)
    {
        if (givenQuests.count(firstQuest))
            return {};

        std::vector<uint32> chain;
        uint32 questId = firstQuest;
        while (questId)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || quest->IsRepeatable() || quest->IsDailyOrWeekly() || quest->IsSeasonal())
                return {};

            if (!chain.empty() && std::find(quest->prevQuests.begin(), quest->prevQuests.end(), int32(chain.back())) == quest->prevQuests.end())
                break;

            if (chain.size() >= sBossQuestChain)
                return {};

            chain.push_back(questId);
            questId = quest->GetNextQuestInChain();
            if (std::find(chain.begin(), chain.end(), questId) != chain.end())
                break;
        }

        return chain;
    }

    void BuildBossQuestChains()
    {
        // Quests an NPC or object hands out. An NPC players can't talk to doesn't count: the
        // Heart of Hakkar on Yojamba Isle is listed as giving its own quest, but can't be selected.
        std::unordered_set<uint32> givenQuests;
        for (auto const& [giver, quest] : *sObjectMgr->GetGOQuestRelationMap())
            givenQuests.insert(quest);

        for (auto const& [giver, quest] : *sObjectMgr->GetCreatureQuestRelationMap())
        {
            CreatureTemplate const* npc = sObjectMgr->GetCreatureTemplate(giver);
            if (npc && (npc->npcflag & UNIT_NPC_FLAG_QUESTGIVER) && !(npc->unit_flags & UNIT_FLAG_NOT_SELECTABLE))
                givenQuests.insert(quest);
        }

        sBossQuestChains.clear();
        for (auto const& [itemId, item] : *sObjectMgr->GetItemTemplateStore())
        {
            if (!item.StartQuest)
                continue;

            std::vector<uint32> chain = BuildChain(item.StartQuest, givenQuests);
            if (!chain.empty())
                sBossQuestChains.emplace(itemId, std::move(chain));
        }

        LOG_INFO("server.loading", "mod-raid-lockout: {} quest starter items can be handed in again when a boss drops them", sBossQuestChains.size());
    }

    // Forgets a finished turn-in. Left alone while a quest of it is in the quest log, and until
    // its last quest is handed in: the item only drops again once the reward has been collected.
    void ForgetFinishedChain(Player* player, std::vector<uint32> const& chain)
    {
        if (!player->IsQuestRewarded(chain.back()))
            return;

        for (uint32 questId : chain)
        {
            QuestStatus const status = player->GetQuestStatus(questId);
            if (status != QUEST_STATUS_NONE && status != QUEST_STATUS_REWARDED)
                return;
        }

        for (uint32 questId : chain)
            player->RemoveRewardedQuest(questId);
    }
}

class RaidLockoutWorldScript : public WorldScript
{
public:
    RaidLockoutWorldScript() : WorldScript("RaidLockoutWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE,
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        // The maps aren't updating while a reload runs, so nothing is reading the chains.
        if (reload)
            BuildBossQuestChains();

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
        BuildBossQuestChains();
    }
};

// A quest starter is about to be added to a boss's loot. The core decides right after this who
// may loot it, so this is the moment to forget the turn-in for group members who finished it.
class RaidLockoutGlobalScript : public GlobalScript
{
public:
    RaidLockoutGlobalScript() : GlobalScript("RaidLockoutGlobalScript", { GLOBALHOOK_ON_BEFORE_DROP_ADD_ITEM }) { }

    void OnBeforeDropAddItem(Player const* looter, Loot& loot, bool /*canRate*/, uint16 /*lootMode*/, LootStoreItem* item, LootStore const& store) override
    {
        if (!sRepeatableBossQuests || !looter || !item)
            return;

        auto itr = sBossQuestChains.find(item->itemid);
        if (itr == sBossQuestChains.end())
            return;

        // A dungeon, raid or world boss, or a chest inside a dungeon or raid (Majordomo's cache).
        if (&store == &LootTemplates_Creature)
        {
            Creature const* boss = ObjectAccessor::GetCreature(*looter, loot.sourceWorldObjectGUID);
            if (!boss || (!boss->IsDungeonBoss() && !boss->isWorldBoss()))
                return;
        }
        else if (&store != &LootTemplates_Gameobject || !looter->GetMap()->IsDungeon())
            return;

        // Only group members on this map: this runs in the map's thread.
        if (Group const* group = looter->GetGroup())
        {
            for (GroupReference const* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member->IsInMap(looter))
                        ForgetFinishedChain(member, itr->second);
        }
        else if (Player* player = ObjectAccessor::FindPlayer(looter->GetGUID()))
            ForgetFinishedChain(player, itr->second);
    }
};

void AddRaidLockoutScripts()
{
    new RaidLockoutWorldScript();
    new RaidLockoutGlobalScript();
}
