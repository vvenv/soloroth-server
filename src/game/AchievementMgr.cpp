/*
 * Achievements. See AchievementMgr.h for the rules.
 */

#include "AchievementMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "Chat.h"
#include "World.h"
#include "ObjectMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>

namespace
{
    char const* const ADDON_PREFIX = "SoloAch";
    // The addon message, prefix and tab included, stays under the client's
    // 255 byte chat line.
    size_t const ADDON_CHUNK = 200;

    bool IsZh(WorldSession const* pSession)
    {
        if (!pSession)
            return false;
        int const index = pSession->GetSessionDbLocaleIndex();
        return index >= 0 && sObjectMgr.GetLocaleForIndex(index) == LOCALE_zhCN;
    }

    // Sends "<tag> a,b,c" lines, as many as the tokens need.
    template <class F>
    void SendChunked(char tag, std::vector<std::string> const& tokens, F send)
    {
        std::string line;
        for (std::string const& token : tokens)
        {
            if (!line.empty() && line.size() + token.size() + 1 > ADDON_CHUNK)
            {
                send(std::string(1, tag) + " " + line);
                line.clear();
            }
            if (!line.empty())
                line += ',';
            line += token;
        }
        if (!line.empty())
            send(std::string(1, tag) + " " + line);
    }
}

AchievementMgr& AchievementMgr::Instance()
{
    static AchievementMgr instance;
    return instance;
}

void AchievementMgr::Load()
{
    m_achievements.clear();
    m_criteria.clear();
    for (auto& list : m_byType)
        list.clear();
    m_byKill.clear();
    m_byArea.clear();
    m_byZone.clear();
    m_byQuest.clear();
    m_byEarned.clear();

    if (std::unique_ptr<QueryResult> result = WorldDatabase.Query(
            "SELECT `id`, `category`, `faction`, `points`, `required_count`, `name`, `name_loc4` FROM `achievement_template`"))
    {
        do
        {
            Field* fields = result->Fetch();
            AchievementEntry& a = m_achievements[fields[0].GetUInt32()];
            a.id = fields[0].GetUInt32();
            a.category = fields[1].GetUInt16();
            a.faction = fields[2].GetUInt8();
            a.points = fields[3].GetUInt8();
            a.requiredCount = fields[4].GetUInt8();
            a.name = fields[5].GetCppString();
            a.nameZh = fields[6].GetCppString();
        } while (result->NextRow());
    }

    if (std::unique_ptr<QueryResult> result = WorldDatabase.Query(
            "SELECT `id`, `achievement`, `type`, `asset1`, `asset2`, `asset3`, `asset4`, `value` FROM `achievement_criteria` ORDER BY `id`"))
    {
        do
        {
            Field* fields = result->Fetch();
            AchievementCriteriaEntry c;
            c.id = fields[0].GetUInt32();
            c.achievement = fields[1].GetUInt32();
            c.type = fields[2].GetUInt8();
            for (int i = 0; i < 4; ++i)
                c.asset[i] = fields[3 + i].GetUInt32();
            c.value = std::max(fields[7].GetUInt32(), 1u);

            auto achievement = m_achievements.find(c.achievement);
            if (achievement == m_achievements.end() || !c.type || c.type >= 8)
            {
                sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "achievement_criteria %u: unknown achievement %u or type %u, skipped.", c.id, c.achievement, c.type);
                continue;
            }
            AchievementCriteriaEntry const* pCriteria = &(m_criteria[c.id] = c);
            achievement->second.criteria.push_back(pCriteria);
            m_byType[c.type].push_back(pCriteria);
            switch (c.type)
            {
                case ACHIEVEMENT_CRITERIA_KILL_CREATURE:    m_byKill.emplace(c.asset[0], pCriteria); break;
                case ACHIEVEMENT_CRITERIA_QUESTS_IN_ZONE:  m_byZone.emplace(c.asset[0], pCriteria); break;
                case ACHIEVEMENT_CRITERIA_COMPLETE_QUEST:  m_byQuest.emplace(c.asset[0], pCriteria); break;
                case ACHIEVEMENT_CRITERIA_EARN_ACHIEVEMENT: m_byEarned.emplace(c.asset[0], pCriteria); break;
                case ACHIEVEMENT_CRITERIA_EXPLORE_AREA:
                    for (uint32 area : c.asset)
                        if (area)
                            m_byArea.emplace(area, pCriteria);
                    break;
                default:
                    break;
            }
        } while (result->NextRow());
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u achievements, %u criteria", uint32(m_achievements.size()), uint32(m_criteria.size()));
}

AchievementEntry const* AchievementMgr::GetAchievement(uint32 id) const
{
    auto itr = m_achievements.find(id);
    return itr != m_achievements.end() ? &itr->second : nullptr;
}

// --------------------------------------------------------------- character --

AchievementMgr::CharacterData& AchievementMgr::GetData(Player* pPlayer)
{
    uint32 const guid = pPlayer->GetGUIDLow();
    auto itr = m_characters.find(guid);
    if (itr != m_characters.end())
        return itr->second;

    CharacterData& data = m_characters[guid];
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT `achievement`, `date` FROM `character_achievement` WHERE `guid` = %u", guid))
    {
        do
        {
            Field* fields = result->Fetch();
            data.earned[fields[0].GetUInt32()] = fields[1].GetUInt32();
        } while (result->NextRow());
    }
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT `criteria`, `counter` FROM `character_achievement_progress` WHERE `guid` = %u", guid))
    {
        do
        {
            Field* fields = result->Fetch();
            data.progress[fields[0].GetUInt32()] = fields[1].GetUInt32();
        } while (result->NextRow());
    }
    return data;
}

bool AchievementMgr::IsAvailable(Player const* pPlayer, AchievementEntry const* pAchievement) const
{
    switch (pAchievement->faction)
    {
        case 1:  return pPlayer->GetTeam() == ALLIANCE;
        case 2:  return pPlayer->GetTeam() == HORDE;
        default: return true;
    }
}

bool AchievementMgr::IsEarned(CharacterData const& data, uint32 achievementId) const
{
    return data.earned.find(achievementId) != data.earned.end();
}

bool AchievementMgr::IsExplored(Player const* pPlayer, uint32 areaId)
{
    AreaEntry const* pArea = AreaEntry::GetById(areaId);
    if (!pArea)
        return false;
    uint32 const offset = pArea->ExploreFlag / 32;
    if (offset >= PLAYER_EXPLORED_ZONES_SIZE)
        return false;
    return pPlayer->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset) & (1u << (pArea->ExploreFlag % 32));
}

void AchievementMgr::CountQuests(Player* pPlayer, uint32& total, std::unordered_map<uint32, uint32>& byZone) const
{
    total = 0;
    for (auto const& itr : pPlayer->GetQuestStatusMap())
    {
        if (!itr.second.m_rewarded || itr.second.uState == QUEST_DELETED)
            continue;
        ++total;
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr.first);
        if (!pQuest || pQuest->GetZoneOrSort() <= 0)
            continue;
        if (AreaEntry const* pArea = AreaEntry::GetById(pQuest->GetZoneOrSort()))
            ++byZone[pArea->ZoneId ? pArea->ZoneId : pArea->Id];
    }
}

uint32 AchievementMgr::Recount(Player* pPlayer, CharacterData& data, bool announce)
{
    size_t const before = data.earned.size();

    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_REACH_LEVEL])
        SetProgress(pPlayer, data, pCriteria, pPlayer->GetLevel(), announce);

    uint32 total;
    std::unordered_map<uint32, uint32> byZone;
    CountQuests(pPlayer, total, byZone);
    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_QUEST_COUNT])
        SetProgress(pPlayer, data, pCriteria, total, announce);
    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_QUESTS_IN_ZONE])
    {
        auto itr = byZone.find(pCriteria->asset[0]);
        SetProgress(pPlayer, data, pCriteria, itr != byZone.end() ? itr->second : 0, announce);
    }
    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_COMPLETE_QUEST])
        SetProgress(pPlayer, data, pCriteria, pPlayer->GetQuestRewardStatus(pCriteria->asset[0]) ? 1 : 0, announce);

    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_EXPLORE_AREA])
    {
        bool explored = false;
        for (uint32 area : pCriteria->asset)
            if (area && IsExplored(pPlayer, area))
                explored = true;
        SetProgress(pPlayer, data, pCriteria, explored ? 1 : 0, announce);
    }

    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_EARN_ACHIEVEMENT])
        SetProgress(pPlayer, data, pCriteria, IsEarned(data, pCriteria->asset[0]) ? 1 : 0, announce);

    // Definitions may have changed since the progress was kept.
    for (auto const& itr : m_achievements)
        Check(pPlayer, data, &itr.second, announce);

    return uint32(data.earned.size() - before);
}

void AchievementMgr::SetProgress(Player* pPlayer, CharacterData& data, AchievementCriteriaEntry const* pCriteria, uint32 counter, bool announce)
{
    AchievementEntry const* pAchievement = GetAchievement(pCriteria->achievement);
    if (!pAchievement || IsEarned(data, pAchievement->id) || !IsAvailable(pPlayer, pAchievement))
        return;

    counter = std::min(counter, pCriteria->value);
    auto itr = data.progress.find(pCriteria->id);
    uint32 const old = itr != data.progress.end() ? itr->second : 0;
    if (counter == old)
        return;
    data.progress[pCriteria->id] = counter;

    if (pCriteria->type == ACHIEVEMENT_CRITERIA_KILL_CREATURE)
        CharacterDatabase.PExecute("REPLACE INTO `character_achievement_progress` (`guid`, `criteria`, `counter`) VALUES (%u, %u, %u)",
            pPlayer->GetGUIDLow(), pCriteria->id, counter);
    if (announce)
        SendAddonMessage(pPlayer, "P " + std::to_string(pCriteria->id) + ":" + std::to_string(counter));

    Check(pPlayer, data, pAchievement, announce);
}

void AchievementMgr::Check(Player* pPlayer, CharacterData& data, AchievementEntry const* pAchievement, bool announce)
{
    if (pAchievement->criteria.empty() || IsEarned(data, pAchievement->id) || !IsAvailable(pPlayer, pAchievement))
        return;

    uint32 done = 0;
    for (auto pCriteria : pAchievement->criteria)
    {
        auto itr = data.progress.find(pCriteria->id);
        if (itr != data.progress.end() && itr->second >= pCriteria->value)
            ++done;
    }
    uint32 const needed = pAchievement->requiredCount ? pAchievement->requiredCount : uint32(pAchievement->criteria.size());
    if (done >= needed)
        Earn(pPlayer, data, pAchievement, announce);
}

void AchievementMgr::Earn(Player* pPlayer, CharacterData& data, AchievementEntry const* pAchievement, bool announce)
{
    uint32 const now = uint32(time(nullptr));
    data.earned[pAchievement->id] = now;
    CharacterDatabase.PExecute("REPLACE INTO `character_achievement` (`guid`, `achievement`, `date`) VALUES (%u, %u, %u)",
        pPlayer->GetGUIDLow(), pAchievement->id, now);

    if (announce)
    {
        SendAddonMessage(pPlayer, "N " + std::to_string(pAchievement->id) + ":" + std::to_string(now));
        bool const zh = IsZh(pPlayer->GetSession());
        std::string const& name = zh && !pAchievement->nameZh.empty() ? pAchievement->nameZh : pAchievement->name;
        ChatHandler(pPlayer).PSendSysMessage(zh ? "你获得了成就|cffffff00[%s]|r！" : "You have earned the achievement |cffffff00[%s]|r!", name.c_str());
    }

    auto range = m_byEarned.equal_range(pAchievement->id);
    for (auto itr = range.first; itr != range.second; ++itr)
        SetProgress(pPlayer, data, itr->second, 1, announce);
}

void AchievementMgr::SendAddonMessage(Player* pPlayer, std::string const& text)
{
    if (!pPlayer->GetSession())
        return;
    std::string const message = std::string(ADDON_PREFIX) + "\t" + text;
    // A whisper from the player to themselves: the client fires
    // CHAT_MSG_ADDON once the sender's name resolves, and theirs always does.
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, message.c_str(), LANG_ADDON, CHAT_TAG_NONE, pPlayer->GetObjectGuid());
    pPlayer->GetSession()->SendPacket(&data);
}

// ------------------------------------------------------------------- hooks --

void AchievementMgr::OnLogin(Player* pPlayer)
{
    if (pPlayer->IsBot())
        return;
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    m_characters.erase(pPlayer->GetGUIDLow());
    CharacterData& data = GetData(pPlayer);
    uint32 const gained = Recount(pPlayer, data, false);
    if (gained)
    {
        bool const zh = IsZh(pPlayer->GetSession());
        ChatHandler(pPlayer).PSendSysMessage(zh ? "按你已经完成的内容，补发了 %u 个成就。" : "%u achievement(s) granted for what you had already done.", gained);
    }
}

void AchievementMgr::OnLogout(Player* pPlayer)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    m_characters.erase(pPlayer->GetGUIDLow());
}

void AchievementMgr::OnLevelUp(Player* pPlayer)
{
    if (pPlayer->IsBot() || m_byType[ACHIEVEMENT_CRITERIA_REACH_LEVEL].empty())
        return;
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);
    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_REACH_LEVEL])
        SetProgress(pPlayer, data, pCriteria, pPlayer->GetLevel(), true);
}

void AchievementMgr::OnQuestRewarded(Player* pPlayer, Quest const* pQuest)
{
    if (pPlayer->IsBot())
        return;
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);

    uint32 total;
    std::unordered_map<uint32, uint32> byZone;
    CountQuests(pPlayer, total, byZone);
    for (auto pCriteria : m_byType[ACHIEVEMENT_CRITERIA_QUEST_COUNT])
        SetProgress(pPlayer, data, pCriteria, total, true);
    for (auto const& zone : byZone)
    {
        auto range = m_byZone.equal_range(zone.first);
        for (auto itr = range.first; itr != range.second; ++itr)
            SetProgress(pPlayer, data, itr->second, zone.second, true);
    }
    auto range = m_byQuest.equal_range(pQuest->GetQuestId());
    for (auto itr = range.first; itr != range.second; ++itr)
        SetProgress(pPlayer, data, itr->second, 1, true);
}

void AchievementMgr::OnAreaExplored(Player* pPlayer, uint32 areaId)
{
    if (pPlayer->IsBot() || m_byArea.find(areaId) == m_byArea.end())
        return;
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);
    auto range = m_byArea.equal_range(areaId);
    for (auto itr = range.first; itr != range.second; ++itr)
        SetProgress(pPlayer, data, itr->second, 1, true);
}

void AchievementMgr::OnCreatureKilled(Creature* pVictim, Player* pTapper, Group* pGroupTap)
{
    auto range = m_byKill.equal_range(pVictim->GetEntry());
    if (range.first == range.second)
        return;

    std::vector<Player*> players;
    if (pGroupTap)
    {
        for (GroupReference* itr = pGroupTap->GetFirstMember(); itr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (pMember && pMember->IsInWorld() && pMember->IsInMap(pVictim))
                players.push_back(pMember);
        }
    }
    else if (pTapper && pTapper->IsInMap(pVictim))
        players.push_back(pTapper);

    std::lock_guard<std::recursive_mutex> guard(m_lock);
    for (Player* pPlayer : players)
    {
        if (pPlayer->IsBot())
            continue;
        CharacterData& data = GetData(pPlayer);
        for (auto itr = range.first; itr != range.second; ++itr)
        {
            auto progress = data.progress.find(itr->second->id);
            SetProgress(pPlayer, data, itr->second, (progress != data.progress.end() ? progress->second : 0) + 1, true);
        }
    }
}

void AchievementMgr::DeleteCharacter(uint32 guidLow)
{
    CharacterDatabase.PExecute("DELETE FROM `character_achievement` WHERE `guid` = %u", guidLow);
    CharacterDatabase.PExecute("DELETE FROM `character_achievement_progress` WHERE `guid` = %u", guidLow);
}

// ------------------------------------------------------------ addon and GM --

// The addon's full state:
//   S                     start: forget what you know
//   E id:time,...         earned achievements, unix time
//   P criteria:counter,.. progress of the achievements not earned yet
//   D                     done
// and as things happen: "P criteria:counter", "N id:time" (newly earned).
void AchievementMgr::SendSync(Player* pPlayer)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);

    std::vector<std::string> earned;
    for (auto const& itr : data.earned)
        earned.push_back(std::to_string(itr.first) + ":" + std::to_string(itr.second));
    std::vector<std::string> progress;
    for (auto const& itr : data.progress)
    {
        auto pCriteria = m_criteria.find(itr.first);
        if (!itr.second || pCriteria == m_criteria.end() || IsEarned(data, pCriteria->second.achievement))
            continue;
        progress.push_back(std::to_string(itr.first) + ":" + std::to_string(itr.second));
    }

    auto send = [pPlayer](std::string const& text) { SendAddonMessage(pPlayer, text); };
    send("S");
    SendChunked('E', earned, send);
    SendChunked('P', progress, send);
    send("D");
}

bool AchievementMgr::Grant(Player* pPlayer, uint32 achievementId)
{
    AchievementEntry const* pAchievement = GetAchievement(achievementId);
    if (!pAchievement)
        return false;
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);
    if (!IsEarned(data, achievementId))
        Earn(pPlayer, data, pAchievement, true);
    return true;
}

bool AchievementMgr::Revoke(Player* pPlayer, uint32 achievementId)
{
    AchievementEntry const* pAchievement = GetAchievement(achievementId);
    if (!pAchievement)
        return false;
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);
    uint32 const guid = pPlayer->GetGUIDLow();
    data.earned.erase(achievementId);
    CharacterDatabase.PExecute("DELETE FROM `character_achievement` WHERE `guid` = %u AND `achievement` = %u", guid, achievementId);
    for (auto pCriteria : pAchievement->criteria)
    {
        data.progress.erase(pCriteria->id);
        CharacterDatabase.PExecute("DELETE FROM `character_achievement_progress` WHERE `guid` = %u AND `criteria` = %u", guid, pCriteria->id);
    }
    // What the character still holds (a level, an explored zone) earns it back.
    Recount(pPlayer, data, false);
    SendSync(pPlayer);
    return true;
}

void AchievementMgr::Reset(Player* pPlayer)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    DeleteCharacter(pPlayer->GetGUIDLow());
    CharacterData& data = m_characters[pPlayer->GetGUIDLow()];
    data = CharacterData();
    Recount(pPlayer, data, false);
    SendSync(pPlayer);
}

void AchievementMgr::GetSummary(Player* pPlayer, uint32& earned, uint32& total, uint32& points, uint32& totalPoints)
{
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    CharacterData& data = GetData(pPlayer);
    earned = total = points = totalPoints = 0;
    for (auto const& itr : m_achievements)
    {
        if (!IsAvailable(pPlayer, &itr.second))
            continue;
        ++total;
        totalPoints += itr.second.points;
        if (IsEarned(data, itr.first))
        {
            ++earned;
            points += itr.second.points;
        }
    }
}

bool ChatHandler::HandleAchievementCommand(char* /*args*/)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    uint32 earned, total, points, totalPoints;
    sAchievementMgr.GetSummary(pPlayer, earned, total, points, totalPoints);
    PSendSysMessage(IsZh(m_session) ? "成就：%u / %u，成就点数：%u / %u。" : "Achievements: %u / %u, points: %u / %u.",
        earned, total, points, totalPoints);
    return true;
}

bool ChatHandler::HandleAchievementSyncCommand(char* /*args*/)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    sAchievementMgr.SendSync(pPlayer);
    return true;
}

// `.achievement add|remove <id>` and `.achievement reset`, on the selected
// player (yourself when nobody is selected).
bool ChatHandler::HandleAchievementAddCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    uint32 id;
    if (!pTarget || !ExtractUInt32(&args, id))
        return false;
    if (!sAchievementMgr.Grant(pTarget, id))
    {
        PSendSysMessage("No achievement %u.", id);
        SetSentErrorMessage(true);
        return false;
    }
    PSendSysMessage("Achievement %u granted to %s.", id, pTarget->GetName());
    return true;
}

bool ChatHandler::HandleAchievementRemoveCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    uint32 id;
    if (!pTarget || !ExtractUInt32(&args, id))
        return false;
    if (!sAchievementMgr.Revoke(pTarget, id))
    {
        PSendSysMessage("No achievement %u.", id);
        SetSentErrorMessage(true);
        return false;
    }
    PSendSysMessage("Achievement %u taken from %s (the ones the character still holds come back).", id, pTarget->GetName());
    return true;
}

bool ChatHandler::HandleAchievementResetCommand(char* /*args*/)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
        return false;
    sAchievementMgr.Reset(pTarget);
    PSendSysMessage("Achievements of %s counted again from the character.", pTarget->GetName());
    return true;
}
