/*
 * Achievements, after the WoW 3.x ones that the 1.12 world holds.
 *
 * Definitions live in the world tables `achievement_template` and
 * `achievement_criteria` (generated with the Achievements addon's data by
 * tools/achievements/gen-achievements.py). A character keeps the
 * achievements it earned in `character_achievement` and the progress that
 * cannot be read back from the character, boss kills, in
 * `character_achievement_progress`. Everything else (level, quests,
 * exploration, other achievements) is counted again from the character at
 * login, so characters older than the system get what they already did.
 *
 * 1.12 has no achievement packets: the Achievements addon asks with
 * `.achievement sync` and the server answers with addon messages (prefix
 * "SoloAch", see SendSync), and pushes progress and new achievements as
 * they happen. Earning one also prints a line in the chat frame.
 *
 * The hooks run on the player's map thread, login and commands on the world
 * thread; a mutex keeps the per-character state.
 */

#ifndef MANGOS_ACHIEVEMENT_MGR_H
#define MANGOS_ACHIEVEMENT_MGR_H

#include "Common.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class Creature;
class Group;
class Quest;

enum AchievementCriteriaType : uint8
{
    ACHIEVEMENT_CRITERIA_REACH_LEVEL      = 1,    // value: level
    ACHIEVEMENT_CRITERIA_QUEST_COUNT      = 2,    // value: quests completed
    ACHIEVEMENT_CRITERIA_QUESTS_IN_ZONE   = 3,    // asset1: zone, value: quests completed there
    ACHIEVEMENT_CRITERIA_EXPLORE_AREA     = 4,    // asset1..4: areas, any of them explored
    ACHIEVEMENT_CRITERIA_KILL_CREATURE    = 5,    // asset1: creature entry, value: kills
    ACHIEVEMENT_CRITERIA_EARN_ACHIEVEMENT = 6,    // asset1: achievement
    ACHIEVEMENT_CRITERIA_COMPLETE_QUEST   = 7,    // asset1: quest
};

struct AchievementCriteriaEntry
{
    uint32 id;
    uint32 achievement;
    uint8 type;
    uint32 asset[4];
    uint32 value;
};

struct AchievementEntry
{
    uint32 id;
    uint16 category;
    uint8 faction;                  // 0 both, 1 Alliance, 2 Horde
    uint8 points;
    uint8 requiredCount;            // 0 = every criteria
    std::string name;
    std::string nameZh;
    std::vector<AchievementCriteriaEntry const*> criteria;
};

class AchievementMgr
{
public:
    static AchievementMgr& Instance();

    void Load();

    void OnLogin(Player* pPlayer);
    void OnLogout(Player* pPlayer);
    void OnLevelUp(Player* pPlayer);
    void OnQuestRewarded(Player* pPlayer, Quest const* pQuest);
    void OnAreaExplored(Player* pPlayer, uint32 areaId);
    // Credits the players who share the kill: the tapping group's members on
    // the creature's map, else the tapping player.
    void OnCreatureKilled(Creature* pVictim, Player* pTapper, Group* pGroupTap);
    static void DeleteCharacter(uint32 guidLow);

    // The full state for the addon, as addon messages.
    void SendSync(Player* pPlayer);
    // GM: grant or take back one achievement, or forget everything and count
    // again from the character. Return false for an unknown achievement.
    bool Grant(Player* pPlayer, uint32 achievementId);
    bool Revoke(Player* pPlayer, uint32 achievementId);
    void Reset(Player* pPlayer);

    AchievementEntry const* GetAchievement(uint32 id) const;
    void GetSummary(Player* pPlayer, uint32& earned, uint32& total, uint32& points, uint32& totalPoints);

private:
    struct CharacterData
    {
        std::unordered_map<uint32, uint32> progress;   // criteria -> counter
        std::unordered_map<uint32, uint32> earned;     // achievement -> unix time
    };

    CharacterData& GetData(Player* pPlayer);
    bool IsAvailable(Player const* pPlayer, AchievementEntry const* pAchievement) const;
    bool IsEarned(CharacterData const& data, uint32 achievementId) const;
    // Recounts what can be read from the character (level, quests,
    // exploration, earned achievements); `announce` false earns silently.
    uint32 Recount(Player* pPlayer, CharacterData& data, bool announce);
    void SetProgress(Player* pPlayer, CharacterData& data, AchievementCriteriaEntry const* pCriteria, uint32 counter, bool announce);
    void Check(Player* pPlayer, CharacterData& data, AchievementEntry const* pAchievement, bool announce);
    void Earn(Player* pPlayer, CharacterData& data, AchievementEntry const* pAchievement, bool announce);
    void CountQuests(Player* pPlayer, uint32& total, std::unordered_map<uint32, uint32>& byZone) const;
    static bool IsExplored(Player const* pPlayer, uint32 areaId);
    static void SendAddonMessage(Player* pPlayer, std::string const& text);

    std::unordered_map<uint32, AchievementEntry> m_achievements;
    std::unordered_map<uint32, AchievementCriteriaEntry> m_criteria;
    std::vector<AchievementCriteriaEntry const*> m_byType[8];
    std::unordered_multimap<uint32, AchievementCriteriaEntry const*> m_byKill;      // creature entry
    std::unordered_multimap<uint32, AchievementCriteriaEntry const*> m_byArea;      // area
    std::unordered_multimap<uint32, AchievementCriteriaEntry const*> m_byZone;      // quests in zone
    std::unordered_multimap<uint32, AchievementCriteriaEntry const*> m_byQuest;     // complete quest
    std::unordered_multimap<uint32, AchievementCriteriaEntry const*> m_byEarned;    // achievement

    std::unordered_map<uint32, CharacterData> m_characters;                        // by character guid
    std::recursive_mutex m_lock;
};

#define sAchievementMgr AchievementMgr::Instance()

#endif
