#ifndef MANGOS_SOLOFARMAI_H
#define MANGOS_SOLOFARMAI_H

#include "PartyBotAI.h"

// Soloroth: the player's own character farming around where it was started
// (`.solo grind` / `fish` / `gather` / `all`). The server drives the body with splines, as it
// does for Charge or fear; the class rotations are the party bots'.

enum SoloFarmMode : uint8
{
    SOLO_FARM_GRIND,
    SOLO_FARM_FISH,
    SOLO_FARM_GATHER,
    SOLO_FARM_ALL,
};

enum SoloFarmStop : uint8
{
    SOLO_STOP_COMMAND,
    SOLO_STOP_MOVED,
    SOLO_STOP_DIED,
    SOLO_STOP_BAGS_FULL,
    SOLO_STOP_LEFT_AREA,
    SOLO_STOP_NO_SKILL,
    SOLO_STOP_NO_POLE,
    SOLO_STOP_NO_WATER,
    SOLO_STOP_SILENT,
};

class SoloFarmAI final : public PartyBotAI
{
public:
    SoloFarmAI(Player* pPlayer, SoloFarmMode mode, float radius);
    ~SoloFarmAI() override;

    static SoloFarmAI* Of(Player* pPlayer);
    // False, with the reason sent to the player, when it cannot start.
    static bool Start(Player* pPlayer, SoloFarmMode mode, float radius);
    static void StopFor(Player* pPlayer, SoloFarmStop reason);
    // A movement key the player pressed while the body was theirs.
    static void OnManualMove(Player* pPlayer);

    void UpdateAI(uint32 const diff) override;
    void Remove() override;

    SoloFarmMode GetMode() const { return m_mode; }
    float GetRadius() const { return m_radius; }
    char const* ModeWire() const;

private:
    void Stop(SoloFarmStop reason);
    void Say(char const* en, char const* zh) const;
    void AssignSoloRole();

    bool RunAwayFromTarget(Unit* pEnemy) override;
    bool UpdateCombat();
    bool Rest();
    bool HealSelf();
    bool UseConsumable(uint32 auraType, uint32 effect = 0);
    bool LootCorpses();
    uint32 FreeBagSlots() const;
    void Blacklist(ObjectGuid guid, uint32 ms);
    bool IsBlacklisted(ObjectGuid guid) const;

    bool DoesGrind() const { return m_mode == SOLO_FARM_GRIND || m_mode == SOLO_FARM_ALL; }
    bool DoesFish() const { return m_mode == SOLO_FARM_FISH || m_mode == SOLO_FARM_ALL; }
    bool DoesGather() const { return m_mode == SOLO_FARM_GATHER || m_mode == SOLO_FARM_ALL; }

    void UpdateGrind();
    Creature* FindTarget();
    void Wander();

    void UpdateFishing();
    bool EnsurePole();
    void RestoreWeapons();
    bool FindFishingSpot();
    bool IsFishable(float px, float py, float pz, float fx, float fy) const;
    uint32 ScoreCast(float px, float py, float pz, float o) const;
    void Reel(GameObject* pBobber);

    bool UpdateGather();
    bool TakeOpenLoot();
    Creature* FindSkinnable();
    GameObject* FindGatherNode();
    uint32 GatherLockType(GameObject const* pGo) const;
    bool HasTool(uint32 itemId) const;

    SoloFarmMode m_mode;
    float m_radius;
    uint32 m_now = 0;
    uint32 m_nextTick = 0;
    uint32 m_level = 0;
    bool m_stopped = false;
    bool m_resting = false;
    std::map<ObjectGuid, uint32> m_blacklist;

    // Grinding.
    ObjectGuid m_pullGuid;
    uint32 m_pullStart = 0;
    uint32 m_nextSearch = 0;
    uint32 m_idleUntil = 0;
    bool m_wandering = false;

    // Fishing.
    uint32 m_fishingSpell = 0;
    float m_castMin = 0.0f;
    float m_castMax = 0.0f;
    ObjectGuid m_mainHand;
    ObjectGuid m_offHand;
    bool m_poleSwapped = false;
    bool m_hasSpot = false;
    float m_spotX = 0.0f;
    float m_spotY = 0.0f;
    float m_spotZ = 0.0f;
    float m_spotO = 0.0f;
    ObjectGuid m_spotHole;
    uint32 m_moveTries = 0;
    uint32 m_castFails = 0;
    uint32 m_casts = 0;
    uint32 m_reelAt = 0;
    std::vector<std::pair<float, float>> m_badSpots;

    // Gathering (herb / mine / skin).
    uint32 m_herbSpell = 0;
    uint32 m_mineSpell = 0;
    uint32 m_skinSpell = 0;
    ObjectGuid m_gatherGuid;
    uint32 m_gatherSpell = 0;
    uint32 m_gatherFails = 0;
};

#endif
