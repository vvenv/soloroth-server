/*
 * Party bots that join on their own in five-player dungeons. See
 * DungeonBotMgr.h for the rules.
 */

#include "DungeonBotMgr.h"
#include "PartyBotAI.h"
#include "PlayerBotMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "Database/DatabaseEnv.h"
#include "Util.h"
#include "Utilities/Random.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include <algorithm>
#include <list>
#include <vector>

namespace
{
    // The companions: one per class and talent tree on each side, always with
    // the same name, race, gender and face, so the same people come back.
    struct RosterBot
    {
        char const* name;
        Team team;
        uint8 botClass;
        uint8 race;
        uint8 gender;
        CombatBotRoles role;
        char const* specEn;
        char const* specZh;
        char const* specHint;   // part of the premade spec names to prefer
    };

    RosterBot const ROSTER[] =
    {
        { "Aldric",    ALLIANCE, CLASS_WARRIOR, RACE_HUMAN,    GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "Brannoc",   ALLIANCE, CLASS_WARRIOR, RACE_DWARF,    GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "Durgan",    ALLIANCE, CLASS_WARRIOR, RACE_DWARF,    GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "Elenya",    ALLIANCE, CLASS_PALADIN, RACE_HUMAN,    GENDER_FEMALE, ROLE_HEALER,    "Holy Paladin",           "神圣骑士",     "holy" },
        { "Hamrik",    ALLIANCE, CLASS_PALADIN, RACE_DWARF,    GENDER_MALE,   ROLE_TANK,      "Protection Paladin",     "防护骑士",     "prot" },
        { "Cedric",    ALLIANCE, CLASS_PALADIN, RACE_HUMAN,    GENDER_MALE,   ROLE_MELEE_DPS, "Retribution Paladin",    "惩戒骑士",     "retribution" },
        { "Sylvaine",  ALLIANCE, CLASS_HUNTER,  RACE_NIGHTELF, GENDER_FEMALE, ROLE_RANGE_DPS, "Beast Mastery Hunter",   "野兽控制猎人", "" },
        { "Gundrik",   ALLIANCE, CLASS_HUNTER,  RACE_DWARF,    GENDER_MALE,   ROLE_RANGE_DPS, "Marksmanship Hunter",    "射击猎人",     "mm" },
        { "Faelan",    ALLIANCE, CLASS_HUNTER,  RACE_NIGHTELF, GENDER_MALE,   ROLE_RANGE_DPS, "Survival Hunter",        "生存猎人",     "sv" },
        { "Vesper",    ALLIANCE, CLASS_ROGUE,   RACE_HUMAN,    GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "Fizzwick",  ALLIANCE, CLASS_ROGUE,   RACE_GNOME,    GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "Nyssa",     ALLIANCE, CLASS_ROGUE,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "Brenna",    ALLIANCE, CLASS_PRIEST,  RACE_DWARF,    GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",      "戒律牧师",     "holy" },
        { "Lucienne",  ALLIANCE, CLASS_PRIEST,  RACE_HUMAN,    GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",            "神圣牧师",     "holy" },
        { "Mordecai",  ALLIANCE, CLASS_PRIEST,  RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",          "暗影牧师",     "shadow" },
        { "Tinkara",   ALLIANCE, CLASS_MAGE,    RACE_GNOME,    GENDER_FEMALE, ROLE_RANGE_DPS, "Arcane Mage",            "奥术法师",     "arcane" },
        { "Corvin",    ALLIANCE, CLASS_MAGE,    RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Fire Mage",              "火焰法师",     "fire" },
        { "Isolde",    ALLIANCE, CLASS_MAGE,    RACE_HUMAN,    GENDER_FEMALE, ROLE_RANGE_DPS, "Frost Mage",             "冰霜法师",     "frost" },
        { "Malachar",  ALLIANCE, CLASS_WARLOCK, RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Affliction Warlock",     "痛苦术士",     "affli" },
        { "Nixwick",   ALLIANCE, CLASS_WARLOCK, RACE_GNOME,    GENDER_MALE,   ROLE_RANGE_DPS, "Demonology Warlock",     "恶魔学识术士", "demo" },
        { "Seraphine", ALLIANCE, CLASS_WARLOCK, RACE_HUMAN,    GENDER_FEMALE, ROLE_RANGE_DPS, "Destruction Warlock",    "毁灭术士",     "ruin" },
        { "Aerwynn",   ALLIANCE, CLASS_DRUID,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_RANGE_DPS, "Balance Druid",          "平衡德鲁伊",   "balance" },
        { "Thalor",    ALLIANCE, CLASS_DRUID,   RACE_NIGHTELF, GENDER_MALE,   ROLE_MELEE_DPS, "Feral Druid",            "野性德鲁伊",   "feral" },
        { "Nerwen",    ALLIANCE, CLASS_DRUID,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_HEALER,    "Restoration Druid",      "恢复德鲁伊",   "resto" },

        { "Grukash",   HORDE,    CLASS_WARRIOR, RACE_ORC,      GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "Rakzan",    HORDE,    CLASS_WARRIOR, RACE_TROLL,    GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "Tahomak",   HORDE,    CLASS_WARRIOR, RACE_TAUREN,   GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "Shakara",   HORDE,    CLASS_SHAMAN,  RACE_ORC,      GENDER_FEMALE, ROLE_RANGE_DPS, "Elemental Shaman",       "元素萨满",     "ele" },
        { "Durgash",   HORDE,    CLASS_SHAMAN,  RACE_ORC,      GENDER_MALE,   ROLE_MELEE_DPS, "Enhancement Shaman",     "增强萨满",     "enha" },
        { "Halona",    HORDE,    CLASS_SHAMAN,  RACE_TAUREN,   GENDER_FEMALE, ROLE_HEALER,    "Restoration Shaman",     "恢复萨满",     "resto" },
        { "Grashnak",  HORDE,    CLASS_HUNTER,  RACE_ORC,      GENDER_MALE,   ROLE_RANGE_DPS, "Beast Mastery Hunter",   "野兽控制猎人", "" },
        { "Zanjira",   HORDE,    CLASS_HUNTER,  RACE_TROLL,    GENDER_FEMALE, ROLE_RANGE_DPS, "Marksmanship Hunter",    "射击猎人",     "mm" },
        { "Kotori",    HORDE,    CLASS_HUNTER,  RACE_TAUREN,   GENDER_MALE,   ROLE_RANGE_DPS, "Survival Hunter",        "生存猎人",     "sv" },
        { "Morwenna",  HORDE,    CLASS_ROGUE,   RACE_UNDEAD,   GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "Skarn",     HORDE,    CLASS_ROGUE,   RACE_ORC,      GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "Vexjin",    HORDE,    CLASS_ROGUE,   RACE_TROLL,    GENDER_MALE,   ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "Zalaya",    HORDE,    CLASS_PRIEST,  RACE_TROLL,    GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",      "戒律牧师",     "holy" },
        { "Elspeth",   HORDE,    CLASS_PRIEST,  RACE_UNDEAD,   GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",            "神圣牧师",     "holy" },
        { "Vorlath",   HORDE,    CLASS_PRIEST,  RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",          "暗影牧师",     "shadow" },
        { "Ashveld",   HORDE,    CLASS_MAGE,    RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Arcane Mage",            "奥术法师",     "arcane" },
        { "Yenjala",   HORDE,    CLASS_MAGE,    RACE_TROLL,    GENDER_FEMALE, ROLE_RANGE_DPS, "Fire Mage",              "火焰法师",     "fire" },
        { "Halvrik",   HORDE,    CLASS_MAGE,    RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Frost Mage",             "冰霜法师",     "frost" },
        { "Morthis",   HORDE,    CLASS_WARLOCK, RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Affliction Warlock",     "痛苦术士",     "affli" },
        { "Grishna",   HORDE,    CLASS_WARLOCK, RACE_ORC,      GENDER_FEMALE, ROLE_RANGE_DPS, "Demonology Warlock",     "恶魔学识术士", "demo" },
        { "Cindrel",   HORDE,    CLASS_WARLOCK, RACE_UNDEAD,   GENDER_FEMALE, ROLE_RANGE_DPS, "Destruction Warlock",    "毁灭术士",     "ruin" },
        { "Aponi",     HORDE,    CLASS_DRUID,   RACE_TAUREN,   GENDER_FEMALE, ROLE_RANGE_DPS, "Balance Druid",          "平衡德鲁伊",   "balance" },
        { "Hoketah",   HORDE,    CLASS_DRUID,   RACE_TAUREN,   GENDER_MALE,   ROLE_MELEE_DPS, "Feral Druid",            "野性德鲁伊",   "feral" },
        { "Mahala",    HORDE,    CLASS_DRUID,   RACE_TAUREN,   GENDER_FEMALE, ROLE_HEALER,    "Restoration Druid",      "恢复德鲁伊",   "resto" },
    };

    uint32 LookSeed(char const* name)
    {
        uint32 h = 2166136261u;
        for (; *name; ++name)
            h = (h ^ uint8(*name)) * 16777619u;
        return h ? h : 1;
    }

    uint32 const UPDATE_EVERY_MS = 500;
    // The client finishes loading the dungeon before the bots appear.
    uint32 const FILL_DELAY_MS = 3000;
    // A fill that waits for a ghost to come back to life gives up after this.
    uint32 const FILL_GIVE_UP_MS = 120000;
    // A bot that never shows up in the group is forgotten after this.
    uint32 const BOT_LOGIN_GRACE_MS = 30000;
    // After a full wipe, wait for mobs to evade before safe-spot revive.
    uint32 const WIPE_REVIVE_DELAY_MS = 8000;
    float const SAFE_AGGRO_MARGIN = 3.0f;
    float const SAFE_SEARCH_RANGE = 40.0f;
    uint32 const SPELL_HONORLESS_TARGET = 2479;
    uint32 const PARTY_SIZE = 5;
    uint8 const NO_PREFERENCE = 0;  // no class


    enum : uint32
    {
        SPELL_SHIELD_SLAM      = 23922,
        SPELL_HOLY_SHIELD      = 20925,
        SPELL_HOLY_SHOCK       = 20473,
        SPELL_SHADOWFORM       = 15473,
        SPELL_MOONKIN_FORM     = 24858,
        SPELL_SWIFTMEND        = 18562,
        SPELL_NATURES_SWIFT_DR = 17116,
        SPELL_MANA_TIDE        = 16190,
        SPELL_STORMSTRIKE      = 17364,
    };

    bool IsDps(CombatBotRoles role)
    {
        return role == ROLE_MELEE_DPS || role == ROLE_RANGE_DPS || role == ROLE_INVALID;
    }

    bool HasShield(Player const* pPlayer)
    {
        Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return pItem && pItem->GetProto()->InventoryType == INVTYPE_SHIELD;
    }

    // The client's language. GetSessionDbcLocale() is the server's DBC locale
    // (enUS here), so the client's own locale comes from its text index.
    bool IsZh(WorldSession const* pSession)
    {
        if (!pSession)
            return false;
        int const index = pSession->GetSessionDbLocaleIndex();
        return index >= 0 && sObjectMgr.GetLocaleForIndex(index) == LOCALE_zhCN;
    }

    // The party bot AI that runs a player, if it is one.
    PartyBotAI* GetPartyBotAI(Player* pPlayer)
    {
        if (!pPlayer || !pPlayer->IsBot() || !pPlayer->AI())
            return nullptr;
        return dynamic_cast<PartyBotAI*>(pPlayer->AI());
    }
}

DungeonBotMgr& DungeonBotMgr::Instance()
{
    static DungeonBotMgr instance;
    return instance;
}

char const* DungeonBotMgr::RoleName(CombatBotRoles role, bool zh)
{
    switch (role)
    {
        case ROLE_TANK:      return zh ? "坦克" : "tank";
        case ROLE_HEALER:    return zh ? "治疗" : "healer";
        case ROLE_MELEE_DPS: return zh ? "近战输出" : "melee dps";
        case ROLE_RANGE_DPS: return zh ? "远程输出" : "ranged dps";
        default:             return zh ? "自动" : "auto";
    }
}

CombatBotRoles DungeonBotMgr::GuessRole(Player const* pPlayer)
{
    switch (pPlayer->GetClass())
    {
        case CLASS_WARRIOR:
            return (pPlayer->HasSpell(SPELL_SHIELD_SLAM) || HasShield(pPlayer)) ? ROLE_TANK : ROLE_MELEE_DPS;
        case CLASS_PALADIN:
            if (pPlayer->HasSpell(SPELL_HOLY_SHIELD))
                return ROLE_TANK;
            if (pPlayer->HasSpell(SPELL_HOLY_SHOCK))
                return ROLE_HEALER;
            return ROLE_MELEE_DPS;
        case CLASS_PRIEST:
            return pPlayer->HasSpell(SPELL_SHADOWFORM) ? ROLE_RANGE_DPS : ROLE_HEALER;
        case CLASS_DRUID:
            if (pPlayer->HasSpell(SPELL_SWIFTMEND) || pPlayer->HasSpell(SPELL_NATURES_SWIFT_DR))
                return ROLE_HEALER;
            return pPlayer->HasSpell(SPELL_MOONKIN_FORM) ? ROLE_RANGE_DPS : ROLE_MELEE_DPS;
        case CLASS_SHAMAN:
            if (pPlayer->HasSpell(SPELL_MANA_TIDE))
                return ROLE_HEALER;
            return pPlayer->HasSpell(SPELL_STORMSTRIKE) ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
        case CLASS_ROGUE:
            return ROLE_MELEE_DPS;
        default:
            return ROLE_RANGE_DPS;
    }
}

// ---------------------------------------------------------------- settings --

DungeonBotMgr::Settings& DungeonBotMgr::GetSettings(Player* pPlayer)
{
    uint32 const guid = pPlayer->GetGUIDLow();
    auto itr = m_settings.find(guid);
    if (itr != m_settings.end())
        return itr->second;

    Settings& s = m_settings[guid];
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT `role`, `auto_fill` FROM `character_party_bots` WHERE `guid` = %u", guid))
    {
        Field* fields = result->Fetch();
        uint8 const role = fields[0].GetUInt8();
        s.role = role <= ROLE_HEALER ? CombatBotRoles(role) : ROLE_INVALID;
        s.autoFill = fields[1].GetUInt8() != 0;
    }
    return s;
}

void DungeonBotMgr::SaveSettings(Player* pPlayer)
{
    Settings const& s = GetSettings(pPlayer);
    CharacterDatabase.PExecute("REPLACE INTO `character_party_bots` (`guid`, `role`, `auto_fill`) VALUES (%u, %u, %u)",
        pPlayer->GetGUIDLow(), uint32(s.role), s.autoFill ? 1u : 0u);
}

CombatBotRoles DungeonBotMgr::GetChosenRole(Player* pPlayer)
{
    return GetSettings(pPlayer).role;
}

CombatBotRoles DungeonBotMgr::GetPlayerRole(Player* pPlayer)
{
    CombatBotRoles const chosen = GetChosenRole(pPlayer);
    return chosen != ROLE_INVALID ? chosen : GuessRole(pPlayer);
}

void DungeonBotMgr::SetChosenRole(Player* pPlayer, CombatBotRoles role)
{
    GetSettings(pPlayer).role = role;
    SaveSettings(pPlayer);
}

bool DungeonBotMgr::IsAutoFill(Player* pPlayer)
{
    return GetSettings(pPlayer).autoFill;
}

void DungeonBotMgr::SetAutoFill(Player* pPlayer, bool on)
{
    GetSettings(pPlayer).autoFill = on;
    SaveSettings(pPlayer);
}

// --------------------------------------------------------------- companions --

void DungeonBotMgr::LoadCompanions()
{
    if (m_companionsLoaded)
        return;
    m_companionsLoaded = true;
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.Query(
            "SELECT `owner_guid`, `slot`, `bot_guid` FROM `character_companions`"))
    {
        do
        {
            Field* fields = result->Fetch();
            m_companions[fields[0].GetUInt32()][fields[1].GetUInt32()] = fields[2].GetUInt32();
            m_companionOwner[fields[2].GetUInt32()] = fields[0].GetUInt32();
        }
        while (result->NextRow());
    }
}

bool DungeonBotMgr::IsCompanion(uint32 botLow)
{
    LoadCompanions();
    return m_companionOwner.find(botLow) != m_companionOwner.end();
}

uint32 DungeonBotMgr::GetCompanion(uint32 ownerLow, uint32 slot)
{
    LoadCompanions();
    auto owner = m_companions.find(ownerLow);
    if (owner == m_companions.end())
        return 0;
    auto itr = owner->second.find(slot);
    return itr != owner->second.end() ? itr->second : 0;
}

void DungeonBotMgr::SetCompanion(uint32 ownerLow, uint32 slot, uint32 botLow)
{
    m_companions[ownerLow][slot] = botLow;
    m_companionOwner[botLow] = ownerLow;
    CharacterDatabase.PExecute("REPLACE INTO `character_companions` (`owner_guid`, `slot`, `bot_guid`) VALUES (%u, %u, %u)",
        ownerLow, slot, botLow);
}

void DungeonBotMgr::ForgetCompanion(uint32 ownerLow, uint32 slot)
{
    if (uint32 const botLow = GetCompanion(ownerLow, slot))
        m_companionOwner.erase(botLow);
    m_companions[ownerLow].erase(slot);
    CharacterDatabase.PExecute("DELETE FROM `character_companions` WHERE `owner_guid` = %u AND `slot` = %u", ownerLow, slot);
}

void DungeonBotMgr::DeleteCompanions(uint32 ownerLow)
{
    LoadCompanions();
    auto owner = m_companions.find(ownerLow);
    if (owner == m_companions.end())
        return;
    for (auto const& itr : owner->second)
    {
        m_companionOwner.erase(itr.second);
        Player::DeleteFromDB(ObjectGuid(HIGHGUID_PLAYER, itr.second), sObjectMgr.GetPlayerAccountIdByGUID(itr.second), false, true);
    }
    m_companions.erase(owner);
    CharacterDatabase.PExecute("DELETE FROM `character_companions` WHERE `owner_guid` = %u", ownerLow);
}

// -------------------------------------------------------------- fill/dismiss --

void DungeonBotMgr::ForgetGoneBots(uint32 leaderLow)
{
    auto itr = m_autoBots.find(leaderLow);
    if (itr == m_autoBots.end())
        return;

    for (auto bot = itr->second.begin(); bot != itr->second.end();)
    {
        // A bot between two maps (following its leader) is briefly not in the
        // world, so only one gone for a while is forgotten.
        if (ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, bot->first)))
            bot->second.lastSeen = m_now;
        if (m_now - bot->second.lastSeen > BOT_LOGIN_GRACE_MS)
            bot = itr->second.erase(bot);
        else
            ++bot;
    }
    if (itr->second.empty())
        m_autoBots.erase(itr);
}

uint32 DungeonBotMgr::Fill(Player* pPlayer, std::string& who, std::string& why, uint32 target, uint32* removedOut)
{
    if (target < 2)
        target = 2;
    if (target > PARTY_SIZE)
        target = PARTY_SIZE;
    if (removedOut)
        *removedOut = 0;

    bool const zh = IsZh(pPlayer->GetSession());

    if (pPlayer->IsTaxiFlying() || pPlayer->InBattleGround() || pPlayer->IsBeingTeleported())
    {
        why = zh ? "现在不能叫机器人（飞行中、战场里或传送中）。" : "Bots cannot join now (flying, in a battleground or teleporting).";
        return 0;
    }
    Map* pMap = pPlayer->GetMap();
    if (!pMap || pMap->IsRaid())
    {
        why = zh ? "团队副本里不会补机器人。" : "Bots do not join in raids.";
        return 0;
    }

    ForgetGoneBots(pPlayer->GetGUIDLow());

    // A saved group can hold companions that went offline with the server.
    if (Group* pOld = pPlayer->GetGroup())
    {
        std::vector<ObjectGuid> gone;
        for (auto const& slot : pOld->GetMemberSlots())
            if (!ObjectAccessor::FindPlayerNotInWorld(slot.guid) && IsCompanion(slot.guid.GetCounter()))
                gone.push_back(slot.guid);
        for (ObjectGuid const& guid : gone)
            if (Group* pNow = pPlayer->GetGroup())
                pNow->RemoveMember(guid, 0);
    }

    // Who is in the party already, and as what.
    std::vector<CombatBotRoles> roles;
    std::vector<uint8> classes;
    std::vector<std::string> names;
    std::vector<uint32> guids;
    Group* pGroup = pPlayer->GetGroup();
    if (pGroup)
    {
        if (pGroup->isRaidGroup())
        {
            why = zh ? "团队里不会补机器人。" : "Bots do not join a raid group.";
            return 0;
        }
        if (pGroup->GetLeaderGuid() != pPlayer->GetObjectGuid())
        {
            why = zh ? "只有队长能叫机器人。" : "Only the party leader can call bots.";
            return 0;
        }
        for (auto const& slot : pGroup->GetMemberSlots())
        {
            guids.push_back(slot.guid.GetCounter());
            Player* pMember = sObjectAccessor.FindPlayer(slot.guid);
            if (!pMember)
            {
                roles.push_back(ROLE_INVALID);
                continue;
            }
            classes.push_back(pMember->GetClass());
            names.push_back(pMember->GetName());
            if (PartyBotAI* pAI = GetPartyBotAI(pMember))
                roles.push_back(pAI->GetRole());
            else
                roles.push_back(GetPlayerRole(pMember));
        }
    }
    else
    {
        // Same order as a group slot, so guids[i] and roles[i] stay paired.
        guids.push_back(pPlayer->GetGUIDLow());
        roles.push_back(GetPlayerRole(pPlayer));
        classes.push_back(pPlayer->GetClass());
        names.push_back(pPlayer->GetName());
    }

    // Bots still logging in count too, or a second fill would double them.
    auto& mine = m_autoBots[pPlayer->GetGUIDLow()];
    for (auto const& bot : mine)
    {
        ObjectGuid const guid(HIGHGUID_PLAYER, bot.first);
        if (pGroup && pGroup->IsMember(guid))
            continue;
        guids.push_back(bot.first);
        roles.push_back(bot.second.role);
        classes.push_back(bot.second.botClass);
        names.push_back(bot.second.name);
    }

    // Too many: send bots home until the party is the size asked for. Damage
    // bots go first, then a healer, then a tank, so a duo or a trio keeps the
    // roles a fill would have added first. The player is never removed.
    if (roles.size() > target)
    {
        auto dropBot = [&](uint32 botLow)
        {
            ObjectGuid const guid(HIGHGUID_PLAYER, botLow);
            if (Group* pNow = pPlayer->GetGroup())
                if (pNow->IsMember(guid))
                    pNow->RemoveMember(guid, 0);
            // In the world: the bot manager logs them out. Still logging in:
            // DeleteBot leaves the entry offline so the login does not finish.
            if (PartyBotAI* pAI = GetPartyBotAI(sObjectAccessor.FindPlayer(guid)))
            {
                if (pAI->botEntry)
                    pAI->botEntry->requestRemoval = true;
            }
            else
                sPlayerBotMgr.DeleteBot(botLow);
            mine.erase(botLow);
        };

        std::vector<uint32> gone;
        uint32 removed = 0;
        while (roles.size() - gone.size() > target)
        {
            int best = -1;
            int bestScore = 99;
            for (uint32 i = 0; i < guids.size(); ++i)
            {
                if (guids[i] == pPlayer->GetGUIDLow())
                    continue;
                if (std::find(gone.begin(), gone.end(), guids[i]) != gone.end())
                    continue;
                ObjectGuid const guid(HIGHGUID_PLAYER, guids[i]);
                bool const bot = mine.find(guids[i]) != mine.end() || IsCompanion(guids[i]) ||
                                 GetPartyBotAI(sObjectAccessor.FindPlayer(guid)) != nullptr;
                if (!bot)
                    continue;
                int score = 0;
                if (i < roles.size() && roles[i] == ROLE_HEALER)
                    score = 1;
                else if (i < roles.size() && roles[i] == ROLE_TANK)
                    score = 2;
                if (best < 0 || score < bestScore || (score == bestScore && int(i) > best))
                {
                    best = int(i);
                    bestScore = score;
                }
            }
            if (best < 0)
                break;
            dropBot(guids[best]);
            gone.push_back(guids[best]);
            ++removed;
        }
        if (removedOut)
            *removedOut = removed;
        if (roles.size() - gone.size() > target)
        {
            why = zh ? "队伍里还有别人，不能缩到这个人数。" : "The party has other players, so it cannot shrink to that size.";
            return 0;
        }
        return 0;
    }

    if (roles.size() >= target)
    {
        if (target == 2)
            why = zh ? "已经是两人。" : "Already a duo.";
        else if (target == 3)
            why = zh ? "已经是三人。" : "Already a trio.";
        else
            why = zh ? "队伍已经满了。" : "The party is already full.";
        return 0;
    }

    bool needTank = std::find(roles.begin(), roles.end(), ROLE_TANK) == roles.end();
    bool needHealer = std::find(roles.begin(), roles.end(), ROLE_HEALER) == roles.end();
    uint32 melee = uint32(std::count(roles.begin(), roles.end(), ROLE_MELEE_DPS));

    Team const team = pPlayer->GetTeam();
    uint32 const ownerLow = pPlayer->GetGUIDLow();
    auto slotOf = [](RosterBot const& r) { return uint32(&r - ROSTER); };
    // Only this character's own companion can be out already.
    auto present = [&](RosterBot const& r)
    {
        uint32 const botLow = GetCompanion(ownerLow, slotOf(r));
        if (!botLow)
            return false;
        return std::find(guids.begin(), guids.end(), botLow) != guids.end() ||
               ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, botLow)) != nullptr;
    };
    // A roster member for one of `wanted` roles, this character's companions
    // first: one of `preferClass` if any is free, else one whose class the
    // party lacks, else any. Each class gets the same chance however many of
    // its trees fit.
    auto pick = [&](std::vector<CombatBotRoles> const& wanted, uint8 preferClass) -> RosterBot const*
    {
        bool known = false;
        for (RosterBot const& r : ROSTER)
            if (r.team == team && std::find(wanted.begin(), wanted.end(), r.role) != wanted.end() && !present(r) &&
                GetCompanion(ownerLow, slotOf(r)))
                known = true;

        std::vector<RosterBot const*> fit, fresh, preferred;
        for (RosterBot const& r : ROSTER)
        {
            if (r.team != team || std::find(wanted.begin(), wanted.end(), r.role) == wanted.end() || present(r))
                continue;
            if (known && !GetCompanion(ownerLow, slotOf(r)))
                continue;
            fit.push_back(&r);
            if (std::find(classes.begin(), classes.end(), r.botClass) == classes.end())
                fresh.push_back(&r);
            if (r.botClass == preferClass)
                preferred.push_back(&r);
        }
        std::vector<RosterBot const*> const& pool = !preferred.empty() ? preferred : !fresh.empty() ? fresh : fit;
        if (pool.empty())
            return nullptr;
        std::vector<uint8> poolClasses;
        for (RosterBot const* r : pool)
            if (std::find(poolClasses.begin(), poolClasses.end(), r->botClass) == poolClasses.end())
                poolClasses.push_back(r->botClass);
        uint8 const botClass = SelectRandomContainerElement(poolClasses);
        std::vector<RosterBot const*> ofClass;
        for (RosterBot const* r : pool)
            if (r->botClass == botClass)
                ofClass.push_back(r);
        return SelectRandomContainerElement(ofClass);
    };

    uint32 const slots = target - uint32(roles.size());
    uint32 added = 0;
    for (uint32 i = 0; i < slots; ++i)
    {
        RosterBot const* r;
        if (needTank)
        {
            // Warriors tank best; the protection paladin only stands in.
            r = pick({ ROLE_TANK }, CLASS_WARRIOR);
            needTank = false;
        }
        else if (needHealer)
        {
            // Priests heal best; take one when the party has none yet.
            bool const hasPriest = std::find(classes.begin(), classes.end(), CLASS_PRIEST) != classes.end();
            r = pick({ ROLE_HEALER }, hasPriest ? NO_PREFERENCE : CLASS_PRIEST);
            needHealer = false;
        }
        else
        {
            // At most two melee, so the tank is not crowded and cleaves hit fewer.
            r = melee >= 2 ? pick({ ROLE_RANGE_DPS }, NO_PREFERENCE) : pick({ ROLE_MELEE_DPS, ROLE_RANGE_DPS }, NO_PREFERENCE);
        }
        if (!r)
            continue;
        if (r->role == ROLE_MELEE_DPS)
            ++melee;

        float x, y, z;
        pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 3.0f, frand(0.0f, 2 * M_PI_F));
        uint32 const slot = slotOf(*r);
        uint32 botLow = GetCompanion(ownerLow, slot);
        std::string name;
        if (botLow && !sObjectMgr.GetPlayerNameByGUID(botLow, name))
        {
            ForgetCompanion(ownerLow, slot);    // its character is gone
            botLow = 0;
        }

        PartyBotAI* ai;
        if (botLow)
        {
            // Race and class 0: PartyBotAI logs the saved character in.
            ai = new PartyBotAI(pPlayer, nullptr, r->role, 0, 0, pPlayer->GetLevel(),
                pPlayer->GetMapId(), pMap->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
            ai->m_specHint = r->specHint;
            ai->m_companion = true;
            if (!sPlayerBotMgr.AddBot(botLow, false, ai))
            {
                delete ai;
                continue;
            }
        }
        else
        {
            name = sObjectMgr.GetPlayerGuidByName(r->name).IsEmpty() ? std::string(r->name) : sObjectMgr.GenerateFreePlayerName();
            normalizePlayerName(name);
            ai = new PartyBotAI(pPlayer, nullptr, r->role, r->race, r->botClass, pPlayer->GetLevel(),
                pPlayer->GetMapId(), pMap->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
            ai->SetIdentity(name, r->gender, LookSeed(name.c_str()));
            ai->m_specHint = r->specHint;
            ai->m_companion = true;
            if (!sPlayerBotMgr.AddBot(ai, true))
            {
                delete ai;
                continue;
            }
            botLow = uint32(ai->botEntry->playerGUID);
            SetCompanion(ownerLow, slot, botLow);
        }
        ai->botEntry->persistent = true;

        AutoBot& bot = mine[botLow];
        bot.role = r->role;
        bot.botClass = r->botClass;
        bot.name = name;
        bot.lastSeen = m_now;
        bot.dungeon = pMap->IsNonRaidDungeon();
        classes.push_back(r->botClass);
        names.push_back(name);
        guids.push_back(botLow);
        if (!who.empty())
            who += zh ? "、" : ", ";
        who += name;
        who += zh ? "（" : " (";
        who += zh ? r->specZh : r->specEn;
        who += zh ? "）" : ")";
        ++added;
    }

    if (!added)
        why = zh ? "机器人没能生成。" : "No bot could be spawned.";
    return added;
}

uint32 DungeonBotMgr::Dismiss(Player* pPlayer, bool autoOnly)
{
    uint32 count = 0;

    // The bots this manager brought, wherever they are. One that is between
    // two maps is not found now; it stays listed and goes on a later call.
    auto mine = m_autoBots.find(pPlayer->GetGUIDLow());
    if (mine != m_autoBots.end())
    {
        for (auto bot = mine->second.begin(); bot != mine->second.end();)
        {
            if (autoOnly && !bot->second.dungeon)
            {
                ++bot;
                continue;
            }
            PartyBotAI* pAI = GetPartyBotAI(sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, bot->first)));
            if (pAI && pAI->botEntry)
            {
                pAI->botEntry->requestRemoval = true;
                ++count;
                bot = mine->second.erase(bot);
            }
            else
                ++bot;
        }
        if (mine->second.empty())
            m_autoBots.erase(mine);
    }

    // Everything else this player leads: bots added with .partybot.
    if (!autoOnly)
    {
        if (Group* pGroup = pPlayer->GetGroup())
        {
            for (auto const& slot : pGroup->GetMemberSlots())
            {
                PartyBotAI* pAI = GetPartyBotAI(sObjectAccessor.FindPlayer(slot.guid));
                if (!pAI || pAI->m_leaderGuid != pPlayer->GetObjectGuid() || !pAI->botEntry || pAI->botEntry->requestRemoval)
                    continue;
                pAI->botEntry->requestRemoval = true;
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------- safe / wipe rez --

void DungeonBotMgr::ClearSafe(Watch& w)
{
    w.hasSafe = false;
    w.safeMap = 0;
    w.safeInstance = 0;
    w.safeX = w.safeY = w.safeZ = w.safeO = 0.f;
}

void DungeonBotMgr::SeedSafe(Watch& w, Player* pPlayer)
{
    if (!pPlayer || !pPlayer->GetMap())
        return;
    w.hasSafe = true;
    w.safeMap = pPlayer->GetMapId();
    w.safeInstance = pPlayer->GetInstanceId();
    w.safeX = pPlayer->GetPositionX();
    w.safeY = pPlayer->GetPositionY();
    w.safeZ = pPlayer->GetPositionZ();
    w.safeO = pPlayer->GetOrientation();
}

bool DungeonBotMgr::IsSafeSpot(Player* pPlayer) const
{
    if (!pPlayer || !pPlayer->IsInWorld())
        return false;

    std::list<Unit*> hostiles;
    MaNGOS::AnyHostileUnitInObjectRangeCheck u_check(pPlayer, pPlayer, SAFE_SEARCH_RANGE);
    MaNGOS::UnitListSearcher<MaNGOS::AnyHostileUnitInObjectRangeCheck> searcher(hostiles, u_check);
    Cell::VisitAllObjects(pPlayer, searcher, SAFE_SEARCH_RANGE);

    for (Unit* u : hostiles)
    {
        Creature* c = u ? u->ToCreature() : nullptr;
        if (!c || !c->IsAlive() || c->IsCivilian())
            continue;
        if (c->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1 | UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PLAYER))
            continue;
        float const aggro = c->GetAttackDistance(pPlayer) + SAFE_AGGRO_MARGIN;
        if (aggro > 0.f && pPlayer->IsWithinDistInMap(c, aggro))
            return false;
    }
    return true;
}

void DungeonBotMgr::MaybeUpdateSafe(Watch& w, Player* pPlayer)
{
    if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsInCombat())
        return;
    if (!IsSafeSpot(pPlayer))
        return;
    SeedSafe(w, pPlayer);
}

bool DungeonBotMgr::PartyWipedOnMap(Player* pPlayer) const
{
    if (!pPlayer || pPlayer->IsAlive())
        return false;
    Map* pMap = pPlayer->GetMap();
    if (!pMap)
        return false;

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
        return true;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember->GetMap() != pMap)
            continue;
        if (pMember->IsAlive())
            return false;
    }
    return true;
}

void DungeonBotMgr::RevivePartyAtSafe(Player* pPlayer, Watch& w)
{
    if (!pPlayer || !w.hasSafe)
        return;

    uint32 const mapId = w.safeMap;
    float const x = w.safeX;
    float const y = w.safeY;
    float const z = w.safeZ;
    float const o = w.safeO;

    auto reviveOne = [&](Player* p)
    {
        if (!p || p->IsAlive() || !p->IsInWorld() || p->IsBeingTeleported())
            return;
        if (p->GetMapId() != mapId)
            return;

        p->TeleportTo(mapId, x, y, z, o);
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(0.5f);
            p->SpawnCorpseBones();
        }
        if (p->IsBot())
            p->CastSpell(p, SPELL_HONORLESS_TARGET, true);
    };

    reviveOne(pPlayer);
    if (Group* pGroup = pPlayer->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (pMember && pMember != pPlayer)
                reviveOne(pMember);
        }
    }

    bool const zh = IsZh(pPlayer->GetSession());
    ChatHandler(pPlayer).SendSysMessage(zh
        ? "团灭了。队伍已在最近的安全点复活。"
        : "Party wiped. The group resurrected at the last safe spot.");
}

// ------------------------------------------------------------------- update --

void DungeonBotMgr::Update(uint32 diff)
{
    m_now += diff;
    m_timer += diff;
    if (m_timer < UPDATE_EVERY_MS)
        return;
    m_timer = 0;

    std::vector<ObjectGuid> players;
    {
        HashMapHolder<Player>::ReadGuard guard(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
            if (itr.second && !itr.second->IsBot())
                players.push_back(itr.first);
    }

    for (ObjectGuid const& guid : players)
    {
        Player* pPlayer = sObjectAccessor.FindPlayer(guid);
        if (!pPlayer || !pPlayer->IsInWorld() || pPlayer->IsBeingTeleported())
            continue;
        Map* pMap = pPlayer->GetMap();
        if (!pMap)
            continue;

        Watch& w = m_watch[guid.GetCounter()];
        bool const inDungeon = pMap->IsNonRaidDungeon();
        if (pMap->GetId() != w.lastMap)
        {
            w.lastMap = pMap->GetId();
            w.fillAt = (inDungeon && IsAutoFill(pPlayer)) ? m_now + FILL_DELAY_MS : 0;
            w.wipeAt = 0;
            ClearSafe(w);
            if (inDungeon && pPlayer->IsAlive())
                SeedSafe(w, pPlayer);
        }

        if (!inDungeon)
        {
            w.fillAt = 0;
            w.wipeAt = 0;
            ClearSafe(w);
            // Out of the dungeon on their feet: the dungeon's bots go home.
            // A ghost on its corpse run keeps them.
            auto mine = m_autoBots.find(guid.GetCounter());
            if (pPlayer->IsAlive() && mine != m_autoBots.end() && !mine->second.empty())
            {
                ForgetGoneBots(guid.GetCounter());
                if (uint32 gone = Dismiss(pPlayer, true))
                {
                    bool const zh = IsZh(pPlayer->GetSession());
                    ChatHandler(pPlayer).PSendSysMessage(zh ? "离开副本，%u 个机器人队友解散了。" : "Left the dungeon: %u bot(s) went home.", gone);
                }
            }
            continue;
        }

        // Safe checkpoint while alive and out of combat.
        if (pPlayer->IsAlive())
        {
            w.wipeAt = 0;
            if (!w.hasSafe)
                SeedSafe(w, pPlayer);
            else
                MaybeUpdateSafe(w, pPlayer);
        }
        else if (PartyWipedOnMap(pPlayer))
        {
            if (!w.wipeAt)
                w.wipeAt = m_now;
            else if (m_now - w.wipeAt >= WIPE_REVIVE_DELAY_MS)
            {
                // No walk recorded (e.g. died on the load-in): use the corpse.
                if (!w.hasSafe)
                    SeedSafe(w, pPlayer);
                RevivePartyAtSafe(pPlayer, w);
                w.wipeAt = 0;
            }
        }
        else
            w.wipeAt = 0;

        if (!w.fillAt || m_now < w.fillAt)
            continue;
        if (!pPlayer->IsAlive() || pPlayer->IsInCombat())
        {
            if (m_now - w.fillAt > FILL_GIVE_UP_MS)
                w.fillAt = 0;
            continue;
        }
        w.fillAt = 0;

        std::string who, why;
        bool const zh = IsZh(pPlayer->GetSession());
        if (Fill(pPlayer, who, why))
            ChatHandler(pPlayer).PSendSysMessage(zh ? "队友加入了队伍：%s（你是%s）。人数在小地图下的组队按钮里改。" : "Joining your party: %s (you play %s). The party button under the minimap changes the size.",
                who.c_str(), RoleName(GetPlayerRole(pPlayer), zh));
        else if (!why.empty())
            ChatHandler(pPlayer).SendSysMessage(why.c_str());
    }

    // Players who logged out: forget where they were, so logging back in
    // inside a dungeon counts as entering it (their bots left with them).
    // (A player between two maps is not in the world but still online.)
    for (auto itr = m_watch.begin(); itr != m_watch.end();)
    {
        if (!ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, itr->first)))
            itr = m_watch.erase(itr);
        else
            ++itr;
    }

    // Leaders who logged out: their bots leave on their own (PartyBotAI).
    for (auto itr = m_autoBots.begin(); itr != m_autoBots.end();)
    {
        if (!ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, itr->first)))
            itr = m_autoBots.erase(itr);
        else
            ++itr;
    }
}

// ----------------------------------------------------------------- commands --

static bool BotsZh(ChatHandler* handler)
{
    WorldSession* s = handler->GetSession();
    return IsZh(s);
}

bool ChatHandler::HandleBotsStatusCommand(char* args)
{
    // `.bots status quiet`: only the machine line, for the GMBox addon.
    bool const quiet = args && std::string(args) == "quiet";
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    bool const zh = BotsZh(this);
    CombatBotRoles const chosen = sDungeonBotMgr.GetChosenRole(pPlayer);
    CombatBotRoles const playing = sDungeonBotMgr.GetPlayerRole(pPlayer);
    bool const autoFill = sDungeonBotMgr.IsAutoFill(pPlayer);
    // The first line is also read by the GMBox addon: keep its shape.
    PSendSysMessage("[bots] role=%s playing=%s auto=%u", DungeonBotMgr::RoleName(chosen, false),
        DungeonBotMgr::RoleName(playing, false), autoFill ? 1u : 0u);
    if (quiet)
        return true;
    if (zh)
        PSendSysMessage("你的职责：%s%s；进五人副本自动补机器人：%s。",
            DungeonBotMgr::RoleName(playing, true), chosen == ROLE_INVALID ? "（自动判断）" : "", autoFill ? "开" : "关");
    else
        PSendSysMessage("You play %s%s; five-player dungeons fill with bots: %s.",
            DungeonBotMgr::RoleName(playing, false), chosen == ROLE_INVALID ? " (guessed)" : "", autoFill ? "on" : "off");
    return true;
}

bool ChatHandler::HandleBotsRoleCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    std::string const arg = args ? args : "";
    CombatBotRoles role;
    if (arg == "tank")
        role = ROLE_TANK;
    else if (arg == "healer" || arg == "heal")
        role = ROLE_HEALER;
    else if (arg == "dps")
        role = CombatBotBaseAI::IsMeleeDamageClass(pPlayer->GetClass()) && pPlayer->GetClass() != CLASS_DRUID &&
               pPlayer->GetClass() != CLASS_SHAMAN ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
    else if (arg == "melee")
        role = ROLE_MELEE_DPS;
    else if (arg == "ranged")
        role = ROLE_RANGE_DPS;
    else if (arg == "auto")
        role = ROLE_INVALID;
    else
    {
        SendSysMessage(BotsZh(this) ? "用法：.bots role tank|healer|dps|auto" : "Usage: .bots role tank|healer|dps|auto");
        SetSentErrorMessage(true);
        return false;
    }
    sDungeonBotMgr.SetChosenRole(pPlayer, role);
    return HandleBotsStatusCommand(nullptr);
}

bool ChatHandler::HandleBotsAutoCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    bool on;
    if (!ExtractOnOff(&args, on))
    {
        SendSysMessage(BotsZh(this) ? "用法：.bots auto on|off" : "Usage: .bots auto on|off");
        SetSentErrorMessage(true);
        return false;
    }
    sDungeonBotMgr.SetAutoFill(pPlayer, on);
    return HandleBotsStatusCommand(nullptr);
}

bool ChatHandler::HandleBotsFillCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;

    // No argument still fills a party of five. 2 and 3 are a duo and a trio.
    // Raid sizes (20 and 40) are refused here: Fill does not join a raid.
    uint32 target = 5;
    if (args)
    {
        while (*args == ' ')
            ++args;
        if (*args)
        {
            char const digit = args[0];
            char* rest = args + 1;
            while (*rest == ' ')
                ++rest;
            if ((digit != '2' && digit != '3' && digit != '5') || *rest)
            {
                SendSysMessage(BotsZh(this)
                    ? "用法：.bots fill [2|3|5]。两人、三人或五人。团队副本是 20 人或 40 人，现在不会补机器人。"
                    : "Usage: .bots fill [2|3|5]. A duo, a trio or five. Raids are 20 or 40 and are not filled.");
                SetSentErrorMessage(true);
                return false;
            }
            target = uint32(digit - '0');
        }
    }

    std::string who, why;
    uint32 removed = 0;
    if (!sDungeonBotMgr.Fill(pPlayer, who, why, target, &removed))
    {
        if (removed && why.empty())
        {
            PSendSysMessage(BotsZh(this) ? "解散了 %u 个机器人，现在是 %u 人。" : "Sent %u bot(s) home. The party is %u.",
                removed, target);
            return true;
        }
        SendSysMessage(why.c_str());
        // "Already a duo" is the answer, not a failure the client should print in red.
        if (why.find("已经") != std::string::npos || why.find("lready") != std::string::npos)
            return true;
        SetSentErrorMessage(true);
        return false;
    }
    PSendSysMessage(BotsZh(this) ? "队友正在赶来：%s。" : "On their way: %s.", who.c_str());
    return true;
}

bool ChatHandler::HandleBotsDismissCommand(char* /*args*/)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    uint32 const gone = sDungeonBotMgr.Dismiss(pPlayer, false);
    PSendSysMessage(BotsZh(this) ? "%u 个机器人队友解散了。" : "%u bot(s) went home.", gone);
    return true;
}
