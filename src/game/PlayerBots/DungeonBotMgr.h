/*
 * Fills a player's party with party bots when they enter a five-player
 * dungeon, and sends the bots home when they leave it alive.
 *
 * Tracks the last "safe" spot the player walked through (outside hostile
 * aggro radii). After a full party wipe and a short wait for mobs to evade,
 * teleports the player and same-map bots there and resurrects them at half
 * health, so standing up in the pack does not re-pull.
 *
 * The manager polls the online players from the world thread (it runs from
 * World::Update next to sPlayerBotMgr), so no teleport or map code needs a
 * hook and nothing touches a map from a map thread.
 *
 * Each character keeps settings in `character_party_bots`: the role the
 * player plays (auto = guessed from class and talents), whether dungeons
 * fill on their own, and engage style (calm = wait until mobs are close,
 * aggressive = charge in). `.bots` (open to every account) changes them,
 * fills a duo, a trio or a party of five, or dismisses by hand. The Bots
 * addon drives it. Raids are 20 or 40 and are not filled.
 *
 * Each roster member a character calls becomes that character's companion
 * (`character_companions`): a saved character on its own made-up account,
 * in the leader's guild, that comes back with its level, talents and gear,
 * and levels up with its leader. The roster covers every 1.12-legal
 * race × class × talent tree. A fill picks slots at random (preferring
 * classes and races the party lacks); calling a slot again reuses that
 * companion. The first character to claim a roster name keeps it; later
 * ones get a generated name.
 */

#ifndef MANGOS_DUNGEON_BOT_MGR_H
#define MANGOS_DUNGEON_BOT_MGR_H

#include "Common.h"
#include "SharedDefines.h"
#include <map>
#include <string>
#include <unordered_map>

class Player;

enum PartyBotEngageStyle : uint8
{
    PARTYBOT_ENGAGE_CALM = 0,        // Wait until the pack is near the party
    PARTYBOT_ENGAGE_AGGRESSIVE = 1,  // Charge / open as soon as there is a target
};

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
    PartyBotEngageStyle GetEngageStyle(Player* pPlayer);
    void SetEngageStyle(Player* pPlayer, PartyBotEngageStyle style);
    static char const* EngageStyleName(PartyBotEngageStyle style, bool zh);

    // Adds bots for the missing roles until the party has `target` members
    // (2, 3 or 5, counting the player), or sends extras home until it does.
    // Picked at random from the fixed roster. Returns how many were added;
    // `who` lists them, `why` says why none were when it returns 0, and
    // `removed` counts bots sent home to shrink the party.
    uint32 Fill(Player* pPlayer, std::string& who, std::string& why, uint32 target = 5, uint32* removed = nullptr);
    // Sends away the bots this player leads: only those filled in a dungeon, or all.
    // Always clears their m_autoBots entries so a later fill is not short a seat.
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
        PartyBotEngageStyle engage = PARTYBOT_ENGAGE_CALM;
    };
    struct Watch
    {
        uint32 lastMap = UINT32_MAX;
        uint32 fillAt = 0;          // world ms; 0 = no fill pending
        uint32 repairAt = 0;        // world ms; next allowed short-party repair
        // Last spot outside hostile aggro range in this dungeon instance.
        bool hasSafe = false;
        uint32 safeMap = 0;
        uint32 safeInstance = 0;
        float safeX = 0.f;
        float safeY = 0.f;
        float safeZ = 0.f;
        float safeO = 0.f;
        uint32 wipeAt = 0;          // world ms when full wipe seen; 0 = none
    };

    void ClearSafe(Watch& w);
    void SeedSafe(Watch& w, Player* pPlayer);
    void MaybeUpdateSafe(Watch& w, Player* pPlayer);
    bool IsSafeSpot(Player* pPlayer) const;
    bool PartyWipedOnMap(Player* pPlayer) const;
    void RevivePartyAtSafe(Player* pPlayer, Watch& w);
    struct AutoBot
    {
        CombatBotRoles role = ROLE_INVALID;
        uint8 botClass = 0;
        uint8 race = 0;
        std::string name;           // roster name, or empty
        uint32 lastSeen = 0;        // world ms: spawned, or last found in the world
        bool dungeon = false;       // filled in a dungeon: goes home when the leader leaves it
    };

    Settings& GetSettings(Player* pPlayer);
    void SaveSettings(Player* pPlayer);
    void ForgetGoneBots(uint32 leaderLow);
    // Party size the leader last asked for (2/3/5). Update re-fills until it holds.
    void SetWantSize(uint32 leaderLow, uint32 size);
    void ClearWantSize(uint32 leaderLow);
    void LoadCompanions();
    uint32 GetCompanion(uint32 ownerLow, uint32 slot);
    void SetCompanion(uint32 ownerLow, uint32 slot, uint32 botLow);
    void ForgetCompanion(uint32 ownerLow, uint32 slot);

    std::unordered_map<uint32, Settings> m_settings;            // by character guid
    std::unordered_map<uint32, Watch> m_watch;                  // by character guid
    std::unordered_map<uint32, std::map<uint32, AutoBot>> m_autoBots; // leader -> bot guid -> bot
    std::unordered_map<uint32, uint32> m_wantSize;                   // leader -> last fill target
    std::unordered_map<uint32, std::map<uint32, uint32>> m_companions; // owner -> roster slot -> bot guid
    std::unordered_map<uint32, uint32> m_companionOwner;              // bot guid -> owner
    // owner -> roster slot -> world ms when the slot may be tried again
    std::unordered_map<uint32, std::map<uint32, uint32>> m_slotCooldown;
    bool m_companionsLoaded = false;
    uint32 m_timer = 0;
    uint32 m_now = 0;
};

#define sDungeonBotMgr DungeonBotMgr::Instance()

#endif
