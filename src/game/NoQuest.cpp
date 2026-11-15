/*
 * There is no quest log. No giver marks or offers a quest, and nothing starts
 * one. Under the hood a quest whose every objective is a kill or an item
 * creatures drop still counts while the character kills, and pays out the
 * moment it is done: the experience, money and reputation of a turn-in, the
 * best fitting choice item, mail for what the bags cannot hold. Only those
 * gains show.
 *
 * Earlier quests of a chain are not required (they are mostly talking and
 * carrying); class, race, level, skill, reputation and exclusive groups are.
 */

#include "NoQuest.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Formulas.h"
#include "Creature.h"
#include "LootMgr.h"
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
    struct NoQuestEntry
    {
        Quest const* quest;
        std::vector<uint32> givers;     // creatures that hand it out
        bool openStart = false;         // an object or an item starts it
    };

    std::unordered_map<uint32, NoQuestEntry> g_quests;
    std::multimap<uint32, uint32> g_byKill;     // creature -> quests killing it counts for
    std::multimap<uint32, uint32> g_byItem;     // item -> quests needing it

    bool CreatureFriendly(Player const* pPlayer, uint32 entry)
    {
        CreatureInfo const* pInfo = sObjectMgr.GetCreatureTemplate(entry);
        FactionTemplateEntry const* pNpc = pInfo ? sObjectMgr.GetFactionTemplateEntry(pInfo->faction) : nullptr;
        FactionTemplateEntry const* pMine = pPlayer->GetFactionTemplateEntry();
        return !pNpc || !pMine || (!pNpc->IsHostileTo(*pMine) && !pMine->IsHostileTo(*pNpc));
    }

    // Items some creature can drop, straight or through a reference.
    std::set<uint32> CreatureDrops()
    {
        std::set<uint32> items;
        uint32 const patch = sWorld.GetWowPatch();
        std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
            "SELECT DISTINCT l.`item` FROM `creature_loot_template` l "
            "JOIN `creature_template` c ON c.`loot_id` = l.`entry` "
            "WHERE l.`mincountOrRef` > 0 AND %u BETWEEN l.`patch_min` AND l.`patch_max` "
            "UNION SELECT DISTINCT r.`item` FROM `creature_loot_template` l "
            "JOIN `creature_template` c ON c.`loot_id` = l.`entry` "
            "JOIN `reference_loot_template` r ON r.`entry` = -l.`mincountOrRef` "
            "WHERE l.`mincountOrRef` < 0 AND r.`mincountOrRef` > 0 "
            "AND %u BETWEEN l.`patch_min` AND l.`patch_max` AND %u BETWEEN r.`patch_min` AND r.`patch_max`",
            patch, patch, patch));
        if (result)
        {
            do
                items.insert(result->Fetch()[0].GetUInt32());
            while (result->NextRow());
        }
        return items;
    }

    void SaveProgress(Player const* pPlayer, uint32 questId, NoQuestProgress const& c)
    {
        CharacterDatabase.PExecute("REPLACE INTO `character_noquest_kills` (`guid`, `quest`, `count1`, `count2`, `count3`, `count4`, "
            "`item1`, `item2`, `item3`, `item4`) VALUES (%u, %u, %u, %u, %u, %u, %u, %u, %u, %u)",
            pPlayer->GetGUIDLow(), questId, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    }

    // Items toward objective j: counted from loot, plus any in the bags.
    uint32 ItemsHeld(Player const* pPlayer, Quest const* pQuest, int j)
    {
        auto const& progress = pPlayer->GetNoQuestKills();
        auto p = progress.find(pQuest->GetQuestId());
        return (p == progress.end() ? 0 : p->second[QUEST_OBJECTIVES_COUNT + j]) + pPlayer->GetItemCount(pQuest->ReqItemId[j]);
    }

    // How much the class wants each primary stat, for the choice reward.
    int32 StatWeight(uint8 playerClass, uint32 stat)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
                return stat == ITEM_MOD_STRENGTH ? 4 : stat == ITEM_MOD_STAMINA ? 2 : stat == ITEM_MOD_AGILITY ? 2 : 0;
            case CLASS_PALADIN:
                return stat == ITEM_MOD_STRENGTH ? 4 : stat == ITEM_MOD_STAMINA ? 2 : stat == ITEM_MOD_INTELLECT ? 2 : 0;
            case CLASS_HUNTER:
                return stat == ITEM_MOD_AGILITY ? 4 : stat == ITEM_MOD_STAMINA ? 2 : stat == ITEM_MOD_INTELLECT ? 1 : 0;
            case CLASS_ROGUE:
                return stat == ITEM_MOD_AGILITY ? 4 : stat == ITEM_MOD_STAMINA ? 2 : stat == ITEM_MOD_STRENGTH ? 1 : 0;
            case CLASS_SHAMAN:
                return stat == ITEM_MOD_INTELLECT ? 3 : stat == ITEM_MOD_STRENGTH ? 2 : stat == ITEM_MOD_STAMINA ? 2 : stat == ITEM_MOD_AGILITY ? 1 : 0;
            case CLASS_DRUID:
                return stat == ITEM_MOD_INTELLECT ? 2 : stat == ITEM_MOD_AGILITY ? 2 : stat == ITEM_MOD_STRENGTH ? 2 : stat == ITEM_MOD_STAMINA ? 2 : stat == ITEM_MOD_SPIRIT ? 1 : 0;
            default:    // priest, mage, warlock
                return stat == ITEM_MOD_INTELLECT ? 4 : stat == ITEM_MOD_SPIRIT ? 2 : stat == ITEM_MOD_STAMINA ? 2 : 0;
        }
    }

    // The choice reward to take: gear the character can wear, of its own armor
    // type and stats, over the rest; failing all, the one that sells for most.
    uint32 ChooseReward(Player const* pPlayer, Quest const* pQuest)
    {
        uint32 best = 0;
        int64 bestScore = -1;
        for (uint32 i = 0; i < pQuest->GetRewChoiceItemsCount(); ++i)
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(pQuest->RewChoiceItemId[i]);
            if (!pProto)
                continue;
            int64 score = pProto->SellPrice;
            bool const gear = pProto->Class == ITEM_CLASS_WEAPON || pProto->Class == ITEM_CLASS_ARMOR;
            if (gear && pPlayer->CanUseItem(pProto) == EQUIP_ERR_OK)
            {
                score += 1000000000;
                bool const bodyArmor = pProto->Class == ITEM_CLASS_ARMOR && pProto->InventoryType != INVTYPE_CLOAK &&
                    pProto->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && pProto->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE;
                if (bodyArmor && pProto->GetProficiencySkill() == pPlayer->GetHighestKnownArmorProficiency())
                    score += 100000000;
                int64 stats = 0;
                for (auto const& stat : pProto->ItemStat)
                    stats += int64(StatWeight(pPlayer->GetClass(), stat.ItemStatType)) * stat.ItemStatValue;
                score += (stats * 100 + pProto->ItemLevel) * 1000;
            }
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }
}

void NoQuest::LoadQuests()
{
    g_quests.clear();
    g_byKill.clear();
    g_byItem.clear();

    std::set<uint32> const drops = CreatureDrops();

    std::multimap<uint32, uint32> givers;   // quest -> creature
    for (auto const& rel : sObjectMgr.GetCreatureQuestRelationsMap())
        givers.emplace(rel.second, rel.first);
    std::set<uint32> openStart;
    for (auto const& rel : sObjectMgr.GetGOQuestRelationsMap())
        openStart.insert(rel.second);
    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
        if (itr.second.StartQuest)
            openStart.insert(itr.second.StartQuest);

    for (auto const& itr : sObjectMgr.GetQuestTemplates())
    {
        Quest const* pQuest = itr.second.get();
        if (pQuest->IsRepeatable() || pQuest->GetQuestMethod() == QUEST_METHOD_DISABLED || pQuest->GetType() == QUEST_TYPE_PVP ||
            pQuest->HasSpecialFlag(QUEST_SPECIAL_FLAG_EXPLORATION_OR_EVENT) || pQuest->GetRepObjectiveFaction())
            continue;

        bool killable = true;
        uint32 objectives = 0;
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
        {
            int32 const target = pQuest->ReqCreatureOrGOId[j];
            if (target < 0 || (target > 0 && pQuest->ReqSpell[j]))
                killable = false;
            else if (target > 0)
                ++objectives;
        }
        for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
        {
            uint32 const item = pQuest->ReqItemId[j];
            if (!item)
                continue;
            // Carried for the giver, or found some other way than on a body.
            if (item == pQuest->GetSrcItemId() || !drops.count(item))
                killable = false;
            ++objectives;
        }
        if (!killable || !objectives)
            continue;

        uint32 const id = pQuest->GetQuestId();
        NoQuestEntry gq;
        gq.quest = pQuest;
        auto range = givers.equal_range(id);
        for (auto g = range.first; g != range.second; ++g)
            gq.givers.push_back(g->second);
        gq.openStart = openStart.count(id) != 0;
        // Nothing hands it out: unused, or from a later patch.
        if (gq.givers.empty() && !gq.openStart)
            continue;

        g_quests.emplace(id, std::move(gq));
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
            if (pQuest->ReqCreatureOrGOId[j] > 0)
                g_byKill.emplace(uint32(pQuest->ReqCreatureOrGOId[j]), id);
        for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
            if (pQuest->ReqItemId[j])
                g_byItem.emplace(pQuest->ReqItemId[j], id);
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> No-quest mode: %u quests complete by killing", uint32(g_quests.size()));
}

bool Player::NoQuestOpen(Quest const* pQuest) const
{
    uint32 const id = pQuest->GetQuestId();
    auto gq = g_quests.find(id);
    if (gq == g_quests.end() || !pQuest->IsActive())
        return false;
    // Taken by hand, or done.
    if (GetQuestStatus(id) != QUEST_STATUS_NONE || GetQuestRewardStatus(id))
        return false;
    if (pQuest->GetMaxLevel() && pQuest->GetMaxLevel() < GetLevel())
        return false;
    // A grey quest would only fill the bags with gear long outgrown.
    if (pQuest->GetQuestLevel() && pQuest->GetQuestLevel() <= MaNGOS::XP::GetGrayLevel(GetLevel()))
        return false;
    if (GetGroup() && GetGroup()->isRaidGroup() && !pQuest->IsAllowedInRaid() && !InBattleGround())
        return false;
    if (!SatisfyQuestClass(pQuest, false) || !SatisfyQuestRace(pQuest, false) || !SatisfyQuestLevel(pQuest, false) ||
        !SatisfyQuestSkill(pQuest, false) || !SatisfyQuestCondition(pQuest, false) || !SatisfyQuestReputation(pQuest, false) ||
        !SatisfyQuestExclusiveGroup(pQuest, false) || !SatisfyQuestNextChain(pQuest, false))
        return false;
    // Many quests name no race: only who hands them out keeps them to a side.
    if (gq->second.openStart)
        return true;
    for (uint32 giver : gq->second.givers)
        if (CreatureFriendly(this, giver))
            return true;
    return false;
}

void NoQuest::FillBossEpics(Player* player, Loot& loot)
{
    if (!player || !player->IsNoQuest())
        return;
    WorldObject const* target = loot.GetLootTarget();
    Creature const* creature = target ? target->ToCreature() : nullptr;
    if (!creature)
        return;

    uint32 const entry = creature->GetEntry();
    uint8 const cls = player->GetClass();
    // creature, class, item — class epics Classic only gave through quest chains.
    struct { uint32 creature; uint8 cls; uint32 item; } const table[] = {
        { 10184, CLASS_WARRIOR, 18348 }, // Onyxia: Quel'Serrar
        { 10184, CLASS_PALADIN, 18348 },
        { 11583, CLASS_WARRIOR, 18348 }, // Nefarian: Quel'Serrar
        { 11583, CLASS_PALADIN, 18348 },
        { 6109,  CLASS_HUNTER,  18713 }, // Azuregos: Rhok'delar
        { 6109,  CLASS_HUNTER,  18715 }, // Lok'delar
        { 11583, CLASS_HUNTER,  18713 },
        { 11583, CLASS_HUNTER,  18715 },
        { 11583, CLASS_PRIEST,  18608 }, // Nefarian: Benediction
        { 14516, CLASS_SHAMAN,  20134 }, // Darkreaver: Skyfury Helm
        { 11583, CLASS_SHAMAN,  20134 },
    };
    for (auto const& row : table)
    {
        if (row.creature != entry || row.cls != cls)
            continue;
        if (player->HasItemCount(row.item, 1, true))
            continue;
        if (loot.items.size() >= MAX_NR_LOOT_ITEMS)
            break;
        LootItem li(row.item, 1);
        li.freeforall = true;
        li.lootOwner = player->GetObjectGuid();
        loot.items.push_back(li);
    }
}

void Player::LoadNoQuest()
{
    // Every character. A quest already in the log would keep showing; drop it.
    m_noQuest = true;
    m_noQuestKills.clear();
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT `quest`, `count1`, `count2`, `count3`, `count4`, `item1`, `item2`, `item3`, `item4` "
            "FROM `character_noquest_kills` WHERE `guid` = %u", GetGUIDLow()))
    {
        do
        {
            Field* fields = result->Fetch();
            auto& c = m_noQuestKills[fields[0].GetUInt32()];
            for (size_t j = 0; j < c.size(); ++j)
                c[j] = fields[1 + j].GetUInt32();
        }
        while (result->NextRow());
    }
    for (int i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
        if (GetQuestSlotQuestId(i))
            RemoveQuestAtSlot(i);
    GrantShamanTotems();
}

void Player::GrantShamanTotems()
{
    if (!IsNoQuest() || GetClass() != CLASS_SHAMAN)
        return;
    // Earth / Fire / Water / Air: quest rewards the trainers cannot teach.
    struct { uint32 level; uint32 item; } const totems[] = {
        { 4, 5175 }, { 10, 5176 }, { 20, 5177 }, { 30, 5178 },
    };
    for (auto const& t : totems)
    {
        if (GetLevel() < t.level || HasItemCount(t.item, 1, true))
            continue;
        ItemPosCountVec dest;
        if (CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, t.item, 1) != EQUIP_ERR_OK)
            continue;
        Item* item = StoreNewItem(dest, t.item, true);
        SendNewItem(item, 1, true, false);
    }
}

// Rewarded as though turned in to the character itself, once every kill and
// every item is there. True when it was.
static bool NoQuestTryComplete(Player* pPlayer, Quest const* pQuest)
{
    uint32 const id = pQuest->GetQuestId();
    auto const& kills = pPlayer->GetNoQuestKills();
    auto k = kills.find(id);
    for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
        if (pQuest->ReqCreatureOrGOId[j] > 0 && (k == kills.end() || k->second[j] < pQuest->ReqCreatureOrGOCount[j]))
            return false;
    for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
        if (pQuest->ReqItemId[j] && ItemsHeld(pPlayer, pQuest, j) < pQuest->ReqItemCount[j])
            return false;

    CharacterDatabase.PExecute("DELETE FROM `character_noquest_kills` WHERE `guid` = %u AND `quest` = %u", pPlayer->GetGUIDLow(), id);
    // Not announced: the experience and items show as gains, the quest not at all.
    pPlayer->RewardQuest(pQuest, ChooseReward(pPlayer, pQuest), pPlayer, false);
    return true;
}

void Player::NoQuestKilled(uint32 entry)
{
    std::vector<Quest const*> touched;
    auto range = g_byKill.equal_range(entry);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        Quest const* pQuest = g_quests.at(itr->second).quest;
        if (!NoQuestOpen(pQuest))
            continue;
        auto& counts = m_noQuestKills[pQuest->GetQuestId()];
        bool changed = false;
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
        {
            if (pQuest->ReqCreatureOrGOId[j] != int32(entry) || counts[j] >= pQuest->ReqCreatureOrGOCount[j])
                continue;
            ++counts[j];
            changed = true;
        }
        if (!changed)
            continue;
        SaveProgress(this, pQuest->GetQuestId(), counts);
        touched.push_back(pQuest);
    }
    for (Quest const* pQuest : touched)
        if (NoQuestTryComplete(this, pQuest))
            m_noQuestKills.erase(pQuest->GetQuestId());
}

bool Player::NoQuestNeedsItem(uint32 itemId) const
{
    auto range = g_byItem.equal_range(itemId);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        Quest const* pQuest = g_quests.at(itr->second).quest;
        if (!NoQuestOpen(pQuest))
            continue;
        for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
            if (pQuest->ReqItemId[j] == itemId && ItemsHeld(this, pQuest, j) < pQuest->ReqItemCount[j])
                return true;
    }
    return false;
}

void Player::NoQuestLootItem(uint32 itemId, uint32 count)
{
    std::vector<Quest const*> touched;
    auto range = g_byItem.equal_range(itemId);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        Quest const* pQuest = g_quests.at(itr->second).quest;
        if (!NoQuestOpen(pQuest))
            continue;
        bool changed = false;
        for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
        {
            if (pQuest->ReqItemId[j] != itemId)
                continue;
            uint32 const held = ItemsHeld(this, pQuest, j);
            if (held >= pQuest->ReqItemCount[j])
                continue;
            m_noQuestKills[pQuest->GetQuestId()][QUEST_OBJECTIVES_COUNT + j] += std::min(count, pQuest->ReqItemCount[j] - held);
            changed = true;
        }
        if (!changed)
            continue;
        SaveProgress(this, pQuest->GetQuestId(), m_noQuestKills[pQuest->GetQuestId()]);
        touched.push_back(pQuest);
    }
    for (Quest const* pQuest : touched)
        if (NoQuestTryComplete(this, pQuest))
            m_noQuestKills.erase(pQuest->GetQuestId());
}

void Player::NoQuestItemAdded(uint32 itemId)
{
    std::vector<Quest const*> touched;
    auto range = g_byItem.equal_range(itemId);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        Quest const* pQuest = g_quests.at(itr->second).quest;
        if (NoQuestOpen(pQuest))
            touched.push_back(pQuest);
    }
    // A reward takes the items, so one item can finish only the first quest.
    for (Quest const* pQuest : touched)
        if (NoQuestOpen(pQuest) && NoQuestTryComplete(this, pQuest))
            m_noQuestKills.erase(pQuest->GetQuestId());
}
