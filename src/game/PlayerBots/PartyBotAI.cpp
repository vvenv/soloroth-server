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

#include "PartyBotAI.h"
#include "PartyBotEncounter.h"
#include "Player.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Bag.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "DungeonBotMgr.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Utilities/Random.h"

#include <algorithm>
#include <cmath>
#include <random>

enum PartyBotSpells
{
    PB_SPELL_FOOD = 1131,
    PB_SPELL_DRINK = 1137,
    PB_SPELL_AUTO_SHOT = 75,
    PB_SPELL_SHOOT_WAND = 5019,
    PB_SPELL_THROW = 2764,
    PB_SPELL_HONORLESS_TARGET = 2479,
};

// Half-second ticks catch Kick / Counterspell windows that a 1s tick misses.
#define PB_UPDATE_INTERVAL 500
#define PB_MIN_FOLLOW_DIST 3.0f
#define PB_MAX_FOLLOW_DIST 6.0f
#define PB_MIN_FOLLOW_ANGLE 0.0f
#define PB_MAX_FOLLOW_ANGLE 6.0f
// Mob is "nearly at" the puller: tank steps in and taunts.
#define PB_PULL_PICKUP_DIST 8.0f
// Calm engage: wait until the pack is this close to the party (leader).
#define PB_CALM_ENGAGE_DIST 8.0f
// Do not open a pull from farther than Auto Shot / Throw can reach.
#define PB_PULL_MAX_DIST 40.0f
// Improved Scorch "Fire Vulnerability" (stacks to 5).
#define PB_SPELL_FIRE_VULNERABILITY 22959

bool PartyBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    if (!m_race && !m_class)
    {
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    return SpawnNewPlayer(sess, m_class, m_race, m_mapId, m_instanceId, m_x, m_y, m_z, m_o, sObjectAccessor.FindPlayer(m_cloneGuid));
}

void PartyBotAI::CloneFromPlayer(Player const* pPlayer)
{
    if (!pPlayer)
        return;

    if (pPlayer->GetLevel() != me->GetLevel())
    {
        me->GiveLevel(pPlayer->GetLevel());
        me->InitTalentForLevel();
        me->SetUInt32Value(PLAYER_XP, 0);
    }

    // Learn all of the target's spells.
    for (const auto& spell : pPlayer->GetSpellMap())
    {
        if (spell.second.disabled)
            continue;

        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;

        uint32 const firstRankId = sSpellMgr.GetFirstSpellInChain(spell.first);
        if (!me->HasSpell(spell.first))
            me->LearnSpell(spell.first, false, (firstRankId == spell.first && GetTalentSpellPos(firstRankId)));
    }

    me->GetHonorMgr().SetHighestRank(pPlayer->GetHonorMgr().GetHighestRank());
    me->GetHonorMgr().SetRank(pPlayer->GetHonorMgr().GetRank());

    // Unequip current gear
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        me->AutoUnequipItemFromSlot(i);

    // Copy gear from target.
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            me->SatisfyItemRequirements(pItem->GetProto());
            me->StoreNewItemInBestSlots(pItem->GetEntry(), 1, pItem->GetEnchantmentId(EnchantmentSlot(0)));
        }
    }
}

void PartyBotAI::LevelUpTo(uint32 level)
{
    me->GiveLevel(level);
    me->InitTalentForLevel();
    me->SetUInt32Value(PLAYER_XP, 0);
    LearnPremadeSpecForClass();
    me->UpdateSkillsToMaxSkillsForLevel();
    UpgradeGear();
    if (me->GetClass() == CLASS_HUNTER)
        EnsureHunterRangedWeapon();
    AddAllSpellReagents();
    EnsureCombatPotions();
    m_resetSpellData = true;
    me->SetHealthPercent(100.0f);
    me->SetPowerPercent(me->GetPowerType(), 100.0f);
    me->SaveToDB();
}

Player* PartyBotAI::GetPartyLeader() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    if (Player* originalLeader = ObjectAccessor::FindPlayerNotInWorld(m_leaderGuid))
    {
        if (me->InBattleGround() == originalLeader->InBattleGround())
        {
            // In case the original spawner is not in the same group as the bots anymore.
            if (pGroup != originalLeader->GetGroup())
                return nullptr;

            // In case the current leader is the bot itself and it's not inside a Battleground.
            ObjectGuid currentLeaderGuid = pGroup->GetLeaderGuid();
            if (currentLeaderGuid == me->GetObjectGuid() && !me->InBattleGround())
                return nullptr;
        }

        return originalLeader;
    }
    return nullptr;
}

bool PartyBotAI::IsValidDistancingTarget(Unit* pTarget, Unit* pEnemy)
{
    if (pTarget->IsInWorld() && pTarget->IsAlive() &&
        pTarget->GetMap() == me->GetMap())
    {
        float const distance = me->GetDistance(pTarget);
        if (distance >= 15.0f && distance <= 30.0f &&
            pTarget->GetDistance(pEnemy) >= 15.0f)
            return true;
    }

    return false;
}

Unit* PartyBotAI::GetDistancingTarget(Unit* pEnemy)
{
    if (Player* pLeader = GetPartyLeader())
        if (IsValidDistancingTarget(pLeader, pEnemy))
            return pLeader;

    Unit* pNonTank = nullptr;
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == me)
                continue;

            if (IsValidDistancingTarget(pMember, pEnemy))
            {
                if (IsTankingForm(pMember->GetShapeshiftForm()) || IsWearingShield(pMember))
                    return pMember;
                else
                    pNonTank = pMember;
            }
        }
    }

    return pNonTank;
}

bool PartyBotAI::UnstickIfFloating()
{
    if (!me->GetMap() || me->IsTaxiFlying() || me->IsBeingTeleported() || me->GetTransport())
        return false;
    if (me->IsInWater())
        return false;
    // WMO dungeons stack floors over water/void; an 80 yd GetHeight often
    // hits a lower deck (BFD) and would Clear the motion stack empty → ASSERT.
    if (me->GetMap()->IsDungeon())
        return false;

    float const x = me->GetPositionX();
    float const y = me->GetPositionY();
    float const z = me->GetPositionZ();
    // Default height search is 10 yd; mesa towns (Thunder Bluff) hang higher.
    float const ground = me->GetMap()->GetHeight(x, y, z, true, 80.0f);
    if (ground <= INVALID_HEIGHT)
        return false;

    float const delta = z - ground;
    if (delta < 2.5f)
        return false;
    // Pathfinding on ramps can sit a few yards above mesh mid-move; only yank
    // a clearly airborne bot while a spline is still running.
    if (!me->movespline->Finalized() && delta < 6.0f)
        return false;

    // Clear(all) empties the stack; always leave Idle or UpdateMotion aborts.
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();
    me->StopMoving(true);
    me->NearTeleportTo(x, y, ground + 0.05f, me->GetOrientation());
    return true;
}

bool PartyBotAI::RunAwayFromTarget(Unit* pEnemy)
{
    if (!pEnemy)
        return false;

    // Dungeon: no long retreat (lava / pits). Same short MoveDistance as ground
    // AoE / whirlwind kite — stop chase, step out of cleave, hold the 25 yd band.
    if (me->GetMap() && me->GetMap()->IsDungeon())
    {
        if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
            IsRangedDamageClass(me->GetClass()) &&
            !IsAttackSpeedOverridenForm(me->GetShapeshiftForm()) &&
            me->GetCombatDistance(pEnemy) < 10.0f)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
            {
                me->SetCasterChaseDistance(25.0f);
                return true;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            {
                me->GetMotionMaster()->Clear(false, true);
                me->GetMotionMaster()->MoveIdle();
            }
            me->SetCasterChaseDistance(25.0f);
            return me->GetMotionMaster()->MoveDistance(pEnemy, 12.0f);
        }
        return false;
    }

    if (Unit* pTarget = GetDistancingTarget(pEnemy))
    {
        me->GetMotionMaster()->MovePoint(0,
            pTarget->GetPositionX(), pTarget->GetPositionY(), pTarget->GetPositionZ(),
            MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
        return true;
    }

    return me->GetMotionMaster()->MoveDistance(pEnemy, 15.0f);
}

bool PartyBotAI::PartyNeedsRest() const
{
    if (me->IsInCombat() || IsInDuel())
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive() || pMember->GetMap() != me->GetMap())
            continue;
        if (pMember->GetDistance(me) > 40.0f)
            continue;
        if (pMember->GetHealthPercent() < 60.0f)
            return true;
        if (IsPartyHealer(pMember) && pMember->GetPowerType() == POWER_MANA &&
            pMember->GetPowerPercent(POWER_MANA) < 40.0f)
            return true;
    }
    return false;
}

bool PartyBotAI::ShouldWaitForPatrol(Unit const* pTarget) const
{
    if (!pTarget || me->IsInCombat() || IsEngageAggressive() || IsInDuel())
        return false;
    // Leader already swung — the pull is theirs; join in.
    if (IsLeaderCommittedTo(pTarget))
        return false;
    // Commanded pull skips the wait.
    if (!m_forceEngageGuid.IsEmpty() &&
        pTarget->GetObjectGuid() == m_forceEngageGuid &&
        WorldTimer::getMSTime() < m_forceEngageUntil)
        return false;

    Player const* pLeader = GetPartyLeader();
    if (!pLeader)
        return false;

    // Linked-pack / call-for-help bubble around the pull target.
    float const linkRadius = 12.0f;
    float const partyDanger = 18.0f;
    std::list<Unit*> near;
    me->GetEnemyListInRadiusAround(const_cast<Unit*>(pTarget), linkRadius, near);

    for (Unit* pUnit : near)
    {
        if (!pUnit || pUnit == pTarget || !pUnit->IsAlive() || pUnit->IsInCombat())
            continue;
        if (!pUnit->IsMoving())
            continue;
        // A patrol is already inside party aggro range — wait for it to walk clear.
        if (pUnit->GetDistance(pLeader) < partyDanger)
            return true;
        // Or still close to the selected pack and near the party path.
        if (pUnit->GetDistance(pLeader) < 30.0f)
            return true;
    }

    // Selected mob itself is patrolling past another idle pack near the leader.
    if (pTarget->IsMoving())
    {
        for (Unit* pUnit : near)
        {
            if (!pUnit || pUnit == pTarget || !pUnit->IsAlive() || pUnit->IsInCombat())
                continue;
            if (pUnit->GetDistance(pLeader) < partyDanger)
                return true;
        }
    }
    return false;
}

Player* PartyBotAI::FindPartyTank() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Player* pBest = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive())
            continue;
        if (!IsPartyTank(pMember))
            continue;
        // Prefer an explicit bot tank over a human-with-shield guess.
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
        {
            if (pAI->m_role == ROLE_TANK)
                return pMember;
        }
        if (!pBest)
            pBest = pMember;
    }
    return pBest;
}

bool PartyBotAI::AreAllHostilesOnTank(Unit const* pAround, float radius) const
{
    if (!pAround || IsInDuel())
        return true;

    Player* pTank = FindPartyTank();
    if (!pTank)
        return true;

    std::list<Unit*> targets;
    me->GetEnemyListInRadiusAround(const_cast<Unit*>(pAround), radius, targets);
    for (Unit* pEnemy : targets)
    {
        if (!pEnemy || !pEnemy->IsInCombat())
            continue;
        if (pEnemy->HasBreakableByDamageCrowdControlAura())
            continue;
        Unit* pVictim = pEnemy->GetVictim();
        if (!pVictim)
            continue;
        if (pVictim == pTank)
            continue;
        // Still loose on a healer / DPS / the player — hold the AoE.
        if (pVictim->IsPlayer())
            return false;
    }
    return true;
}

bool PartyBotAI::TryRegainLineOfSight(Unit* pVictim)
{
    if (!pVictim || m_role == ROLE_TANK || IsInDuel())
        return false;
    if (me->IsWithinLOSInMap(pVictim))
        return false;
    if (me->IsNonMeleeSpellCasted() || me->IsMounted())
        return false;

    // Melee already path via chase; this is for casters stuck behind a pillar.
    if (m_role == ROLE_MELEE_DPS && me->CanReachWithMeleeAutoAttack(pVictim))
        return false;

    // Already pathing — let MoveChase walk the corner.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        return true;

    // Prefer stepping out beside the tank (classic corner), else chase the mob around the wall.
    Unit* pPivot = FindPartyTank();
    if (!pPivot)
        pPivot = GetPartyLeader();
    if (pPivot && pPivot != me && me->GetDistance(pPivot) > 3.0f &&
        me->GetDistance(pPivot) < me->GetDistance(pVictim))
        BeginChasing(pPivot);
    else
        BeginChasing(pVictim);
    return true;
}

bool PartyBotAI::EnsurePetAttacks(Unit* pVictim)
{
    if (!pVictim || !IsValidHostileTarget(pVictim))
        return false;

    Pet* pPet = me->GetPet();
    if (!pPet || !pPet->IsAlive())
        return false;

    if (pPet->GetVictim() == pVictim)
        return false;

    if (CreatureAI* pAI = pPet->AI())
        pAI->AttackStart(pVictim);
    return true;
}

bool PartyBotAI::TryDragPackToParty()
{
    if (m_role != ROLE_TANK || IsInDuel())
        return false;

    Player* pLeader = GetPartyLeader();
    Unit* pVictim = me->GetVictim();
    if (!pLeader || !pVictim || pVictim->GetVictim() != me)
        return false;
    // Early pull only — once the pack is low, plant and threat.
    if (pVictim->GetHealthPercent() < 75.0f)
        return false;
    if (me->GetDistance(pLeader) <= PB_CALM_ENGAGE_DIST + 4.0f)
        return false;
    if (me->GetDistance(pLeader) > 40.0f)
        return false;
    // Stay for sheep drag / snap peels instead.
    if (FindBreakableCrowdControlNear(15.0f))
        return false;
    if (m_snapThreatUntil && WorldTimer::getMSTime() < m_snapThreatUntil)
        return false;
    // Keep melee contact while walking back.
    if (!me->CanReachWithMeleeAutoAttack(pVictim) && me->GetDistance(pVictim) > 12.0f)
        return false;

    MovementGeneratorType const mt = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (mt == POINT_MOTION_TYPE || mt == DISTANCING_MOTION_TYPE)
        return true;

    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MovePoint(0,
        pLeader->GetPositionX(), pLeader->GetPositionY(), pLeader->GetPositionZ(),
        MOVE_PATHFINDING);
    return true;
}

bool PartyBotAI::TryBandage()
{
    if (me->IsInCombat() || IsInDuel() || me->GetVictim())
        return false;
    if (me->GetHealthPercent() > 70.0f)
        return false;
    // Food / drink already ticking — do not interrupt.
    if (me->HasAura(PB_SPELL_FOOD) || me->HasAura(PB_SPELL_DRINK) || me->IsNonMeleeSpellCasted())
        return false;

    Item* pBest = nullptr;
    int32 bestPower = 0;
    auto consider = [&](Item* pItem)
    {
        if (!pItem)
            return;
        ItemPrototype const* pProto = pItem->GetProto();
        if (!pProto || pProto->Class != ITEM_CLASS_CONSUMABLE ||
            me->CanUseItem(pProto) != EQUIP_ERR_OK)
            return;
        for (auto const& itemSpell : pProto->Spells)
        {
            if (!itemSpell.SpellId || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
            // Bandages: cast time, typically peaceful-only.
            if (!pSpell || pSpell->GetCastTime(me) <= 0)
                continue;
            if (!me->IsSpellReady(pSpell, pProto))
                continue;
            int32 power = 0;
            bool heal = false;
            for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (pSpell->Effect[i] == SPELL_EFFECT_HEAL ||
                    (pSpell->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
                     pSpell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL))
                {
                    heal = true;
                    power = std::max(power, pSpell->CalculateSimpleValue(SpellEffectIndex(i)));
                }
            }
            if (!heal)
                continue;
            if (power > bestPower)
            {
                bestPower = power;
                pBest = pItem;
            }
        }
    };

    for (int slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (int bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Item* pBagItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (!pBagItem || !pBagItem->IsBag())
            continue;
        Bag* pBag = static_cast<Bag*>(pBagItem);
        for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
            consider(pBag->GetItemByPos(uint8(slot)));
    }

    if (!pBest)
        return false;
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
    return UseItemEffect(pBest, false);
}

void PartyBotAI::GetHealerCareThresholds(float& directSelf, float& directGroup,
                                         float& hotSelf, float& hotGroup,
                                         bool& allowPreHeal) const
{
    // Default: heal early, pre-heal freely.
    directSelf = 60.0f;
    directGroup = 80.0f;
    hotSelf = 80.0f;
    hotGroup = 90.0f;
    allowPreHeal = true;

    if (GetRole() != ROLE_HEALER || me->GetPowerType() != POWER_MANA)
        return;

    // Someone is dying — full care regardless of mana.
    if (PartyMemberBelow(40.0f))
        return;

    Unit* pThreat = me->GetVictim();
    if (!pThreat)
    {
        if (Player* pTank = FindPartyTank())
            pThreat = pTank->GetVictim();
    }
    bool const hard = IsDangerousFight(pThreat);
    float const mana = me->GetPowerPercent(POWER_MANA);

    if (hard)
    {
        // Elite / boss: keep the tank topped; still skip tiny top-offs when OOM.
        if (mana < 30.0f)
        {
            directSelf = 50.0f;
            directGroup = 70.0f;
            hotSelf = 70.0f;
            hotGroup = 80.0f;
            allowPreHeal = false;
        }
        return;
    }

    // Trash: a pro healer wanders and lets HoTs work — save mana for the next elite.
    if (mana < 50.0f)
    {
        directSelf = 45.0f;
        directGroup = 60.0f;
        hotSelf = 65.0f;
        hotGroup = 70.0f;
        allowPreHeal = false;
    }
    else if (mana < 70.0f)
    {
        directSelf = 50.0f;
        directGroup = 70.0f;
        hotSelf = 75.0f;
        hotGroup = 80.0f;
        allowPreHeal = false;
    }
    else
    {
        directSelf = 55.0f;
        directGroup = 75.0f;
        allowPreHeal = false; // trash rarely needs predictive heals
    }
}

bool PartyBotAI::TryTankFaceAwayFromParty()
{
    if (m_role != ROLE_TANK || IsInDuel() || !me->IsInCombat())
        return false;

    Unit* pVictim = me->GetVictim();
    if (!pVictim || pVictim->GetVictim() != me)
        return false;
    // Only named frontal bosses — not every elite (that danced the whole dungeon).
    uint32 const encFlags = PartyBotEncounter::GetFlags(pVictim);
    if (!(encFlags & PB_ENC_FACE_AWAY))
        return false;
    if (me->IsNonMeleeSpellCasted() || !me->CanReachWithMeleeAutoAttack(pVictim))
        return false;
    // Sheep drag / snap peels own the movement.
    if (FindBreakableCrowdControlNear(15.0f))
        return false;
    if (m_snapThreatUntil && WorldTimer::getMSTime() < m_snapThreatUntil)
        return false;
    if (m_repositionUntil && WorldTimer::getMSTime() < m_repositionUntil)
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    float sumX = 0.0f, sumY = 0.0f;
    int count = 0;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;
        if (pMember->GetDistance(pVictim) > 40.0f)
            continue;
        sumX += pMember->GetPositionX();
        sumY += pMember->GetPositionY();
        ++count;
    }
    if (count == 0)
        return false;

    float const partyX = sumX / count;
    float const partyY = sumY / count;
    float const bossX = pVictim->GetPositionX();
    float const bossY = pVictim->GetPositionY();

    // Angle boss→party vs boss→tank. Want roughly opposite the party.
    float const angParty = std::atan2(partyY - bossY, partyX - bossX);
    float const angTank = std::atan2(me->GetPositionY() - bossY, me->GetPositionX() - bossX);
    float diff = std::fabs(angParty - angTank);
    while (diff > M_PI_F)
        diff = std::fabs(diff - 2.0f * M_PI_F);

    // ~90°+ from the party is good enough — do not micro-adjust every tick.
    if (diff > 1.4f)
    {
        me->SetInFront(pVictim);
        me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
        return false;
    }

    // Already stepping — keep swinging / casting while the point finishes.
    MovementGeneratorType const mt = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (mt == POINT_MOTION_TYPE || mt == DISTANCING_MOTION_TYPE)
        return false;

    float const melee = std::max(3.0f, pVictim->GetCombatReach() + me->GetCombatReach());
    float const targetAng = angParty + M_PI_F;
    float x = bossX + std::cos(targetAng) * melee;
    float y = bossY + std::sin(targetAng) * melee;
    float z = pVictim->GetPositionZ();
    me->UpdateAllowedPositionZ(x, y, z);

    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
    m_repositionUntil = WorldTimer::getMSTime() + 4 * IN_MILLISECONDS;
    return false;
}

bool PartyBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;

    if (me->GetVictim())
        return false;

    // Still hurt and not eating yet: bandage before sitting for food.
    if (TryBandage())
        return true;

    bool const isEating = me->HasAura(PB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(PB_SPELL_DRINK);

    // Start earlier than 100%, finish near full — a real group waits on the healer.
    bool const needToEat = me->GetHealthPercent() < (isEating ? 95.0f : 70.0f) ||
        (PartyNeedsRest() && me->GetHealthPercent() < 95.0f);
    bool const needToDrink = (me->GetPowerType() == POWER_MANA) &&
        (me->GetPowerPercent(POWER_MANA) < (isDrinking ? 90.0f : 50.0f) ||
         (PartyNeedsRest() && me->GetPowerPercent(POWER_MANA) < 90.0f));

    if (!needToEat && !needToDrink)
        return false;

    // Between pulls: top potions / ammo so a long dungeon does not run dry.
    EnsureCombatPotions();
    if (me->GetClass() == CLASS_HUNTER)
        AddHunterAmmo();

    if (!isEating && needToEat)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_FOOD))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_DRINK))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

// Resurrection, Redemption, Ancestral Spirit, Rebirth. Every rank: a premade
// spec learns the highest rank only, and a healer class often has none of them
// (a balance druid, a protection paladin).
static bool KnowsResurrection(Player const* pPlayer)
{
    static uint32 const spells[] =
    {
        2006, 2010, 10880, 10881, 20770,
        7328, 10322, 10324, 20772, 20773,
        2008, 20609, 20610, 20776, 20777,
        20484, 20739, 20742, 20747, 20748,
    };
    for (uint32 spellId : spells)
        if (pPlayer->HasSpell(spellId))
            return true;
    return false;
}

bool PartyBotAI::AllyWillResurrect() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive() || pMember->GetMapId() != me->GetMapId())
            continue;
        if (KnowsResurrection(pMember))
            return true;
    }
    return false;
}

bool PartyBotAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!CombatBotBaseAI::CanTryToCastSpell(pTarget, pSpellEntry))
        return false;

    if (IsInDuel() || pSpellEntry->IsPositiveSpell())
        return true;

    // Do not open a sheep / hibernate / blind / shackle (player's or a bot's).
    if (pTarget && pTarget != me && pTarget->HasBreakableByDamageCrowdControlAura())
        return false;

    if (pSpellEntry->IsAreaOfEffectSpell())
    {
        // Marked CC: hold every AoE so the marked target is not opened by splash.
        if (!m_marksToCC.empty())
            return false;

        float radius;
        if (pSpellEntry->EffectRadiusIndex[0])
            radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[0]));
        else if (pSpellEntry->EffectRadiusIndex[1])
            radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[1]));
        else if (pSpellEntry->EffectRadiusIndex[2])
            radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[2]));
        else
            radius = 10.0f;

        // Classic dungeon habit: no Blizzard / Volley until every add is on the tank.
        if (m_role != ROLE_TANK && !AreAllHostilesOnTank(pTarget, radius))
            return false;

        std::list<Unit*> targets;
        me->GetEnemyListInRadiusAround(pTarget, radius, targets);

        for (auto const& pEnemy : targets)
        {
            if (pEnemy->HasBreakableByDamageCrowdControlAura())
                return false;

            // do not cast aoe if it will pull aggro
            if (m_role != ROLE_TANK &&
                ((pEnemy->GetLevel() + 5) > me->GetLevel()) &&
                ((pEnemy->GetHealth() * 4) > me->GetHealth()) &&
                pEnemy->GetVictim() && pEnemy->GetVictim() != me &&
                pEnemy->IsValidAttackTarget(me) &&
                pEnemy->CanHaveThreatList())
            {
                float const myThreat = pEnemy->GetThreatManager().getThreat(me);
                float const victimThreat = pEnemy->GetThreatManager().getThreat(pEnemy->GetVictim());

                if (victimThreat < (myThreat + me->GetMaxHealth()))
                    return false;
            }
        }
    }

    return true;
}

bool PartyBotAI::CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const
{
    if (IsInDuel())
        return true;

    if (pSpellEntry->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) &&
        AreOthersOnSameTarget(pTarget->GetObjectGuid()))
        return false;

    if (pSpellEntry->HasSingleTargetAura())
    {
        auto const& singleAuras = me->GetSingleCastSpellTargets();
        if (singleAuras.find(pSpellEntry) != singleAuras.end())
            return false;
    }

    return true;
}

Unit* PartyBotAI::FindBreakableCrowdControlNear(float radius) const
{
    auto pick = [](std::list<Unit*> const& targets) -> Unit*
    {
        for (Unit* pUnit : targets)
            if (pUnit && pUnit->HasBreakableByDamageCrowdControlAura())
                return pUnit;
        return nullptr;
    };

    std::list<Unit*> targets;
    me->GetEnemyListInRadiusAround(me, radius, targets);
    if (Unit* pCC = pick(targets))
        return pCC;

    if (Unit* pVictim = me->GetVictim())
    {
        targets.clear();
        me->GetEnemyListInRadiusAround(pVictim, radius, targets);
        return pick(targets);
    }
    return nullptr;
}

uint32 PartyBotAI::GetEncounterFlags() const
{
    return PartyBotEncounter::GetFlags(me->GetVictim());
}

bool PartyBotAI::ChaseFromBehind(Unit* pVictim) const
{
    if (!pVictim)
        return m_role == ROLE_MELEE_DPS || m_role == ROLE_HEALER;

    uint32 const flags = PartyBotEncounter::GetFlags(pVictim);
    if (flags & PB_ENC_MELEE_BEHIND)
        return m_role == ROLE_MELEE_DPS || m_role == ROLE_HEALER;

    // Default habit: melee and healers still prefer the rear on trash.
    return m_role == ROLE_MELEE_DPS || m_role == ROLE_HEALER;
}

int PartyBotAI::GetEnemyCastInterruptScore(Unit const* pEnemy) const
{
    if (!pEnemy || !pEnemy->IsNonMeleeSpellCasted(false, false, true))
        return -1;

    uint32 const encFlags = PartyBotEncounter::GetFlags(pEnemy);
    bool const hardInterrupt = (encFlags & PB_ENC_INTERRUPT_HARD) != 0;

    Spell const* pSpell = pEnemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!pSpell)
        pSpell = pEnemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (!pSpell || !pSpell->m_spellInfo)
    {
        if (hardInterrupt)
            return 80;
        return IsRangedEnemy(pEnemy) ? 40 : 10;
    }

    SpellEntry const* info = pSpell->m_spellInfo;

    // Healing casts (mob "Heal" / Renew / …) are the main reason to interrupt.
    if (info->IsHealSpell())
        return 100;

    // Named dungeon casters: treat every other cast as kick-worthy.
    if (hardInterrupt)
        return 90;

    // Fear / charm / sleep / polymorph — stop before the party scatters.
    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (info->Effect[i] != SPELL_EFFECT_APPLY_AURA)
            continue;
        switch (info->EffectApplyAuraName[i])
        {
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_CHARM:
            case SPELL_AURA_MOD_CONFUSE:
            case SPELL_AURA_MOD_STUN:
                return 90;
            default:
                break;
        }
    }

    // Ground AoE about to land on the party — kick it or the floor eats everyone.
    if (!info->IsPositiveSpell() && info->IsAreaOfEffectSpell())
    {
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE ||
                info->Effect[i] == SPELL_EFFECT_PERSISTENT_AREA_AURA ||
                info->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE)
                return 85;
        }
    }

    // Direct damage or school nuke from a caster beats filler casts.
    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (info->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE ||
            info->Effect[i] == SPELL_EFFECT_HEALTH_LEECH)
            return IsRangedEnemy(pEnemy) ? 60 : 40;
    }

    if (IsRangedEnemy(pEnemy))
        return 50;

    return 10;
}

Unit* PartyBotAI::FindEnemyCastToInterrupt() const
{
    Unit* best = nullptr;
    int bestScore = -1;

    auto consider = [&](Unit* pEnemy)
    {
        if (!pEnemy || !IsValidHostileTarget(pEnemy))
            return;
        // Counterspell / Earth Shock reach; melee interrupts fail their own range check.
        if (!me->IsWithinDist(pEnemy, 30.0f))
            return;

        int const score = GetEnemyCastInterruptScore(pEnemy);
        if (score > bestScore)
        {
            bestScore = score;
            best = pEnemy;
        }
        else if (score == bestScore && best && score >= 0)
        {
            // Same priority: prefer the closer caster.
            if (me->GetDistance(pEnemy) < me->GetDistance(best))
                best = pEnemy;
        }
    };

    consider(me->GetVictim());
    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || pMember == me)
                continue;

            consider(pMember->GetVictim());
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
    }

    return best;
}

bool PartyBotAI::CanInterruptTarget(Unit const* pEnemy) const
{
    if (!pEnemy || !IsValidHostileTarget(pEnemy))
        return false;

    auto ready = [&](SpellEntry const* pSpell, float maxDist) -> bool
    {
        if (!pSpell || !me->IsSpellReady(pSpell))
            return false;
        if (!me->IsWithinDist(pEnemy, maxDist))
            return false;
        return true;
    };

    switch (me->GetClass())
    {
        case CLASS_ROGUE:
            return ready(m_spells.rogue.pKick, 5.0f) || ready(m_spells.rogue.pGouge, 5.0f);
        case CLASS_WARRIOR:
            return ready(m_spells.warrior.pShieldBash, 5.0f) ||
                   ready(m_spells.warrior.pPummel, 5.0f) ||
                   ready(m_spells.warrior.pConcussionBlow, 5.0f);
        case CLASS_MAGE:
            return ready(m_spells.mage.pCounterspell, 30.0f);
        case CLASS_SHAMAN:
            return ready(m_spells.shaman.pEarthShock, 20.0f);
        case CLASS_PRIEST:
            return ready(m_spells.priest.pSilence, 30.0f);
        case CLASS_PALADIN:
            return ready(m_spells.paladin.pHammerOfJustice, 10.0f);
        case CLASS_DRUID:
            return ready(m_spells.druid.pBash, 5.0f);
        default:
            return false;
    }
}

bool PartyBotAI::IsAssignedToInterrupt(Unit const* pEnemy) const
{
    if (!pEnemy || IsInDuel())
        return true;
    if (!CanInterruptTarget(pEnemy))
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return true;

    // Someone already casting at this caster: do not double-kick.
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;
        if (!pMember->IsNonMeleeSpellCasted(false, false, true))
            continue;
        if (pMember->GetTargetGuid() == pEnemy->GetObjectGuid())
            return false;
    }

    // Closest ready interrupter owns the kick; guid breaks a distance tie.
    float const myDist = me->GetDistance(pEnemy);
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive() || pMember->GetMap() != me->GetMap())
            continue;
        PartyBotAI const* pAI = dynamic_cast<PartyBotAI const*>(pMember->AI());
        if (!pAI || !pAI->CanInterruptTarget(pEnemy))
            continue;

        float const dist = pMember->GetDistance(pEnemy);
        if (dist + 0.5f < myDist)
            return false;
        if (dist <= myDist + 0.5f &&
            pMember->GetObjectGuid() < me->GetObjectGuid())
            return false;
    }
    return true;
}

bool PartyBotAI::TryInterruptEnemyCast()
{
    if (IsInDuel())
        return false;

    Unit* pTarget = FindEnemyCastToInterrupt();
    if (!pTarget || !IsAssignedToInterrupt(pTarget))
        return false;

    int const score = GetEnemyCastInterruptScore(pTarget);
    // Kick / Shield Bash / Pummel are long CDs — on hard fights save them for
    // heals, CC, and ground AoE; ignore filler casts.
    bool const preciousKick =
        me->GetClass() == CLASS_ROGUE || me->GetClass() == CLASS_WARRIOR;
    Unit* pThreat = me->GetVictim() ? me->GetVictim() : pTarget;
    if (preciousKick && score < 60 && IsDangerousFight(pThreat))
        return false;

    auto trySpell = [&](SpellEntry const* pSpell) -> bool
    {
        if (!pSpell || !CanTryToCastSpell(pTarget, pSpell))
            return false;
        return DoCastSpell(pTarget, pSpell) == SPELL_CAST_OK;
    };

    switch (me->GetClass())
    {
        case CLASS_ROGUE:
            // Kick first; Gouge also stops the cast but wastes the lockout window.
            if (trySpell(m_spells.rogue.pKick))
                return true;
            if (trySpell(m_spells.rogue.pGouge))
                return true;
            break;
        case CLASS_WARRIOR:
            // Defensive tanks: Shield Bash. DPS in Berserker: Pummel.
            if (trySpell(m_spells.warrior.pShieldBash))
                return true;
            if (trySpell(m_spells.warrior.pPummel))
                return true;
            if (trySpell(m_spells.warrior.pConcussionBlow))
                return true;
            break;
        case CLASS_MAGE:
            if (trySpell(m_spells.mage.pCounterspell))
                return true;
            break;
        case CLASS_SHAMAN:
            if (trySpell(m_spells.shaman.pEarthShock))
                return true;
            break;
        case CLASS_PRIEST:
            if (trySpell(m_spells.priest.pSilence))
                return true;
            break;
        case CLASS_PALADIN:
            if (trySpell(m_spells.paladin.pHammerOfJustice))
                return true;
            break;
        case CLASS_DRUID:
            if (trySpell(m_spells.druid.pBash))
                return true;
            break;
        default:
            break;
    }

    return false;
}

bool PartyBotAI::IsRangedEnemy(Unit const* pEnemy) const
{
    if (!pEnemy)
        return false;

    // Mage / priest / warlock / … class on the creature, or mid-cast.
    if (pEnemy->IsCaster())
        return true;

    // Standing off and shooting / casting at someone in the fight.
    if (Unit const* pVictim = pEnemy->GetVictim())
    {
        if (!pEnemy->CanReachWithMeleeAutoAttack(pVictim) &&
            pEnemy->GetCombatDistance(pVictim) > 8.0f)
            return true;
    }

    return false;
}

Unit* PartyBotAI::FindRangedEnemyToSilence() const
{
    Unit* best = nullptr;
    float bestDist = 0.0f;

    auto consider = [&](Unit* pEnemy)
    {
        if (!pEnemy || !IsValidHostileTarget(pEnemy) || !IsRangedEnemy(pEnemy))
            return;
        if (!me->IsWithinDist(pEnemy, 30.0f))
            return;
        if (pEnemy->HasAuraType(SPELL_AURA_MOD_SILENCE) ||
            pEnemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED))
            return;

        float const dist = me->GetDistance(pEnemy);
        if (!best || dist < bestDist)
        {
            best = pEnemy;
            bestDist = dist;
        }
    };

    consider(me->GetVictim());
    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || pMember == me)
                continue;

            consider(pMember->GetVictim());
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
    }

    return best;
}

bool PartyBotAI::TrySilenceRangedEnemy()
{
    if (IsInDuel())
        return false;

    // 1.12 player Silence is the priest talent. Counterspell only works mid-cast
    // (handled by TryInterruptEnemyCast).
    if (!m_spells.priest.pSilence)
        return false;

    Unit* pTarget = FindRangedEnemyToSilence();
    if (!pTarget || !CanTryToCastSpell(pTarget, m_spells.priest.pSilence))
        return false;

    return DoCastSpell(pTarget, m_spells.priest.pSilence) == SPELL_CAST_OK;
}

bool PartyBotAI::PullFightAwayFromCrowdControl()
{
    if (m_role != ROLE_TANK || IsInDuel())
        return false;

    Unit* pVictim = me->GetVictim();
    if (!pVictim || !IsValidHostileTarget(pVictim))
        return false;

    // Sheep sits still; keep the active fight outside typical AoE / cleave range.
    float const searchRadius = 18.0f;
    float const safeDist = 14.0f;
    Unit* pCC = FindBreakableCrowdControlNear(searchRadius);
    if (!pCC)
        return false;

    if (me->GetDistance(pCC) >= safeDist && pVictim->GetDistance(pCC) >= safeDist)
        return false;

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        return true;

    // Drag the pack away from the sheep. Try a few bearings so a wall or pit
    // does not strand the tank (Ragefire lava etc.).
    float const offsets[] = { 0.0f, 0.9f, -0.9f, 1.8f, -1.8f, 2.7f, -2.7f };
    for (float offset : offsets)
        if (me->GetMotionMaster()->MoveDistance(pCC, safeDist + 4.0f, offset))
            return true;
    return false;
}

bool PartyBotAI::AttackStart(Unit* pVictim)
{
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // Classic pre-pot: mana flask/pot as the elite pull starts, not mid-GCD later.
    TryPrePullConsumable(pVictim);

    if (me->Attack(pVictim, true))
    {
        BeginChasing(pVictim);
        return true;
    }

    return false;
}

bool PartyBotAI::IsEngageAggressive() const
{
    if (Player* pLeader = GetPartyLeader())
        return sDungeonBotMgr.GetEngageStyle(pLeader) == PARTYBOT_ENGAGE_AGGRESSIVE;
    return false;
}

bool PartyBotAI::IsInCalmEngageRange(Unit const* pTarget) const
{
    if (!pTarget)
        return false;

    // Anchor on the leader so the party does not run ahead of the pull spot.
    if (Player const* pLeader = GetPartyLeader())
        return pLeader->GetDistance(pTarget) <= PB_CALM_ENGAGE_DIST;

    return me->GetDistance(pTarget) <= PB_CALM_ENGAGE_DIST;
}

bool PartyBotAI::IsLeaderCommittedTo(Unit const* pTarget) const
{
    Player* pLeader = GetPartyLeader();
    if (!pLeader || !pTarget)
        return false;

    // Leader is swinging / casting at it, or it is on the leader.
    if (pLeader->GetVictim() == pTarget && (pLeader->IsInCombat() || pTarget->IsInCombat()))
        return true;
    if (pTarget->GetVictim() == pLeader)
        return true;
    for (Unit* pAttacker : pLeader->GetAttackers())
        if (pAttacker == pTarget)
            return true;

    // Threat after a ranged open (Frostbolt, Auto Shot) before the mob reaches them.
    // getThreat is non-const on ThreatManager; the leader check above covers most cases.
    if (pTarget->CanHaveThreatList())
    {
        ThreatManager& threat = const_cast<Unit*>(pTarget)->GetThreatManager();
        if (threat.getThreat(pLeader) > 0.0f)
            return true;
    }

    return false;
}

void PartyBotAI::AllowForcedEngage(ObjectGuid targetGuid, uint32 durationMs)
{
    m_forceEngageGuid = targetGuid;
    m_forceEngageUntil = WorldTimer::getMSTime() + durationMs;
}

bool PartyBotAI::CanEngageTarget(Unit const* pTarget) const
{
    if (!pTarget)
        return false;

    if (IsEngageAggressive())
        return true;

    // Already on us, or we are already committed to this fight.
    if (pTarget->GetVictim() == me)
        return true;
    for (Unit* pAttacker : me->GetAttackers())
        if (pAttacker == pTarget)
            return true;
    if (me->GetVictim() == pTarget && me->IsInCombat())
        return true;

    // Commanded pull / attackstart may open from any range for a short window.
    if (!m_forceEngageGuid.IsEmpty() &&
        pTarget->GetObjectGuid() == m_forceEngageGuid &&
        WorldTimer::getMSTime() < m_forceEngageUntil)
        return true;

    // Fresh pack: wait out a patrol that would chain into the party.
    if (ShouldWaitForPatrol(pTarget))
        return false;

    // You opened the pull: join at the party, do not bee-line through side packs.
    // Melee wait until the mob is near you (or you are already next to it). Ranged
    // may shoot from beside the leader once the fight is theirs.
    if (IsLeaderCommittedTo(pTarget))
    {
        if (me->GetDistance(pTarget) <= PB_CALM_ENGAGE_DIST)
            return true;

        Player const* pLeader = GetPartyLeader();
        float const partyRadius = PB_CALM_ENGAGE_DIST + 4.0f;
        bool const atParty = !pLeader || me->GetDistance(pLeader) <= partyRadius;
        if (!atParty)
            return false;

        if (IsInCalmEngageRange(pTarget))
            return true;

        if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
            IsRangedDamageClass(me->GetClass()) &&
            !IsAttackSpeedOverridenForm(me->GetShapeshiftForm()) &&
            me->GetDistance(pTarget) <= 30.0f)
            return true;

        return false;
    }

    return IsInCalmEngageRange(pTarget);
}

int PartyBotAI::GetRangedPullScore(Unit const* pTarget) const
{
    if (!pTarget || !me->IsValidAttackTarget(pTarget))
        return -1;

    float const dist = me->GetDistance(pTarget);
    if (dist > PB_PULL_MAX_DIST)
        return -1;

    int base = 0;
    switch (me->GetClass())
    {
        case CLASS_HUNTER:
            if (m_role == ROLE_RANGE_DPS)
                base = 100;
            break;
        case CLASS_MAGE:
            if (m_role == ROLE_RANGE_DPS)
                base = 90;
            break;
        case CLASS_WARLOCK:
            if (m_role == ROLE_RANGE_DPS)
                base = 80;
            break;
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_DRUID:
            if (m_role == ROLE_RANGE_DPS)
                base = 70;
            break;
        case CLASS_ROGUE:
            // Throw pull: melee role, but only if we can equip a thrown weapon.
            if (m_role == ROLE_MELEE_DPS && me->HasSpell(PB_SPELL_THROW))
                base = 60;
            break;
        default:
            break;
    }

    if (!base)
        return -1;

    // Prefer the closer puller among the same class tier.
    return base * 1000 - static_cast<int>(dist);
}

bool PartyBotAI::HasThrowableWeapon() const
{
    Item* pRanged = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    return pRanged && pRanged->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_THROWN;
}

bool PartyBotAI::EnsureThrowableWeapon()
{
    if (!me->HasSpell(PB_SPELL_THROW))
        return false;

    if (HasThrowableWeapon())
        return true;

    ItemPrototype const* best = nullptr;
    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* pProto = &itr.second;
        if (pProto->Class != ITEM_CLASS_WEAPON || pProto->SubClass != ITEM_SUBCLASS_WEAPON_THROWN)
            continue;
        if (!pProto->Discovered || pProto->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            continue;
        if (pProto->RequiredLevel > me->GetLevel())
            continue;
        if (me->CanUseItem(pProto) != EQUIP_ERR_OK)
            continue;
        if (!best || best->ItemLevel < pProto->ItemLevel)
            best = pProto;
    }

    if (!best)
        best = sObjectMgr.GetItemPrototype(2947); // Small Throwing Knife

    if (!best)
        return false;

    uint32 const maxStack = best->GetMaxStackSize();
    uint32 const count = maxStack > 1 ? (maxStack < 100u ? maxStack : 100u) : 1u;
    me->SatisfyItemRequirements(best);
    me->StoreNewItemInBestSlots(best->ItemId, count);
    return HasThrowableWeapon();
}

bool PartyBotAI::TryThrowPull(Unit* pTarget)
{
    if (!pTarget || !EnsureThrowableWeapon())
        return false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    me->StopMoving();
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();
    me->SetInFront(pTarget);
    me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
    // Ranged open: no melee swings, stay put after the throw.
    me->Attack(pTarget, false);
    return me->CastSpell(pTarget, PB_SPELL_THROW, false) == SPELL_CAST_OK;
}

void PartyBotAI::BeginRangedPullHold(ObjectGuid targetGuid, ObjectGuid pullerGuid, uint32 durationMs)
{
    m_pullHoldTargetGuid = targetGuid;
    m_pullHoldPullerGuid = pullerGuid;
    m_pullHoldDeadline = WorldTimer::getMSTime() + durationMs;
}

void PartyBotAI::ClearRangedPullHold()
{
    m_pullHoldTargetGuid.Clear();
    m_pullHoldPullerGuid.Clear();
    m_pullHoldDeadline = 0;
}

bool PartyBotAI::UpdateRangedPullHold()
{
    if (m_pullHoldTargetGuid.IsEmpty())
        return false;

    Unit* pTarget = me->GetMap() ? me->GetMap()->GetUnit(m_pullHoldTargetGuid) : nullptr;
    if (!pTarget || !pTarget->IsAlive() || !IsValidHostileTarget(pTarget))
    {
        ClearRangedPullHold();
        return false;
    }

    uint32 const now = WorldTimer::getMSTime();
    if (now >= m_pullHoldDeadline)
    {
        ClearRangedPullHold();
        AttackStart(pTarget);
        return false;
    }

    Unit* pPuller = me->GetMap()->GetUnit(m_pullHoldPullerGuid);
    float const distToPuller = pPuller ? pTarget->GetDistance(pPuller) : me->GetDistance(pTarget);
    float const distToMe = me->GetDistance(pTarget);

    // Pull connected (mob running or hitting someone) and nearly at the puller / tank.
    bool const pullLive = pTarget->IsInCombat() || pTarget->GetVictim() != nullptr;
    if (pullLive && (distToPuller <= PB_PULL_PICKUP_DIST || distToMe <= PB_PULL_PICKUP_DIST))
    {
        ClearRangedPullHold();
        AttackStart(pTarget);
        return false;
    }

    // Stay with the party; do not Charge into the pack.
    if (me->GetVictim())
        me->AttackStop(true);
    if (!me->IsStopped())
        me->StopMoving();
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
    {
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
    return true;
}

bool PartyBotAI::PartyAlreadyPulling(ObjectGuid targetGuid) const
{
    if (targetGuid.IsEmpty())
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    uint32 const now = WorldTimer::getMSTime();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember)
            continue;
        PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI());
        if (!pAI)
            continue;
        if (!pAI->m_forceEngageGuid.IsEmpty() &&
            pAI->m_forceEngageGuid == targetGuid &&
            now < pAI->m_forceEngageUntil)
            return true;
        if (!pAI->m_pullHoldTargetGuid.IsEmpty() &&
            pAI->m_pullHoldTargetGuid == targetGuid &&
            now < pAI->m_pullHoldDeadline)
            return true;
    }
    return false;
}

bool PartyBotAI::StartPartyRangedPull(Unit* pTarget, uint32 durationMs)
{
    if (!pTarget || !me->IsValidAttackTarget(pTarget))
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    // Caller is the chosen puller.
    AllowForcedEngage(pTarget->GetObjectGuid(), durationMs);

    bool opened = false;
    if (me->GetClass() == CLASS_ROGUE)
        opened = TryThrowPull(pTarget);
    else
        opened = AttackStart(pTarget);

    if (!opened)
    {
        m_forceEngageGuid.Clear();
        m_forceEngageUntil = 0;
        return false;
    }

    if (me->GetClass() == CLASS_ROGUE)
    {
        // Stay put after the throw so the tank can pick up at the party.
        uint32 const hold = std::min(durationMs, 4u * IN_MILLISECONDS);
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
        m_updateTimer.Reset(hold);
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;

        PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI());
        if (!pAI)
            continue;

        if (pAI->m_role == ROLE_MELEE_DPS || pAI->m_role == ROLE_RANGE_DPS)
        {
            pMember->StopMoving();
            pMember->GetMotionMaster()->Clear(false, true);
            pMember->GetMotionMaster()->MoveIdle();
            pAI->m_updateTimer.Reset(durationMs);
            continue;
        }

        if (pAI->m_role == ROLE_TANK && pMember->IsValidAttackTarget(pTarget))
            pAI->BeginRangedPullHold(pTarget->GetObjectGuid(), me->GetObjectGuid(), durationMs);
    }

    return true;
}

bool PartyBotAI::TryAssistLeaderPull(Player* pLeader)
{
    if (!pLeader || IsInDuel() || IsEngageAggressive())
        return false;
    if (me->IsInCombat() || pLeader->IsInCombat() || pLeader->IsMounted())
        return false;
    // Walking owns the pull timing: only open once they stop on a selected mob.
    if (pLeader->IsMoving())
        return false;

    ObjectGuid const selGuid = pLeader->GetSelectionGuid();
    if (selGuid.IsEmpty())
        return false;

    Unit* pTarget = me->GetMap()->GetUnit(selGuid);
    if (!pTarget || !pTarget->IsAlive() || !IsValidHostileTarget(pTarget))
        return false;
    // Already engaged: IsLeaderCommittedTo / SelectAttackTarget handle the join-in.
    if (pTarget->IsInCombat() || pTarget->GetVictim())
        return false;
    if (pLeader->GetDistance(pTarget) > PB_PULL_MAX_DIST)
        return false;
    if (!pLeader->IsWithinLOSInMap(pTarget))
        return false;
    // Patrol walking into us — a pro waits; do not soft-open into a chain pull.
    if (ShouldWaitForPatrol(pTarget))
        return false;
    // Close enough for calm engage — walk up with the leader, do not ranged-open.
    if (IsInCalmEngageRange(pTarget))
        return false;
    if (PartyAlreadyPulling(pTarget->GetObjectGuid()))
        return false;

    int const myScore = GetRangedPullScore(pTarget);
    if (myScore < 0)
        return false;

    // Only the best-scoring bot orchestrates, so one pull runs once.
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
            if (pAI->GetRangedPullScore(pTarget) > myScore)
                return false;
    }

    return StartPartyRangedPull(pTarget, 10 * IN_MILLISECONDS);
}

Unit* PartyBotAI::GetMarkedTarget(RaidTargetIcon mark) const
{
    if (!me->GetGroup())
        return nullptr;

    ObjectGuid targetGuid = me->GetGroup()->GetTargetWithIcon(mark);
    if (targetGuid.IsUnit())
        return me->GetMap()->GetUnit(targetGuid);

    return nullptr;
}

Unit* PartyBotAI::SelectAttackTarget(Player* pLeader) const
{
    if (IsInDuel())
    {
        if (me->m_duel->opponent && IsValidHostileTarget(me->m_duel->opponent))
            return me->m_duel->opponent;
    }
    else
    {
        // Stick to marked target in combat.
        if (me->IsInCombat() || pLeader->GetVictim())
        {
            for (auto markId : m_marksToFocus)
            {
                ObjectGuid targetGuid = me->GetGroup()->GetTargetWithIcon(markId);
                if (targetGuid.IsUnit())
                    if (Unit* pVictim = me->GetMap()->GetUnit(targetGuid))
                        if (IsValidHostileTarget(pVictim))
                            return pVictim;
            }
        }

        // Who is the leader attacking.
        if (Unit* pVictim = pLeader->GetVictim())
        {
            if (IsValidHostileTarget(pVictim))
                return pVictim;
        }

        // Soft assist: what the leader has selected (tab / click), within pull range.
        if (!pLeader->GetSelectionGuid().IsEmpty())
        {
            if (Unit* pSel = me->GetMap()->GetUnit(pLeader->GetSelectionGuid()))
            {
                if (IsValidHostileTarget(pSel) &&
                    pLeader->IsWithinDist(pSel, PB_PULL_MAX_DIST) &&
                    me->IsWithinDist(pSel, PB_PULL_MAX_DIST + 10.0f))
                    return pSel;
            }
        }

        // Already in a multi-pull with no mark/leader target: finish the weakest.
        if (me->IsInCombat())
        {
            if (Unit* pFocus = SelectFocusFireTarget())
                return pFocus;
        }
    }

    // Who is attacking me.
    for (const auto pAttacker : me->GetAttackers())
    {
        if (IsValidHostileTarget(pAttacker))
            return pAttacker;
    }

    if (!IsInDuel())
    {
        // Check if other group members are under attack.
        if (Unit* pPartyAttacker = SelectPartyAttackTarget())
            return pPartyAttacker;
    }

    // Assist pet if its in combat.
    if (Pet* pPet = me->GetPet())
    {
        if (Unit* pPetAttacker = pPet->GetAttackerForHelper())
            if (IsValidHostileTarget(pPetAttacker))
                return pPetAttacker;
    }

    return nullptr;
}

Unit* PartyBotAI::SelectPartyAttackTarget() const
{
    // Prefer the weakest engaged mob so the party finishes one before opening another.
    if (Unit* pFocus = SelectFocusFireTarget())
        return pFocus;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            for (const auto pAttacker : pMember->GetAttackers())
            {
                if (IsValidHostileTarget(pAttacker) &&
                    me->IsWithinDist(pAttacker, 50.0f))
                    return pAttacker;
            }
        }
    }

    return nullptr;
}

Unit* PartyBotAI::SelectFocusFireTarget() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Unit* pBest = nullptr;
    float bestHp = 101.0f;
    auto consider = [&](Unit* pAttacker)
    {
        if (!pAttacker || !IsValidHostileTarget(pAttacker) || !me->IsWithinDist(pAttacker, 50.0f))
            return;
        if (pAttacker->HasBreakableByDamageCrowdControlAura())
            return;
        float const hp = pAttacker->GetHealthPercent();
        // Prefer nearly dead, then the lowest. Elites get a slight bias so the boss dies first
        // once trash is under 35% — avoids a shredder wipe while finishing a woodcarver.
        float score = hp;
        if (pAttacker->IsCreature() && static_cast<Creature*>(pAttacker)->IsElite() && hp > 35.0f)
            score -= 5.0f;
        if (!pBest || score < bestHp)
        {
            pBest = pAttacker;
            bestHp = score;
        }
    };

    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember)
            continue;
        for (Unit* pAttacker : pMember->GetAttackers())
            consider(pAttacker);
        if (Unit* pVictim = pMember->GetVictim())
            if (pVictim->IsInCombat())
                consider(pVictim);
    }
    return pBest;
}

Unit* PartyBotAI::SelectPartyPeelTarget() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Unit const* pCurrent = me->GetVictim();
    Player const* pLeader = GetPartyLeader();

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;

        // Only peel off the healer or the player. Chasing every DPS add is why
        // the tank runs laps in a multi-pull.
        bool priority = (pMember == pLeader);
        if (!priority)
        {
            if (PartyBotAI const* pAI = dynamic_cast<PartyBotAI const*>(pMember->AI()))
                priority = (pAI->m_role == ROLE_HEALER);
        }
        if (!priority)
            continue;

        for (Unit* pAttacker : pMember->GetAttackers())
        {
            if (pAttacker == pCurrent)
                continue;
            if (!IsValidHostileTarget(pAttacker) || !me->IsWithinDist(pAttacker, 50.0f))
                continue;
            // Already on us — Thunder Clap / cleave handles it; do not re-chase.
            if (pAttacker->GetVictim() == me)
                continue;

            return pAttacker;
        }
    }

    return nullptr;
}

Player* PartyBotAI::SelectResurrectionTarget() const
{
    if (IsInDuel() || !m_resurrectionSpell)
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    // Closest corpse on this map. Range and line of sight are checked by the
    // caller: a body around a corner is still the one to walk to.
    Player* pClosest = nullptr;
    float closest = 0.0f;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || pMember->GetDeathState() != CORPSE)
            continue;
        if (pMember->GetMapId() != me->GetMapId())
            continue;

        float const dist = me->GetDistance(pMember);
        if (!pClosest || dist < closest)
        {
            pClosest = pMember;
            closest = dist;
        }
    }
    return pClosest;
}

bool PartyBotAI::MoveToResurrect()
{
    Player* pCorpse = SelectResurrectionTarget();
    if (!pCorpse)
    {
        m_rezTargetGuid.Clear();
        return false;
    }

    // In 1.12 the body stays where it fell. Following the leader walks away from it.
    if (m_resurrectionSpell->IsTargetInRange(me, pCorpse) && me->IsWithinLOSInMap(pCorpse))
    {
        if (CanTryToCastSpell(pCorpse, m_resurrectionSpell) &&
            DoCastSpell(pCorpse, m_resurrectionSpell) == SPELL_CAST_OK)
            return true;

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return true;
    }

    if (m_rezTargetGuid != pCorpse->GetObjectGuid() ||
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
    {
        m_rezTargetGuid = pCorpse->GetObjectGuid();
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MovePoint(0, pCorpse->GetPositionX(), pCorpse->GetPositionY(), pCorpse->GetPositionZ(), MOVE_PATHFINDING);
    }
    return true;
}

Player* PartyBotAI::SelectShieldTarget() const
{
    if (IsInDuel())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Player* pBest = nullptr;
    float bestScore = 1000.0f;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;
        if (pMember->IsImmuneToMechanic(MECHANIC_SHIELD) || pMember->GetAttackers().empty())
            continue;
        // Tank gets a shield earlier; DPS only when actually low.
        float const line = IsPartyTank(pMember) ? 95.0f : 70.0f;
        if (pMember->GetHealthPercent() >= line)
            continue;
        float score = pMember->GetHealthPercent();
        if (IsPartyTank(pMember))
            score -= 20.0f;
        if (score < bestScore)
        {
            bestScore = score;
            pBest = pMember;
        }
    }
    return pBest;
}

bool PartyBotAI::CrowdControlMarkedTargets()
{
    SpellEntry const* pSpellEntry = GetCrowdControlSpell();
    if (!pSpellEntry)
        return false;

    for (auto mark : m_marksToCC)
    {
        if (Unit* pTarget = GetMarkedTarget(mark))
        {
            if (!pTarget->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                IsValidHostileTarget(pTarget) && !AreOthersOnSameTarget(pTarget->GetObjectGuid()))
            {
                if (CanTryToCastSpell(pTarget, pSpellEntry))
                {
                    if (DoCastSpell(pTarget, pSpellEntry) == SPELL_CAST_OK)
                    {
                        me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void PartyBotAI::AddToPlayerGroup()
{
    Player* pPlayer = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pPlayer)
        return;

    Group* group = pPlayer->GetGroup();
    if (!group)
    {
        group = new Group;
        // new group: if can't add then delete
        if (!group->Create(pPlayer->GetObjectGuid(), pPlayer->GetName()))
        {
            delete group;
            return;
        }
        sObjectMgr.AddGroup(group);
    }

    if (me->GetGroup() != group)
    {
        if (me->GetGroup())
            me->RemoveFromGroup();

        group->AddMember(me->GetObjectGuid(), me->GetName());
    }
}

void PartyBotAI::OnPacketReceived(WorldPacket const* packet)
{
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        {
            if (m_initialized)
                m_resetSpellData = true;
            return;
        }
        case SMSG_DUEL_REQUESTED:
        {
            auto data = std::make_unique<WorldPackets::Duel::DuelAccepted>();
            data->playerGuid = me->GetObjectGuid();
            me->GetSession()->QueuePacket(std::move(data));
            return;
        }
        case SMSG_PARTYKILLLOG:
        {
            if (!me)
                return;

            if (Group const* pGroup = me->GetGroup())
            {
                if (pGroup->GetLootMethod() == ROUND_ROBIN ||
                    pGroup->GetLootMethod() == GROUP_LOOT ||
                    pGroup->GetLootMethod() == NEED_BEFORE_GREED)
                {
                    ObjectGuid victimGuid = *(((uint64*)(*packet).contents()) + 1);
                    if (Creature* pCreature = me->GetMap()->GetCreature(victimGuid))
                    {
                        pCreature->m_Events.AddLambdaEventAtOffset([pCreature, guid = me->GetGUID()]()
                        {
                            if (pCreature->loot.roundRobinPlayer == guid)
                            {
                                // unassign loot from bot so real players can loot
                                pCreature->loot.roundRobinPlayer = 0;
                                pCreature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
                            }
                        }, 1);
                    }
                }
            }
            
            return;
        }
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void PartyBotAI::OnPlayerLogin()
{
    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);

    // A new companion is in the database from the start, so one sent away
    // before it is dressed comes back and levels up from there.
    if (m_companion && m_race && m_class)
    {
        me->EnableSaving();
        me->SaveToDB();
    }
}

void PartyBotAI::UpdateAI(uint32 const diff)
{
    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(PB_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported())
        return;

    if (m_initialized && me->IsAlive() && UnstickIfFloating())
        return;

    if (!m_initialized)
    {
        AddToPlayerGroup();

        if (m_race && m_class) // temporary character
        {
            if (m_level && m_level != me->GetLevel())
            {
                me->GiveLevel(m_level);
                me->InitTalentForLevel();
                me->SetUInt32Value(PLAYER_XP, 0);
            }

            if (!m_cloneGuid.IsEmpty())
            {
                CloneFromPlayer(sObjectAccessor.FindPlayer(m_cloneGuid));
                AutoAssignRole();
            }
            else
            {
                LearnPremadeSpecForClass();

                if (m_role == ROLE_INVALID)
                    AutoAssignRole();

                AutoEquipGear(sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_AUTO_EQUIP));

                // fix client bug causing some item slots to not be visible
                if (Player* pLeader = GetPartyLeader())
                {
                    me->SetVisibility(VISIBILITY_OFF);
                    pLeader->UpdateVisibilityOf(pLeader, me);
                    me->SetVisibility(VISIBILITY_ON);
                }
            }
            me->UpdateSkillsToMaxSkillsForLevel();

            if (m_companion)
                me->SaveToDB();
        }
        else // loaded from db
        {
            if (m_role == ROLE_INVALID)
                AutoAssignRole();

            if (me->IsGameMaster())
                me->SetGameMaster(false);

            me->TeleportTo(m_mapId, m_x, m_y, m_z, m_o);
        }
        // Companions only level with their leader.
        if (m_companion)
            me->SetPersonalXpRate(0.0f);

        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        EnsureCombatPotions();
        EnsureDefaultMarks();
        if (me->GetClass() == CLASS_HUNTER)
            AddHunterAmmo();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        m_initialized = true;
        return;
    }

    if (m_resetSpellData)
    {
        ResetSpellData();
        PopulateSpellData();
        m_resetSpellData = false;
    }

    Player* pLeader = GetPartyLeader();
    if (!pLeader)
    {
        botEntry->requestRemoval = true;
        return;
    }

    if (!pLeader->IsInWorld())
        return;

    if (m_companion && pLeader->GetLevel() > me->GetLevel() && !me->IsInCombat())
        LevelUpTo(pLeader->GetLevel());

    if (pLeader->InBattleGround() &&
        !me->InBattleGround())
    {
        if (m_receivedBgInvite)
        {
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
            return;
        }

        // Remain idle until we can join battleground.
        return;
    }

    if (pLeader->IsTaxiFlying())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return;
    }

    if (me->HasUnitState(UNIT_STATE_FEIGN_DEATH) && me->HasAuraType(SPELL_AURA_FEIGN_DEATH) &&
       !me->IsInCombat() && (!me->GetPet() || !me->GetPet()->IsInCombat()) &&
       !me->SelectRandomUnfriendlyTarget(nullptr, 20.0f, false, true))
        me->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        bool const wasFeared = me->HasAuraType(SPELL_AURA_MOD_FEAR) ||
            me->HasAuraType(SPELL_AURA_MOD_CHARM) ||
            me->HasAuraType(SPELL_AURA_MOD_CONFUSE);
        BreakCrowdControlEffects();
        // Tank just broke fear: snap threat for a few seconds so adds do not chew the healer.
        if (wasFeared && m_role == ROLE_TANK)
            m_snapThreatUntil = WorldTimer::getMSTime() + 4000;
        return;
    }

    if (me->IsDead())
    {
        if (me->InBattleGround())
        {
            if (me->GetDeathState() == CORPSE)
            {
                me->BuildPlayerRepop();
                me->RepopAtGraveyard();
            }
        }
        else if (me->GetDeathState() == DEAD)
        {
            // Already a ghost (released outside a dungeon). Stand up where we are.
            me->ResurrectPlayer(0.5f);
            me->SpawnCorpseBones();
            me->CastSpell(me, PB_SPELL_HONORLESS_TARGET, true);
            m_corpseSeconds = 0;
        }
        else if (me->IsRessurectRequested())
        {
            // A resurrect spell is waiting on an accept. The packet handler accepts it.
        }
        else
        {
            // Full wipe in a five-player dungeon: DungeonBotMgr teleports the
            // party to the last safe spot. Standing up here would re-pull.
            bool partyWipe = false;
            if (me->GetMap() && me->GetMap()->IsNonRaidDungeon())
            {
                partyWipe = true;
                if (Group* pGroup = me->GetGroup())
                {
                    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        if (Player* pMember = itr->getSource())
                            if (pMember->GetMap() == me->GetMap() && pMember->IsAlive())
                            {
                                partyWipe = false;
                                break;
                            }
                    }
                }
            }

            bool memberInCombat = false;
            if (!partyWipe)
            {
                if (Group* pGroup = me->GetGroup())
                {
                    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        if (Player* pMember = itr->getSource())
                            if (pMember != me && pMember->IsInCombat())
                                memberInCombat = true;
                    }
                }
            }

            // The leader released and is at the graveyard outside. Standing up
            // here would teleport us out to them (the follow catch-up).
            bool const leaderGhostOutside = pLeader && !pLeader->IsAlive() && pLeader->GetMapId() != me->GetMapId();

            if (partyWipe || memberInCombat || leaderGhostOutside)
                m_corpseSeconds = 0;
            else
            {
                ++m_corpseSeconds;
                // Someone who knows a rez spell gets time to walk over and cast.
                // A healer class with no such spell (a balance druid) does not count.
                uint32 const wait = AllyWillResurrect() ? 45 : 5;
                if (m_corpseSeconds >= wait)
                {
                    me->ResurrectPlayer(0.5f);
                    me->SpawnCorpseBones();
                    me->CastSpell(me, PB_SPELL_HONORLESS_TARGET, true);
                    m_corpseSeconds = 0;
                }
            }
        }

        return;
    }

    m_corpseSeconds = 0;

    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        // Stop auto shot if no target.
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
            return;
        }
        else if (me->GetClass() == CLASS_PRIEST && GetRole() == ROLE_HEALER)
        {
            // Wand is autorepeat: drop it when someone needs a heal/shield so that
            // UpdateInCombatAI_Priest can act. Otherwise keep shooting.
            // Weakened Soul makes the self immune to MECHANIC_SHIELD: without that
            // check CanTryToCastSpell stays true after the absorb falls off, and the
            // wand is interrupted every tick while DoCastSpell keeps failing.
            bool const canSelfShield = m_spells.priest.pPowerWordShield &&
                !me->IsImmuneToMechanic(MECHANIC_SHIELD) &&
                CanTryToCastSpell(me, m_spells.priest.pPowerWordShield);
            bool const needCare =
                canSelfShield ||
                SelectShieldTarget() ||
                SelectHealTarget(60.0f, 80.0f) ||
                SelectPeriodicHealTarget(80.0f, 90.0f);
            if (needCare)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                return;
        }
        else
            return;
    }

    if (Spell* pCurrentSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        // Interrupt pre casted heals if target is not injured.
        if (pCurrentSpell->getState() == SPELL_STATE_PREPARING &&
            pCurrentSpell->m_spellInfo->IsHealSpell())
        {
            if (Unit* pTarget = pCurrentSpell->m_targets.getUnitTarget())
            {
                if (pTarget->GetHealth() == pTarget->GetMaxHealth())
                {
                    me->InterruptSpell(CURRENT_GENERIC_SPELL, true);
                }
            }
        }
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    if (!me->IsInCombat())
    {
        // Before drinking or following: a corpse does not come to us.
        if (MoveToResurrect())
            return;

        if (DrinkAndEat())
        {
            if (!me->IsWithinDistInMap(pLeader, 100.0f))
            {
                me->SetHealth(me->GetMaxHealth());
                if (me->GetPowerType() == POWER_MANA)
                    me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
            }
            else if (me->IsMounted())
                me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
            return;
        }

        // Teleport to leader if too far away.
        if (!me->IsWithinDistInMap(pLeader, 100.0f) && !IsInDuel())
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            char name[128] = {};
            snprintf(name, sizeof(name), "%s", pLeader->GetName());
            ChatHandler(me).HandleGonameCommand(name);
            return;
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    // Ranged pull in progress: tank holds until the mob is nearly at the puller.
    if (UpdateRangedPullHold())
    {
        if (!me->IsMoving() &&
            me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            me->GetMotionMaster()->MoveFollow(pLeader, urand(PB_MIN_FOLLOW_DIST, PB_MAX_FOLLOW_DIST),
                frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
        return;
    }

    // Standing still with a far mob selected: hunter / mage opens, tank waits nearby.
    if (!me->IsInCombat() && !me->IsMounted() && !PartyNeedsRest() && TryAssistLeaderPull(pLeader))
        return;

    if (!me->IsInCombat() && !me->IsMounted())
    {
        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        if (me->IsNonMeleeSpellCasted())
            return;
    }

    Unit* pVictim = me->GetVictim();

    // Calm mode: do not run at a far pack — release and wait by the leader.
    // Leader-committed targets pass CanEngageTarget and are not released.
    if (pVictim && IsValidHostileTarget(pVictim) && !CanEngageTarget(pVictim))
    {
        me->AttackStop(true);
        if (!me->IsStopped())
            me->StopMoving();
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        pVictim = nullptr;
    }

    // Healer still drinking: nobody opens a fresh pull.
    if (!me->IsInCombat() && PartyNeedsRest())
    {
        if (pVictim)
        {
            me->AttackStop(true);
            pVictim = nullptr;
        }
        if (!me->IsMoving() &&
            me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            me->GetMotionMaster()->MoveFollow(pLeader, urand(PB_MIN_FOLLOW_DIST, PB_MAX_FOLLOW_DIST),
                frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
        return;
    }

    // Priests (including healers) assist so they can wand when nothing needs a heal.
    // Other healers stay on heal-only targeting.
    if (GetRole() != ROLE_HEALER || me->GetClass() == CLASS_PRIEST)
    {
        if (!pVictim || !IsValidHostileTarget(pVictim))
        {
            if (pVictim)
                me->AttackStop();

            if (Unit* pNew = SelectAttackTarget(pLeader))
            {
                if (CanEngageTarget(pNew) && AttackStart(pNew))
                    return;
            }
        }
    }

    if (!me->IsInCombat())
    {
        // Mount if leader is mounted and we don't have a target.
        if (pLeader->IsMounted() && !me->GetVictim())
        {
            if (!me->IsMounted())
            {
                // Leave shapeshift before mounting.
                if (me->IsInDisallowedMountForm() &&
                    me->GetDisplayId() != me->GetNativeDisplayId() &&
                    me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
                    me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

                auto auraList = pLeader->GetAurasByType(SPELL_AURA_MOUNTED);
                if (!auraList.empty())
                {
                    bool oldStateCastTime = me->HasCheatOption(PLAYER_CHEAT_NO_CAST_TIME);
                    bool oldStatePower = me->HasCheatOption(PLAYER_CHEAT_NO_POWER);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, true);
                    me->CastSpell(me, (*auraList.begin())->GetId(), true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, oldStateCastTime);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, oldStatePower);
                }
            }
        }
        else if (me->IsMounted())
            me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    }

    pVictim = me->GetVictim();
    if (!me->IsMoving())
    {
        if (!pVictim)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(pLeader, urand(PB_MIN_FOLLOW_DIST, PB_MAX_FOLLOW_DIST), frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
        }
        else if (CanEngageTarget(pVictim))
        {
            // SetInFront alone is server-only; without MSG_MOVE_SET_FACING others still see
            // the bot sideways after chase stops (ranged stand-and-shoot, or a mob that
            // circled). Same pattern as BattleBotAI.
            if (!me->HasInArc(pVictim, 2 * M_PI_F / 3))
            {
                me->SetInFront(pVictim);
                me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
            }

            if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
               (GetRole() == ROLE_MELEE_DPS || m_role == ROLE_TANK) &&
                IsValidHostileTarget(pVictim) &&
                AttackStart(pVictim))
                return;

            switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            {
                case IDLE_MOTION_TYPE:
                case FOLLOW_MOTION_TYPE:
                    // Ranged: only close the gap when out of shot range. Inside 8–30 yd they
                    // should stand and shoot (class AI stops chase); do not charge to melee.
                    if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
                        IsRangedDamageClass(me->GetClass()) &&
                        !IsAttackSpeedOverridenForm(me->GetShapeshiftForm()))
                    {
                        if (me->GetCombatDistance(pVictim) > 30.0f)
                            BeginChasing(pVictim);
                    }
                    else
                        BeginChasing(pVictim);
                    break;
            }
        }
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}


void PartyBotAI::UpdateOutOfCombatAI()
{
    if (!IsInDuel())
    {
        if (m_role != ROLE_TANK && me->GetVictim() && CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

void PartyBotAI::UpdateInCombatAI()
{
    if (!IsInDuel())
    {
        if (m_role == ROLE_TANK)
        {
            Unit* pVictim = me->GetVictim();

            // Defend party members — but stick to the current target. The old
            // "always SelectPartyAttackTarget while tanking" swapped every tick
            // in a multi-pull and the warrior ran between every add.
            if (!pVictim)
            {
                if (Unit* pLoose = SelectPartyAttackTarget())
                    if (CanEngageTarget(pLoose))
                        AttackStart(pLoose);
            }
            else if (pVictim->GetVictim() == me)
            {
                if (Unit* pPeel = SelectPartyPeelTarget())
                {
                    if (CanEngageTarget(pPeel))
                    {
                        me->AttackStop(true);
                        AttackStart(pPeel);
                    }
                }
            }

            // Player (or a bot) sheeped an add: drag the rest clear so AoE does not open it.
            bool const dragging = PullFightAwayFromCrowdControl();

            // Far open: walk the pack back to the party so casters keep LoS at the corner.
            if (!dragging && TryDragPackToParty())
                return;

            // Elite/boss: stand opposite the party so cleave hits the wall.
            if (!dragging && TryTankFaceAwayFromParty())
                return;

            // Post-fear / multi-pull: taunt every loose mob, not only the current target.
            bool const snapWindow = m_snapThreatUntil &&
                WorldTimer::getMSTime() < m_snapThreatUntil;
            if (snapWindow || dragging)
            {
                if (TryTankSnapThreat())
                    return;
            }

            // Taunt current target if it is attacking someone else.
            pVictim = me->GetVictim();
            if (pVictim && pVictim->GetVictim() != me)
            {
                for (const auto& pSpellEntry : m_spellListTaunt)
                {
                    if (CanTryToCastSpell(pVictim, pSpellEntry))
                    {
                        if (DoCastSpell(pVictim, pSpellEntry) == SPELL_CAST_OK)
                            return;
                    }
                }
            }

            if (dragging)
                return;
        }
        else if (CrowdControlMarkedTargets())
            return;
    }

    // Survive first: potion / stone before the next swing or cast.
    {
        bool const crowded = me->GetAttackers().size() >= 2;
        float const hpLine = crowded ? 55.0f : 40.0f;
        if (me->GetHealthPercent() < hpLine && UseCombatConsumable(SPELL_EFFECT_HEAL))
            return;

        Unit* pVictim = me->GetVictim();
        bool const ending = !crowded && pVictim && pVictim->GetHealthPercent() < 15.0f &&
            me->GetHealthPercent() >= hpLine;
        float const manaLine = crowded ? 40.0f : 30.0f;
        if (!ending && me->GetPowerType() == POWER_MANA &&
            me->GetPowerPercent(POWER_MANA) < manaLine &&
            UseCombatConsumable(SPELL_EFFECT_ENERGIZE))
            return;
    }

    // Step out while a hostile AoE is still casting — before the floor drops.
    if (TryScatterFromDangerousCast())
        return;

    // Get out of Rain of Fire / Blizzard / Consecration before the next cast.
    if (TryAvoidGroundDamage())
        return;

    // Herod-style Whirlwind: melee leave the spin, then resume.
    if (TryKiteWhirlwind())
        return;

    // Free Action / Limited Invuln when rooted or nearly dead.
    if (TryUtilityConsumable())
        return;

    // Healer stays in heal range of the tank; ranged unstack from cleave.
    if (TryHealerStackWithTank())
        return;
    if (TryRangedSpread())
        return;

    // Ranged / healer: leave melee (root/slow, then step) before the rotation GCD.
    if (TryKeepRangedDistance())
        return;

    // Dump threat before the next damaging GCD (Fade / Feign / Cower / …).
    if (TryThreatDump())
        return;

    // Pet on the kill target; casters step out when a wall ate their last cast.
    if (Unit* pVictim = me->GetVictim())
    {
        EnsurePetAttacks(pVictim);
        if (TryRegainLineOfSight(pVictim))
        {
            // Keep moving this tick; spells stay gated by LOS in CanTryToCastSpell.
        }
    }

    // Fear Ward / racial damage CD before the rotation spends the GCD.
    if (TryFearWard())
        return;
    if (TryRacialCombatAbility(me->GetVictim()))
        return;

    // Dry healer first; battle-rez a dead healer/tank while the fight is still up.
    if (TryInnervate())
        return;
    if (TryBattleResurrection())
        return;

    // Tank (or designated bot) keeps skull on the kill target and moon on sheep.
    UpdateAutoRaidMarks();

    // Before rotation / healing: stop enemy heals and other casts (own target or an add).
    if (TryInterruptEnemyCast())
        return;

    // Someone is dying: healers skip silence / sheep / friendly dispel and go triage.
    bool const triage = GetRole() == ROLE_HEALER && PartyMemberBelow(40.0f);

    if (!triage)
    {
        // Ranged / caster packs: Silence when we have it, even between their casts.
        if (TrySilenceRangedEnemy())
            return;

        // Multi-pull: park one add so healers and tanks are not flooded.
        if (m_role != ROLE_TANK && TryCrowdControlExtra())
            return;

        if (CheckForDispelTargets())
            return;

        // Strip enemy magic buffs (Power Word: Shield, enrage auras, …) before damage.
        if (TryStripEnemyBuffs())
            return;

        // Multi-DoT spare adds once the kill target already has its DoT.
        if (TrySpreadDots())
            return;

        // Paladin: free a rooted tank / healer before the rotation GCD.
        if (TryBlessingOfFreedom())
            return;
    }

    // About to rip aggro: healers may still heal; DPS hold the next damage spell.
    if (ShouldHoldFire() && GetRole() != ROLE_HEALER)
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();
}

Player* PartyBotAI::SelectFearWardTarget() const
{
    if (!m_spells.priest.pFearWard)
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return (me->IsAlive() && CanTryToCastSpell(me, m_spells.priest.pFearWard)) ? me : nullptr;

    Player* pTank = nullptr;
    Player* pLeader = GetPartyLeader();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive() || pMember->GetMap() != me->GetMap())
            continue;
        if (pMember->GetDistance(me) > 30.0f)
            continue;
        if (PartyBotAI const* pAI = dynamic_cast<PartyBotAI const*>(pMember->AI()))
        {
            if (pAI->m_role == ROLE_TANK)
            {
                pTank = pMember;
                break;
            }
        }
    }

    Player* pPick = pTank ? pTank : pLeader;
    if (!pPick)
        pPick = me;
    if (!pPick->IsAlive())
        return nullptr;
    // Already warded.
    if (pPick->HasAura(m_spells.priest.pFearWard->Id))
        return nullptr;
    return pPick;
}

bool PartyBotAI::TryFearWard()
{
    if (me->GetClass() != CLASS_PRIEST || !m_spells.priest.pFearWard)
        return false;
    if (me->GetShapeshiftForm() != FORM_NONE)
        return false;

    // Mid-fight: only spend the GCD when the table flags fear, or OOC pre-buff.
    uint32 const encFlags = GetEncounterFlags();
    if (me->IsInCombat() && !(encFlags & PB_ENC_FEAR_WARD))
    {
        // Still refresh if the tank is already feared / about to be.
        Unit* pVictim = me->GetVictim();
        if (!pVictim || !IsDangerousFight(pVictim))
            return false;
    }

    Player* pTarget = SelectFearWardTarget();
    if (!pTarget || !CanTryToCastSpell(pTarget, m_spells.priest.pFearWard))
        return false;
    return DoCastSpell(pTarget, m_spells.priest.pFearWard) == SPELL_CAST_OK;
}

Player* PartyBotAI::SelectInnervateTarget() const
{
    if (!m_spells.druid.pInnervate)
        return nullptr;

    Group* pGroup = me->GetGroup();
    Player* pBest = nullptr;
    float bestMana = 100.0f;

    auto consider = [&](Player* pMember)
    {
        if (!pMember || !pMember->IsAlive() || pMember->GetMap() != me->GetMap())
            return;
        if (pMember->GetPowerType() != POWER_MANA)
            return;
        if (pMember->GetDistance(me) > 30.0f)
            return;
        if (pMember->HasAura(m_spells.druid.pInnervate->Id))
            return;
        float const mana = pMember->GetPowerPercent(POWER_MANA);
        // Healers first when dry; otherwise only help someone critically low.
        bool const healer = IsPartyHealer(pMember) || pMember == me;
        float const line = healer ? 35.0f : 10.0f;
        if (mana >= line)
            return;
        float score = mana;
        if (healer)
            score -= 40.0f;
        if (score < bestMana)
        {
            bestMana = score;
            pBest = pMember;
        }
    };

    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            consider(itr->getSource());
    }
    else
        consider(me);

    return pBest;
}

bool PartyBotAI::TryInnervate()
{
    if (me->GetClass() != CLASS_DRUID || !m_spells.druid.pInnervate)
        return false;
    // Cat/bear cannot Innervate; leave form only when we are not the tank mid-peel.
    if (me->GetShapeshiftForm() != FORM_NONE && me->GetShapeshiftForm() != FORM_MOONKIN)
    {
        if (m_role == ROLE_TANK)
            return false;
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
    }

    Player* pTarget = SelectInnervateTarget();
    if (!pTarget || !CanTryToCastSpell(pTarget, m_spells.druid.pInnervate))
        return false;
    return DoCastSpell(pTarget, m_spells.druid.pInnervate) == SPELL_CAST_OK;
}

bool PartyBotAI::TryBattleResurrection()
{
    if (IsInDuel() || !me->IsInCombat())
        return false;

    SpellEntry const* pRez = m_spells.druid.pRebirth;
    if (!pRez || me->GetClass() != CLASS_DRUID)
        return false;
    if (!me->IsSpellReady(pRez))
        return false;

    // Healers leave form; tanks keep bear unless nobody else can rez.
    if (me->GetShapeshiftForm() != FORM_NONE && me->GetShapeshiftForm() != FORM_MOONKIN)
    {
        if (m_role == ROLE_TANK && GetRole() != ROLE_HEALER)
            return false;
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
    }

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    // Prefer healer > tank > others. Skip if someone is already rezzing them.
    Player* pBest = nullptr;
    int bestScore = -1;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || pMember->GetDeathState() != CORPSE)
            continue;
        if (pMember->GetMapId() != me->GetMapId() || !me->IsWithinDist(pMember, 40.0f))
            continue;

        bool someoneRezzing = false;
        for (GroupReference* jtr = pGroup->GetFirstMember(); jtr != nullptr; jtr = jtr->next())
        {
            Player* pOther = jtr->getSource();
            if (!pOther || pOther == me || !pOther->IsAlive())
                continue;
            if (pOther->IsNonMeleeSpellCasted(false, false, true) &&
                pOther->GetTargetGuid() == pMember->GetObjectGuid())
            {
                someoneRezzing = true;
                break;
            }
        }
        if (someoneRezzing)
            continue;

        int score = 10;
        if (IsPartyHealer(pMember))
            score = 100;
        else if (IsPartyTank(pMember))
            score = 80;
        else if (pMember->GetObjectGuid() == m_leaderGuid)
            score = 90;
        if (score > bestScore)
        {
            bestScore = score;
            pBest = pMember;
        }
    }

    if (!pBest || !CanTryToCastSpell(pBest, pRez))
        return false;
    if (!pRez->IsTargetInRange(me, pBest) || !me->IsWithinLOSInMap(pBest))
        return false;
    return DoCastSpell(pBest, pRez) == SPELL_CAST_OK;
}

bool PartyBotAI::TryApplyWarlockCurse(Unit* pVictim)
{
    if (!pVictim || me->GetClass() != CLASS_WARLOCK)
        return false;

    SpellEntry const* pCurse = nullptr;
    bool const elite = pVictim->IsCreature() &&
        (static_cast<Creature*>(pVictim)->IsElite() ||
         static_cast<Creature*>(pVictim)->IsWorldBoss());

    // Caster packs: Tongues slows their casts more than Agony ticks help.
    if (IsRangedEnemy(pVictim) && m_spells.warlock.pCurseofTongues)
        pCurse = m_spells.warlock.pCurseofTongues;
    // Elites / bosses: raid curse for the whole party.
    else if (elite && m_spells.warlock.pCurseoftheElements)
        pCurse = m_spells.warlock.pCurseoftheElements;
    else if (elite && m_spells.warlock.pCurseofShadow)
        pCurse = m_spells.warlock.pCurseofShadow;
    // Armored melee trash: Reck raises physical damage taken.
    else if (IsMeleeWeaponClass(pVictim->GetClass()) &&
             m_spells.warlock.pCurseofRecklessness &&
             pVictim->GetHealthPercent() > 40.0f)
        pCurse = m_spells.warlock.pCurseofRecklessness;
    else if (m_spells.warlock.pCurseofAgony)
        pCurse = m_spells.warlock.pCurseofAgony;

    if (!pCurse || pVictim->HasAura(pCurse->Id))
        return false;
    if (!CanTryToCastSpell(pVictim, pCurse))
        return false;
    return DoCastSpell(pVictim, pCurse) == SPELL_CAST_OK;
}

bool PartyBotAI::TryCrowdControlExtra()
{
    if (IsInDuel())
        return false;

    SpellEntry const* pCC = GetCrowdControlSpell();
    if (!pCC)
        return false;

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return false;

    // Need a real multi-pull before parking an add.
    uint32 hostiles = me->GetAttackers().size();
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
                hostiles = std::max<uint32>(hostiles, pMember->GetAttackers().size());
        }
    }
    if (hostiles < 2)
        return false;

    Unit* pBest = nullptr;
    float bestScore = -1.0f;
    auto consider = [&](Unit* pUnit)
    {
        if (!pUnit || pUnit == pVictim || !IsValidHostileTarget(pUnit))
            return;
        if (pUnit->HasBreakableByDamageCrowdControlAura() || pUnit->GetHealthPercent() <= 20.0f)
            return;
        if (pCC->TargetCreatureType && !(pCC->TargetCreatureType & pUnit->GetCreatureTypeMask()))
            return;
        if (pCC->GetMechanic() && pUnit->IsImmuneToMechanic(Mechanics(pCC->GetMechanic())))
            return;
        if (!CanTryToCastSpell(pUnit, pCC) || !CanUseCrowdControl(pCC, pUnit))
            return;
        // Casters first (priest healers, ranged packs), then the healthiest spare.
        float score = pUnit->GetHealthPercent();
        if (IsRangedEnemy(pUnit))
            score += 100.0f;
        if (pUnit->IsNonMeleeSpellCasted(false, false, true))
            score += 50.0f;
        if (score > bestScore)
        {
            pBest = pUnit;
            bestScore = score;
        }
    };

    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || pMember == me)
                continue;
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
    }

    if (!pBest)
        return false;
    if (DoCastSpell(pBest, pCC) == SPELL_CAST_OK)
    {
        me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
        return true;
    }
    return false;
}

bool PartyBotAI::ShouldHoldFire() const
{
    if (m_role == ROLE_TANK || IsInDuel())
        return false;
    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return false;
    // About to rip: stop damaging so the tank can catch up.
    return IsNearTankThreat(pVictim, 0.90f);
}

bool PartyBotAI::TryThreatDump()
{
    if (m_role == ROLE_TANK || IsInDuel())
        return false;

    Unit* pVictim = me->GetVictim();
    bool const highThreat = pVictim && IsNearTankThreat(pVictim, 0.80f);
    bool const gotAggro = !me->GetAttackers().empty();
    if (!highThreat && !gotAggro)
        return false;

    // Only dump when we are in real danger of a rip or already tanking.
    if (!gotAggro && !IsNearTankThreat(pVictim, 0.85f))
        return false;

    switch (me->GetClass())
    {
        case CLASS_PRIEST:
            if (m_spells.priest.pFade && CanTryToCastSpell(me, m_spells.priest.pFade))
                return DoCastSpell(me, m_spells.priest.pFade) == SPELL_CAST_OK;
            break;
        case CLASS_HUNTER:
            if (m_spells.hunter.pFeignDeath && CanTryToCastSpell(me, m_spells.hunter.pFeignDeath))
                return DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK;
            break;
        case CLASS_ROGUE:
            if (gotAggro && m_spells.rogue.pVanish && CanTryToCastSpell(me, m_spells.rogue.pVanish))
                return DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK;
            if (gotAggro && m_spells.rogue.pEvasion && CanTryToCastSpell(me, m_spells.rogue.pEvasion))
                return DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK;
            break;
        case CLASS_DRUID:
            if (me->GetShapeshiftForm() == FORM_CAT && m_spells.druid.pCower &&
                CanTryToCastSpell(me, m_spells.druid.pCower))
                return DoCastSpell(me, m_spells.druid.pCower) == SPELL_CAST_OK;
            break;
        case CLASS_WARLOCK:
            // No Fade; Death Coil the mob on us as an emergency peel.
            if (gotAggro && m_spells.warlock.pDeathCoil)
            {
                Unit* pAttacker = *me->GetAttackers().begin();
                if (pAttacker && CanTryToCastSpell(pAttacker, m_spells.warlock.pDeathCoil))
                    return DoCastSpell(pAttacker, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK;
            }
            break;
        default:
            break;
    }
    return false;
}

bool PartyBotAI::PartyMemberBelow(float hpPercent) const
{
    if (me->GetHealthPercent() < hpPercent)
        return true;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive() || pMember->IsGameMaster())
            continue;
        if (pMember->GetHealthPercent() < hpPercent)
            return true;
    }
    return false;
}

bool PartyBotAI::TryStripEnemyBuffs()
{
    SpellEntry const* pPurge = nullptr;
    switch (me->GetClass())
    {
        case CLASS_SHAMAN:
            pPurge = m_spells.shaman.pPurge;
            break;
        case CLASS_PRIEST:
            pPurge = m_spells.priest.pDispelMagic;
            break;
        default:
            return false;
    }
    if (!pPurge)
        return false;

    Unit* pBest = nullptr;
    int bestScore = 0;
    auto consider = [&](Unit* pEnemy)
    {
        if (!pEnemy || !IsValidHostileTarget(pEnemy))
            return;
        if (!CanTryToCastSpell(pEnemy, pPurge))
            return;
        int const score = GetEnemyBuffStripScore(pEnemy, pPurge);
        if (score > bestScore)
        {
            bestScore = score;
            pBest = pEnemy;
        }
    };

    consider(me->GetVictim());
    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember)
                continue;
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
    }

    if (!pBest || bestScore <= 0)
        return false;
    return DoCastSpell(pBest, pPurge) == SPELL_CAST_OK;
}

bool PartyBotAI::TryPrePullConsumable(Unit* pTarget)
{
    if (!pTarget || IsInDuel())
        return false;
    // Only on hard opens — trash does not deserve a Major Mana Potion.
    if (!IsDangerousFight(pTarget) &&
        !(pTarget->IsCreature() && static_cast<Creature*>(pTarget)->IsElite()))
        return false;
    if (m_prePotUntil && WorldTimer::getMSTime() < m_prePotUntil)
        return false;

    bool used = false;
    // Pre-pot mana so the first GCD is full (healers and casters).
    if (me->GetPowerType() == POWER_MANA &&
        me->GetPowerPercent(POWER_MANA) < 90.0f &&
        UseCombatConsumable(SPELL_EFFECT_ENERGIZE))
        used = true;

    // Lock out for a minute so the next tick does not drain the bag.
    m_prePotUntil = WorldTimer::getMSTime() + 60 * IN_MILLISECONDS;
    return used;
}

bool PartyBotAI::TryScatterFromDangerousCast()
{
    // Tank holds the pack in place; DPS/healers leave the blast radius.
    if (m_role == ROLE_TANK || IsInDuel())
        return false;

    Unit* pDanger = nullptr;
    float safeDist = 0.0f;

    auto consider = [&](Unit* pEnemy)
    {
        if (!pEnemy || !pEnemy->IsAlive() || !pEnemy->IsNonMeleeSpellCasted(false, false, true))
            return;
        if (!IsValidHostileTarget(pEnemy) && !pEnemy->IsValidAttackTarget(me))
            return;

        Spell const* pSpell = pEnemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!pSpell)
            pSpell = pEnemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!pSpell || !pSpell->m_spellInfo)
            return;

        SpellEntry const* info = pSpell->m_spellInfo;
        if (info->IsPositiveSpell() || !info->IsAreaOfEffectSpell())
            return;

        bool damaging = false;
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE ||
                info->Effect[i] == SPELL_EFFECT_PERSISTENT_AREA_AURA ||
                info->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
                info->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
            {
                damaging = true;
                break;
            }
        }
        if (!damaging)
            return;

        float radius = 10.0f;
        if (info->EffectRadiusIndex[0])
            radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(info->EffectRadiusIndex[0]));
        else if (info->EffectRadiusIndex[1])
            radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(info->EffectRadiusIndex[1]));
        else if (info->EffectRadiusIndex[2])
            radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(info->EffectRadiusIndex[2]));

        // Already outside the blast — nothing to do.
        if (me->GetDistance(pEnemy) > radius + 1.0f)
            return;

        // Prefer the biggest blast still overlapping us.
        float const need = radius + 4.0f;
        if (!pDanger || need > safeDist)
        {
            pDanger = pEnemy;
            safeDist = need;
        }
    };

    consider(me->GetVictim());
    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember)
                continue;
            consider(pMember->GetVictim());
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
    }

    if (!pDanger)
        return false;

    if (me->IsNonMeleeSpellCasted())
        me->InterruptNonMeleeSpells(false);

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        return true;

    return me->GetMotionMaster()->MoveDistance(pDanger, safeDist);
}

bool PartyBotAI::TryAvoidGroundDamage()
{
    Unit* pFrom = nullptr;
    auto scan = [&](Unit::AuraList const& list)
    {
        for (Aura* aura : list)
        {
            if (!aura)
                continue;
            Unit* pCaster = aura->GetCaster();
            if (!pCaster || !pCaster->IsAlive() || pCaster->IsFriendlyTo(me))
                continue;
            pFrom = pCaster;
            return;
        }
    };
    scan(me->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE));
    if (!pFrom)
        scan(me->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT));
    if (!pFrom)
        return false;

    // Tank holds for threat unless the floor is eating them alive.
    if (m_role == ROLE_TANK && me->GetHealthPercent() > 40.0f)
        return false;

    if (me->IsNonMeleeSpellCasted())
        me->InterruptNonMeleeSpells(false);

    // Already stepping out.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        return true;

    // Short step — same dungeon-safe MoveDistance as RunAwayFromTarget.
    return me->GetMotionMaster()->MoveDistance(pFrom, 10.0f);
}

bool PartyBotAI::TryHealerStackWithTank()
{
    if (m_role != ROLE_HEALER || IsInDuel() || !me->IsInCombat())
        return false;
    // Never yank a heal mid-cast for soft spacing.
    if (me->IsNonMeleeSpellCasted() || PartyMemberBelow(50.0f))
        return false;
    if (m_repositionUntil && WorldTimer::getMSTime() < m_repositionUntil)
        return false;

    Player* pTank = FindPartyTank();
    if (!pTank || pTank == me)
        return false;

    uint32 const encFlags = GetEncounterFlags();
    bool const stackHard = (encFlags & PB_ENC_STACK_TANK) != 0;
    // Wide dead zone — only correct when clearly wrong, not every 500ms.
    float const minDist = stackHard ? 2.0f : 3.0f;
    float const maxDist = stackHard ? 10.0f : 28.0f;

    float const dist = me->GetDistance(pTank);
    if (dist >= minDist && dist <= maxDist)
        return false;

    MovementGeneratorType const mt = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (mt == DISTANCING_MOTION_TYPE || mt == POINT_MOTION_TYPE || mt == CHASE_MOTION_TYPE)
        return false;

    if (dist < minDist)
    {
        // Only step off the tank when standing in cleave (or soak asks for a band).
        if (!stackHard && pTank->GetAttackers().empty())
            return false;
        if (me->GetMotionMaster()->MoveDistance(pTank, stackHard ? 6.0f : 10.0f))
            m_repositionUntil = WorldTimer::getMSTime() + 3 * IN_MILLISECONDS;
        return false;
    }

    // Out of heal range — walk in, but keep this tick's heal/GCD free.
    BeginChasing(pTank);
    m_repositionUntil = WorldTimer::getMSTime() + 3 * IN_MILLISECONDS;
    return false;
}

bool PartyBotAI::TryRangedSpread()
{
    // Healers do not unstack by default — it fights TryHealerStackWithTank.
    if (m_role != ROLE_RANGE_DPS || IsInDuel())
        return false;
    if (!me->IsInCombat() || me->IsNonMeleeSpellCasted())
        return false;
    if (m_repositionUntil && WorldTimer::getMSTime() < m_repositionUntil)
        return false;

    uint32 const encFlags = GetEncounterFlags();
    bool const spreadHard = (encFlags & PB_ENC_SPREAD) != 0;
    // Default: only when something is actually hitting us (not "2 mobs exist nearby").
    if (!spreadHard && me->GetAttackers().size() < 2)
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    float const tooClose = spreadHard ? 5.0f : 1.5f;
    float const stepOut = spreadHard ? 8.0f : 5.0f;

    Player* pTooClose = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;
        if (me->GetDistance(pMember) > tooClose)
            continue;
        // Prefer unstacking from another caster over melee.
        if (IsRangedDamageClass(pMember->GetClass()))
        {
            pTooClose = pMember;
            break;
        }
        if (!pTooClose)
            pTooClose = pMember;
    }
    if (!pTooClose)
        return false;

    // Only the lower-guid bot steps — stops two casters chasing each other.
    if (me->GetObjectGuid() > pTooClose->GetObjectGuid())
        return false;

    MovementGeneratorType const mt = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (mt == DISTANCING_MOTION_TYPE || mt == POINT_MOTION_TYPE)
        return false;

    if (me->GetMotionMaster()->MoveDistance(pTooClose, stepOut))
        m_repositionUntil = WorldTimer::getMSTime() + 3 * IN_MILLISECONDS;
    return false;
}

bool PartyBotAI::TryKeepRangedDistance()
{
    // Party ranged / healers: do not stand in melee trading hits. Root or slow,
    // then step toward the tank (or a short dungeon backstep). Bear / cat / enh
    // melee roles keep swinging.
    if (IsInDuel())
        return false;
    if (m_role != ROLE_RANGE_DPS && m_role != ROLE_HEALER)
        return false;
    if (!IsRangedDamageClass(me->GetClass()) ||
        IsAttackSpeedOverridenForm(me->GetShapeshiftForm()))
        return false;

    Unit* pEnemy = nullptr;
    float best = 999.0f;
    for (Unit* pAttacker : me->GetAttackers())
    {
        if (!pAttacker || !pAttacker->IsAlive())
            continue;
        float const d = me->GetCombatDistance(pAttacker);
        if (d < best)
        {
            best = d;
            pEnemy = pAttacker;
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (pVictim->IsAlive())
        {
            float const d = me->GetCombatDistance(pVictim);
            if (!pEnemy || d < best)
            {
                best = d;
                pEnemy = pVictim;
            }
        }
    }
    if (!pEnemy)
        return false;

    // Healers only leave when something is actually hitting them — otherwise
    // stay in heal range of the tank.
    if (m_role == ROLE_HEALER && GetAttackersInRangeCount(10.0f) == 0)
        return false;

    bool const inMelee = pEnemy->CanReachWithMeleeAutoAttack(me);
    // Auto Shot / most caster spells lose the dead zone under 8 yd; start a
    // little earlier when the mob is on us, the way a player backs up.
    bool const tooClose = best < 8.0f ||
        (best < 12.0f && pEnemy->GetVictim() == me &&
         (inMelee || GetAttackersInRangeCount(12.0f) > 0));
    if (!tooClose)
        return false;

    bool const distancing =
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE;

    // Instant (or short) tools that make the gap stick — then step.
    if (!me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED))
    {
        switch (me->GetClass())
        {
            case CLASS_MAGE:
                if (m_spells.mage.pFrostNova &&
                    !pEnemy->HasUnitState(UNIT_STATE_ROOT) &&
                    !pEnemy->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                    CanTryToCastSpell(me, m_spells.mage.pFrostNova) &&
                    DoCastSpell(me, m_spells.mage.pFrostNova) == SPELL_CAST_OK)
                    break;
                if (m_spells.mage.pBlink &&
                    (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                     me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                    CanTryToCastSpell(me, m_spells.mage.pBlink) &&
                    DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return true;
                break;
            case CLASS_HUNTER:
                if (GetAttackersInRangeCount(8.0f))
                {
                    Unit* pAttacker = *me->GetAttackers().begin();
                    if (m_spells.hunter.pScareBeast &&
                        CanTryToCastSpell(pAttacker, m_spells.hunter.pScareBeast) &&
                        CanUseCrowdControl(m_spells.hunter.pScareBeast, pAttacker) &&
                        DoCastSpell(pAttacker, m_spells.hunter.pScareBeast) == SPELL_CAST_OK)
                        return true;
                    if (m_spells.hunter.pDisengage &&
                        CanTryToCastSpell(pAttacker, m_spells.hunter.pDisengage) &&
                        DoCastSpell(pAttacker, m_spells.hunter.pDisengage) == SPELL_CAST_OK)
                        return true;
                }
                if (inMelee && m_spells.hunter.pWingClip &&
                    CanTryToCastSpell(pEnemy, m_spells.hunter.pWingClip))
                    DoCastSpell(pEnemy, m_spells.hunter.pWingClip);
                else if (m_spells.hunter.pConcussiveShot &&
                    CanTryToCastSpell(pEnemy, m_spells.hunter.pConcussiveShot) &&
                    DoCastSpell(pEnemy, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                    return true;
                break;
            case CLASS_WARLOCK:
                if (m_spells.warlock.pDeathCoil && inMelee &&
                    CanTryToCastSpell(pEnemy, m_spells.warlock.pDeathCoil) &&
                    DoCastSpell(pEnemy, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                    return true;
                if (m_spells.warlock.pHowlofTerror &&
                    GetAttackersInRangeCount(10.0f) > 1 &&
                    CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror) &&
                    DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                    return true;
                break;
            case CLASS_PRIEST:
                if (m_spells.priest.pPsychicScream &&
                    GetAttackersInRangeCount(8.0f) > 0 &&
                    CanTryToCastSpell(me, m_spells.priest.pPsychicScream) &&
                    DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                    return true;
                break;
            case CLASS_SHAMAN:
                if (m_spells.shaman.pFrostShock &&
                    CanTryToCastSpell(pEnemy, m_spells.shaman.pFrostShock) &&
                    DoCastSpell(pEnemy, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                    return true;
                break;
            case CLASS_DRUID:
                if ((me->GetShapeshiftForm() == FORM_NONE ||
                     me->GetShapeshiftForm() == FORM_MOONKIN) &&
                    m_spells.druid.pEntanglingRoots &&
                    !pEnemy->HasUnitState(UNIT_STATE_ROOT) &&
                    CanTryToCastSpell(pEnemy, m_spells.druid.pEntanglingRoots) &&
                    DoCastSpell(pEnemy, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                    return true;
                break;
            default:
                break;
        }
    }

    // Hold the kite — do not cancel it with Aimed Shot / Shadow Bolt mid-step.
    if (distancing)
        return true;

    if (me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CAN_NOT_MOVE))
        return false;

    // Healers finish the heal; DPS drop a cast only when swings are already landing.
    if (me->IsNonMeleeSpellCasted())
    {
        if (m_role == ROLE_HEALER)
            return false;
        if (!inMelee && best >= 8.0f)
            return false;
        me->InterruptNonMeleeSpells(false);
    }

    if (!me->IsStopped())
        me->StopMoving();
    me->GetMotionMaster()->Clear();
    if (RunAwayFromTarget(pEnemy))
    {
        me->SetCasterChaseDistance(25.0f);
        return true;
    }
    return false;
}

bool PartyBotAI::TryKiteWhirlwind()
{
    if (m_role == ROLE_TANK || m_role == ROLE_HEALER || IsInDuel())
        return false;
    if (!me->IsInCombat())
        return false;

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return false;

    uint32 const encFlags = PartyBotEncounter::GetFlags(pVictim);
    if (!(encFlags & PB_ENC_KITE_WHIRLWIND))
        return false;
    if (!PartyBotEncounter::HasWhirlwindAura(pVictim))
        return false;

    // Already clear of the spin.
    if (me->GetDistance(pVictim) >= 12.0f)
        return false;

    if (me->IsNonMeleeSpellCasted())
        me->InterruptNonMeleeSpells(false);

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        return true;

    // Short step — same dungeon-safe MoveDistance path as ground AoE.
    return me->GetMotionMaster()->MoveDistance(pVictim, 14.0f);
}

bool PartyBotAI::TryBlessingOfFreedom()
{
    if (me->GetClass() != CLASS_PALADIN || !m_spells.paladin.pBlessingOfFreedom)
        return false;
    if (me->GetShapeshiftForm() != FORM_NONE)
        return false;

    Player* pBest = nullptr;
    int bestScore = -1;
    auto consider = [&](Player* pMember)
    {
        if (!pMember || !pMember->IsAlive())
            return;
        bool const locked = pMember->HasUnitState(UNIT_STATE_ROOT) ||
            pMember->HasAuraType(SPELL_AURA_MOD_ROOT) ||
            pMember->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
        if (!locked)
            return;
        if (!CanTryToCastSpell(pMember, m_spells.paladin.pBlessingOfFreedom))
            return;
        int score = 10;
        if (IsPartyTank(pMember))
            score += 40;
        else if (IsPartyHealer(pMember))
            score += 25;
        else if (pMember->GetObjectGuid() == m_leaderGuid)
            score += 30;
        if (pMember == me)
            score += 5;
        if (score > bestScore)
        {
            bestScore = score;
            pBest = pMember;
        }
    };

    consider(me);
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            consider(itr->getSource());
    }
    if (!pBest)
        return false;
    return DoCastSpell(pBest, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK;
}

bool PartyBotAI::IsAssignedToDispel(Player const* pFriend, SpellEntry const* pSpellEntry) const
{
    if (!pFriend || !pSpellEntry)
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return true;

    Player* pBest = nullptr;
    float bestDist = 9999.0f;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive())
            continue;
        PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI());
        if (!pAI)
            continue;
        // Only bots that actually know this cleanse spell compete.
        if (!pMember->HasSpell(pSpellEntry->Id) || !pMember->IsSpellReady(pSpellEntry))
            continue;
        if (!pMember->IsWithinDist(pFriend, 30.0f) || !pMember->IsWithinLOSInMap(pFriend))
            continue;
        float const dist = pMember->GetDistance(pFriend);
        if (!pBest || dist < bestDist - 0.01f ||
            (std::fabs(dist - bestDist) < 0.01f && pMember->GetObjectGuid() < pBest->GetObjectGuid()))
        {
            pBest = pMember;
            bestDist = dist;
        }
    }
    return !pBest || pBest == me;
}

bool PartyBotAI::TrySpreadDots()
{
    if (m_role == ROLE_TANK || m_role == ROLE_HEALER || IsInDuel())
        return false;

    SpellEntry const* pDot = nullptr;
    switch (me->GetClass())
    {
        case CLASS_WARLOCK:
            pDot = m_spells.warlock.pCorruption;
            break;
        case CLASS_PRIEST:
            if (GetRole() != ROLE_HEALER)
                pDot = m_spells.priest.pShadowWordPain;
            break;
        case CLASS_HUNTER:
            pDot = m_spells.hunter.pSerpentSting;
            break;
        case CLASS_DRUID:
            if (m_role == ROLE_RANGE_DPS)
                pDot = m_spells.druid.pMoonfire;
            break;
        default:
            return false;
    }
    if (!pDot)
        return false;

    Unit* pMain = me->GetVictim();
    // Only spread once the kill target already has the DoT — keep focus fire first.
    if (!pMain || !pMain->HasAura(pDot->Id))
        return false;

    Unit* pBest = nullptr;
    float bestScore = -1.0f;
    auto consider = [&](Unit* pEnemy)
    {
        if (!pEnemy || pEnemy == pMain || !IsValidHostileTarget(pEnemy))
            return;
        if (pEnemy->HasBreakableByDamageCrowdControlAura())
            return;
        if (pEnemy->GetHealthPercent() < 25.0f)
            return;
        if (!me->IsWithinDist(pEnemy, 30.0f))
            return;
        if (pEnemy->HasAura(pDot->Id) || !CanTryToCastSpell(pEnemy, pDot))
            return;

        float score = pEnemy->GetHealthPercent();
        if (IsRangedEnemy(pEnemy))
            score += 20.0f;
        if (score > bestScore)
        {
            bestScore = score;
            pBest = pEnemy;
        }
    };

    for (Unit* pAttacker : me->GetAttackers())
        consider(pAttacker);

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember)
                continue;
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
            if (Unit* pVictim = pMember->GetVictim())
                if (pVictim->IsInCombat())
                    consider(pVictim);
        }
    }

    if (!pBest)
        return false;
    return DoCastSpell(pBest, pDot) == SPELL_CAST_OK;
}

void PartyBotAI::EnsureDefaultMarks()
{
    // Classic dungeon convention: skull = kill, moon = sheep / hibernate.
    if (m_marksToFocus.empty())
        m_marksToFocus.push_back(RAID_TARGET_ICON_SKULL);
    if (m_marksToCC.empty())
        m_marksToCC.push_back(RAID_TARGET_ICON_MOON);
}

bool PartyBotAI::IsAutoMarkController() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup || IsInDuel())
        return false;

    Player* pBest = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember->GetObjectGuid() == m_leaderGuid)
            continue;
        PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI());
        if (!pAI)
            continue;

        if (!pBest)
        {
            pBest = pMember;
            continue;
        }

        PartyBotAI* pBestAI = dynamic_cast<PartyBotAI*>(pBest->AI());
        bool const amTank = pAI->m_role == ROLE_TANK;
        bool const bestTank = pBestAI && pBestAI->m_role == ROLE_TANK;
        if (amTank && !bestTank)
            pBest = pMember;
        else if (amTank == bestTank && pMember->GetObjectGuid() < pBest->GetObjectGuid())
            pBest = pMember;
    }
    return pBest == me;
}

void PartyBotAI::UpdateAutoRaidMarks()
{
    if (!IsAutoMarkController())
        return;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;

    EnsureDefaultMarks();

    // Skull on the focus-fire / kill target (not a sheeped add).
    Unit* pKill = SelectFocusFireTarget();
    if (!pKill)
        pKill = me->GetVictim();
    if (pKill && IsValidHostileTarget(pKill) && !pKill->HasBreakableByDamageCrowdControlAura())
    {
        if (pGroup->GetTargetWithIcon(RAID_TARGET_ICON_SKULL) != pKill->GetObjectGuid())
            pGroup->SetTargetIcon(RAID_TARGET_ICON_SKULL, pKill->GetObjectGuid());
    }

    // Moon on an already-CCd add, else a spare caster worth parking.
    Unit* pMoon = FindBreakableCrowdControlNear(40.0f);
    if (!pMoon)
    {
        Unit* pMain = pKill ? pKill : me->GetVictim();
        Unit* pBest = nullptr;
        float bestScore = -1.0f;
        auto consider = [&](Unit* pEnemy)
        {
            if (!pEnemy || pEnemy == pMain || !IsValidHostileTarget(pEnemy))
                return;
            if (pEnemy->GetHealthPercent() < 20.0f || !me->IsWithinDist(pEnemy, 40.0f))
                return;
            float score = IsRangedEnemy(pEnemy) ? 50.0f : 10.0f;
            score += pEnemy->GetHealthPercent() * 0.1f;
            if (score > bestScore)
            {
                bestScore = score;
                pBest = pEnemy;
            }
        };
        for (Unit* pAttacker : me->GetAttackers())
            consider(pAttacker);
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember)
                continue;
            for (Unit* pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
        pMoon = pBest;
    }
    if (pMoon)
    {
        if (pGroup->GetTargetWithIcon(RAID_TARGET_ICON_MOON) != pMoon->GetObjectGuid())
            pGroup->SetTargetIcon(RAID_TARGET_ICON_MOON, pMoon->GetObjectGuid());
    }
}

bool PartyBotAI::TryTankSnapThreat()
{
    if (m_role != ROLE_TANK || IsInDuel())
        return false;

    auto tryTaunt = [&](Unit* pEnemy) -> bool
    {
        if (!pEnemy || !IsValidHostileTarget(pEnemy) || !me->IsWithinDist(pEnemy, 30.0f))
            return false;
        if (pEnemy->GetVictim() == me)
            return false;
        for (SpellEntry const* pSpellEntry : m_spellListTaunt)
        {
            if (CanTryToCastSpell(pEnemy, pSpellEntry) &&
                DoCastSpell(pEnemy, pSpellEntry) == SPELL_CAST_OK)
                return true;
        }
        return false;
    };

    // Prefer peeling off the healer / player, then any loose add.
    if (Unit* pPeel = SelectPartyPeelTarget())
    {
        if (tryTaunt(pPeel))
            return true;
        if (CanEngageTarget(pPeel) && AttackStart(pPeel))
            return true;
    }

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || pMember == me)
                continue;
            for (Unit* pAttacker : pMember->GetAttackers())
            {
                if (tryTaunt(pAttacker))
                    return true;
            }
        }
    }

    for (Unit* pAttacker : me->GetAttackers())
    {
        if (pAttacker->GetVictim() != me && tryTaunt(pAttacker))
            return true;
    }

    return false;
}

bool PartyBotAI::CheckForDispelTargets()
{
    if (me->GetShapeshiftForm() != FORM_NONE)
        return false;

    auto tryFriendly = [&](SpellEntry const* pSpell) -> bool
    {
        if (!pSpell)
            return false;
        Player* pFriend = SelectDispelTarget(pSpell);
        if (!pFriend || !IsAssignedToDispel(pFriend, pSpell))
            return false;
        if (!CanTryToCastSpell(pFriend, pSpell))
            return false;
        return DoCastSpell(pFriend, pSpell) == SPELL_CAST_OK;
    };

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            if (tryFriendly(m_spells.paladin.pCleanse))
                return true;
            break;
        case CLASS_SHAMAN:
            // Disease often hurts more than poison on dungeon trash — try it first.
            if (tryFriendly(m_spells.shaman.pCureDisease))
                return true;
            if (tryFriendly(m_spells.shaman.pCurePoison))
                return true;
            break;
        case CLASS_MAGE:
            if (tryFriendly(m_spells.mage.pRemoveLesserCurse))
                return true;
            break;
        case CLASS_PRIEST:
            // Magic CC / DoTs before disease; offensive strip is TryStripEnemyBuffs.
            if (tryFriendly(m_spells.priest.pDispelMagic))
                return true;
            if (tryFriendly(m_spells.priest.pAbolishDisease))
                return true;
            break;
        case CLASS_DRUID:
        {
            SpellEntry const* pPoison = m_spells.druid.pAbolishPoison ?
                m_spells.druid.pAbolishPoison : m_spells.druid.pCurePoison;
            if (tryFriendly(pPoison))
                return true;
            if (tryFriendly(m_spells.druid.pRemoveCurse))
                return true;
            break;
        }
        default:
            break;
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_TANK &&
        m_spells.paladin.pRighteousFury &&
        CanTryToCastSpell(me, m_spells.paladin.pRighteousFury))
    {
        if (DoCastSpell(me, m_spells.paladin.pRighteousFury) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.paladin.pBlessingBuff)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;
}

void PartyBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       (m_role != ROLE_TANK) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
           (me->GetHealthPercent() > 80.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }
    }

    if (!me->GetAttackers().empty())
    {
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield))
        {
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.paladin.pTurnEvil &&
            m_role != ROLE_TANK)
        {
            Unit* pAttacker = SelectAttackerDifferentFrom(me->GetVictim());
            if (pAttacker && pAttacker->GetCreatureType() == CREATURE_TYPE_UNDEAD &&
                CanTryToCastSpell(pAttacker, m_spells.paladin.pTurnEvil))
            {
                if (DoCastSpell(pAttacker, m_spells.paladin.pTurnEvil) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (GetRole() == ROLE_HEALER)
    {
        float directSelf, directGroup, hotSelf, hotGroup;
        bool allowPreHeal = true;
        GetHealerCareThresholds(directSelf, directGroup, hotSelf, hotGroup, allowPreHeal);

        if (m_spells.paladin.pHolyShock &&
            me->GetHealthPercent() < 50.0f &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShock))
        {
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(me, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;
        }

        if (FindAndHealInjuredAlly(directSelf, directGroup))
            return;

        if (allowPreHeal && FindAndPreHealTarget())
            return;
    }
    else
    {
        if (m_spells.paladin.pLayOnHands &&
           (me->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(me, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(me, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }

        bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

        if (!hasSeal &&
            m_spells.paladin.pSeal &&
            CanTryToCastSpell(me, m_spells.paladin.pSeal))
        {
            me->CastSpell(me, m_spells.paladin.pSeal, false);
        }

        if (Unit* pVictim = me->GetVictim())
        {
            if (hasSeal && m_spells.paladin.pJudgement &&
               (me->GetPowerPercent(POWER_MANA) > 30.0f) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfJustice &&
               (pVictim->IsNonMeleeSpellCasted() ||
               (me->GetHealthPercent() < 20.0f && !me->GetAttackers().empty())) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfWrath &&
                pVictim->GetHealthPercent() < 20.0f &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pConsecration &&
               (GetAttackersInRangeCount(10.0f) > 2) &&
                CanTryToCastSpell(me, m_spells.paladin.pConsecration))
            {
                if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyShock &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
            {
                if (m_spells.paladin.pDivineFavor &&
                    CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
                {
                    DoCastSpell(me, m_spells.paladin.pDivineFavor);
                }

                if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pExorcism &&
                pVictim->IsCreature() &&
                (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyWrath &&
                pVictim->IsCreature() &&
               (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD ||
                pVictim->GetCreatureType() == CREATURE_TYPE_DEMON) &&
               (me->GetAttackers().size() < 3) && // too much pushback
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHolyWrath) == SPELL_CAST_OK)
                    return;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }
        }
    }

    // Blessing of Freedom on allies is TryBlessingOfFreedom in the shared pipeline.

    if (GetRole() != ROLE_HEALER &&
        me->GetHealthPercent() < 30.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
    {
        if (SummonShamanTotems())
            return;

        UpdateInCombatAI_Shaman();
    }
}

void PartyBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pManaTideTotem &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER)
    {
        if (Unit* pVictim = me->GetVictim())
        {
            if (m_spells.shaman.pElementalMastery &&
                ShouldBurnMajorCooldown(pVictim) &&
                me->GetAttackers().empty() &&
                CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
            {
                if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pEarthShock &&
                pVictim->IsNonMeleeSpellCasted(false, false, true) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFrostShock &&
                pVictim->IsMoving() &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pStormstrike &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                    return;
            }

            // Flame Shock DoT before filler; Chain Lightning only on a real pack.
            if (m_spells.shaman.pFlameShock &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pChainLightning &&
                me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 1 &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pLightningBolt &&
               (GetRole() == ROLE_RANGE_DPS || !me->CanReachWithMeleeAutoAttack(pVictim)) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (SummonShamanTotems())
        return;

    if (GetRole() == ROLE_HEALER)
    {
        float directSelf, directGroup, hotSelf, hotGroup;
        bool allowPreHeal = true;
        GetHealerCareThresholds(directSelf, directGroup, hotSelf, hotGroup, allowPreHeal);
        // Shaman defaults were generous on the group line; keep mana-aware floors.
        if (FindAndHealInjuredAlly(std::min(directSelf, 50.0f), directGroup))
            return;

        if (allowPreHeal && FindAndPreHealTarget())
            return;
    }
    else if (me->GetHealthPercent() < 20.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheHawk &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
    {
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Hunter();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {
        float const shootDist = me->GetCombatDistance(pVictim);
        // Kiting / Disengage / Clip live in TryKeepRangedDistance. Here: hold the
        // Auto Shot band, and only melee-fill when rooted inside the dead zone.
        if (shootDist > 30.0f)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                BeginChasing(pVictim);
        }
        else if (shootDist >= 8.0f &&
                 me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE ||
                me->IsMoving() || !me->movespline->Finalized())
            {
                me->StopMoving();
                me->GetMotionMaster()->Clear();
            }
        }
        else if (shootDist < 8.0f)
        {
            // Stuck in the dead zone (rooted / no path): Monkey + melee fillers.
            if (m_spells.hunter.pAspectOfTheMonkey &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pFeignDeath &&
               (me->GetHealthPercent() < 20.0f) &&
                CanTryToCastSpell(me, m_spells.hunter.pFeignDeath))
            {
                if (DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK)
                    return;
            }

            if (pVictim->CanReachWithMeleeAutoAttack(me) ||
                me->HasUnitState(UNIT_STATE_ROOT))
            {
                if (m_spells.hunter.pMongooseBite &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pMongooseBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.hunter.pRaptorStrike &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike) == SPELL_CAST_OK)
                        return;
                }
            }
            return;
        }

        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        // Rapid Fire on elites / multi-pulls — not on gray trash.
        if (m_spells.hunter.pRapidFire &&
            ShouldBurnMajorCooldown(pVictim) &&
            CanTryToCastSpell(me, m_spells.hunter.pRapidFire))
        {
            if (DoCastSpell(me, m_spells.hunter.pRapidFire) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pVolley &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pVolley))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pVolley) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_AUTO_SHOT) &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {
            // Auto Shot needs a still caster; clear leftover chase / facing motion.
            if (!me->movespline->Finalized() || me->IsMoving() || !me->IsStopped())
                me->StopMoving();
            me->SetInFront(pVictim);
            me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
            switch (me->CastSpell(pVictim, PB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:
                case SPELL_FAILED_NO_AMMO:
                {
                    AddHunterAmmo();
                    break;
                }
            }
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;
        }

        // Multi-Shot only when a second mob sits in the cone (sheep-safe via CanTry).
        if (m_spells.hunter.pMultiShot &&
            me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAspectOfTheHawk &&
            CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
        {
            if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                return;
        }
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Mage()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.mage.pArcaneIntellect, m_spells.mage.pArcaneBrilliance, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    // Restock the mana gem between pulls so combat pots prefer it.
    if (m_spells.mage.pConjureManaGem &&
        !HasConjuredConsumable(SPELL_EFFECT_ENERGIZE) &&
        CanTryToCastSpell(me, m_spells.mage.pConjureManaGem))
    {
        if (DoCastSpell(me, m_spells.mage.pConjureManaGem) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.mage.pArcaneIntellect ||
        !me->HasGCD(m_spells.mage.pArcaneIntellect)))
    {
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();
}

void PartyBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {
        // Combustion is a long CD — save it for elites / packed pulls, not gray trash.
        if (m_spells.mage.pCombustion &&
            ShouldBurnMajorCooldown(pVictim) &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPyroblast &&
           ((m_spells.mage.pPresenceOfMind && me->HasAura(m_spells.mage.pPresenceOfMind->Id)) ||
            (!pVictim->IsInCombat() && (pVictim->GetMaxHealth() > me->GetMaxHealth()) && (me->GetDistance(pVictim) > 30.0f))) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }
        else if (GetAttackersInRangeCount(10.0f))
        {
            if (m_spells.mage.pManaShield &&
               (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
                CanTryToCastSpell(me, m_spells.mage.pManaShield))
            {
                if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                    return;
            }

            if ((GetRole() != ROLE_MELEE_DPS) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
            {
                if (m_spells.mage.pBlink &&
                    (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                        me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                    CanTryToCastSpell(me, m_spells.mage.pBlink))
                {
                    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                        me->GetMotionMaster()->Clear();

                    if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                        return;
                }

                if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
                {
                    if (m_spells.mage.pFrostNova &&
                       !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
                       !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                        CanTryToCastSpell(me, m_spells.mage.pFrostNova))
                    {
                        DoCastSpell(me, m_spells.mage.pFrostNova);
                    }

                    if (RunAwayFromTarget(pVictim))
                    {
                        me->SetCasterChaseDistance(25.0f);
                        return;
                    }
                }
            }
        }

        if (me->GetEnemyCountInRadiusAround(me, 10.0f) > 1)
        {
            if (m_spells.mage.pConeofCold && !me->IsMoving() &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
            return;

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pBlizzard &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pBlizzard))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pBlizzard) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPolymorph)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (pTarget->GetHealthPercent() > 20.0f &&
                    CanTryToCastSpell(pTarget, m_spells.mage.pPolymorph) &&
                    CanUseCrowdControl(m_spells.mage.pPolymorph, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
            ShouldBurnMajorCooldown(pVictim) &&
            (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPresenceOfMind &&
            ShouldBurnMajorCooldown(pVictim) &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;
        }

        // Fire talent tree (Combustion / Pyro / Blast Wave): stack Improved Scorch, then Fireball.
        bool const fireSpec = m_spells.mage.pCombustion || m_spells.mage.pPyroblast ||
                              m_spells.mage.pBlastWave;
        if (fireSpec && m_spells.mage.pScorch &&
            pVictim->GetHealthPercent() > 15.0f &&
            GetAuraStackCount(pVictim, PB_SPELL_FIRE_VULNERABILITY) < 5 &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireBlast &&
            (pVictim->GetHealthPercent() < 25.0f || fireSpec) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;
        }

        if (fireSpec && m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        // Execute-range Scorch when Frostbolt / Fireball did not fire.
        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Priest()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPowerWordFortitude, m_spells.priest.pPrayerofFortitude, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pDivineSpirit, m_spells.priest.pPrayerofSpirit, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection, m_spells.priest.pPrayerofShadowProtection, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    // Pre-pull Fear Ward on the tank — same habit as a dungeon priest.
    if (TryFearWard())
        return;

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Priest();
}

bool PartyBotAI::HasUsableWand() const
{
    if (!me->HasSpell(PB_SPELL_SHOOT_WAND))
        return false;

    Item* pRanged = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    return pRanged && pRanged->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
}

bool PartyBotAI::TryShootWand(Unit* pVictim)
{
    if (!pVictim || !HasUsableWand() || me->IsMoving())
        return false;

    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        return true;

    if (me->GetCombatDistance(pVictim) > 30.0f)
        return false;

    // Chase / melee swings cancel Shoot; stand still and wand.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
    {
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
    if (me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
    {
        me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
        me->SendMeleeAttackStop(pVictim);
    }
    if (me->GetVictim() != pVictim)
        me->Attack(pVictim, false);

    me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    return true;
}

void PartyBotAI::UpdateInCombatAI_Priest()
{
    if (m_spells.priest.pPowerWordShield &&
        !me->IsImmuneToMechanic(MECHANIC_SHIELD) &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;
    }

    // Fade is handled by TryThreatDump when threat is high; here only emergency
    // shackle when something is already hitting the priest.
    if (!me->GetAttackers().empty() &&
        m_role != ROLE_TANK &&
        m_spells.priest.pShackleUndead)
    {
        Unit* pAttacker = *me->GetAttackers().begin();
        if ((pAttacker->GetHealth() > me->GetHealth()) &&
            CanTryToCastSpell(pAttacker, m_spells.priest.pShackleUndead) &&
            CanUseCrowdControl(m_spells.priest.pShackleUndead, pAttacker))
        {
            if (DoCastSpell(pAttacker, m_spells.priest.pShackleUndead) == SPELL_CAST_OK)
                return;
        }
    }

    // Inner Focus before a triage heal, or when mana is already thin.
    if (m_spells.priest.pInnerFocus &&
       (PartyMemberBelow(45.0f) || me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    if (GetRole() == ROLE_HEALER || (!me->GetVictim() && me->GetShapeshiftForm() == FORM_NONE))
    {
        // Shield allies being attacked.
        if (m_spells.priest.pPowerWordShield)
        {
            if (Player* pTarget = SelectShieldTarget())
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPowerWordShield))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        float directSelf = 60.0f, directGroup = 80.0f, hotSelf = 80.0f, hotGroup = 90.0f;
        bool allowPreHeal = true;
        if (GetRole() == ROLE_HEALER)
            GetHealerCareThresholds(directSelf, directGroup, hotSelf, hotGroup, allowPreHeal);

        // Direct heal more seriously injured.
        if (Unit* pTarget = SelectHealTarget(directSelf, directGroup))
            if (HealInjuredTargetDirect(pTarget))
                return;

        // Apply HoT aura for small injuries.
        if (Unit* pTarget = SelectPeriodicHealTarget(hotSelf, hotGroup))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        if (GetRole() == ROLE_HEALER && allowPreHeal && FindAndPreHealTarget())
            return;

        // Healer with nothing to heal: fall through and wand the kill target.
        if (GetRole() != ROLE_HEALER)
            return;
    }

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return;

    if (GetRole() != ROLE_HEALER)
    {
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;
        }

        // Prefer Silence on casters / ranged even between casts (shared path also
        // covers adds). Keep a victim-only fallback here for the current target.
        if (m_spells.priest.pSilence &&
            IsRangedEnemy(pVictim) &&
            !pVictim->HasAuraType(SPELL_AURA_MOD_SILENCE) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            pVictim->GetPowerPercent(POWER_MANA) > 20.0f &&
            IsRangedEnemy(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pHolyFire &&
                CanTryToCastSpell(pVictim, m_spells.priest.pHolyFire))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pHolyFire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;
            }
        }
    }
    else if (GetRole() == ROLE_HEALER && m_spells.priest.pHolyFire &&
             me->GetPowerPercent(POWER_MANA) > 85.0f &&
             IsDangerousFight(pVictim) &&
             CanTryToCastSpell(pVictim, m_spells.priest.pHolyFire))
    {
        // Only burn mana on filler during hard fights when nearly full.
        if (DoCastSpell(pVictim, m_spells.priest.pHolyFire) == SPELL_CAST_OK)
            return;
    }

    // Healer idle filler (saves mana); DPS last resort when spells did not fire.
    if (GetRole() == ROLE_HEALER &&
        me->GetCombatDistance(pVictim) > 30.0f &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
        BeginChasing(pVictim);

    TryShootWand(pVictim);
}

void PartyBotAI::UpdateOutOfCombatAI_Warlock()
{
    if (m_spells.warlock.pDetectInvisibility)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
        {
            if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
            {
                if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    // Restock a healthstone between pulls (soul shard comes from AddAllSpellReagents).
    if (m_spells.warlock.pCreateHealthstone &&
        !HasConjuredConsumable(SPELL_EFFECT_HEAL) &&
        CanTryToCastSpell(me, m_spells.warlock.pCreateHealthstone))
    {
        if (DoCastSpell(me, m_spells.warlock.pCreateHealthstone) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pBanish &&
            me->GetAttackers().size() > 1)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.warlock.pBanish) &&
                CanUseCrowdControl(m_spells.warlock.pBanish, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.warlock.pBanish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warlock.pRainOfFire &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pRainOfFire))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pRainOfFire) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDemonicSacrifice)
        {
            if (Pet* pPet = me->GetPet())
            {
                if (pPet->IsAlive() &&
                    CanTryToCastSpell(pPet, m_spells.warlock.pDemonicSacrifice))
                {
                    if (DoCastSpell(pPet, m_spells.warlock.pDemonicSacrifice) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.warlock.pImmolate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCorruption &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowWard &&
            me->GetHealthPercent() < 80.0f &&
            CanTryToCastSpell(me, m_spells.warlock.pShadowWard))
        {
            if (DoCastSpell(me, m_spells.warlock.pShadowWard) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pFear &&
            pVictim->GetVictim() == me &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pFear))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pFear) == SPELL_CAST_OK)
                return;
        }

        if (TryApplyWarlockCurse(pVictim))
            return;

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.warlock.pHowlofTerror &&
            GetAttackersInRangeCount(10.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror))
        {
            if (DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warrior()
{
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);
        else if (m_spells.warrior.pBloodrage &&
            (me->GetPower(POWER_RAGE) < 100) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        // Calm mode: no Charge into a far pack (that pulls extras).
        if (m_spells.warrior.pCharge &&
            CanEngageTarget(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;
        }
    }
}

void PartyBotAI::UpdateInCombatAI_Warrior()
{
    if (Unit* pVictim = me->GetVictim())
    {
        // Tanks stay Defensive so Taunt / shield skills work. Berserker on
        // under-level trash (old level check) left them unable to peel.
        if (m_role == ROLE_TANK || me->GetHealthPercent() < 20.0f)
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else if (m_spells.warrior.pBerserkerStance &&
                 CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
        {
            DoCastSpell(me, m_spells.warrior.pBerserkerStance);
        }

        if (m_spells.warrior.pBattleShout &&
            !me->HasAura(m_spells.warrior.pBattleShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }

        // Open the rage faucet at the start of a pull.
        if (m_spells.warrior.pBloodrage &&
            me->GetPower(POWER_RAGE) < 200 &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            if (DoCastSpell(me, m_spells.warrior.pBloodrage) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {
            if (!me->GetAttackers().empty())
            {
                if (m_spells.warrior.pShieldBlock &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.warrior.pShieldWall &&
                   (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;
                }
            }

            // Revenge procs after dodge/parry/block (CasterAuraState gates CanTry).
            if (m_spells.warrior.pRevenge &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pRevenge))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pRevenge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pThunderClap &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pThunderClap))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pThunderClap) == SPELL_CAST_OK)
                return;
        }

        // Build to 5 Sunders, then refresh only when stacks drop (CanTryToCastSpell
        // already allows stackable auras below max).
        if (m_spells.warrior.pSunderArmor &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pSunderArmor))
        {
            uint32 const stacks = GetAuraStackCount(pVictim, m_spells.warrior.pSunderArmor->Id);
            uint32 const maxStacks = m_spells.warrior.pSunderArmor->GetStackAmount();
            if (stacks < maxStacks)
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pSunderArmor) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pHamstring &&
            pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                return;
        }

        // AoE snare when several runners are on us (cheaper than Hamstring each).
        if (m_spells.warrior.pPiercingHowl &&
            GetAttackersInRangeCount(10.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warrior.pPiercingHowl))
        {
            if (DoCastSpell(me, m_spells.warrior.pPiercingHowl) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRend &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRetaliation &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pSweepingStrikes &&
            CanTryToCastSpell(me, m_spells.warrior.pSweepingStrikes) &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2))
        {
            if (DoCastSpell(me, m_spells.warrior.pSweepingStrikes) == SPELL_CAST_OK)
                return;
        }

        if (m_role != ROLE_TANK &&
            ShouldBurnMajorCooldown(pVictim) &&
           (me->GetHealthPercent() > 60.0f) &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;
        }

        // Arms filler: Slam when MS is down and we are not mid-HS dump as tank.
        if (m_role != ROLE_TANK &&
            m_spells.warrior.pSlam &&
            !m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pSlam))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pSlam) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pWhirlwind &&
            me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warrior.pWhirlwind))
        {
            if (DoCastSpell(me, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;
        }

        // Demo on the pack once; skip while the current target already has it.
        if (m_spells.warrior.pDemoralizingShout &&
            m_role == ROLE_TANK &&
            !pVictim->HasAura(m_spells.warrior.pDemoralizingShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pDemoralizingShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pDemoralizingShout) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            BeginChasing(pVictim);
        }

        // Tank: queue HS/Cleave earlier for threat. DPS: dump rage above 30.
        uint32 const rageLine = (m_role == ROLE_TANK) ? 150 : 300;
        if (me->GetPower(POWER_RAGE) > rageLine)
        {
            if (m_spells.warrior.pCleave && me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1)
            {
                if (CanTryToCastSpell(pVictim, m_spells.warrior.pCleave))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pCleave) == SPELL_CAST_OK)
                        return;
                }
            }
            else if (m_spells.warrior.pHeroicStrike &&
                     CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                    return;
            }
        }
    }
    else // no victim
    {
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::ShouldEnterStealth() const
{
    if (me->IsMounted())
        return false;

    if (me->GetVictim() || me->InBattleGround() || me->IsFFAPvP())
        return true;

    if (me->GetHealthPercent() < 10.0f)
        return true;

    if (Player* pLeader = GetPartyLeader())
    {
        if (pLeader->IsDead() || pLeader->IsFeigningDeathSuccessfully() ||
            pLeader->HasAuraType(SPELL_AURA_MOD_STEALTH) ||
            pLeader->HasAuraType(SPELL_AURA_MOD_INVISIBILITY))
            return true;
    }

    return false;
}

bool PartyBotAI::EnterStealthIfNeeded(SpellEntry const* pStealthSpell)
{
    if (pStealthSpell)
    {
        bool const shouldStealth = ShouldEnterStealth();

        if (me->HasAura(pStealthSpell->Id))
        {
            if (!shouldStealth)
                me->RemoveAurasDueToSpellByCancel(pStealthSpell->Id);
        }
        else
        {
            if (shouldStealth &&
                CanTryToCastSpell(me, pStealthSpell) &&
                DoCastSpell(me, pStealthSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;
    }

    if (EnterStealthIfNeeded(m_spells.rogue.pStealth))
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Rogue();
}

void PartyBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else
        {
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {
                        if (RunAwayFromTarget(pVictim))
                            return;
                    }
                }
            }
        }

        if (me->GetComboPoints() > 4)
        {
            // SnD uptime first; Kidney to lock a caster; Expose on long elite fights;
            // Rupture; Eviscerate to finish. No random finishers.
            SpellEntry const* pFinisher = nullptr;
            if (m_spells.rogue.pSliceAndDice &&
               !me->HasAura(m_spells.rogue.pSliceAndDice->Id) &&
                pVictim->GetHealthPercent() > 10.0f)
                pFinisher = m_spells.rogue.pSliceAndDice;
            else if (m_spells.rogue.pKidneyShot &&
                     !pVictim->IsImmuneToMechanic(MECHANIC_STUN) &&
                     (pVictim->IsNonMeleeSpellCasted(false, false, true) || IsRangedEnemy(pVictim)))
                pFinisher = m_spells.rogue.pKidneyShot;
            else if (m_spells.rogue.pExposeArmor &&
                     IsDangerousFight(pVictim) &&
                     pVictim->GetHealthPercent() > 50.0f &&
                     CanTryToCastSpell(pVictim, m_spells.rogue.pExposeArmor))
                pFinisher = m_spells.rogue.pExposeArmor;
            else if (m_spells.rogue.pRupture &&
                     pVictim->GetHealthPercent() > 40.0f &&
                     CanTryToCastSpell(pVictim, m_spells.rogue.pRupture))
                pFinisher = m_spells.rogue.pRupture;
            else if (m_spells.rogue.pEviscerate)
                pFinisher = m_spells.rogue.pEviscerate;

            if (pFinisher && CanTryToCastSpell(pVictim, pFinisher) &&
                DoCastSpell(pVictim, pFinisher) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pRiposte &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pRiposte))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pRiposte) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pBlind)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind) &&
                    CanUseCrowdControl(m_spells.rogue.pBlind, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
            ShouldBurnMajorCooldown(pVictim) &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {
            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;
            }

            // Cold Blood right before a finisher window / Ambush — not on every GCD.
            if (m_spells.rogue.pColdBlood &&
                me->GetComboPoints() >= 4 &&
                ShouldBurnMajorCooldown(pVictim) &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry &&
                me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::EnterCombatDruidForm()
{
    if (m_spells.druid.pCatForm &&
        GetRole() == ROLE_MELEE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pCatForm))
    {
        if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pBearForm &&
       (m_role == ROLE_TANK || GetRole() == ROLE_MELEE_DPS) &&
        CanTryToCastSpell(me, m_spells.druid.pBearForm))
    {
        if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pMoonkinForm &&
        GetRole() == ROLE_RANGE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
    {
        if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Druid()
{
    // Make sure bot leaves combat form if his role is changed to healer.
    if (GetRole() == ROLE_HEALER && me->GetShapeshiftForm() != FORM_NONE &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return;
    }

    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.druid.pMarkoftheWild, m_spells.druid.pGiftoftheWild, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.druid.pThorns)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
        {
            if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
            {
                if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (EnterCombatDruidForm())
            return;

        if ((me->GetPowerPercent(POWER_MANA) > 80.0f) &&
            FindAndHealInjuredAlly())
            return;
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {
        if (EnterStealthIfNeeded(m_spells.druid.pProwl))
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Druid();
}

void PartyBotAI::UpdateInCombatAI_Druid()
{
    ShapeshiftForm const form = me->GetShapeshiftForm();

    if (m_spells.druid.pBarkskin &&
        (form == FORM_NONE || form == FORM_MOONKIN) &&
        (me->GetHealthPercent() < 50.0f) &&
        CanTryToCastSpell(me, m_spells.druid.pBarkskin))
    {
        if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
            return;
    }

    if (form == FORM_NONE)
    {
        if (m_spells.druid.pHibernate &&
            m_role != ROLE_TANK &&
            !me->GetAttackers().empty())
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate) &&
                CanUseCrowdControl(m_spells.druid.pHibernate, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;
            }
        }

        // Nature's Swiftness + big heal when someone is about to die.
        if (m_spells.druid.pNaturesSwiftness &&
            CanTryToCastSpell(me, m_spells.druid.pNaturesSwiftness))
        {
            if (Unit* pCritical = SelectHealTarget(25.0f, 30.0f))
            {
                if (DoCastSpell(me, m_spells.druid.pNaturesSwiftness) == SPELL_CAST_OK)
                {
                    HealInjuredTargetDirect(pCritical);
                    return;
                }
            }
        }

        float directSelf = 60.0f, directGroup = 70.0f, hotSelf = 80.0f, hotGroup = 90.0f;
        bool allowPreHeal = true;
        if (GetRole() == ROLE_HEALER)
            GetHealerCareThresholds(directSelf, directGroup, hotSelf, hotGroup, allowPreHeal);

        // Prioritize applying HoTs.
        if (Unit* pTarget = SelectPeriodicHealTarget(hotSelf, hotGroup))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        // Direct heal.
        if (Unit* pTarget = SelectHealTarget(directSelf, directGroup))
            if (HealInjuredTargetDirect(pTarget))
                return;

        // Party Innervate is TryInnervate in the shared combat pipeline.

        if (GetRole() == ROLE_HEALER && allowPreHeal && FindAndPreHealTarget())
            return;

        if (EnterCombatDruidForm())
            return;
    }

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return;

    if (form != FORM_NONE &&
        me->HasUnitState(UNIT_STATE_ROOT) &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) &&
        (m_role != ROLE_TANK || !me->CanReachWithMeleeAutoAttack(pVictim)))
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

    if (GetRole() == ROLE_HEALER)
        return;

    switch (form)
    {
        case FORM_CAT:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
            {
                if (m_spells.druid.pPounce &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pRavage &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pTigersFury &&
                    CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                {
                    if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                        return;
                }
                return;
            }

            // Cower on high threat is TryThreatDump; skip the unconditional cast.

            if (me->GetComboPoints() > 4)
            {
                if (m_spells.druid.pFerociousBite &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pRip &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                        return;
                }
            }

            if (!me->CanReachWithMeleeAutoAttack(pVictim))
            {
                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pDash &&
                    pVictim->IsMoving() &&
                    CanTryToCastSpell(me, m_spells.druid.pDash))
                {
                    if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pShred &&
                CanTryToCastSpell(pVictim, m_spells.druid.pShred))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pRake &&
                CanTryToCastSpell(pVictim, m_spells.druid.pRake))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pClaw &&
                CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
        case FORM_BEAR:
        case FORM_DIREBEAR:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (m_spells.druid.pFeralCharge &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pBash &&
                CanTryToCastSpell(pVictim, m_spells.druid.pBash))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFrenziedRegeneration &&
                (me->GetHealthPercent() < 30.0f) &&
                CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
            {
                if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pEnrage &&
                me->GetPower(POWER_RAGE) < 200 &&
                me->GetHealthPercent() > 40.0f &&
                CanTryToCastSpell(me, m_spells.druid.pEnrage))
            {
                if (DoCastSpell(me, m_spells.druid.pEnrage) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFaerieFireFeral &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                    return;
            }

            if ((me->GetPower(POWER_RAGE) > 800) ||
                (GetAttackersInRangeCount(10.0f) > 1))
            {
                if (m_spells.druid.pDemoralizingRoar &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pSwipe &&
                    me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pMaul &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                    return;
            }
            break;
        }
        case FORM_NONE:
        case FORM_MOONKIN:
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                me->GetDistance(pVictim) > 30.0f)
            {
                BeginChasing(pVictim);
            }
            else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                    (pVictim->GetVictim() == me) &&
                    !me->HasUnitState(UNIT_STATE_ROOT) &&
                    (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
            {
                if (m_spells.druid.pEntanglingRoots &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                        return;
                }
                me->SetCasterChaseDistance(25.0f);
                if (RunAwayFromTarget(pVictim))
                    return;
            }

            if (m_spells.druid.pFaerieFire &&
               (pVictim->GetClass() == CLASS_ROGUE) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pInsectSwarm &&
                CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                    return;
            }

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                return;

            if (m_spells.druid.pHurricane &&
               (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pHurricane))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pHurricane) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pMoonfire &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pStarfire &&
               (pVictim->GetHealthPercent() > 50.0f) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pWrath &&
                CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
    }
}
