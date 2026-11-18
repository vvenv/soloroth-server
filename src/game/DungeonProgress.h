/*
 * Per-character dungeon and raid progress for the Dungeon addon.
 *
 * 1.12 has no encounter journal. The server remembers, for the instance the
 * character is bound to, which bosses are down, and how many times the
 * wing's last boss has been killed. The addon asks with ".dungeon progress"
 * and the answer is addon messages, prefix "SoloDun":
 *
 *   S                  start
 *   H clears\tname     this wing, and how many times it has been cleared
 *   K 0|1\tname        one boss, in order; 1 = down
 *   D                  show
 *   X                  not in a tracked instance; hide
 *
 * A clear is one kill of that wing's last boss. A kill the achievements
 * already recorded counts as one, once. Resetting the instance starts a new
 * boss list; the clear count stays.
 */

#ifndef MANGOS_DUNGEON_PROGRESS_H
#define MANGOS_DUNGEON_PROGRESS_H

#include "Common.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

class Player;
class Creature;
class Group;
class Map;

class DungeonProgress
{
public:
    static DungeonProgress& Instance();

    void Load();

    // Credits the same players as achievements: the tapping group's members
    // on the creature's map, else the tapping player. Bots are skipped.
    void OnCreatureKilled(Creature* pVictim, Player* pTapper, Group* pGroupTap);

    // Snapshot for the addon. Backfills a clear from achievements or from a
    // last boss that is already down in this instance.
    void Send(Player* pPlayer);

    static void DeleteCharacter(uint32 guidLow);

private:
    struct Boss
    {
        uint32 map;
        uint32 wing;
        uint32 entry;
        bool finalBoss;
    };

    struct Wing
    {
        uint32 map;
        uint32 wing;
        std::string name;
        std::string nameZh;
        float ax = 0, ay = 0, az = 0;
        uint32 anchors = 0;
        std::vector<Boss const*> bosses;
        Boss const* finalBoss = nullptr;
    };

    Wing const* WingFor(Player* pPlayer) const;
    bool SpawnDown(Map* pMap, uint32 lowguid) const;
    std::unordered_set<uint32> LoadDown(uint32 guid, uint32 instanceId) const;
    void Remember(uint32 guid, uint32 instanceId, uint32 entry);
    // Returns the stored count. `exists` is false when the row is absent.
    uint32 LoadClears(uint32 guid, Wing const* pWing, bool& exists) const;
    void SaveClears(uint32 guid, Wing const* pWing, uint32 clears);
    void NoteKill(Player* pPlayer, Boss const* pBoss, uint32 instanceId);
    void Push(Player* pPlayer, bool backfill);

    std::string BossName(uint32 entry, int loc) const;
    std::string WingName(Wing const* pWing, int loc) const;
    static void SendAddon(Player* pPlayer, std::string const& text);
    static int Locale(Player* pPlayer);

    std::vector<Boss> m_bosses;
    std::vector<Wing> m_wings;
    std::unordered_map<uint32, Boss const*> m_byEntry;
    std::unordered_map<uint32, std::vector<uint32>> m_spawns; // entry -> creature guid
    std::mutex m_lock;
};

#define sDungeonProgress DungeonProgress::Instance()

#endif
