/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef MANGOS_PARTYBOTAI_H
#define MANGOS_PARTYBOTAI_H

#include "CombatBotBaseAI.h"
#include "Group.h"
#include "ObjectAccessor.h"

class PartyBotAI : public CombatBotBaseAI
{
public:

    PartyBotAI(Player* pLeader, Player* pClone, CombatBotRoles role, uint8 race, uint8 class_, uint8 level, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : CombatBotBaseAI(), m_race(race), m_class(class_), m_level(level), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o)
    {
        m_role = role;
        m_leaderGuid = pLeader->GetObjectGuid();
        m_cloneGuid = pClone ? pClone->GetObjectGuid() : ObjectGuid();
        m_updateTimer.Reset(2000);
    }
    PartyBotAI(Player* pLeader, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : CombatBotBaseAI(), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o)
    {
        m_role = ROLE_INVALID;
        m_leaderGuid = pLeader->GetObjectGuid();
        m_updateTimer.Reset(2000);
    }

    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) final;
    void OnPlayerLogin() final;
    void UpdateAI(uint32 const diff) override;
    void OnPacketReceived(WorldPacket const* packet) final;

    void CloneFromPlayer(Player const* pPlayer);
    void AddToPlayerGroup();
    void LevelUpTo(uint32 level);

    bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const override;
    Player* GetPartyLeader() const;
    bool AttackStart(Unit* pVictim);
    // .partybot pull: score for who should open from range (hunter / mage / throw).
    int GetRangedPullScore(Unit const* pTarget) const;
    bool HasThrowableWeapon() const;
    bool EnsureThrowableWeapon();
    bool TryThrowPull(Unit* pTarget);
    // Tank waits until the mob is nearly at the ranged puller, then engages.
    void BeginRangedPullHold(ObjectGuid targetGuid, ObjectGuid pullerGuid, uint32 durationMs);
    void ClearRangedPullHold();
    bool UpdateRangedPullHold();
    Unit* SelectAttackTarget(Player* pLeader) const;
    Unit* SelectPartyAttackTarget() const;
    Player* SelectResurrectionTarget() const;
    bool MoveToResurrect();
    bool AllyWillResurrect() const;
    Player* SelectShieldTarget() const;
    // Equipped wand plus Shoot (5019): healer idle filler, and OOM/last-resort for casters.
    bool HasUsableWand() const;
    bool TryShootWand(Unit* pVictim);
    Unit* GetMarkedTarget(RaidTargetIcon mark) const;
    virtual bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const;
    bool DrinkAndEat();
    bool IsValidDistancingTarget(Unit* pTarget, Unit* pEnemy);
    Unit* GetDistancingTarget(Unit* pEnemy);
    virtual bool RunAwayFromTarget(Unit* pEnemy);
    // Polymorph / hibernate / blind / shackle nearby: do not splash it, and the
    // tank drags the rest of the pack clear so the party can use AoE.
    Unit* FindBreakableCrowdControlNear(float radius) const;
    bool PullFightAwayFromCrowdControl();
    bool CrowdControlMarkedTargets();
    // Kick / Shield Bash / Counterspell / Earth Shock / … — heals first, then any cast.
    int GetEnemyCastInterruptScore(Unit const* pEnemy) const;
    Unit* FindEnemyCastToInterrupt() const;
    bool TryInterruptEnemyCast();
    bool EnterCombatDruidForm();
    bool ShouldEnterStealth() const;
    bool EnterStealthIfNeeded(SpellEntry const* pStealthSpell);

    bool CheckForDispelTargets();
    void UpdateInCombatAI() final;
    void UpdateOutOfCombatAI() final;
    void UpdateInCombatAI_Paladin() final;
    void UpdateOutOfCombatAI_Paladin() final;
    void UpdateInCombatAI_Shaman() final;
    void UpdateOutOfCombatAI_Shaman() final;
    void UpdateInCombatAI_Hunter() final;
    void UpdateOutOfCombatAI_Hunter() final;
    void UpdateInCombatAI_Mage() final;
    void UpdateOutOfCombatAI_Mage() final;
    void UpdateInCombatAI_Priest() final;
    void UpdateOutOfCombatAI_Priest() final;
    void UpdateInCombatAI_Warlock() final;
    void UpdateOutOfCombatAI_Warlock() final;
    void UpdateInCombatAI_Warrior() final;
    void UpdateOutOfCombatAI_Warrior() final;
    void UpdateInCombatAI_Rogue() final;
    void UpdateOutOfCombatAI_Rogue() final;
    void UpdateInCombatAI_Druid() final;
    void UpdateOutOfCombatAI_Druid() final;

    std::vector<RaidTargetIcon> m_marksToCC;
    std::vector<RaidTargetIcon> m_marksToFocus;
    ShortTimeTracker m_updateTimer;
    ObjectGuid m_leaderGuid;
    ObjectGuid m_cloneGuid;
    uint8 m_race = 0;
    uint8 m_class = 0;
    uint8 m_level = 0;
    uint32 m_mapId = 0;
    uint32 m_instanceId = 0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    float m_o = 0.0f;
    bool m_resetSpellData = false;
    bool m_companion = false;   // a saved character that levels with its leader (DungeonBotMgr)
    ObjectGuid m_rezTargetGuid;
    uint32 m_corpseSeconds = 0;
    // Active while a commanded ranged pull is in flight (tank only).
    ObjectGuid m_pullHoldTargetGuid;
    ObjectGuid m_pullHoldPullerGuid;
    uint32 m_pullHoldDeadline = 0;
};

#endif
