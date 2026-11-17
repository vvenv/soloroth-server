#include "SoloFarmAI.h"
#include "Player.h"
#include "Pet.h"
#include "Bag.h"
#include "DBCStores.h"
#include "Creature.h"
#include "GameObject.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "PathFinder.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Formulas.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "CreatureGroups.h"
#include "World.h"
#include "WorldSession.h"
#include "DBCEnums.h"
#include "Utilities/Random.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{
    // Fishing from Artisan down to Apprentice.
    uint32 const FISHING_SPELLS[] = { 18248, 7732, 7731, 7620 };
    uint32 const HERB_SPELLS[] = { 11993, 3570, 2368, 2366 };
    uint32 const MINE_SPELLS[] = { 10248, 3564, 2576, 2575 };
    uint32 const SKIN_SPELLS[] = { 10768, 8618, 8617, 8613 };
    uint32 const ITEM_MINING_PICK = 2901;
    uint32 const ITEM_SKINNING_KNIFE = 7005;

    uint32 const TICK_MS = 250;
    uint32 const SEARCH_MS = 1000;
    uint32 const PULL_TIMEOUT_MS = 20000;
    uint32 const BLACKLIST_MS = 60000;
    uint32 const GUARDED_RECHECK_MS = 10000;
    // Below this a caster or hunter backs off from a mob in melee range; above it, stands and fights.
    float const RUN_AWAY_HEALTH_PCT = 40.0f;
    // On top of a mob's aggro radius: spline drift, and the mob's own wandering.
    float const AGGRO_MARGIN = 2.0f;
    // Two adds and more: skipped however rested we are.
    uint32 const MAX_ADDS = 1;
    // Where a hunter or caster stops to open (CombatBotBaseAI::BeginChasing holds 25 yd).
    float const RANGED_PULL_DIST = 25.0f;
    // Assist: how far a mob the player targeted (while standing) may be. No area search.
    float const ASSIST_PULL_DIST = 30.0f;
    float const KITE_DIST = 15.0f;
    // Below this a caster stops spending mana spells: wand if equipped, else melee.
    float const OOM_MANA_PCT = 10.0f;
    uint32 const SPELL_SHOOT_WAND = 5019;
    uint32 const PATH_CANDIDATES = 6;
    uint32 const WANDER_TRIES = 8;
    // Past the yards around us: how far the next camp may be. Grids beyond this are not loaded.
    float const ROAM_LOOK = 140.0f;

    float EffectRadius(SpellEntry const* pSpell, float fallback)
    {
        float radius = 0.0f;
        if (!pSpell)
            return fallback;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!pSpell->EffectRadiusIndex[i])
                continue;
            if (SpellRadiusEntry const* pEntry = sSpellRadiusStore.LookupEntry(pSpell->EffectRadiusIndex[i]))
                radius = std::max(radius, Spells::GetSpellRadius(pEntry));
        }
        return radius > 0.0f ? radius : fallback;
    }

    bool AppliesFear(SpellEntry const* pSpell)
    {
        if (!pSpell)
            return false;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (pSpell->Effect[i] == SPELL_EFFECT_APPLY_AURA && pSpell->EffectApplyAuraName[i] == SPELL_AURA_MOD_FEAR)
                return true;
        return false;
    }

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        return std::sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
    }

    float DistToSegment2d(float px, float py, float ax, float ay, float bx, float by)
    {
        float const dx = bx - ax;
        float const dy = by - ay;
        float const len2 = dx * dx + dy * dy;
        float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        return Dist2d(px, py, ax + t * dx, ay + t * dy);
    }

    float AngleGap(float a, float b)
    {
        float d = std::fmod(std::fabs(a - b), 2.0f * M_PI_F);
        return d > M_PI_F ? 2.0f * M_PI_F - d : d;
    }

    bool IsPole(Item const* pItem)
    {
        ItemPrototype const* pProto = pItem ? pItem->GetProto() : nullptr;
        return pProto && pProto->Class == ITEM_CLASS_WEAPON && pProto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
    }

    bool IsZh(Player const* pPlayer)
    {
        int const index = pPlayer->GetSession()->GetSessionDbLocaleIndex();
        return index >= 0 && sObjectMgr.GetLocaleForIndex(index) == LOCALE_zhCN;
    }

    uint32 BestKnownSpell(Player const* pPlayer, uint32 const* spells, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            if (pPlayer->HasSpell(spells[i]))
                return spells[i];
        return 0;
    }

    template <class F>
    void ForEachBagItem(Player const* pPlayer, F f)
    {
        FOR_BACKPACK_SLOTS(slot)
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                f(pItem);

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Item* pBagItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
                if (pBagItem->IsBag())
                    for (uint32 slot = 0; slot < static_cast<Bag*>(pBagItem)->GetBagSize(); ++slot)
                        if (Item* pItem = static_cast<Bag*>(pBagItem)->GetItemByPos(uint8(slot)))
                            f(pItem);
    }

    struct AliveCreatureCheck
    {
        float x, y, range;
        bool operator()(Creature* pCreature) const
        {
            return pCreature->IsAlive() && pCreature->GetDistance2d(x, y) <= range;
        }
    };

    struct LootableCorpseCheck
    {
        Player* me;
        bool operator()(Creature* pCreature) const
        {
            return !pCreature->IsAlive() && pCreature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE) &&
                pCreature->IsTappedBy(me) && me->IsWithinDistInMap(pCreature, 30.0f);
        }
    };

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    struct FishingHoleCheck
    {
        float x, y, range;
        bool operator()(GameObject* pGo) const
        {
            return pGo->GetGOInfo()->type == GAMEOBJECT_TYPE_FISHINGHOLE && pGo->isSpawned() &&
                pGo->GetDistance2d(x, y) <= range;
        }
    };
#endif

    // SetFacingToObject launches a facing spline with MOVEFLAG_FORWARD; the next
    // Player::SetPosition then interrupts casts that have SPELL_INTERRUPT_FLAG_MOVEMENT
    // (skin / herb / mine). Settle first and face without a spline.
    bool ReadyToGatherCast(Player* me, WorldObject const* pTarget)
    {
        if (!me->movespline->Finalized() || me->IsMoving() || !me->IsStopped())
        {
            me->StopMoving();
            return false;
        }
        if (!me->HasInArc(pTarget, 2.0f * M_PI_F / 3.0f))
            me->SetOrientation(me->GetAngle(pTarget));
        return true;
    }
}

SoloFarmAI::SoloFarmAI(Player* pPlayer, SoloFarmMode mode, float radius)
    : PartyBotAI(pPlayer, pPlayer->GetMapId(), pPlayer->GetInstanceId(),
        pPlayer->GetPositionX(), pPlayer->GetPositionY(), pPlayer->GetPositionZ(), pPlayer->GetOrientation()),
      m_mode(mode), m_radius(radius)
{
    SetPlayer(pPlayer);
    m_ownCharacter = true;
    m_initialized = true;
    m_level = pPlayer->GetLevel();
    ResetSpellData();
    PopulateSpellData();
    AssignSoloRole();

    for (uint32 spellId : FISHING_SPELLS)
    {
        if (!pPlayer->HasSpell(spellId))
            continue;
        if (SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spellId))
        {
            m_fishingSpell = spellId;
            SpellRangeEntry const* pRange = sSpellRangeStore.LookupEntry(pSpell->rangeIndex);
            m_castMin = Spells::GetSpellMinRange(pRange);
            m_castMax = Spells::GetSpellMaxRange(pRange);
            break;
        }
    }

    m_herbSpell = BestKnownSpell(pPlayer, HERB_SPELLS, sizeof(HERB_SPELLS) / sizeof(HERB_SPELLS[0]));
    m_mineSpell = BestKnownSpell(pPlayer, MINE_SPELLS, sizeof(MINE_SPELLS) / sizeof(MINE_SPELLS[0]));
    m_skinSpell = BestKnownSpell(pPlayer, SKIN_SPELLS, sizeof(SKIN_SPELLS) / sizeof(SKIN_SPELLS[0]));
    m_wanderSteps = urand(2, 5);
}

char const* SoloFarmAI::ModeWire() const
{
    switch (m_mode)
    {
        case SOLO_FARM_FISH: return "fish";
        case SOLO_FARM_GATHER: return "gather";
        case SOLO_FARM_ALL: return "all";
        case SOLO_FARM_FIGHT: return "fight";
        default: return "grind";
    }
}

SoloFarmAI::~SoloFarmAI()
{
    // Replaced without a stop (a mind control takes the body): the addon still hears of it.
    if (me && !m_stopped && me->GetSession())
        ChatHandler(me).SendSysMessage("[solo] farm=off");
}

SoloFarmAI* SoloFarmAI::Of(Player* pPlayer)
{
    return pPlayer ? dynamic_cast<SoloFarmAI*>(pPlayer->AI()) : nullptr;
}

bool SoloFarmAI::Start(Player* pPlayer, SoloFarmMode mode, float radius)
{
    ChatHandler chat(pPlayer);
    bool const zh = IsZh(pPlayer);

    if (pPlayer->IsBot())
        return false;

    if (SoloFarmAI* pOld = Of(pPlayer))
        pOld->Stop(SOLO_STOP_SILENT);

    if (pPlayer->AI() || !pPlayer->IsAlive() || pPlayer->IsTaxiFlying() || pPlayer->GetTransport() ||
        pPlayer->GetMap()->IsBattleGround())
    {
        chat.SendSysMessage(zh ? "现在不能开始（角色死亡、被控制、在飞行或船上、或在战场里）。"
                               : "Cannot start now (dead, controlled, flying, on a ship or in a battleground).");
        return false;
    }

    if (mode == SOLO_FARM_FISH)
    {
        bool known = false;
        for (uint32 spellId : FISHING_SPELLS)
            known = known || pPlayer->HasSpell(spellId);
        if (!known)
        {
            chat.SendSysMessage(zh ? "还没学钓鱼：先找钓鱼训练师。" : "You have not learned Fishing yet: find a fishing trainer.");
            return false;
        }

        bool pole = IsPole(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND));
        ForEachBagItem(pPlayer, [&](Item* pItem)
        {
            pole = pole || (IsPole(pItem) && pPlayer->CanUseItem(pItem->GetProto()) == EQUIP_ERR_OK);
        });
        if (!pole)
        {
            chat.SendSysMessage(zh ? "没有能用的鱼竿：装备一根或放进背包。" : "No usable fishing pole: equip one or carry it.");
            return false;
        }
    }

    // A profession is not required to start; herbs, ore and skinning still need the skill.

    pPlayer->SetAI(new SoloFarmAI(pPlayer, mode, radius));

    SoloFarmAI* pAI = Of(pPlayer);
    chat.PSendSysMessage("[solo] farm=%s %u", pAI ? pAI->ModeWire() : "grind", uint32(radius));
    switch (mode)
    {
        case SOLO_FARM_FISH:
            chat.PSendSysMessage(zh ? "开始自动钓鱼：在这里 %u 码内找鱼群或水面，自动下钩、收竿、拾取。输入 .solo stop，或在角色站定时按移动键即可停止。"
                                    : "Auto fishing within %u yards of here: finds a school or open water, casts, reels in and loots. Type .solo stop, or press a movement key while standing, to stop.",
                uint32(radius));
            break;
        case SOLO_FARM_GATHER:
            chat.PSendSysMessage(zh ? "开始自动采集：身边 %u 码内采药、挖矿、剥皮（有技能和工具才做）。这一圈没有点，就朝下一处矿或草药跑；再没有就往前跑，偶尔随机换向。不进主城和敌对城镇。输入 .solo stop，或在角色站定时按移动键即可停止。"
                                    : "Auto gathering within %u yards: herbs, ore and skinning (only with the skill and tools). When nothing is that close it runs to the next node, or ahead in a random direction. It stays out of capital cities and enemy towns. Type .solo stop, or press a movement key while standing, to stop.",
                uint32(radius));
            break;
        case SOLO_FARM_ALL:
            chat.PSendSysMessage(zh ? "开始自动挂机：身边 %u 码内能打就打、能采就采、能钓就钓（有技能才做）。这一圈没有事，就朝下一处怪或采集点跑；再没有就往前跑，偶尔随机换向。不进主城和敌对城镇。输入 .solo stop，或在角色站定时按移动键即可停止。"
                                    : "Auto farming within %u yards: fights, gathers and fishes when you can. When nothing is that close it runs to the next camp, or ahead in a random direction. It stays out of capital cities and enemy towns. Type .solo stop, or press a movement key while standing, to stop.",
                uint32(radius));
            break;
        case SOLO_FARM_FIGHT:
            chat.SendSysMessage(zh ? "开始半自动打怪：你自己走路。怪物攻击你，或你停下来并选中 30 码内的怪时，自动追击、放技能。血量或法力过低会喝药水；法力耗尽时有魔杖就继续射击，否则上去近战；站着不动、没有选中目标时血蓝低了会坐下吃喝。定身后会后撤或闪现；多只怪会控住一只。不游荡找怪。走路不会停止；停止用 .solo stop。"
                                   : "Assist on: you walk. It fights anything that attacks you, and a mob you target while standing still within 30 yards: chase and spells. It drinks a potion when health or mana is low; out of mana it wands if equipped, else melees. Standing still with nothing selected, it sits to eat or drink when low. It backs off or blinks after a root, and crowd-controls an extra mob. It does not wander. Walking does not stop it; .solo stop does.");
            break;
        default:
            chat.PSendSysMessage(zh ? "开始自动打怪：身边 %u 码内找怪、战斗、拾取；血蓝低了会坐下吃喝休息。这一圈没有合适的怪，就朝下一处怪跑；再没有就往前跑，偶尔随机换向。不进主城和敌对城镇。战斗中血量或法力过低会喝药水；法力耗尽时有魔杖就继续射击，否则上去近战；定身后会后撤或闪现；多只怪会控住一只。输入 .solo stop，或在角色站定时按移动键即可停止。"
                                    : "Auto grinding within %u yards: picks fights, loots, and sits to eat or drink when low. When nothing is that close it runs to the next camp, or ahead in a random direction. It stays out of capital cities and enemy towns. In a fight it drinks a potion when health or mana is low; out of mana it wands if equipped, else melees. It backs off or blinks after a root, and crowd-controls an extra mob. Type .solo stop, or press a movement key while standing, to stop.",
                uint32(radius));
            break;
    }
    return true;
}

void SoloFarmAI::StopFor(Player* pPlayer, SoloFarmStop reason)
{
    if (SoloFarmAI* pAI = Of(pPlayer))
        pAI->Stop(reason);
}

void SoloFarmAI::OnManualMove(Player* pPlayer)
{
    // Assist exists so the player can walk. A key only ends the modes that steer.
    if (SoloFarmAI* pAI = Of(pPlayer))
        if (pAI->PlayerSteers())
            return;
    StopFor(pPlayer, SOLO_STOP_MOVED);
}

void SoloFarmAI::Remove()
{
    Player* pPlayer = me;
    me = nullptr;
    if (pPlayer && pPlayer->AI() == this)
        pPlayer->SetAI(nullptr);
    delete this;
}

void SoloFarmAI::Say(char const* en, char const* zh) const
{
    ChatHandler(me).SendSysMessage(IsZh(me) ? zh : en);
}

void SoloFarmAI::Stop(SoloFarmStop reason)
{
    m_stopped = true;

    if (me->IsInWorld())
    {
        if (DoesFish() && me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            me->InterruptSpell(CURRENT_CHANNELED_SPELL);
        if (me->IsNonMeleeSpellCasted(false))
            me->InterruptSpell(CURRENT_GENERIC_SPELL);
        if (me->IsAlive())
            RestoreWeapons();

        switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            case CHASE_MOTION_TYPE:
            case POINT_MOTION_TYPE:
            case FOLLOW_MOTION_TYPE:
            case DISTANCING_MOTION_TYPE:
                me->GetMotionMaster()->Clear(false, true);
                me->GetMotionMaster()->MoveIdle();
                if (!me->movespline->Finalized())
                    me->StopMoving();
                break;
            default:
                break;
        }
    }

    if (me->GetSession())
    {
        ChatHandler(me).SendSysMessage("[solo] farm=off");

        bool const zh = IsZh(me);
        char const* what = zh ? "挂机" : "farming";
        switch (m_mode)
        {
            case SOLO_FARM_FISH: what = zh ? "钓鱼" : "fishing"; break;
            case SOLO_FARM_GATHER: what = zh ? "采集" : "gathering"; break;
            case SOLO_FARM_GRIND: what = zh ? "打怪" : "grinding"; break;
            case SOLO_FARM_FIGHT: what = zh ? "半自动打怪" : "assist"; break;
            default: break;
        }
        char line[256] = {};
        // "自动打怪" reads wrong in front of "半自动打怪".
        bool const assist = m_mode == SOLO_FARM_FIGHT;
        switch (reason)
        {
            case SOLO_STOP_COMMAND:
                snprintf(line, sizeof(line), assist ? (zh ? "已停止半自动打怪。" : "Assist stopped.")
                                                    : (zh ? "已停止自动%s。" : "Auto %s stopped."), what);
                break;
            case SOLO_STOP_MOVED:
                snprintf(line, sizeof(line), assist ? (zh ? "你移动了角色，半自动打怪已停止。" : "You moved, so assist stopped.")
                                                    : (zh ? "你移动了角色，自动%s已停止。" : "You moved, so auto %s stopped."), what);
                break;
            case SOLO_STOP_DIED:
                snprintf(line, sizeof(line), assist ? (zh ? "角色死亡，半自动打怪已停止。" : "You died; assist stopped.")
                                                    : (zh ? "角色死亡，自动%s已停止。" : "You died; auto %s stopped."), what);
                break;
            case SOLO_STOP_BAGS_FULL:
                snprintf(line, sizeof(line), assist ? (zh ? "背包满了，半自动打怪已停止。" : "Your bags are full; assist stopped.")
                                                    : (zh ? "背包满了，自动%s已停止。" : "Your bags are full; auto %s stopped."), what);
                break;
            case SOLO_STOP_LEFT_AREA:
                snprintf(line, sizeof(line), assist ? (zh ? "离开了区域，半自动打怪已停止。" : "You left the area; assist stopped.")
                                                    : (zh ? "离开了区域，自动%s已停止。" : "You left the area; auto %s stopped."), what);
                break;
            case SOLO_STOP_NO_POLE:
                snprintf(line, sizeof(line), zh ? "换不上鱼竿（背包没有鱼竿，或放不下副手），自动钓鱼已停止。"
                                                : "Could not equip a fishing pole (none in the bags, or no room for the off-hand); auto fishing stopped.");
                break;
            case SOLO_STOP_NO_WATER:
                snprintf(line, sizeof(line), zh ? "%u 码内找不到能下钩又走得到的水面，自动钓鱼已停止。"
                                                : "No fishable water you can walk to within %u yards; auto fishing stopped.", uint32(m_radius));
                break;
            default:
                break;
        }
        if (*line)
            ChatHandler(me).SendSysMessage(line);
    }

    Remove();
}

void SoloFarmAI::AssignSoloRole()
{
    AutoAssignRole();
    // Nobody to heal but ourselves: healers fight, and heal themselves when low.
    if (m_role == ROLE_HEALER)
        m_role = me->GetClass() == CLASS_PALADIN ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
}

bool SoloFarmAI::NeedsCrowdControlWindow() const
{
    if (me->GetAttackers().size() >= 2)
        return true;
    // Low: a controlled target is what lets us back off, heal or bandage at all.
    return me->GetHealthPercent() < RUN_AWAY_HEALTH_PCT;
}

bool SoloFarmAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!PartyBotAI::CanTryToCastSpell(pTarget, pSpellEntry))
        return false;

    // Arcane Explosion, Cone of Cold, Multi-Shot and the like land on a sheep
    // standing next to the kill target. One hit and the control is gone.
    if (!pSpellEntry->IsAreaOfEffectSpell() || pSpellEntry->IsPositiveSpell())
        return true;

    float const radius = EffectRadius(pSpellEntry, 8.0f);
    for (auto const pAttacker : me->GetAttackers())
    {
        if (!IsBreakableControl(pAttacker))
            continue;
        if (me->GetDistance(pAttacker) <= radius)
            return false;
        if (pTarget && pTarget != me && pTarget->GetDistance(pAttacker) <= radius)
            return false;
    }
    return true;
}

bool SoloFarmAI::CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const
{
    if (!NeedsCrowdControlWindow())
        return false;
    return PartyBotAI::CanUseCrowdControl(pSpellEntry, pTarget);
}

bool SoloFarmAI::TryEmergencyCrowdControl(Unit* pVictim)
{
    if (!pVictim || !NeedsCrowdControlWindow())
        return false;
    // Multi-pull: class AI already sheeps/blinds a different attacker via CanUseCrowdControl.
    if (me->GetAttackers().size() >= 2)
        return false;

    SpellEntry const* pCC = GetCrowdControlSpell();
    if (!pCC || !CanTryToCastSpell(pVictim, pCC) ||
        !PartyBotAI::CanUseCrowdControl(pCC, pVictim))
        return false;
    return DoCastSpell(pVictim, pCC) == SPELL_CAST_OK;
}

bool SoloFarmAI::IsBreakableControl(Unit const* pTarget) const
{
    if (!pTarget)
        return false;
    for (auto const& aura : pTarget->GetSpellAuraHolderMap())
    {
        SpellEntry const* pSpell = aura.second->GetSpellProto();
        if (!pSpell || pSpell->IsPositiveSpell())
            continue;
        // Polymorph, fear, hibernate, shackle, blind, banish: any damage removes them.
        if (pSpell->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS))
            return true;
    }
    return false;
}

bool SoloFarmAI::HasOurDamageOverTime(Unit const* pTarget) const
{
    if (!pTarget)
        return false;
    for (auto const& aura : pTarget->GetSpellAuraHolderMap())
    {
        if (aura.second->GetCasterGuid() != me->GetObjectGuid())
            continue;
        SpellEntry const* pSpell = aura.second->GetSpellProto();
        if (!pSpell)
            continue;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (pSpell->Effect[i] != SPELL_EFFECT_APPLY_AURA)
                continue;
            uint32 const kind = pSpell->EffectApplyAuraName[i];
            if (kind == SPELL_AURA_PERIODIC_DAMAGE || kind == SPELL_AURA_PERIODIC_DAMAGE_PERCENT ||
                kind == SPELL_AURA_PERIODIC_LEECH)
                return true;
        }
    }
    return false;
}

bool SoloFarmAI::WantsDistance() const
{
    if (me->GetClass() == CLASS_HUNTER)
        return true;
    if (GetRole() != ROLE_RANGE_DPS)
        return false;
    // Bear and cat have no cast kit. Caster form and a moonkin do.
    return me->GetShapeshiftForm() == FORM_NONE || me->GetShapeshiftForm() == FORM_MOONKIN;
}

void SoloFarmAI::HoldPetOffControlled()
{
    Pet* pPet = me->GetPet();
    if (!pPet || !pPet->IsAlive() || !pPet->AI())
        return;

    Unit* pPetTarget = pPet->GetVictim();
    if (!pPetTarget || !IsBreakableControl(pPetTarget))
        return;

    Unit* pKill = me->GetVictim();
    if (pKill && pKill != pPetTarget && pKill->IsAlive() && !IsBreakableControl(pKill))
    {
        if (CharmInfo* pCharm = pPet->GetCharmInfo())
            pCharm->SetIsCommandAttack(true);
        pPet->AI()->AttackStart(pKill);
        return;
    }
    pPet->AttackStop();
}

std::vector<SpellEntry const*> SoloFarmAI::LongControlSpells() const
{
    std::vector<SpellEntry const*> spells;
    auto add = [&](SpellEntry const* pSpell)
    {
        if (pSpell)
            spells.push_back(pSpell);
    };
    switch (me->GetClass())
    {
        case CLASS_MAGE: add(m_spells.mage.pPolymorph); break;
        case CLASS_PRIEST: add(m_spells.priest.pShackleUndead); break;
        case CLASS_WARLOCK:
            add(m_spells.warlock.pBanish);
            add(m_spells.warlock.pFear);
            break;
        case CLASS_DRUID: add(m_spells.druid.pHibernate); break;
        case CLASS_HUNTER: add(m_spells.hunter.pScareBeast); break;
        case CLASS_ROGUE: add(m_spells.rogue.pBlind); break;
        default: break;
    }
    return spells;
}

bool SoloFarmAI::TryControlExtra(Unit* pVictim)
{
    // A polymorph cast stops movement. Do not start one in the middle of a retreat.
    if (me->IsMoving() || !me->movespline->Finalized() ||
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        return false;

    std::vector<Unit*> extras;
    auto consider = [&](Unit* pUnit)
    {
        if (!pUnit || pUnit == pVictim || !pUnit->IsAlive() || !IsValidHostileTarget(pUnit))
            return;
        if (IsBreakableControl(pUnit) || HasOurDamageOverTime(pUnit) || pUnit->GetHealthPercent() <= 20.0f)
            return;
        if (std::find(extras.begin(), extras.end(), pUnit) == extras.end())
            extras.push_back(pUnit);
    };
    for (auto const pAttacker : me->GetAttackers())
        consider(pAttacker);
    if (Pet* pPet = me->GetPet())
        for (auto const pAttacker : pPet->GetAttackers())
            consider(pAttacker);
    if (extras.empty())
        return false;

    // Frost Nova hits everything around us, including a sheep we just cast.
    // Root first, then control once the nova is on cooldown.
    if (me->GetClass() == CLASS_MAGE && m_spells.mage.pFrostNova && me->IsSpellReady(m_spells.mage.pFrostNova))
    {
        float const radius = EffectRadius(m_spells.mage.pFrostNova, 10.0f);
        for (Unit* pAdd : extras)
            if (me->GetDistance(pAdd) <= radius && !pAdd->HasUnitState(UNIT_STATE_ROOT))
                return false;
    }

    std::sort(extras.begin(), extras.end(), [](Unit* a, Unit* b)
    {
        return a->GetHealthPercent() > b->GetHealthPercent();
    });

    std::vector<Threat> const threats = FindThreats();
    auto runsIntoPack = [&](Unit* pAdd)
    {
        for (Threat const& threat : threats)
            if (threat.pCreature && pAdd->GetDistance(threat.pCreature) < 12.0f)
                return true;
        return false;
    };

    for (SpellEntry const* pSpell : LongControlSpells())
    {
        bool const fear = AppliesFear(pSpell);
        for (Unit* pAdd : extras)
        {
            if (pSpell->TargetCreatureType && !(pSpell->TargetCreatureType & pAdd->GetCreatureTypeMask()))
                continue;
            if (pSpell->GetMechanic() && pAdd->IsImmuneToMechanic(Mechanics(pSpell->GetMechanic())))
                continue;
            if (fear && runsIntoPack(pAdd))
                continue;
            if (!CanTryToCastSpell(pAdd, pSpell) || !PartyBotAI::CanUseCrowdControl(pSpell, pAdd))
                continue;
            if (DoCastSpell(pAdd, pSpell) == SPELL_CAST_OK)
                return true;
        }
    }
    return false;
}

bool SoloFarmAI::TryBlinkAway(Unit* pEnemy)
{
    SpellEntry const* pBlink = m_spells.mage.pBlink;
    if (me->GetClass() != CLASS_MAGE || !pBlink || !pEnemy)
        return false;
    // Already out of the swing. A blink from there just spends the cooldown.
    if (me->GetCombatDistance(pEnemy) > 10.0f)
        return false;
    if (!CanTryToCastSpell(me, pBlink))
        return false;

    // Blink teleports forward along facing, about one nova's width past the mob.
    float const reach = EffectRadius(pBlink, 15.0f);
    std::vector<Threat> const threats = FindThreats();
    static float const offsets[] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f };
    for (float offset : offsets)
    {
        float const o = me->GetAngle(pEnemy) + M_PI_F + offset;
        float const x = me->GetPositionX() + reach * std::cos(o);
        float const y = me->GetPositionY() + reach * std::sin(o);
        float const z = me->GetPositionZ();
        bool wakes = false;
        for (Threat const& threat : threats)
        {
            if (std::fabs(threat.z - z) > threat.reach)
                continue;
            if (Dist2d(threat.x, threat.y, x, y) <= threat.reach)
                wakes = true;
        }
        if (wakes)
            continue;

        if (!me->movespline->Finalized() || me->IsMoving())
            me->StopMoving();
        me->SetOrientation(o);
        return DoCastSpell(me, pBlink) == SPELL_CAST_OK;
    }
    return false;
}

bool SoloFarmAI::TryEscapeMelee(Unit* pVictim)
{
    if (!pVictim || !WantsDistance() || me->GetMap()->IsDungeon())
        return false;

    // A retreat is already running. Casting here (Cone of Cold, Arcane Explosion)
    // stops it on top of the pack and breaks a root we just spent. Blink is the
    // exception: Nova and Blink share the global cooldown, so the teleport can
    // only happen once the walk has already started.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
    {
        if (me->GetCombatDistance(pVictim) <= 10.0f && TryBlinkAway(pVictim))
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return true;
    }

    bool const close = me->GetCombatDistance(pVictim) < 8.0f || GetAttackersInRangeCount(8.0f) > 0;
    if (!close)
        return false;

    if (!CanOutpaceAttackers())
    {
        // Root the pack, then leave on a later tick once the aura has landed.
        // Nova and Blink share the global cooldown, so they cannot both go off now.
        if (me->GetClass() == CLASS_MAGE && m_spells.mage.pFrostNova &&
            !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
            !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
            CanTryToCastSpell(me, m_spells.mage.pFrostNova) &&
            DoCastSpell(me, m_spells.mage.pFrostNova) == SPELL_CAST_OK)
            return true;

        if (me->GetClass() == CLASS_DRUID && me->GetShapeshiftForm() == FORM_NONE &&
            m_spells.druid.pEntanglingRoots && !IsBreakableControl(pVictim) &&
            !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
            CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots) &&
            DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
            return true;

        return false;
    }

    if (TryBlinkAway(pVictim))
        return true;

    // Disengage leaps backward, so the cast has to face the mob.
    if (me->GetClass() == CLASS_HUNTER && m_spells.hunter.pDisengage &&
        me->GetCombatDistance(pVictim) < 8.0f &&
        CanTryToCastSpell(pVictim, m_spells.hunter.pDisengage) &&
        DoCastSpell(pVictim, m_spells.hunter.pDisengage) == SPELL_CAST_OK)
        return true;

    return RunAwayFromTarget(pVictim);
}

void SoloFarmAI::UpdateAI(uint32 const diff)
{
    m_now += diff;
    if (m_now < m_nextTick)
        return;
    m_nextTick = m_now + TICK_MS;

    if (!me->IsInWorld() || me->IsBeingTeleported())
        return;

    if (!me->IsAlive())
    {
        // Assist stays armed through a corpse run. The other modes end.
        if (!PlayerSteers())
            Stop(SOLO_STOP_DIED);
        else
            ReleaseBody(true);
        return;
    }

    if (PlayerSteers())
    {
        // No camp radius: the player is the one crossing the map.
        if (me->IsTaxiFlying() || me->GetTransport())
            return;
        if (me->GetMap()->IsBattleGround())
        {
            Say("Battlegrounds turn assist off.", "战场里不能半自动打怪，已停止。");
            Stop(SOLO_STOP_SILENT);
            return;
        }
    }
    else if (me->GetMapId() != m_mapId || me->IsTaxiFlying() ||
        !me->IsWithinDist2d(m_x, m_y, m_radius + 60.0f))
    {
        Stop(SOLO_STOP_LEFT_AREA);
        return;
    }

    if (me->GetLevel() != m_level)
    {
        m_level = me->GetLevel();
        ResetSpellData();
        PopulateSpellData();
        AssignSoloRole();
    }

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !me->GetVictim())
        me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);

    Pet* pPet = me->GetPet();
    bool const attacked = !me->GetAttackers().empty() || (pPet && !pPet->GetAttackers().empty());
    if (attacked || me->GetVictim())
    {
        if (DoesFish() && me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            me->InterruptSpell(CURRENT_CHANNELED_SPELL);
        if (DoesFish())
            RestoreWeapons();
        if (UpdateCombat())
            return;
    }

    if (PlayerSteers())
    {
        UpdateFight();
        return;
    }

    if (m_mode == SOLO_FARM_FISH)
    {
        UpdateFishing();
        return;
    }

    if (UpdateCombat())
        return;

    if (Conjure())
        return;

    // Quest goobers / chests before profession nodes and pulls.
    if (DoesQuestObjects() && UpdateQuestObjects())
        return;

    if (DoesGather() && UpdateGather())
        return;

    // In ALL mode only stick to a fishing school (not open water), so grinding still happens.
    if (m_mode == SOLO_FARM_ALL && m_fishingSpell &&
        (me->GetCurrentSpell(CURRENT_CHANNELED_SPELL) || (m_hasSpot && !m_spotHole.IsEmpty())))
    {
        UpdateFishing();
        return;
    }
    if (m_mode == SOLO_FARM_ALL && m_hasSpot && m_spotHole.IsEmpty())
        m_hasSpot = false;

    if (DoesGrind())
    {
        UpdateGrind();
        // Between pulls, pick up a school if one is in range.
        if (m_mode == SOLO_FARM_ALL && m_fishingSpell && m_pullGuid.IsEmpty() && !m_resting &&
            m_now >= m_nextSearch && FindFishingSpot() && m_spotHole.IsEmpty())
            m_hasSpot = false;
        return;
    }

    if (DoesGather())
        Wander();
}

// ------------------------------------------------------------------ combat --

// Alone, backing off from a mob that keeps up (slowed included) only takes hits on the way
// and can lead it home to leash; even when low, potion / heal / CC beat running. Back off
// only when the ones next to us cannot follow, and not when a lone target is nearly dead.
bool SoloFarmAI::RunAwayFromTarget(Unit* pEnemy)
{
    if (!CanOutpaceAttackers())
        return false;
    if (!pEnemy || me->GetMap()->IsDungeon())
        return false;
    if (me->GetAttackers().size() <= 1 &&
        (pEnemy->GetHealthPercent() < 20.0f || pEnemy->GetHealth() * 3 < me->GetHealth()))
        return false;

    // Straight back first, then fanning out; no way clear of other mobs, stand and fight.
    static float const offsets[] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.8f, -1.8f };
    std::vector<Threat> const threats = FindThreats();
    for (float offset : offsets)
    {
        float x, y, z;
        pEnemy->GetNearPoint(me, x, y, z, 0.0f, KITE_DIST, pEnemy->GetAngle(me) + offset);
        if (CountAdds(threats, x, y, z) == 0 && me->GetMotionMaster()->MoveDistance(pEnemy, KITE_DIST, offset))
            return true;
    }
    return false;
}

std::vector<SpellEntry const*> SoloFarmAI::RunnerStopSpells(Unit const* pVictim) const
{
    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
            return { m_spells.warrior.pHamstring, m_spells.warrior.pConcussionBlow, m_spells.warrior.pPiercingHowl };
        case CLASS_PALADIN:
            return { m_spells.paladin.pHammerOfJustice };
        case CLASS_HUNTER:
            return { m_spells.hunter.pConcussiveShot, m_spells.hunter.pWingClip };
        case CLASS_ROGUE:
            return { m_spells.rogue.pKidneyShot, m_spells.rogue.pGouge };
        case CLASS_PRIEST:
            return { m_spells.priest.pMindFlay };
        case CLASS_SHAMAN:
            return { m_spells.shaman.pFrostShock };
        case CLASS_MAGE:
        {
            // Frost Nova hits around us only.
            SpellEntry const* pNova = me->IsWithinDist(pVictim, 8.0f) ? m_spells.mage.pFrostNova : nullptr;
            return { pNova, m_spells.mage.pFrostbolt, m_spells.mage.pConeofCold };
        }
        case CLASS_WARLOCK:
            return { m_spells.warlock.pCurseofExhaustion };
        case CLASS_DRUID:
            return { m_spells.druid.pBash, m_spells.druid.pEntanglingRoots };
        default:
            return {};
    }
}

// Creature::DoFleeToGetAssistance: a mob low on health runs off and brings back kin from
// CreatureFamilyFleeAssistanceRadius (30 yd). Root, snare or stun it before it gets there.
bool SoloFarmAI::StopRunner(Unit* pVictim)
{
    if (pVictim->HasAuraType(SPELL_AURA_MOD_FEAR) || pVictim->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
        return false;
    MovementGeneratorType const type = pVictim->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (type != ASSISTANCE_MOTION_TYPE && type != TIMED_FLEEING_MOTION_TYPE && !pVictim->HasUnitState(UNIT_STATE_FLEEING))
        return false;

    for (SpellEntry const* pSpell : RunnerStopSpells(pVictim))
        if (pSpell && CanTryToCastSpell(pVictim, pSpell) && DoCastSpell(pVictim, pSpell) == SPELL_CAST_OK)
            return true;
    return false;
}

bool SoloFarmAI::CanOutpaceAttackers() const
{
    bool near = false;
    for (auto const pAttacker : me->GetAttackers())
    {
        if (!pAttacker->IsAlive() || me->GetDistance(pAttacker) > 10.0f)
            continue;
        near = true;
        if (pAttacker->GetVictim() != me)
            continue;
        if (pAttacker->HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_CONFUSED))
            continue;
        return false;
    }
    return near;
}

bool SoloFarmAI::IsAlly(Unit const* pUnit) const
{
    if (!pUnit)
        return false;
    // The victim of a pull is a player, or a pet / guardian whose owner is.
    Player* pPlayer = pUnit->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!pPlayer || pPlayer->GetMapId() != me->GetMapId())
        return false;
    if (pPlayer == me)
        return true;
    Group const* pGroup = me->GetGroup();
    return pGroup && pGroup == pPlayer->GetGroup();
}

bool SoloFarmAI::FightingAlly(Creature const* pCreature) const
{
    return pCreature && IsAlly(pCreature->GetVictim());
}

Unit* SoloFarmAI::SelectAllyAttacker() const
{
    // Assist mode keeps the body until the player is attacked or targets something.
    if (PlayerSteers())
        return nullptr;
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Unit* pBest = nullptr;
    float best = 0.0f;
    auto consider = [&](Unit* pUnit)
    {
        if (!pUnit || !pUnit->IsAlive() || pUnit->GetMapId() != me->GetMapId())
            return;
        for (auto const pAttacker : pUnit->GetAttackers())
        {
            if (!pAttacker->IsAlive() || !IsValidHostileTarget(pAttacker))
                continue;
            if (Creature* pCreature = pAttacker->ToCreature())
                if (pCreature->IsInEvadeMode())
                    continue;
            // A party pull beside us. One a long run away is the next camp, not this fight.
            if (!pAttacker->IsWithinDist(me, m_radius + 40.0f))
                continue;
            float const distance = me->GetDistance(pAttacker);
            if (!pBest || distance < best)
            {
                pBest = pAttacker;
                best = distance;
            }
        }
    };

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;
        consider(pMember);
        consider(pMember->GetPet());
    }
    return pBest;
}

bool SoloFarmAI::UpdateCombat()
{
    Unit* pVictim = me->GetVictim();
    if (pVictim && !IsValidHostileTarget(pVictim))
    {
        me->AttackStop();
        pVictim = nullptr;
    }

    // Party combat flags us too (1.12). A pull that has not turned to us yet is dropped
    // so a mob already hitting a party member is the one we run to.
    Unit* pAlly = SelectAllyAttacker();
    if (pVictim && pAlly && pVictim != pAlly && !FightingAlly(pVictim->ToCreature()))
    {
        me->AttackStop();
        pVictim = nullptr;
    }

    if (!pVictim)
    {
        float best = 0.0f;
        for (auto const pAttacker : me->GetAttackers())
        {
            if (!IsValidHostileTarget(pAttacker))
                continue;
            float const distance = me->GetDistance(pAttacker);
            if (!pVictim || distance < best)
            {
                pVictim = pAttacker;
                best = distance;
            }
        }

        if (!pVictim)
            if (Pet* pPet = me->GetPet())
                if (Unit* pPetAttacker = pPet->GetAttackerForHelper())
                    if (IsValidHostileTarget(pPetAttacker))
                        pVictim = pPetAttacker;

        if (!pVictim && pAlly)
        {
            pVictim = pAlly;
            m_pullGuid = pAlly->GetObjectGuid();
            m_pullStart = m_now;
            m_wandering = false;
        }

        if (!pVictim && !m_pullGuid.IsEmpty())
            if (Unit* pPull = me->GetMap()->GetUnit(m_pullGuid))
                if (pPull->IsAlive() && IsValidHostileTarget(pPull))
                    pVictim = pPull;

        if (!pVictim)
        {
            m_pullGuid.Clear();
            return false;
        }

        AttackStart(pVictim);
        return true;
    }

    // A pull that never turns into a fight (out of reach, or it does not fight back), or one
    // that resets: leave it alone for a while.
    Creature* pCreature = pVictim->ToCreature();
    if ((pVictim->GetObjectGuid() == m_pullGuid && !me->IsInCombat() && m_now - m_pullStart > PULL_TIMEOUT_MS) ||
        (pCreature && pCreature->IsInEvadeMode()))
    {
        Blacklist(pVictim->GetObjectGuid(), BLACKLIST_MS);
        me->AttackStop();
        m_pullGuid.Clear();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
        return true;
    }

    // A sheep (or fear, shackle, blind) is the kill target only when it was the only mob.
    // With another still swinging, fight that one and leave the control alone.
    if (IsBreakableControl(pVictim))
    {
        Unit* pLoose = nullptr;
        auto considerLoose = [&](Unit* pUnit)
        {
            if (!pLoose && pUnit && pUnit != pVictim && pUnit->IsAlive() &&
                !IsBreakableControl(pUnit) && IsValidHostileTarget(pUnit))
                pLoose = pUnit;
        };
        for (auto const pAttacker : me->GetAttackers())
            considerLoose(pAttacker);
        if (Pet* pPet = me->GetPet())
            for (auto const pAttacker : pPet->GetAttackers())
                considerLoose(pAttacker);

        if (pLoose && AttackStart(pLoose))
            pVictim = pLoose;
    }

    HoldPetOffControlled();

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);
    if (m_resting)
        m_resting = false;

    // Wand Shoot is autorepeat: moving (including a chase tick) cancels it. Keep shooting.
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && me->GetClass() != CLASS_HUNTER)
        return true;

    // Out of mana: wand at range when equipped, otherwise close in and swing.
    bool const casterOom = !IsBreakableControl(pVictim) && GetRole() == ROLE_RANGE_DPS &&
        me->GetClass() != CLASS_HUNTER && me->GetShapeshiftForm() == FORM_NONE &&
        me->GetPowerType() == POWER_MANA && me->GetPowerPercent(POWER_MANA) < OOM_MANA_PCT;
    bool const useWand = casterOom && HasUsableWand();
    bool const goMelee = casterOom && !useWand;
    if (useWand)
    {
        if (me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            me->SendMeleeAttackStop(pVictim);
        }
    }
    else if (GetRole() == ROLE_RANGE_DPS && me->GetClass() != CLASS_HUNTER &&
        me->GetShapeshiftForm() == FORM_NONE && me->GetPowerType() == POWER_MANA)
    {
        // AttackStart turns on auto-attack; with mana (or no wand while OOM) keep swings as needed.
        if (goMelee && !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            me->Attack(pVictim, true);
        else if (!goMelee && me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            me->SendMeleeAttackStop(pVictim);
        }
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return true;

    // The controlled target is the only one left: drink, heal, and step back
    // instead of breaking it with the next swing or bolt.
    if (IsBreakableControl(pVictim))
    {
        bool const stillLow = me->GetHealthPercent() < 80.0f ||
            (me->GetPowerType() == POWER_MANA && me->GetPowerPercent(POWER_MANA) < 50.0f);
        if (stillLow)
        {
            if (me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            {
                me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                me->SendMeleeAttackStop(pVictim);
            }
            if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            if (me->GetHealthPercent() < 85.0f && HealSelf())
                return true;
            if (me->GetHealthPercent() < 70.0f && UseConsumable(0, SPELL_EFFECT_HEAL, true))
                return true;
            if (me->GetPowerType() == POWER_MANA && me->GetPowerPercent(POWER_MANA) < 60.0f &&
                UseConsumable(0, SPELL_EFFECT_ENERGIZE, true))
                return true;
            TryEscapeMelee(pVictim);
            return true;
        }
    }

    bool const crowded = me->GetAttackers().size() >= 2;
    float const hpLine = crowded ? 55.0f : 40.0f;
    if (me->GetHealthPercent() < hpLine)
    {
        if (HealSelf())
            return true;
        if (UseConsumable(0, SPELL_EFFECT_HEAL, true))
            return true;
    }
    // A mana potion with the mob nearly dead just sits on the shared potion cooldown.
    bool const ending = !crowded && pVictim->GetHealthPercent() < 15.0f && me->GetHealthPercent() >= hpLine;
    float const manaLine = crowded ? 40.0f : 30.0f;
    if (!ending && me->GetPowerType() == POWER_MANA && me->GetPowerPercent(POWER_MANA) < manaLine &&
        UseConsumable(0, SPELL_EFFECT_ENERGIZE, true))
        return true;

    if (StopRunner(pVictim))
        return true;

    if (TryControlExtra(pVictim))
        return true;

    // 1v1 and low: sheep the kill target, then the block above drinks and steps back.
    // Not during a retreat: the cast would stop it.
    if (!crowded && !me->IsMoving() && me->movespline->Finalized() &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE &&
        TryEmergencyCrowdControl(pVictim))
        return true;

    if (TryEscapeMelee(pVictim))
        return true;

    // After potions and CC: OOM with a wand keeps the range and shoots.
    if (useWand)
    {
        if (!me->IsMoving() && me->movespline->Finalized() && me->GetCombatDistance(pVictim) <= 30.0f)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            {
                me->GetMotionMaster()->Clear(false, true);
                me->GetMotionMaster()->MoveIdle();
            }
            me->CastSpell(pVictim, SPELL_SHOOT_WAND, false);
            return true;
        }
        if (!me->IsMoving())
            BeginChasing(pVictim);
        return true;
    }

    // Rooted, and already outside the swing: do not chase back in while the root holds.
    // OOM without a wand: drop the range hold and walk into melee.
    bool const holdRange = !goMelee && WantsDistance() && CanOutpaceAttackers() &&
        me->GetCombatDistance(pVictim) < 20.0f;
    if (!holdRange && !me->IsMoving())
    {
        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
            (goMelee || GetRole() == ROLE_MELEE_DPS || GetRole() == ROLE_TANK) &&
            AttackStart(pVictim))
            return true;

        if (goMelee && me->HasDistanceCasterMovement())
            me->SetCasterChaseDistance(0.0f);

        switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            case IDLE_MOTION_TYPE:
            case FOLLOW_MOTION_TYPE:
            case POINT_MOTION_TYPE:
                BeginChasing(pVictim);
                break;
            default:
                break;
        }
    }

    // Also before the fight starts: a caster opens from range, which is what starts it.
    UpdateInCombatAI();
    return true;
}

bool SoloFarmAI::HealSelf()
{
    if (m_spellListDirectHeal.empty() && m_spellListPeriodicHeal.empty())
        return false;
    return HealInjuredTarget(me);
}

// Rest() then holds until 95% health and mana.
void SoloFarmAI::BeginRest()
{
    if (m_resting)
        return;
    m_resting = true;
    m_wandering = false;
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();
    if (!me->movespline->Finalized())
        me->StopMoving();
}

void SoloFarmAI::CancelRest()
{
    if (!m_resting)
        return;
    m_resting = false;
    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);
}

bool SoloFarmAI::HasUsableWand() const
{
    if (!me->HasSpell(SPELL_SHOOT_WAND))
        return false;
    Item* pRanged = me->GetWeaponForAttack(RANGED_ATTACK, true, true);
    return pRanged && pRanged->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
}

bool SoloFarmAI::Rest()
{
    bool const usesMana = me->GetPowerType() == POWER_MANA;
    bool const needHealth = me->GetHealthPercent() < (m_resting ? 95.0f : 60.0f);
    bool const needMana = usesMana && me->GetPowerPercent(POWER_MANA) < (m_resting ? 95.0f : 40.0f);

    if (!needHealth && !needMana)
    {
        CancelRest();
        return false;
    }

    if (!m_resting)
    {
        BeginRest();
        return true;
    }

    if (!me->movespline->Finalized() || me->IsNonMeleeSpellCasted(false))
        return true;

    if (needHealth && me->GetHealthPercent() < 80.0f && HealSelf())
        return true;

    if (me->IsInCombat())
        return true;

    if (needHealth && !me->HasAuraType(SPELL_AURA_MOD_REGEN))
        UseConsumable(SPELL_AURA_MOD_REGEN);
    if (needMana && !me->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
        UseConsumable(SPELL_AURA_MOD_POWER_REGEN);

    // Food and drink sit on their own. Without them, sit for the spirit regen tick.
    if (me->GetStandState() == UNIT_STAND_STATE_STAND &&
        !me->HasAuraType(SPELL_AURA_MOD_REGEN) && !me->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
        me->SetStandState(UNIT_STAND_STATE_SIT);

    return true;
}

bool SoloFarmAI::UseConsumable(uint32 auraType, uint32 effect, bool combat)
{
    struct Candidate
    {
        Item* item;
        int32 power;
        bool conjured;
        uint32 itemLevel;
    };
    std::vector<Candidate> picks;

    ForEachBagItem(me, [&](Item* pItem)
    {
        ItemPrototype const* pProto = pItem->GetProto();
        if (pProto->Class != ITEM_CLASS_CONSUMABLE || me->CanUseItem(pProto) != EQUIP_ERR_OK)
            return;

        for (auto const& itemSpell : pProto->Spells)
        {
            if (!itemSpell.SpellId || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
            if (!pSpell || !me->IsSpellReady(pSpell, pProto))
                continue;
            // Bandages and food cannot be used in combat (1.12): cast time, or the peaceful-only flag.
            if (combat && (pSpell->HasAttribute(SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL) || pSpell->GetCastTime(me) > 0))
                continue;

            int32 power = 0;
            bool match = false;
            for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                bool const aura = auraType && pSpell->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
                    pSpell->EffectApplyAuraName[i] == auraType;
                bool const direct = effect && pSpell->Effect[i] == effect &&
                    (effect != SPELL_EFFECT_ENERGIZE || pSpell->EffectMiscValue[i] == POWER_MANA);
                if (!aura && !direct)
                    continue;
                match = true;
                power = std::max(power, pSpell->CalculateSimpleValue(SpellEffectIndex(i)));
            }
            if (!match)
                continue;

            picks.push_back({ pItem, power, pProto->IsConjuredConsumable(), pProto->ItemLevel });
            break;
        }
    });

    if (picks.empty())
        return false;

    // A healthstone or mana gem does not share the potion cooldown. Spend that first.
    std::sort(picks.begin(), picks.end(), [&](Candidate const& a, Candidate const& b)
    {
        if (combat && a.conjured != b.conjured)
            return a.conjured && !b.conjured;
        if (combat && a.power != b.power)
            return a.power > b.power;
        return a.itemLevel > b.itemLevel;
    });

    // Out of combat, one best food or drink. In combat, the next candidate if that cast fails.
    uint32 const tries = combat ? picks.size() : 1;
    for (uint32 i = 0; i < tries; ++i)
        if (UseItemEffect(picks[i].item))
            return true;
    return false;
}

// Mage water / food / mana gem and warlock healthstone, best known rank, topped up between
// pulls. Not while eating or drinking: a cast stands up and cancels the regen aura.
bool SoloFarmAI::Conjure()
{
    if (me->IsInCombat() || me->IsNonMeleeSpellCasted(false) ||
        me->HasAuraType(SPELL_AURA_MOD_REGEN) || me->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
        return false;

    struct ConjureLine { std::initializer_list<uint32> ranks; uint32 keep; };
    static ConjureLine const lines[] =
    {
        { { 5504, 5505, 5506, 6127, 10138, 10139, 10140 }, 20 },  // Conjure Water
        { { 587, 597, 990, 6129, 10144, 10145, 28612 }, 10 },     // Conjure Food
        { { 759, 3552, 10053, 10054 }, 1 },                       // Conjure Mana Agate .. Ruby
        { { 6201, 6202, 5699, 11729, 11730 }, 1 },                // Create Healthstone
    };

    for (auto const& line : lines)
    {
        SpellEntry const* pSpell = nullptr;
        for (uint32 spellId : line.ranks)
            if (me->HasSpell(spellId))
                pSpell = sSpellMgr.GetSpellEntry(spellId);
        if (!pSpell)
            continue;

        uint32 const itemId = pSpell->EffectItemType[EFFECT_INDEX_0];
        if (!itemId || me->GetItemCount(itemId) >= line.keep)
            continue;

        bool reagents = true;
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (pSpell->Reagent[i] > 0 && !me->HasItemCount(pSpell->Reagent[i], pSpell->ReagentCount[i]))
                reagents = false;
        if (!reagents)
            continue;

        ItemPosCountVec dest;
        if (me->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK)
            continue;
        if (!CanTryToCastSpell(me, pSpell))
            continue;

        if (!me->movespline->Finalized() || me->IsMoving())
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
            m_wandering = false;
        }
        if (me->GetStandState() != UNIT_STAND_STATE_STAND)
            me->SetStandState(UNIT_STAND_STATE_STAND);
        if (DoCastSpell(me, pSpell) == SPELL_CAST_OK)
            return true;
    }
    return false;
}

bool SoloFarmAI::LootCorpses()
{
    std::list<Creature*> corpses;
    LootableCorpseCheck check{ me };
    MaNGOS::CreatureListSearcher<LootableCorpseCheck> searcher(corpses, check);
    Cell::VisitGridObjects(me, searcher, 30.0f);

    for (Creature* pCorpse : corpses)
        if (!me->SoloLootCreature(pCorpse))
            return false;
    return true;
}

uint32 SoloFarmAI::FreeBagSlots() const
{
    uint32 free = 0;
    FOR_BACKPACK_SLOTS(slot)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;

    // Quivers, ammo pouches and soul bags take only their own kind.
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Item* pBag = me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
            if (pBag->IsBag() && pBag->GetProto()->Class == ITEM_CLASS_CONTAINER &&
                pBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                free += static_cast<Bag*>(pBag)->GetFreeSlots();
    return free;
}

void SoloFarmAI::Blacklist(ObjectGuid guid, uint32 ms)
{
    m_blacklist[guid] = m_now + ms;
}

bool SoloFarmAI::IsBlacklisted(ObjectGuid guid) const
{
    auto const itr = m_blacklist.find(guid);
    return itr != m_blacklist.end() && itr->second > m_now;
}

// ------------------------------------------------------------------- assist --

void SoloFarmAI::ReleaseBody(bool force)
{
    if (!force && (me->GetVictim() || !me->GetAttackers().empty()))
        return;

    switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        case CHASE_MOTION_TYPE:
        case DISTANCING_MOTION_TYPE:
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            if (!me->movespline->Finalized())
                me->StopMoving();
            break;
        default:
            break;
    }
}

bool SoloFarmAI::TryAssistPull()
{
    // Walking owns the body. A targeted mob is a pull only once the player has stopped.
    // Still flagged in combat after a kill is fine: they stopped and picked the next mob.
    if (me->IsMoving() || !me->movespline->Finalized() || me->GetVictim())
        return false;

    Unit* pTarget = me->GetMap()->GetUnit(me->GetSelectionGuid());
    Creature* pCreature = pTarget ? pTarget->ToCreature() : nullptr;
    if (!pCreature || !pCreature->IsAlive())
        return false;
    if (pCreature->IsPet() || pCreature->IsTotem() || !pCreature->GetOwnerGuid().IsEmpty())
        return false;
    if (pCreature->GetCreatureInfo()->type == CREATURE_TYPE_CRITTER || pCreature->IsCivilian() ||
        pCreature->IsGuard() || pCreature->GetUInt32Value(UNIT_NPC_FLAGS))
        return false;
    if (pCreature->IsInEvadeMode() || IsBlacklisted(pCreature->GetObjectGuid()))
        return false;
    if (pCreature->HasLootRecipient() && !pCreature->IsTappedBy(me))
        return false;
    // A mob already hitting someone outside the party. One hitting a party member is the pull.
    if (pCreature->GetVictim() && !IsAlly(pCreature->GetVictim()))
        return false;
    if (!IsValidHostileTarget(pCreature) || !me->IsWithinLOSInMap(pCreature))
        return false;
    if (me->GetDistance(pCreature) > ASSIST_PULL_DIST)
        return false;

    m_pullGuid = pCreature->GetObjectGuid();
    m_pullStart = m_now;
    return AttackStart(pCreature);
}

void SoloFarmAI::UpdateFight()
{
    // Combat (aggro or a pull already in progress) is handled before this runs.
    ReleaseBody();

    // Walking owns the body: stand up from a sit-to-rest between pulls.
    if (me->IsMoving() || !me->movespline->Finalized())
    {
        CancelRest();
        return;
    }

    if (TryAssistPull())
    {
        CancelRest();
        UpdateCombat();
        return;
    }

    // Buff or rest while they stand with nothing targeted. Out-of-combat buffs clear the
    // current target, so a selected mob (or a quest giver) is left alone.
    // No conjure: that would take the body they are steering for a cast.
    if (me->IsMounted() || !me->GetSelectionGuid().IsEmpty())
    {
        CancelRest();
        return;
    }

    if (Rest())
        return;

    UpdateOutOfCombatAI();
}

// ----------------------------------------------------------------- grinding --

void SoloFarmAI::UpdateGrind()
{
    if (!LootCorpses() || FreeBagSlots() == 0)
    {
        Stop(SOLO_STOP_BAGS_FULL);
        return;
    }

    if (Rest())
        return;

    UpdateOutOfCombatAI();
    if (m_isBuffing || me->IsNonMeleeSpellCasted(false))
        return;

    if (m_now >= m_nextSearch)
    {
        m_nextSearch = m_now + SEARCH_MS;
        uint32 adds = 0;
        if (Creature* pTarget = FindTarget(adds))
        {
            // Outdoors, a second mob waits until we are rested. In a dungeon the
            // pack is the only way forward: sitting would never make it smaller.
            if (adds > PackLimit())
            {
                BeginRest();
                return;
            }
            m_pullGuid = pTarget->GetObjectGuid();
            m_pullStart = m_now;
            m_wandering = false;
            AttackStart(pTarget);
            return;
        }
    }

    Wander();
}

Creature* SoloFarmAI::FindTarget(uint32& adds)
{
    float const px = me->GetPositionX();
    float const py = me->GetPositionY();
    std::list<Creature*> creatures;
    AliveCreatureCheck check{ px, py, m_radius };
    MaNGOS::CreatureListSearcher<AliveCreatureCheck> searcher(creatures, check);
    Cell::VisitGridObjects(px, py, me->GetMap(), searcher, m_radius);

    uint32 const grayLevel = MaNGOS::XP::GetGrayLevel(me->GetLevel());
    uint32 const topLevel = me->GetLevel() + (InPartyDungeon() ? 10 : 2);

    std::vector<std::pair<float, WorldObject*>> picks;
    std::vector<std::pair<float, WorldObject*>> engaged;
    for (Creature* pCreature : creatures)
    {
        if (pCreature->IsPet() || pCreature->IsTotem() || !pCreature->GetOwnerGuid().IsEmpty())
            continue;
        if (pCreature->GetCreatureInfo()->type == CREATURE_TYPE_CRITTER || pCreature->IsCivilian() ||
            pCreature->IsGuard())
            continue;
        // Vendors, quest givers, trainers of the other side: never a grinding target.
        if (pCreature->GetUInt32Value(UNIT_NPC_FLAGS))
            continue;
        // Outdoors an elite or a gray mob is not a pull. Dungeon trash is elite, and
        // the level spread of one instance is wider than the outdoor +2.
        bool const ours = FightingAlly(pCreature);
        bool const dungeon = InPartyDungeon();
        if (!ours && pCreature->IsWorldBoss())
            continue;
        if (!ours && pCreature->IsElite() && !dungeon)
            continue;
        if (!ours && !dungeon && pCreature->GetLevel() <= grayLevel)
            continue;
        if (!ours && pCreature->GetLevel() > topLevel)
            continue;
        if (pCreature->IsInEvadeMode() || (!ours && IsBlacklisted(pCreature->GetObjectGuid())))
            continue;
        if (pCreature->HasLootRecipient() && !pCreature->IsTappedBy(me))
            continue;
        // Someone else's fight. The party's own pull is taken below, adds and all.
        if (pCreature->GetVictim() && !ours)
            continue;
        if (!IsValidHostileTarget(pCreature))
            continue;
        // No line of sight required: the path check below is what decides we can walk there.
        // A mob behind a barn is a camp to run to, not a reason to stand.

        (ours ? engaged : picks).emplace_back(me->GetDistance(pCreature), pCreature);
    }

    if (!engaged.empty())
    {
        std::sort(engaged.begin(), engaged.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        adds = 0;
        return static_cast<Creature*>(engaged.front().second);
    }

    float const stopShort = GetRole() == ROLE_RANGE_DPS ? RANGED_PULL_DIST : 0.0f;
    uint32 const cap = InPartyDungeon() ? PackLimit() : MAX_ADDS;
    return static_cast<Creature*>(PickApproach(picks, cap, stopShort, true, &adds));
}

// Rested, one extra is fine: the class AI sheeps or blinds it. Otherwise only clean pulls.
uint32 SoloFarmAI::AllowedAdds() const
{
    bool const fresh = me->GetHealthPercent() >= 90.0f &&
        (me->GetPowerType() != POWER_MANA || me->GetPowerPercent(POWER_MANA) >= 80.0f);
    return fresh ? 1 : 0;
}

std::vector<SoloFarmAI::Threat> SoloFarmAI::FindThreats() const
{
    // Wide enough to cover a roam leg, not only the yards we will pull inside.
    float const range = std::max(m_radius + 40.0f, ROAM_LOOK + 20.0f);
    float const px = me->GetPositionX();
    float const py = me->GetPositionY();
    std::list<Creature*> creatures;
    AliveCreatureCheck check{ px, py, range };
    MaNGOS::CreatureListSearcher<AliveCreatureCheck> searcher(creatures, check);
    Cell::VisitGridObjects(px, py, me->GetMap(), searcher, range);

    std::vector<Threat> threats;
    for (Creature* pCreature : creatures)
    {
        if (pCreature->IsInCombat() || pCreature->IsPet() || !pCreature->GetOwnerGuid().IsEmpty())
            continue;
        if (!pCreature->CanInitiateAttack() || pCreature->IsInEvadeMode() || !pCreature->IsHostileTo(me))
            continue;
        float reach = pCreature->GetAttackDistance(me);
        if (reach <= 0.0f)
            continue;
        reach += AGGRO_MARGIN;

        // A random mover strolls into a fight anywhere within its wander radius of home.
        float x = pCreature->GetPositionX();
        float y = pCreature->GetPositionY();
        float z = pCreature->GetPositionZ();
        if (pCreature->GetDefaultMovementType() == RANDOM_MOTION_TYPE && pCreature->GetWanderDistance() > 0.0f)
        {
            pCreature->GetRespawnCoord(x, y, z);
            reach += pCreature->GetWanderDistance();
        }
        threats.push_back({ pCreature, x, y, z, reach });
    }
    return threats;
}

uint32 SoloFarmAI::CountAdds(std::vector<Threat> const& threats, float x, float y, float z,
    Creature* pTarget, float stopShort, float maxLength) const
{
    PathInfo path(me);
    path.calculate(x, y, z, false);
    float const limit = maxLength > 0.0f ? maxLength : m_radius * 3.0f;
    if (!(path.getPathType() & PATHFIND_NORMAL) || path.Length() > limit)
        return NO_PATH;

    std::set<Creature*> adds;
    PointsArray const& points = path.getPath();
    for (size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const& a = points[i - 1];
        G3D::Vector3 const& b = points[i];
        if (Dist2d(a.x, a.y, x, y) <= stopShort)
            break;
        for (Threat const& threat : threats)
        {
            Creature* pCreature = threat.pCreature;
            if (pCreature == pTarget)
                continue;
            if (std::fabs(threat.z - (a.z + b.z) * 0.5f) > threat.reach)
                continue;
            if (DistToSegment2d(threat.x, threat.y, a.x, a.y, b.x, b.y) <= threat.reach)
                adds.insert(pCreature);
        }
    }

    if (pTarget)
    {
        // Creature::CallAssistance: a hit mob calls in idle kin that can see it.
        float const radius = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (radius > 0.0f)
        {
            std::list<Creature*> helpers;
            MaNGOS::AnyAssistCreatureInRangeCheck check(pTarget, me, radius);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyAssistCreatureInRangeCheck> searcher(helpers, check);
            Cell::VisitGridObjects(pTarget, searcher, radius);
            adds.insert(helpers.begin(), helpers.end());
        }

        CreatureGroup* pGroup = pTarget->GetCreatureGroup();
        if (pGroup && pGroup->HasGroupFlag(OPTION_AGGRO_TOGETHER))
        {
            auto const addMember = [&](Creature* pMember)
            {
                if (pMember && pMember != pTarget && pMember->IsAlive() && !pMember->IsInCombat())
                    adds.insert(pMember);
            };
            pGroup->DoForAllMembers(pTarget->GetMap(), addMember);
            addMember(pTarget->GetMap()->GetCreature(pGroup->GetOriginalLeaderGuid()));
        }
    }
    return uint32(adds.size());
}

WorldObject* SoloFarmAI::PickApproach(std::vector<std::pair<float, WorldObject*>>& picks, uint32 allowed,
    float stopShort, bool fight, uint32* pAdds, float maxLength)
{
    std::sort(picks.begin(), picks.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    std::vector<Threat> const threats = picks.empty() ? std::vector<Threat>() : FindThreats();
    WorldObject* pBest = nullptr;
    uint32 bestAdds = NO_PATH;
    uint32 tries = 0;
    for (auto const& pick : picks)
    {
        if (++tries > PATH_CANDIDATES)
            break;

        WorldObject* pObject = pick.second;
        uint32 const adds = CountAdds(threats, pObject->GetPositionX(), pObject->GetPositionY(),
            pObject->GetPositionZ(), fight ? pObject->ToCreature() : nullptr, stopShort, maxLength);
        if (adds == NO_PATH)
        {
            Blacklist(pObject->GetObjectGuid(), BLACKLIST_MS);
            continue;
        }
        // Guards may die to the grind or wander off; look again soon.
        if (adds > allowed)
        {
            Blacklist(pObject->GetObjectGuid(), GUARDED_RECHECK_MS);
            continue;
        }
        if (adds < bestAdds)
        {
            pBest = pObject;
            bestAdds = adds;
        }
        if (!adds)
            break;
    }
    if (pAdds)
        *pAdds = bestAdds;
    return pBest;
}

bool SoloFarmAI::InPartyDungeon() const
{
    return me->GetMap()->IsDungeon() && !me->GetMap()->IsRaid();
}

uint32 SoloFarmAI::PackLimit() const
{
    if (!InPartyDungeon())
        return AllowedAdds();
    return 3;
}

float SoloFarmAI::RoamLeg() const
{
    // LEFT_AREA is m_radius + 60 from the camp set when the leg starts.
    return std::max(36.0f, std::min(80.0f, m_radius + 40.0f));
}

void SoloFarmAI::NoteCamp()
{
    m_x = me->GetPositionX();
    m_y = me->GetPositionY();
    m_z = me->GetPositionZ();
}

bool SoloFarmAI::PlaceOk(float x, float y, float z) const
{
    TerrainInfo const* pTerrain = me->GetTerrain();
    if (!pTerrain)
        return true;
    AreaEntry const* pArea = AreaEntry::GetById(pTerrain->GetAreaId(x, y, z));
    if (!pArea)
        return true;
    if (pArea->Flags & AREA_FLAG_CAPITAL)
        return false;
    if (!(pArea->Flags & AREA_FLAG_CITY))
        return true;
    if (pArea->Team == AREATEAM_ALLY && me->GetTeam() == HORDE)
        return false;
    if (pArea->Team == AREATEAM_HORDE && me->GetTeam() == ALLIANCE)
        return false;
    return true;
}

WorldObject* SoloFarmAI::FindRoamGoal()
{
    float const px = me->GetPositionX();
    float const py = me->GetPositionY();
    // Inside the pull radius the local search already decided. Look past it.
    float const inner = std::max(12.0f, m_radius * 0.85f);
    std::vector<std::pair<float, WorldObject*>> picks;

    if (DoesGrind())
    {
        uint32 const grayLevel = MaNGOS::XP::GetGrayLevel(me->GetLevel());
        uint32 const topLevel = me->GetLevel() + (InPartyDungeon() ? 10 : 2);
        std::list<Creature*> creatures;
        AliveCreatureCheck check{ px, py, ROAM_LOOK };
        MaNGOS::CreatureListSearcher<AliveCreatureCheck> searcher(creatures, check);
        Cell::VisitGridObjects(px, py, me->GetMap(), searcher, ROAM_LOOK);

        for (Creature* pCreature : creatures)
        {
            if (me->GetDistance(pCreature) < inner)
                continue;
            if (pCreature->IsPet() || pCreature->IsTotem() || !pCreature->GetOwnerGuid().IsEmpty())
                continue;
            if (pCreature->GetCreatureInfo()->type == CREATURE_TYPE_CRITTER || pCreature->IsCivilian() ||
                pCreature->IsGuard() || pCreature->GetUInt32Value(UNIT_NPC_FLAGS))
                continue;
            if (pCreature->IsWorldBoss())
                continue;
            if (pCreature->IsElite() && !InPartyDungeon())
                continue;
            if (!InPartyDungeon() && pCreature->GetLevel() <= grayLevel)
                continue;
            if (pCreature->GetLevel() > topLevel)
                continue;
            if (pCreature->IsInEvadeMode() || IsBlacklisted(pCreature->GetObjectGuid()))
                continue;
            if (pCreature->HasLootRecipient() && !pCreature->IsTappedBy(me))
                continue;
            if (pCreature->GetVictim() && !FightingAlly(pCreature))
                continue;
            if (!IsValidHostileTarget(pCreature) || !PlaceOk(pCreature->GetPositionX(), pCreature->GetPositionY(), pCreature->GetPositionZ()))
                continue;
            picks.emplace_back(me->GetDistance(pCreature), pCreature);
        }
    }

    auto considerGo = [&](GameObject* pGo, bool gather)
    {
        if (!pGo || me->GetDistance(pGo) < inner || IsBlacklisted(pGo->GetObjectGuid()))
            return;
        if (!PlaceOk(pGo->GetPositionX(), pGo->GetPositionY(), pGo->GetPositionZ()))
            return;
        if (gather)
        {
            if (!GatherLockType(pGo))
                return;
        }
        else if (!IsQuestInteractGo(pGo) || !pGo->PlayerCanUse(me))
            return;
        picks.emplace_back(me->GetDistance(pGo), pGo);
    };

    if ((DoesGather() && (m_herbSpell || m_mineSpell)) || DoesQuestObjects())
    {
        std::list<GameObject*> objects;
        struct Check
        {
            float x, y, range;
            bool operator()(GameObject* pGo) const
            {
                return pGo->isSpawned() && !pGo->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_IN_USE) &&
                    (pGo->GetGoType() == GAMEOBJECT_TYPE_CHEST || pGo->GetGoType() == GAMEOBJECT_TYPE_GOOBER) &&
                    pGo->GetDistance2d(x, y) <= range;
            }
        };
        Check check{ px, py, ROAM_LOOK };
        MaNGOS::GameObjectListSearcher<Check> searcher(objects, check);
        Cell::VisitGridObjects(px, py, me->GetMap(), searcher, ROAM_LOOK);
        for (GameObject* pGo : objects)
        {
            if (DoesGather() && (m_herbSpell || m_mineSpell))
                considerGo(pGo, true);
            if (DoesQuestObjects())
                considerGo(pGo, false);
        }
    }

    // A cave path to something we can already see is much longer than the straight line.
    return PickApproach(picks, PackLimit(), 0.0f, true, nullptr, 300.0f);
}

bool SoloFarmAI::RandomRoamPoint(float& x, float& y, float& z) const
{
    float const px = me->GetPositionX();
    float const py = me->GetPositionY();
    float const pz = me->GetPositionZ();
    float const leg = RoamLeg();
    float const facing = me->GetOrientation();
    bool const anyWay = m_wanderSteps == 0;

    std::vector<Threat> const threats = FindThreats();
    uint32 const allowed = AllowedAdds();
    uint32 bestAdds = NO_PATH;
    float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
    for (uint32 i = 0; i < WANDER_TRIES && bestAdds; ++i)
    {
        // Most empty-ground legs keep going. Every few, a real random direction, so it is not a straight line.
        bool const spin = anyWay || i >= WANDER_TRIES / 2;
        float const ang = spin ? frand(0.0f, 2.0f * M_PI_F) : facing + frand(-1.1f, 1.1f);
        float const dist = frand(leg * 0.65f, leg);
        x = px + dist * std::cos(ang);
        y = py + dist * std::sin(ang);
        z = pz;
        if (!me->GetMap()->GetWalkHitPosition(nullptr, px, py, pz, x, y, z, NAV_GROUND))
            continue;
        if (Dist2d(x, y, px, py) < 18.0f)
            continue;
        if (!PlaceOk(x, y, z))
            continue;
        uint32 const adds = CountAdds(threats, x, y, z, nullptr, 0.0f, leg + 30.0f);
        if (adds < bestAdds)
        {
            bestAdds = adds;
            bestX = x;
            bestY = y;
            bestZ = z;
        }
    }

    // A cave bend is a wall at 8 yards. A straight 18-yard ray never leaves the corner.
    if (bestAdds > allowed)
    {
        uint32 const nav = InPartyDungeon() ? (NAV_GROUND | NAV_STEEP_SLOPES) : NAV_GROUND;
        for (uint32 i = 0; i < WANDER_TRIES && bestAdds; ++i)
        {
            x = px;
            y = py;
            z = pz;
            if (!me->GetMap()->GetWalkRandomPosition(nullptr, x, y, z, InPartyDungeon() ? 14.0f : 22.0f, nav))
                continue;
            if (Dist2d(x, y, px, py) < 6.0f)
                continue;
            if (!PlaceOk(x, y, z))
                continue;
            uint32 const adds = CountAdds(threats, x, y, z, nullptr, 0.0f, 80.0f);
            if (adds < bestAdds)
            {
                bestAdds = adds;
                bestX = x;
                bestY = y;
                bestZ = z;
            }
        }
    }
    if (bestAdds > allowed)
        return false;
    x = bestX;
    y = bestY;
    z = bestZ;
    return true;
}

void SoloFarmAI::Wander()
{
    // A winding tunnel is longer than the leash. Slide the camp while we are on a leg
    // we chose; a chase still stops if it runs away from where the pull started.
    if (!me->movespline->Finalized())
    {
        if (m_wandering)
            NoteCamp();
        return;
    }

    if (m_wandering)
    {
        m_wandering = false;
        NoteCamp();
        m_idleUntil = m_now + urand(400, 1000);
        return;
    }
    if (m_now < m_idleUntil)
        return;

    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool aimed = false;
    if (WorldObject* pGoal = FindRoamGoal())
    {
        float const dist = me->GetDistance(pGoal);
        // Pathfinding goes around the bend. A straight ray stops on the first wall.
        if (dist > 8.0f && dist <= RoamLeg() &&
            PlaceOk(pGoal->GetPositionX(), pGoal->GetPositionY(), pGoal->GetPositionZ()))
        {
            x = pGoal->GetPositionX();
            y = pGoal->GetPositionY();
            z = pGoal->GetPositionZ();
            aimed = true;
        }
    }

    if (!aimed && !RandomRoamPoint(x, y, z))
    {
        m_wanderSteps = 0;
        m_idleUntil = m_now + urand(700, 1400);
        return;
    }

    if (!aimed)
    {
        if (m_wanderSteps == 0)
            m_wanderSteps = urand(2, 4);
        else
            --m_wanderSteps;
    }

    // The leash is from here, so this run does not count as leaving the area.
    // Cave ramps are flagged steep; excluding them ends the path a few steps in.
    NoteCamp();
    uint32 const flags = MOVE_PATHFINDING | (InPartyDungeon() ? 0 : MOVE_EXCLUDE_STEEP_SLOPES);
    me->GetMotionMaster()->MovePoint(0, x, y, z, flags);
    m_wandering = true;
}

// ------------------------------------------------------------------ fishing --

bool SoloFarmAI::HasTool(uint32 itemId) const
{
    return me->HasItemCount(itemId, 1, false);
}

uint32 SoloFarmAI::GatherLockType(GameObject const* pGo) const
{
    if (!pGo || pGo->GetGoType() != GAMEOBJECT_TYPE_CHEST)
        return 0;
    LockEntry const* pLock = sLockStore.LookupEntry(pGo->GetGOInfo()->GetLockId());
    if (!pLock)
        return 0;
    for (int i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (pLock->Type[i] != LOCK_KEY_SKILL)
            continue;
        uint32 const lockType = pLock->Index[i];
        if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
            continue;
        SkillType const skill = SkillByLockType(LockType(lockType));
        if (me->GetSkillValue(skill) < int32(pLock->Skill[i]))
            continue;
        if (lockType == LOCKTYPE_MINING && !m_mineSpell)
            continue;
        if (lockType == LOCKTYPE_HERBALISM && !m_herbSpell)
            continue;
        if (lockType == LOCKTYPE_MINING && !HasTool(ITEM_MINING_PICK))
            continue;
        return lockType;
    }
    return 0;
}

bool SoloFarmAI::TakeOpenLoot()
{
    ObjectGuid const lootGuid = me->GetLootGuid();
    if (lootGuid.IsEmpty())
        return false;

    bool all = true;
    if (lootGuid.IsGameObject())
    {
        if (GameObject* pGo = me->GetMap()->GetGameObject(lootGuid))
            all = me->SoloTakeLoot(pGo->loot);
    }
    else if (lootGuid.IsCreature())
    {
        if (Creature* pCreature = me->GetMap()->GetCreature(lootGuid))
            all = me->SoloTakeLoot(pCreature->loot);
    }
    me->GetSession()->DoLootRelease(lootGuid);
    m_gatherGuid.Clear();
    m_gatherSpell = 0;
    m_gatherFails = 0;
    m_questGuid.Clear();
    m_questFails = 0;
    if (!all)
        Stop(SOLO_STOP_BAGS_FULL);
    return true;
}

Creature* SoloFarmAI::FindSkinnable()
{
    if (!m_skinSpell || !HasTool(ITEM_SKINNING_KNIFE))
        return nullptr;

    std::list<Creature*> corpses;
    struct Check
    {
        Player* me;
        float x, y, range;
        bool operator()(Creature* pCreature) const
        {
            return !pCreature->IsAlive() &&
                pCreature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE) &&
                pCreature->IsSkinnableBy(me) &&
                pCreature->GetCreatureType() != CREATURE_TYPE_CRITTER &&
                (pCreature->lootForSkin || pCreature->loot.isLooted()) &&
                pCreature->GetDistance2d(x, y) <= range &&
                me->IsWithinDistInMap(pCreature, range + 5.0f);
        }
    };
    Check check{ me, me->GetPositionX(), me->GetPositionY(), m_radius };
    MaNGOS::CreatureListSearcher<Check> searcher(corpses, check);
    Cell::VisitGridObjects(me, searcher, m_radius);

    std::vector<std::pair<float, WorldObject*>> picks;
    for (Creature* pCreature : corpses)
    {
        if (IsBlacklisted(pCreature->GetObjectGuid()))
            continue;
        int32 const req = me->GetSkillValue(SKILL_SKINNING) < 100
            ? (int32(pCreature->GetLevel()) - 10) * 10
            : int32(pCreature->GetLevel()) * 5;
        if (req > me->GetSkillValue(SKILL_SKINNING))
            continue;
        picks.emplace_back(me->GetDistance(pCreature), pCreature);
    }
    return static_cast<Creature*>(PickApproach(picks, 0, 0.0f, false));
}

bool SoloFarmAI::CanOpenQuestChest(GameObject const* pGo) const
{
    if (!pGo || pGo->GetGoType() != GAMEOBJECT_TYPE_CHEST)
        return false;
    // Herb / mine nodes stay on the profession path.
    if (GatherLockType(pGo))
        return false;

    uint32 const lockId = pGo->GetGOInfo()->GetLockId();
    if (!lockId)
        return true;

    LockEntry const* pLock = sLockStore.LookupEntry(lockId);
    if (!pLock)
        return true;

    bool anyReq = false;
    for (int i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (pLock->Type[i] == LOCK_KEY_ITEM)
        {
            anyReq = true;
            if (pLock->Index[i] && HasTool(pLock->Index[i]))
                return true;
        }
        else if (pLock->Type[i] == LOCK_KEY_SKILL)
        {
            uint32 const lockType = pLock->Index[i];
            if (lockType == LOCKTYPE_HERBALISM || lockType == LOCKTYPE_MINING)
                return false;
            SkillType const skill = SkillByLockType(LockType(lockType));
            // OPEN / TREASURE / KNEELING and the like: no skill, Opening-spell path.
            if (skill == SKILL_NONE)
                return true;
            anyReq = true;
            if (me->GetSkillValue(skill) >= int32(pLock->Skill[i]))
                return true;
        }
    }
    return !anyReq;
}

bool SoloFarmAI::IsQuestInteractGo(GameObject const* pGo) const
{
    if (!pGo || !pGo->isSpawned())
        return false;
    if (pGo->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT | GO_FLAG_IN_USE))
        return false;

    switch (pGo->GetGoType())
    {
        case GAMEOBJECT_TYPE_GOOBER:
        {
            // Gossip menus need a click we cannot finish; page text is fine (Use still rewards).
            if (pGo->GetGOInfo()->goober.gossipID)
                return false;
            // Only objects this character still needs: ReqGO count, or a bound incomplete quest.
            if (me->HasQuestForGO(int32(pGo->GetEntry())))
                return true;
            int32 const questId = pGo->GetGOInfo()->goober.questId;
            return questId > 0 && me->GetQuestStatus(uint32(questId)) == QUEST_STATUS_INCOMPLETE;
        }
        case GAMEOBJECT_TYPE_CHEST:
            if (!pGo->ActivateToQuest(me))
                return false;
            return CanOpenQuestChest(pGo);
        default:
            return false;
    }
}

GameObject* SoloFarmAI::FindQuestObject()
{
    std::list<GameObject*> objects;
    struct Check
    {
        float x, y, range;
        bool operator()(GameObject* pGo) const
        {
            return pGo->isSpawned() &&
                (pGo->GetGoType() == GAMEOBJECT_TYPE_GOOBER || pGo->GetGoType() == GAMEOBJECT_TYPE_CHEST) &&
                pGo->GetDistance2d(x, y) <= range;
        }
    };
    Check check{ me->GetPositionX(), me->GetPositionY(), m_radius };
    MaNGOS::GameObjectListSearcher<Check> searcher(objects, check);
    Cell::VisitGridObjects(me, searcher, m_radius);

    std::vector<std::pair<float, WorldObject*>> picks;
    for (GameObject* pGo : objects)
    {
        if (IsBlacklisted(pGo->GetObjectGuid()) || !IsQuestInteractGo(pGo))
            continue;
        if (!pGo->PlayerCanUse(me))
            continue;
        picks.emplace_back(me->GetDistance(pGo), pGo);
    }
    return static_cast<GameObject*>(PickApproach(picks, 0, 0.0f, false));
}

bool SoloFarmAI::UpdateQuestObjects()
{
    if (TakeOpenLoot())
        return true;

    if (me->IsNonMeleeSpellCasted(false))
        return true;

    if (me->IsInCombat())
        return false;

    if (FreeBagSlots() == 0)
    {
        Stop(SOLO_STOP_BAGS_FULL);
        return true;
    }

    if (Rest())
        return true;

    if (!m_questGuid.IsEmpty())
    {
        GameObject* pGo = me->GetMap()->GetGameObject(m_questGuid);
        if (!pGo || !pGo->isSpawned() || !IsQuestInteractGo(pGo))
        {
            m_questGuid.Clear();
            m_questFails = 0;
        }
        else
        {
            // Locked chests (Tirisfal Pumpkin, etc.) use the Opening spell's range.
            if (!pGo->IsAtInteractDistance(me))
            {
                if (!me->IsMoving())
                    me->GetMotionMaster()->MovePoint(0, pGo->GetPositionX(), pGo->GetPositionY(),
                        pGo->GetPositionZ(), MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
                return true;
            }
            if (!ReadyToGatherCast(me, pGo))
                return true;

            bool ok = false;
            if (pGo->GetGoType() == GAMEOBJECT_TYPE_GOOBER)
            {
                pGo->Use(me);
                ok = true;
            }
            else if (pGo->GetGoType() == GAMEOBJECT_TYPE_CHEST)
            {
                // Kneeling / Opening locks need the real open spell; SoloLoot is the fallback
                // when the character never learned the create-info Opening ranks.
                if (SpellEntry const* pOpen = pGo->GetSpellForLock(me))
                {
                    if (me->CastSpell(pGo, pOpen, false) == SPELL_CAST_OK)
                        return true;
                }
                else
                {
                    ok = me->SoloLootGameObject(pGo);
                    if (!ok)
                    {
                        Stop(SOLO_STOP_BAGS_FULL);
                        m_questGuid.Clear();
                        m_questFails = 0;
                        return true;
                    }
                }
            }

            if (ok)
            {
                m_questGuid.Clear();
                m_questFails = 0;
                return true;
            }
            if (++m_questFails >= 3)
            {
                Blacklist(m_questGuid, BLACKLIST_MS);
                m_questGuid.Clear();
                m_questFails = 0;
            }
            return true;
        }
    }

    if (GameObject* pGo = FindQuestObject())
    {
        m_questGuid = pGo->GetObjectGuid();
        m_questFails = 0;
        return true;
    }
    return false;
}

GameObject* SoloFarmAI::FindGatherNode()
{
    if (!m_herbSpell && !m_mineSpell)
        return nullptr;

    std::list<GameObject*> objects;
    struct Check
    {
        float x, y, range;
        bool operator()(GameObject* pGo) const
        {
            return pGo->isSpawned() && !pGo->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_IN_USE) &&
                pGo->GetGoType() == GAMEOBJECT_TYPE_CHEST &&
                pGo->GetDistance2d(x, y) <= range;
        }
    };
    Check check{ me->GetPositionX(), me->GetPositionY(), m_radius };
    MaNGOS::GameObjectListSearcher<Check> searcher(objects, check);
    Cell::VisitGridObjects(me, searcher, m_radius);

    std::vector<std::pair<float, WorldObject*>> picks;
    for (GameObject* pGo : objects)
    {
        if (IsBlacklisted(pGo->GetObjectGuid()) || !GatherLockType(pGo))
            continue;
        picks.emplace_back(me->GetDistance(pGo), pGo);
    }
    return static_cast<GameObject*>(PickApproach(picks, 0, 0.0f, false));
}

bool SoloFarmAI::UpdateGather()
{
    if (TakeOpenLoot())
        return true;

    if (me->IsNonMeleeSpellCasted(false))
        return true;

    if (me->IsInCombat())
        return false;

    if (!LootCorpses() || FreeBagSlots() == 0)
    {
        Stop(SOLO_STOP_BAGS_FULL);
        return true;
    }

    if (Rest())
        return true;

    // Finish a gather we already walked to.
    if (!m_gatherGuid.IsEmpty() && m_gatherSpell)
    {
        if (m_gatherGuid.IsCreature())
        {
            Creature* pCorpse = me->GetMap()->GetCreature(m_gatherGuid);
            if (!pCorpse || !pCorpse->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
            {
                m_gatherGuid.Clear();
                m_gatherSpell = 0;
            }
            else if (!me->IsWithinDistInMap(pCorpse, INTERACTION_DISTANCE + 1.0f))
            {
                if (!me->IsMoving())
                    me->GetMotionMaster()->MovePoint(0, pCorpse->GetPositionX(), pCorpse->GetPositionY(),
                        pCorpse->GetPositionZ(), MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
                return true;
            }
            else
            {
                if (!ReadyToGatherCast(me, pCorpse))
                    return true;
                if (me->CastSpell(pCorpse, m_gatherSpell, false) == SPELL_CAST_OK)
                {
                    m_gatherFails = 0;
                    return true;
                }
                if (++m_gatherFails >= 3)
                {
                    Blacklist(m_gatherGuid, BLACKLIST_MS);
                    m_gatherGuid.Clear();
                    m_gatherSpell = 0;
                    m_gatherFails = 0;
                }
                return true;
            }
        }
        else if (m_gatherGuid.IsGameObject())
        {
            GameObject* pGo = me->GetMap()->GetGameObject(m_gatherGuid);
            if (!pGo || !pGo->isSpawned() || !GatherLockType(pGo))
            {
                m_gatherGuid.Clear();
                m_gatherSpell = 0;
            }
            else
            {
                float const reach = pGo->GetGOInfo()->GetInteractionDistance() + 1.0f;
                if (!me->IsWithinDistInMap(pGo, reach))
                {
                    if (!me->IsMoving())
                        me->GetMotionMaster()->MovePoint(0, pGo->GetPositionX(), pGo->GetPositionY(),
                            pGo->GetPositionZ(), MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
                    return true;
                }
                if (!ReadyToGatherCast(me, pGo))
                    return true;
                if (me->CastSpell(pGo, m_gatherSpell, false) == SPELL_CAST_OK)
                {
                    m_gatherFails = 0;
                    return true;
                }
                if (++m_gatherFails >= 3)
                {
                    Blacklist(m_gatherGuid, BLACKLIST_MS);
                    m_gatherGuid.Clear();
                    m_gatherSpell = 0;
                    m_gatherFails = 0;
                }
                return true;
            }
        }
    }

    if (Creature* pCorpse = FindSkinnable())
    {
        m_gatherGuid = pCorpse->GetObjectGuid();
        m_gatherSpell = m_skinSpell;
        m_gatherFails = 0;
        return true;
    }

    if (GameObject* pNode = FindGatherNode())
    {
        m_gatherGuid = pNode->GetObjectGuid();
        m_gatherSpell = GatherLockType(pNode) == LOCKTYPE_MINING ? m_mineSpell : m_herbSpell;
        m_gatherFails = 0;
        return true;
    }

    return false;
}

void SoloFarmAI::UpdateFishing()
{
    if (!LootCorpses() || FreeBagSlots() == 0)
    {
        Stop(SOLO_STOP_BAGS_FULL);
        return;
    }

    if (me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        GameObject* pBobber = me->GetGameObject(m_fishingSpell);
        if (pBobber && m_castPending)
        {
            m_castPending = false;
            m_castFails = 0;
            ++m_casts;
        }
        if (pBobber && pBobber->getLootState() == GO_READY)
        {
            if (!m_reelAt)
                m_reelAt = m_now + urand(300, 900);
            else if (m_now >= m_reelAt)
            {
                m_reelAt = 0;
                Reel(pBobber);
            }
        }
        return;
    }
    m_reelAt = 0;

    // Combat lingers a few seconds after a kill; casting, eating and the pole swap all fail in it.
    if (me->IsInCombat())
        return;

    if (Rest())
        return;

    if (!EnsurePole())
    {
        if (m_mode == SOLO_FARM_FISH)
            Stop(SOLO_STOP_NO_POLE);
        else
            m_hasSpot = false;
        return;
    }

    if (m_hasSpot && !m_spotHole.IsEmpty())
    {
        GameObject* pHole = me->GetMap()->GetGameObject(m_spotHole);
        if (!pHole || !pHole->isSpawned())
            m_hasSpot = false;
    }

    // In open water, look again now and then for a school that has spawned since.
    if (m_hasSpot && m_spotHole.IsEmpty() && m_casts >= 12)
    {
        m_casts = 0;
        m_hasSpot = false;
    }

    if (!m_hasSpot && !FindFishingSpot())
    {
        if (m_mode == SOLO_FARM_FISH)
            Stop(SOLO_STOP_NO_WATER);
        return;
    }

    if (me->GetDistance2d(m_spotX, m_spotY) > 2.0f)
    {
        if (!me->movespline->Finalized())
            return;
        if (++m_moveTries > 3)
        {
            m_badSpots.emplace_back(m_spotX, m_spotY);
            m_hasSpot = false;
            return;
        }
        me->GetMotionMaster()->MovePoint(0, m_spotX, m_spotY, m_spotZ, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0.0f, m_spotO);
        return;
    }
    if (!me->movespline->Finalized())
        return;
    m_moveTries = 0;

    if (AngleGap(me->GetOrientation(), m_spotO) > 0.05f)
    {
        me->SetFacingTo(m_spotO);
        return;
    }

    // A server-side channel starts on the next spell update, so the bobber shows up a tick later.
    // A cast that left none found no water where it fell (SPELL_FAILED_NOT_FISHABLE).
    bool failed = m_castPending;
    m_castPending = false;
    if (!failed)
    {
        if (me->CastSpell(me, m_fishingSpell, false) == SPELL_CAST_OK)
        {
            m_castPending = true;
            return;
        }
        failed = true;
    }

    if (failed && ++m_castFails >= 3)
    {
        m_badSpots.emplace_back(m_spotX, m_spotY);
        m_hasSpot = false;
        m_castFails = 0;
    }
}

void SoloFarmAI::Reel(GameObject* pBobber)
{
    pBobber->Use(me);

    // A catch opens a loot window on the bobber, or on the school it landed in.
    ObjectGuid const lootGuid = me->GetLootGuid();
    if (lootGuid.IsEmpty())
        return;

    bool all = true;
    if (GameObject* pGo = me->GetMap()->GetGameObject(lootGuid))
        all = me->SoloTakeLoot(pGo->loot);
    me->GetSession()->DoLootRelease(lootGuid);

    if (!all)
        Stop(SOLO_STOP_BAGS_FULL);
}

bool SoloFarmAI::EnsurePole()
{
    Item* pMain = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (IsPole(pMain))
        return true;

    Item* pPole = nullptr;
    ForEachBagItem(me, [&](Item* pItem)
    {
        if (IsPole(pItem) && me->CanUseItem(pItem->GetProto()) == EQUIP_ERR_OK &&
            (!pPole || pItem->GetProto()->ItemLevel > pPole->GetProto()->ItemLevel))
            pPole = pItem;
    });
    if (!pPole)
        return false;

    if (!m_poleSwapped)
    {
        Item* pOff = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        m_mainHand = pMain ? pMain->GetObjectGuid() : ObjectGuid();
        m_offHand = pOff ? pOff->GetObjectGuid() : ObjectGuid();
        m_poleSwapped = true;
    }

    me->SwapItem(pPole->GetPos(), (INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND);
    return IsPole(me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND));
}

void SoloFarmAI::RestoreWeapons()
{
    if (!m_poleSwapped)
        return;
    m_poleSwapped = false;

    if (Item* pMain = me->GetItemByGuid(m_mainHand))
        if (!pMain->IsEquipped())
            me->SwapItem(pMain->GetPos(), (INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND);
    if (Item* pOff = me->GetItemByGuid(m_offHand))
        if (!pOff->IsEquipped())
            me->SwapItem(pOff->GetPos(), (INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_OFFHAND);
}

// The checks `Spell::EffectTransmitted` makes where the bobber falls.
bool SoloFarmAI::IsFishable(float px, float py, float pz, float fx, float fy) const
{
    TerrainInfo const* pTerrain = me->GetTerrain();
    GridMapLiquidData liquid;
    if (!pTerrain->IsSwimmable(fx, fy, pz + 1.0f, 1.5f, &liquid))
        pTerrain->IsSwimmable(fx, fy, liquid.level, 1.5f, &liquid);

    if (std::fabs(liquid.depth_level) < 1.0f)
        return false;
    return me->GetMap()->isInLineOfSight(px, py, pz + 2.0f, fx, fy, liquid.level);
}

// Out of nine points spread over where a cast from here can fall, how many are water.
uint32 SoloFarmAI::ScoreCast(float px, float py, float pz, float o) const
{
    float const mid = (m_castMin + m_castMax) / 2.0f;
    float const spread = (m_castMax - m_castMin) / (m_castMax + me->GetObjectBoundingRadius()) / 2.0f;

    uint32 score = 0;
    for (float distance : { m_castMin, mid, m_castMax })
        for (float offset : { -spread, 0.0f, spread })
            if (IsFishable(px, py, pz, px + distance * std::cos(o + offset), py + distance * std::sin(o + offset)))
                ++score;
    return score;
}

bool SoloFarmAI::FindFishingSpot()
{
    struct Spot
    {
        float x, y, z, o, cost;
        ObjectGuid hole;
        bool here;
    };
    std::vector<Spot> spots;

    Map* pMap = me->GetMap();
    TerrainInfo const* pTerrain = me->GetTerrain();
    float const mid = (m_castMin + m_castMax) / 2.0f;
    // Fish stays on the water where it started. The other modes fish where they have roamed to.
    bool const pinned = m_mode == SOLO_FARM_FISH;
    float const cx = pinned ? m_x : me->GetPositionX();
    float const cy = pinned ? m_y : me->GetPositionY();
    float const top = std::max(me->GetPositionZ(), pinned ? m_z : me->GetPositionZ()) + 10.0f;

    auto isBad = [&](float x, float y)
    {
        for (auto const& bad : m_badSpots)
            if (Dist2d(x, y, bad.first, bad.second) < 3.0f)
                return true;
        return false;
    };
    // Dry land, or water too shallow to swim in.
    auto ground = [&](float x, float y, float& z)
    {
        z = pMap->GetHeight(x, y, top, true, 50.0f);
        return z > INVALID_HEIGHT && !pTerrain->IsSwimmable(x, y, z, 1.5f);
    };

    // Where the player stands and faces, when that is already a good cast.
    if (!isBad(me->GetPositionX(), me->GetPositionY()) &&
        ScoreCast(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation()) >= 8)
        spots.push_back({ me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation(), -1.0f, ObjectGuid(), true });

    // Schools first, from the shore around them, facing them.
    std::list<GameObject*> holes;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    FishingHoleCheck holeCheck{ cx, cy, m_radius + m_castMax };
    MaNGOS::GameObjectListSearcher<FishingHoleCheck> holeSearcher(holes, holeCheck);
    Cell::VisitGridObjects(cx, cy, pMap, holeSearcher, m_radius + m_castMax);
#endif

    for (GameObject* pHole : holes)
    {
        for (float distance : { mid, (m_castMin + mid) / 2.0f })
        {
            for (uint32 i = 0; i < 24; ++i)
            {
                float const angle = 2.0f * M_PI_F * i / 24.0f;
                float const x = pHole->GetPositionX() + distance * std::cos(angle);
                float const y = pHole->GetPositionY() + distance * std::sin(angle);
                float z;
                if (Dist2d(x, y, cx, cy) > m_radius || isBad(x, y) || !ground(x, y, z))
                    continue;

                float const o = std::fmod(angle + M_PI_F, 2.0f * M_PI_F);
                if (!IsFishable(x, y, z, pHole->GetPositionX(), pHole->GetPositionY()) || ScoreCast(x, y, z, o) < 7)
                    continue;
                spots.push_back({ x, y, z, o, me->GetDistance2d(x, y) - 1000.0f, pHole->GetObjectGuid(), false });
            }
        }
    }

    // Open water: a grid over the area, each point's first direction that casts well.
    float const step = m_radius > 30.0f ? 4.0f : 3.0f;
    for (float dx = -m_radius; dx <= m_radius; dx += step)
    {
        for (float dy = -m_radius; dy <= m_radius; dy += step)
        {
            if (dx * dx + dy * dy > m_radius * m_radius)
                continue;
            float const x = cx + dx;
            float const y = cy + dy;
            float z;
            if (isBad(x, y) || !ground(x, y, z))
                continue;

            for (uint32 i = 0; i < 16; ++i)
            {
                float const o = 2.0f * M_PI_F * i / 16.0f;
                if (!IsFishable(x, y, z, x + mid * std::cos(o), y + mid * std::sin(o)) || ScoreCast(x, y, z, o) < 8)
                    continue;
                spots.push_back({ x, y, z, o, me->GetDistance2d(x, y), ObjectGuid(), false });
                break;
            }
        }
    }

    std::sort(spots.begin(), spots.end(), [](Spot const& a, Spot const& b) { return a.cost < b.cost; });

    uint32 tries = 0;
    for (Spot const& spot : spots)
    {
        if (++tries > 8)
            break;

        float z = spot.z;
        if (!spot.here)
        {
            PathInfo path(me);
            path.calculate(spot.x, spot.y, spot.z, false);
            Vector3 const end = path.getActualEndPosition();
            if (!(path.getPathType() & PATHFIND_NORMAL) || Dist2d(end.x, end.y, spot.x, spot.y) > 1.5f)
            {
                m_badSpots.emplace_back(spot.x, spot.y);
                continue;
            }
            z = end.z;
        }

        m_spotX = spot.x;
        m_spotY = spot.y;
        m_spotZ = z;
        m_spotO = spot.o;
        m_spotHole = spot.hole;
        m_hasSpot = true;
        m_moveTries = 0;
        m_castFails = 0;
        m_casts = 0;
        m_castPending = false;
        return true;
    }
    return false;
}
