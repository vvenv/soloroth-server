/*
 * Fills a player's party with party bots when they enter a five-player
 * dungeon, and sends the bots home when they leave it alive.
 *
 * The manager polls the online players from the world thread (it runs from
 * World::Update next to sPlayerBotMgr), so no teleport or map code needs a
 * hook and nothing touches a map from a map thread.
 *
 * Each character keeps two settings in `character_party_bots`: the role the
 * player plays (auto = guessed from class and talents) and whether dungeons
 * fill on their own. `.bots` (open to every account) changes them, fills a
 * duo, a trio or a party of five, or dismisses by hand. The Bots addon
 * drives it. Raids are 20 or 40 and are not filled.
 *
 * Each roster member a character calls becomes that character's companion
 * (`character_companions`): a saved character on its own made-up account,
 * in the leader's guild, that comes back with its level, talents and gear,
 * and levels up with its leader. The first character to call a roster
 * member gets its roster name; later ones get a generated name.
 */

#ifndef MANGOS_DUNGEON_BOT_MGR_H
#define MANGOS_DUNGEON_BOT_MGR_H

#include "Common.h"
#include "SharedDefines.h"
#include <map>
#include <string>
#include <unordered_map>

class Player;

class DungeonBotMgr
{
public:
    static DungeonBotMgr& Instance();

    void Update(uint32 diff);

    // The role the player plays: their own choice, else a guess.
    CombatBotRoles GetPlayerRole(Player* pPlayer);
    CombatBotRoles GetChosenRole(Player* pPlayer);
    void SetChosenRole(Player* pPlayer, CombatBotRoles role);
    bool IsAutoFill(Player* pPlayer);
    void SetAutoFill(Player* pPlayer, bool on);

    // Adds bots for the missing roles until the party has `target` members
    // (2, 3 or 5, counting the player), or sends extras home until it does.
    // Picked from a fixed roster of companions. Returns how many were added;
    // `who` lists them, `why` says why none were when it returns 0, and
    // `removed` counts bots sent home to shrink the party.
    uint32 Fill(Player* pPlayer, std::string& who, std::string& why, uint32 target = 5, uint32* removed = nullptr);
    // Sends away the bots this player leads: only those filled in a dungeon, or all.
    uint32 Dismiss(Player* pPlayer, bool autoOnly);

    static CombatBotRoles GuessRole(Player const* pPlayer);
    static char const* RoleName(CombatBotRoles role, bool zh);

    bool IsCompanion(uint32 botLow);
    // Deletes the characters of an owner's companions (the owner is being deleted).
    void DeleteCompanions(uint32 ownerLow);

private:
    struct Settings
    {
        CombatBotRoles role = ROLE_INVALID;
        bool autoFill = true;
    };
    struct Watch
    {
        uint32 lastMap = UINT32_MAX;
        uint32 fillAt = 0;          // world ms; 0 = no fill pending
    };
    struct AutoBot
    {
        CombatBotRoles role = ROLE_INVALID;
        uint8 botClass = 0;
        std::string name;           // roster name, or empty
        uint32 lastSeen = 0;        // world ms: spawned, or last found in the world
        bool dungeon = false;       // filled in a dungeon: goes home when the leader leaves it
    };

    Settings& GetSettings(Player* pPlayer);
    void SaveSettings(Player* pPlayer);
    void ForgetGoneBots(uint32 leaderLow);
    void LoadCompanions();
    uint32 GetCompanion(uint32 ownerLow, uint32 slot);
    void SetCompanion(uint32 ownerLow, uint32 slot, uint32 botLow);
    void ForgetCompanion(uint32 ownerLow, uint32 slot);

    std::unordered_map<uint32, Settings> m_settings;            // by character guid
    std::unordered_map<uint32, Watch> m_watch;                  // by character guid
    std::unordered_map<uint32, std::map<uint32, AutoBot>> m_autoBots; // leader -> bot guid -> bot
    std::unordered_map<uint32, std::map<uint32, uint32>> m_companions; // owner -> roster slot -> bot guid
    std::unordered_map<uint32, uint32> m_companionOwner;              // bot guid -> owner
    bool m_companionsLoaded = false;
    uint32 m_timer = 0;
    uint32 m_now = 0;
};

#define sDungeonBotMgr DungeonBotMgr::Instance()

#endif
