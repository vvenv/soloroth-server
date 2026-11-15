#ifndef MANGOS_SOLOFARMAI_H
#define MANGOS_SOLOFARMAI_H

#include "PartyBotAI.h"

// Soloroth: the player's own character farming around where it was started
// (`.solo grind` / `fish` / `gather` / `all`), or fighting while they steer
// (`.solo fight`). The server drives the body with splines, as it does for
// Charge or fear; the class rotations are the party bots'. Fight mode never
// picks a destination: the player walks, and a spline is only the chase.

enum SoloFarmMode : uint8
{
    SOLO_FARM_GRIND,
    SOLO_FARM_FISH,
    SOLO_FARM_GATHER,
    SOLO_FARM_ALL,
    SOLO_FARM_FIGHT,
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
    // Alone, the mob turns to face us: circling for its back only drags us past its friends.
    bool ChaseFromBehind(Unit* /*pVictim*/) const override { return false; }
    bool CanOutpaceAttackers() const;
    std::vector<SpellEntry const*> RunnerStopSpells(Unit const* pVictim) const;
    bool StopRunner(Unit* pVictim);
    bool UpdateCombat();
    bool Rest();
    void BeginRest();
    bool HealSelf();
    bool UseConsumable(uint32 auraType, uint32 effect = 0);
    bool Conjure();
    bool LootCorpses();
    uint32 FreeBagSlots() const;
    void Blacklist(ObjectGuid guid, uint32 ms);
    bool IsBlacklisted(ObjectGuid guid) const;

    bool DoesGrind() const { return m_mode == SOLO_FARM_GRIND || m_mode == SOLO_FARM_ALL; }
    bool DoesFish() const { return m_mode == SOLO_FARM_FISH || m_mode == SOLO_FARM_ALL; }
    bool DoesGather() const { return m_mode == SOLO_FARM_GATHER || m_mode == SOLO_FARM_ALL; }
    // The player keeps the body between pulls. A movement key must not end the mode.
    bool PlayerSteers() const { return m_mode == SOLO_FARM_FIGHT; }
    // Ground objects that exist for a quest. Off for a character with no quest log.
    bool DoesQuestObjects() const
    {
        return (m_mode == SOLO_FARM_GRIND || m_mode == SOLO_FARM_GATHER || m_mode == SOLO_FARM_ALL) &&
            !me->IsNoQuest();
    }
    // Multi-pull, or a 1v1 window to kite / bandage / potion / self-heal.
    bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const override;
    bool NeedsCrowdControlWindow() const;
    // Mage sheep only hits a non-victim add; in 1v1 emergencies CC the kill target instead.
    bool TryEmergencyCrowdControl(Unit* pVictim);

    // An idle mob that attacks on sight, and how close we may pass the middle of its roam.
    struct Threat
    {
        Creature* pCreature;
        float x, y, z;
        float reach;
    };
    std::vector<Threat> FindThreats() const;
    // Mobs a walk to (x, y, z) wakes: threats along the path, stopping `stopShort` from the end,
    // plus those that assist pTarget once it is hit. NO_PATH when unreachable or too long.
    uint32 CountAdds(std::vector<Threat> const& threats, float x, float y, float z,
        Creature* pTarget = nullptr, float stopShort = 0.0f) const;
    static uint32 const NO_PATH = 0xFFFFFFFF;
    uint32 AllowedAdds() const;
    // Nearest first, the fewest adds within `allowed`; `fight` counts the target's assisters.
    WorldObject* PickApproach(std::vector<std::pair<float, WorldObject*>>& picks, uint32 allowed,
        float stopShort, bool fight, uint32* pAdds = nullptr);

    void UpdateFight();
    // Hand the body back once nothing is being fought, so client movement applies again.
    // `force` clears a chase even when a victim is still set (the player just died).
    void ReleaseBody(bool force = false);
    // The mob the player has targeted, when they are standing still within pull range.
    bool TryAssistPull();
    void UpdateGrind();
    // `adds`: the extra mobs the returned pull is expected to bring.
    Creature* FindTarget(uint32& adds);
    void Wander();

    void UpdateFishing();
    bool EnsurePole();
    void RestoreWeapons();
    bool FindFishingSpot();
    bool IsFishable(float px, float py, float pz, float fx, float fy) const;
    uint32 ScoreCast(float px, float py, float pz, float o) const;
    void Reel(GameObject* pBobber);

    bool UpdateGather();
    bool UpdateQuestObjects();
    bool TakeOpenLoot();
    Creature* FindSkinnable();
    GameObject* FindGatherNode();
    GameObject* FindQuestObject();
    bool IsQuestInteractGo(GameObject const* pGo) const;
    bool CanOpenQuestChest(GameObject const* pGo) const;
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
    bool m_castPending = false;
    std::vector<std::pair<float, float>> m_badSpots;

    // Gathering (herb / mine / skin).
    uint32 m_herbSpell = 0;
    uint32 m_mineSpell = 0;
    uint32 m_skinSpell = 0;
    ObjectGuid m_gatherGuid;
    uint32 m_gatherSpell = 0;
    uint32 m_gatherFails = 0;

    // Quest goobers / chests (walk up and Use / SoloLootGameObject).
    ObjectGuid m_questGuid;
    uint32 m_questFails = 0;
};

#endif
