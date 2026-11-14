/*
 * `.dungeonquests`: how far a character is with each five-player dungeon's
 * quests, so the player knows before going in which ones are still to pick up.
 * A dungeon's quests are the ones the quest log files under it (ZoneOrSort).
 */

#include "Player.h"
#include "Chat.h"
#include "World.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "GridMap.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace
{
    // Quest-log zones of each dungeon, the first one naming it. These are
    // not always the instance's own area: Gnomeregan's quests sit under the
    // Dun Morogh Gnomeregan, Uldaman's under the Badlands one, and so on.
    std::vector<std::vector<uint32>> const DUNGEON_ZONES = {
        { 2437 },       // Ragefire Chasm
        { 718 },        // Wailing Caverns
        { 1581 },       // The Deadmines
        { 209, 236 },   // Shadowfang Keep
        { 719 },        // Blackfathom Deeps
        { 717 },        // The Stockade
        { 796 },        // Scarlet Monastery
        { 133 },        // Gnomeregan
        { 1717 },       // Razorfen Kraul
        { 722 },        // Razorfen Downs
        { 1517 },       // Uldaman
        { 978 },        // Zul'Farrak
        { 2100 },       // Maraudon
        { 1417 },       // Sunken Temple
        { 1584 },       // Blackrock Depths
        { 1583 },       // Blackrock Spire
        { 2557 },       // Dire Maul
        { 2057 },       // Scholomance
        { 2017 },       // Stratholme
    };

    enum Starter : uint8 { STARTER_CREATURE, STARTER_GAMEOBJECT, STARTER_ITEM };

    struct QuestStart
    {
        Starter type;
        uint32 entry;
        uint32 zone;    // where the first spawn stands, 0 for items
    };

    struct DungeonQuest
    {
        Quest const* quest;
        std::vector<QuestStart> starts;
    };

    // Ordered best first: a group of exclusive quests counts as its best member.
    enum State : uint8 { STATE_DONE, STATE_LOG, STATE_OPEN, STATE_LATER, STATE_SKIP };

    std::map<uint32, std::vector<DungeonQuest>> g_index;    // by the dungeon's first zone
    std::multimap<uint32, uint32> g_givers;                 // every quest -> creatures that hand it out
    std::set<uint32> g_openStart;                           // quests an object or item starts
    bool g_indexed = false;

    void BuildIndex()
    {
        g_indexed = true;
        std::map<uint32, uint32> dungeonOf;
        for (auto const& zones : DUNGEON_ZONES)
            for (uint32 zone : zones)
                dungeonOf[zone] = zones.front();

        std::map<uint32, DungeonQuest> byQuest;
        for (auto const& itr : sObjectMgr.GetQuestTemplates())
        {
            Quest const* pQuest = itr.second.get();
            if (pQuest->GetZoneOrSort() <= 0 || pQuest->IsRepeatable() || pQuest->GetQuestMethod() == QUEST_METHOD_DISABLED)
                continue;
            if (dungeonOf.find(uint32(pQuest->GetZoneOrSort())) == dungeonOf.end())
                continue;
            byQuest[pQuest->GetQuestId()].quest = pQuest;
        }

        std::map<uint32, uint32> creatureZone, goZone;
        for (auto const& rel : sObjectMgr.GetCreatureQuestRelationsMap())
        {
            g_givers.emplace(rel.second, rel.first);
            if (byQuest.count(rel.second))
            {
                byQuest[rel.second].starts.push_back({ STARTER_CREATURE, rel.first, 0 });
                creatureZone[rel.first] = 0;
            }
        }
        for (auto const& rel : sObjectMgr.GetGOQuestRelationsMap())
        {
            g_openStart.insert(rel.second);
            if (byQuest.count(rel.second))
            {
                byQuest[rel.second].starts.push_back({ STARTER_GAMEOBJECT, rel.first, 0 });
                goZone[rel.first] = 0;
            }
        }
        for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
        {
            if (!itr.second.StartQuest)
                continue;
            g_openStart.insert(itr.second.StartQuest);
            if (byQuest.count(itr.second.StartQuest))
                byQuest[itr.second.StartQuest].starts.push_back({ STARTER_ITEM, itr.first, 0 });
        }

        // The first spawn of each starter, for "where do I pick it up".
        auto doCreature = [&](std::pair<uint32 const, CreatureData> const& itr)
        {
            auto z = creatureZone.find(itr.second.creature_id[0]);
            if (z != creatureZone.end() && !z->second)
            {
                WorldLocation const& pos = itr.second.position;
                z->second = sTerrainMgr.GetZoneId(pos.mapId, pos.x, pos.y, pos.z);
            }
            return false;
        };
        sObjectMgr.DoCreatureData(doCreature);
        auto doGameObject = [&](std::pair<uint32 const, GameObjectData> const& itr)
        {
            auto z = goZone.find(itr.second.id);
            if (z != goZone.end() && !z->second)
            {
                WorldLocation const& pos = itr.second.position;
                z->second = sTerrainMgr.GetZoneId(pos.mapId, pos.x, pos.y, pos.z);
            }
            return false;
        };
        sObjectMgr.DoGOData(doGameObject);

        for (auto& itr : byQuest)
        {
            DungeonQuest& dq = itr.second;
            // Quests nothing hands out (unused or later patches) are not counted.
            if (dq.starts.empty())
                continue;
            for (QuestStart& s : dq.starts)
                s.zone = s.type == STARTER_CREATURE ? creatureZone[s.entry]
                       : s.type == STARTER_GAMEOBJECT ? goZone[s.entry] : 0;
            g_index[dungeonOf[uint32(dq.quest->GetZoneOrSort())]].push_back(dq);
        }
    }

    bool CreatureFriendly(Player* pPlayer, uint32 entry)
    {
        CreatureInfo const* pInfo = sObjectMgr.GetCreatureTemplate(entry);
        FactionTemplateEntry const* pNpc = pInfo ? sObjectMgr.GetFactionTemplateEntry(pInfo->faction) : nullptr;
        FactionTemplateEntry const* pMine = pPlayer->GetFactionTemplateEntry();
        return !pNpc || !pMine || (!pNpc->IsHostileTo(*pMine) && !pMine->IsHostileTo(*pNpc));
    }

    bool StarterFriendly(Player* pPlayer, QuestStart const& s)
    {
        return s.type != STARTER_CREATURE || CreatureFriendly(pPlayer, s.entry);
    }

    // Whether the character can ever get to this quest: its race and class,
    // someone not hostile who hands it out, and a way through its earlier
    // quests (a Horde-only chain closes the rest of it to the Alliance).
    bool Attainable(Player* pPlayer, Quest const* pQuest, std::map<uint32, bool>& memo, uint32 depth = 0)
    {
        uint32 const id = pQuest->GetQuestId();
        if (pPlayer->GetQuestRewardStatus(id) || pPlayer->GetQuestStatus(id) != QUEST_STATUS_NONE)
            return true;
        auto known = memo.find(id);
        if (known != memo.end())
            return known->second;
        memo[id] = false;   // a loop in the chain counts as closed
        if (depth > 10 || !pPlayer->SatisfyQuestRace(pQuest, false) || !pPlayer->SatisfyQuestClass(pQuest, false))
            return false;
        if (!g_openStart.count(id))
        {
            auto range = g_givers.equal_range(id);
            if (std::none_of(range.first, range.second,
                    [pPlayer](std::pair<uint32 const, uint32> const& g) { return CreatureFriendly(pPlayer, g.second); }))
                return false;
        }
        if (!pQuest->prevQuests.empty() &&
            std::none_of(pQuest->prevQuests.begin(), pQuest->prevQuests.end(), [&](int32 prev)
            {
                Quest const* pPrev = sObjectMgr.GetQuestTemplate(uint32(std::abs(prev)));
                return !pPrev || Attainable(pPlayer, pPrev, memo, depth + 1);
            }))
            return false;
        memo[id] = true;
        return true;
    }

    State Classify(Player* pPlayer, DungeonQuest const& dq, std::map<uint32, bool>& memo)
    {
        Quest const* pQuest = dq.quest;
        uint32 const id = pQuest->GetQuestId();
        if (pPlayer->GetQuestRewardStatus(id))
            return STATE_DONE;
        // Not rewarded, so complete or failed still means it is in the log.
        QuestStatus const status = pPlayer->GetQuestStatus(id);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_FAILED)
            return STATE_LOG;
        if (!pPlayer->SatisfyQuestSkill(pQuest, false) || !pPlayer->SatisfyQuestExclusiveGroup(pQuest, false) ||
            !Attainable(pPlayer, pQuest, memo))
            return STATE_SKIP;
        return pPlayer->CanTakeQuest(pQuest, false) ? STATE_OPEN : STATE_LATER;
    }

    struct Entry
    {
        DungeonQuest const* dq;
        State state;
    };

    // The quests of one dungeon that count for this character, exclusive
    // groups folded into their best member.
    std::vector<Entry> Collect(Player* pPlayer, uint32 dungeon)
    {
        std::vector<Entry> out;
        std::map<int32, size_t> groups;
        std::map<uint32, bool> memo;
        for (DungeonQuest const& dq : g_index[dungeon])
        {
            State const state = Classify(pPlayer, dq, memo);
            if (state == STATE_SKIP)
                continue;
            int32 const group = dq.quest->GetExclusiveGroup();
            if (group > 0)
            {
                auto g = groups.find(group);
                if (g != groups.end())
                {
                    if (state < out[g->second].state)
                        out[g->second] = { &dq, state };
                    continue;
                }
                groups[group] = out.size();
            }
            out.push_back({ &dq, state });
        }
        return out;
    }

    bool IsZh(WorldSession const* pSession)
    {
        if (!pSession)
            return false;
        int const index = pSession->GetSessionDbLocaleIndex();
        return index >= 0 && sObjectMgr.GetLocaleForIndex(index) == LOCALE_zhCN;
    }

    std::string QuestTitle(Quest const* pQuest, int loc)
    {
        if (QuestLocale const* ql = sObjectMgr.GetQuestLocale(pQuest->GetQuestId()))
            if (loc >= 0 && ql->Title.size() > size_t(loc) && !ql->Title[loc].empty())
                return ql->Title[loc];
        return pQuest->GetTitle();
    }

    std::string StarterText(QuestStart const& s, int loc, bool zh)
    {
        std::string name;
        if (s.type == STARTER_CREATURE)
        {
            if (CreatureInfo const* pInfo = sObjectMgr.GetCreatureTemplate(s.entry))
                name = pInfo->name;
            if (CreatureLocale const* cl = sObjectMgr.GetCreatureLocale(s.entry))
                if (loc >= 0 && cl->Name.size() > size_t(loc) && !cl->Name[loc].empty())
                    name = cl->Name[loc];
        }
        else if (s.type == STARTER_GAMEOBJECT)
        {
            if (GameObjectInfo const* pInfo = sObjectMgr.GetGameObjectTemplate(s.entry))
                name = pInfo->name;
            if (GameObjectLocale const* gl = sObjectMgr.GetGameObjectLocale(s.entry))
                if (loc >= 0 && gl->Name.size() > size_t(loc) && !gl->Name[loc].empty())
                    name = gl->Name[loc];
        }
        else
        {
            if (ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(s.entry))
                name = pProto->Name1;
            if (ItemLocale const* il = sObjectMgr.GetItemLocale(s.entry))
                if (loc >= 0 && il->Name.size() > size_t(loc) && !il->Name[loc].empty())
                    name = il->Name[loc];
            return zh ? "物品「" + name + "」开始" : "starts from the item " + name;
        }
        if (s.zone)
        {
            std::string zone;
            if (AreaEntry const* pArea = AreaEntry::GetById(s.zone))
                zone = pArea->Name;
            sObjectMgr.GetAreaLocaleString(s.zone, loc, &zone);
            if (!zone.empty())
                name += zh ? "（" + zone + "）" : " (" + zone + ")";
        }
        return name;
    }
}

bool ChatHandler::HandleDungeonQuestsCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    if (!g_indexed)
        BuildIndex();

    std::string const arg = args ? args : "";
    bool const zh = IsZh(m_session);

    // `.dungeonquests quiet`: a machine line per dungeon, for the GMBox addon.
    if (arg.empty() || arg == "quiet")
    {
        for (auto const& zones : DUNGEON_ZONES)
        {
            uint32 n[4] = {};
            for (Entry const& e : Collect(pPlayer, zones.front()))
                ++n[e.state];
            // Also read by the GMBox addon: keep its shape.
            PSendSysMessage("[dq] %u done=%u log=%u open=%u later=%u", zones.front(), n[0], n[1], n[2], n[3]);
        }
        if (arg.empty())
            SendSysMessage(zh ? "用法：.dungeonquests <副本区域号>（GMBox 副本页右键副本即可）"
                              : "Usage: .dungeonquests <dungeon zone> (right click a dungeon in GMBox)");
        return true;
    }

    uint32 const dungeon = uint32(atoi(arg.c_str()));
    if (std::none_of(DUNGEON_ZONES.begin(), DUNGEON_ZONES.end(),
            [dungeon](std::vector<uint32> const& z) { return z.front() == dungeon; }))
    {
        SendSysMessage(zh ? "没有这个副本。" : "No such dungeon.");
        SetSentErrorMessage(true);
        return false;
    }

    int const loc = m_session->GetSessionDbLocaleIndex();
    std::vector<Entry> entries = Collect(pPlayer, dungeon);
    std::stable_sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b)
    {
        if (a.state != b.state)
            return a.state > b.state;
        return a.dq->quest->GetMinLevel() < b.dq->quest->GetMinLevel();
    });

    std::string dungeonName;
    if (AreaEntry const* pArea = AreaEntry::GetById(dungeon))
        dungeonName = pArea->Name;
    sObjectMgr.GetAreaLocaleString(dungeon, loc, &dungeonName);
    uint32 n[4] = {};
    for (Entry const& e : entries)
        ++n[e.state];
    if (zh)
        PSendSysMessage("%s的任务：完成 %u，日志里 %u，现在能接 %u，还不能接 %u。",
            dungeonName.c_str(), n[STATE_DONE], n[STATE_LOG], n[STATE_OPEN], n[STATE_LATER]);
    else
        PSendSysMessage("%s quests: %u done, %u in the log, %u to pick up now, %u not yet.",
            dungeonName.c_str(), n[STATE_DONE], n[STATE_LOG], n[STATE_OPEN], n[STATE_LATER]);

    for (Entry const& e : entries)
    {
        Quest const* pQuest = e.dq->quest;
        std::string const title = QuestTitle(pQuest, loc);
        if (e.state == STATE_DONE)
            continue;
        if (e.state == STATE_LOG)
        {
            PSendSysMessage(zh ? "  [%u] %s：在任务日志里" : "  [%u] %s: in your quest log",
                pQuest->GetQuestLevel(), title.c_str());
            continue;
        }

        std::string where;
        for (QuestStart const& s : e.dq->starts)
            if (StarterFriendly(pPlayer, s))
            {
                where = StarterText(s, loc, zh);
                break;
            }
        if (e.state == STATE_OPEN)
        {
            PSendSysMessage(zh ? "  [%u] %s：现在能接，%s" : "  [%u] %s: take it now, %s",
                pQuest->GetQuestLevel(), title.c_str(), where.c_str());
            continue;
        }

        std::string why;
        if (!pPlayer->SatisfyQuestLevel(pQuest, false))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), zh ? "%u 级才能接" : "needs level %u", pQuest->GetMinLevel());
            why = buf;
        }
        else if (!pPlayer->SatisfyQuestPreviousQuest(pQuest, false))
        {
            Quest const* pPrev = pQuest->GetPrevQuestId() ? sObjectMgr.GetQuestTemplate(uint32(std::abs(pQuest->GetPrevQuestId()))) : nullptr;
            why = pPrev ? (zh ? "先完成「" + QuestTitle(pPrev, loc) + "」" : "first finish \"" + QuestTitle(pPrev, loc) + "\"")
                        : (zh ? "要先完成前置任务" : "needs an earlier quest");
        }
        else
            why = zh ? "条件还不满足" : "not available yet";
        PSendSysMessage(zh ? "  [%u] %s：%s，%s" : "  [%u] %s: %s, %s",
            pQuest->GetQuestLevel(), title.c_str(), why.c_str(), where.c_str());
    }
    return true;
}
