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
    // Joins the leader's party (creates one if needed). False until that works.
    bool AddToPlayerGroup();
    void LevelUpTo(uint32 level);

    bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const override;
    Player* GetPartyLeader() const;
    bool AttackStart(Unit* pVictim);
    // Leader's engage style (calm = wait for the pack; aggressive = charge in).
    bool IsEngageAggressive() const;
    // Duo/trio (`.bots fill 2|3`): leader's selected mob is an immediate go.
    bool IsSmallParty() const;
    bool IsInCalmEngageRange(Unit const* pTarget) const;
    // Leader already swung / cast / got hit by this mob — party may open from any range.
    bool IsLeaderCommittedTo(Unit const* pTarget) const;
    bool CanEngageTarget(Unit const* pTarget) const;
    // `.partybot pull` / `attackstart`: allow opening this guid from any range for a while.
    void AllowForcedEngage(ObjectGuid targetGuid, uint32 durationMs);
    // .partybot pull: score for who should open from range (hunter / mage / throw).
    int GetRangedPullScore(Unit const* pTarget) const;
    bool HasThrowableWeapon() const;
    bool EnsureThrowableWeapon();
    bool TryThrowPull(Unit* pTarget);
    // Tank waits until the mob is nearly at the ranged puller, then engages.
    void BeginRangedPullHold(ObjectGuid targetGuid, ObjectGuid pullerGuid, uint32 durationMs);
    void ClearRangedPullHold();
    bool UpdateRangedPullHold();
    // Leader stands still with a far hostile selected: best ranged bot opens, tank holds.
    bool PartyAlreadyPulling(ObjectGuid targetGuid) const;
    // True while a tank bot is still waiting to pick up this ranged-pull target.
    bool IsTankHoldingRangedPull(Unit const* pTarget) const;
    bool StartPartyRangedPull(Unit* pTarget, uint32 durationMs);
    bool TryAssistLeaderPull(Player* pLeader);
    Unit* SelectAttackTarget(Player* pLeader) const;
    Unit* SelectPartyAttackTarget() const;
    // A different mob on the healer/player — peel without thrashing every add.
    Unit* SelectPartyPeelTarget() const;
    // Stick to one mob while building threat; taunt others instead of running laps.
    void LockTankFocus(Unit const* pTarget, uint32 durationMs = 3000);
    bool CanTankChaseSwitch(Unit const* pNew) const;
    // Idle pack inside radius — AoE threat would chain-pull.
    bool HasIdleHostileInRange(float radius) const;
    Player* SelectResurrectionTarget() const;
    bool MoveToResurrect();
    bool AllyWillResurrect() const;
    Player* SelectShieldTarget() const;
    // Equipped wand plus Shoot (5019): healer idle filler, and OOM/last-resort for casters.
    bool HasUsableWand() const;
    // Learn Wands/Shoot if missing; repair a broken wand so Shoot is not refused.
    bool EnsureWandReady();
    bool TryShootWand(Unit* pVictim);
    Unit* GetMarkedTarget(RaidTargetIcon mark) const;
    virtual bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const;
    bool DrinkAndEat();
    // Player bots have no client fall packets; snap down if left hanging in air.
    bool UnstickIfFloating();
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
    // Caster / ranged pack members: Silence when we have it (not only mid-cast).
    bool IsRangedEnemy(Unit const* pEnemy) const;
    Unit* FindRangedEnemyToSilence() const;
    bool TrySilenceRangedEnemy();
    bool EnterCombatDruidForm();
    bool ShouldEnterStealth() const;
    bool EnterStealthIfNeeded(SpellEntry const* pStealthSpell);

    bool CheckForDispelTargets();
    // Priest: Fear Ward the tank (or the player) before and during scary pulls.
    Player* SelectFearWardTarget() const;
    bool TryFearWard();
    // Among interrupters in range, only one acts (closest; guid breaks ties).
    bool CanInterruptTarget(Unit const* pEnemy) const;
    bool IsAssignedToInterrupt(Unit const* pEnemy) const;
    // Druid: Innervate the driest healer (or self); Rebirth a dead ally mid-fight.
    Player* SelectInnervateTarget() const;
    bool TryInnervate();
    bool TryBattleResurrection();
    // Warlock: Tongues on casters, Elements on elites, Agony on trash.
    bool TryApplyWarlockCurse(Unit* pVictim);
    // Sheep / blind / hibernate a spare add once the kill target is locked.
    bool TryCrowdControlExtra();
    // Lowest-HP engaged add: finish one before opening the next.
    Unit* SelectFocusFireTarget() const;
    // Focus (skull / lowest HP) is nearly dead — dump into it instead of cleaving.
    bool ShouldFocusBeforeCleave() const;
    // Fade / Feign / Cower / Vanish when threat is about to rip.
    bool TryThreatDump();
    // True when our threat is so high that the next cast would pull — skip damage.
    bool ShouldHoldFire() const;
    // Healer mana / party HP still recovering: stay put and drink instead of pulling.
    bool PartyNeedsRest() const;
    // Fresh pull: a patrol is walking into the party — wait instead of chaining packs.
    bool ShouldWaitForPatrol(Unit const* pTarget) const;
    // Party tank player (bot ROLE_TANK or human with shield / tank class).
    Player* FindPartyTank() const;
    // True when every combat hostile in radius is already on the tank (safe for Blizzard / Volley).
    bool AreAllHostilesOnTank(Unit const* pAround, float radius) const;
    // Ranged: step toward the tank/leader when a pillar ate the cast.
    bool TryRegainLineOfSight(Unit* pVictim);
    // DoCastSpell StopMoving can leave melee IDLE out of reach — restart chase.
    void ResumeMeleeChaseIfNeeded(Unit* pVictim);
    // Gate class-AI MoveChase the same way as AttackStart (idle-pack clearance).
    void BeginChasing(Unit* pVictim) const override;
    // Tank: early in a far pull, drag the pack back to the party (LoS corner).
    bool TryDragPackToParty();
    // Walking me→dest would enter an idle hostile's aggro (chain-pull risk).
    bool WouldMoveAggroExtras(float destX, float destY, float destZ,
                              Unit const* pIgnore = nullptr) const;
    // Healer/leader peel: chase even through packs; otherwise plant.
    bool IsUrgentPeelTarget(Unit const* pTarget) const;
    // Hunter / Warlock: keep the pet on the kill target.
    bool EnsurePetAttacks(Unit* pVictim);
    // Mana pot right as an elite / hard pull opens (classic pre-pot window).
    bool TryPrePullConsumable(Unit* pTarget);
    // Step out of hostile Rain of Fire / Blizzard / Consecration ticks.
    bool TryAvoidGroundDamage();
    // Healer: hold an 8–18 yd band on the tank (cleave-safe heal range).
    bool TryHealerStackWithTank();
    // Ranged: step apart when stacked on another party member (cleave / rain).
    bool TryRangedSpread();
    // Ranged: leave melee (root/slow, then step). Healers with a tank Fade+stack instead.
    bool TryKeepRangedDistance();
    // Paladin: Blessing of Freedom on a rooted/snared ally (tank/healer first).
    bool TryBlessingOfFreedom();
    // Step out of a hostile AoE cast (Blizzard / Rain of Fire / …) before it lands.
    bool TryScatterFromDangerousCast();
    // Tank: stand opposite the party so boss cleave faces the wall, not the healers.
    bool TryTankFaceAwayFromParty();
    // Melee: step out while a boss Whirlwind / Blade Flurry aura is up.
    bool TryKiteWhirlwind();
    // Encounter table flags for the current victim (0 = none).
    uint32 GetEncounterFlags() const;
    bool ChaseFromBehind(Unit* pVictim) const override;
    // OOC: bandage when still hurt after food is not enough / not started.
    bool TryBandage();
    // Healer care lines: tighter on trash / low mana, looser on elites / triage.
    void GetHealerCareThresholds(float& directSelf, float& directGroup,
                                 float& hotSelf, float& hotGroup,
                                 bool& allowPreHeal) const;
    // Among bots that can dispel this ally, only the closest one acts.
    bool IsAssignedToDispel(Player const* pFriend, SpellEntry const* pSpellEntry) const;
    // Taunt every loose mob on the party (post-fear / multi-pull), not only current target.
    bool TryTankSnapThreat();
    // Any living party member (or self) below the HP line — healers drop utility for triage.
    bool PartyMemberBelow(float hpPercent) const;
    // Shaman Purge / Priest Dispel Magic on enemy magic buffs (shields, enrages).
    bool TryStripEnemyBuffs();
    // Spread Corruption / SW:P / Serpent Sting to spare engaged adds.
    bool TrySpreadDots();
    // Default skull=kill / moon=CC; tank bot keeps icons on the right mobs.
    void EnsureDefaultMarks();
    bool IsAutoMarkController() const;
    void UpdateAutoRaidMarks();
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
    uint8 m_groupJoinTries = 0; // init ticks spent waiting to join the leader's party
    ObjectGuid m_rezTargetGuid;
    uint32 m_corpseSeconds = 0;
    // Active while a commanded ranged pull is in flight (tank only).
    ObjectGuid m_pullHoldTargetGuid;
    ObjectGuid m_pullHoldPullerGuid;
    uint32 m_pullHoldDeadline = 0;
    // Forced open (pull / attackstart) until this time.
    ObjectGuid m_forceEngageGuid;
    uint32 m_forceEngageUntil = 0;
    // After breaking fear / stun: tank snaps threat for a few seconds.
    uint32 m_snapThreatUntil = 0;
    // Tank: do not chase-switch off this guid until the stick window ends
    // (or the mob is already attacking us). Stops A↔B laps on healer adds.
    ObjectGuid m_tankFocusGuid;
    uint32 m_tankFocusUntil = 0;
    // Pre-pot lockout so we do not chug every tick on the same pull.
    uint32 m_prePotUntil = 0;
    // Soft reposition (spread / stack / face-away) lockout — stops combat thrashing.
    uint32 m_repositionUntil = 0;
};

#endif
