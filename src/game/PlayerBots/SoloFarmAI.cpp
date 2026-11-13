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
#include "SpellMgr.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Formulas.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "WorldSession.h"
#include "Utilities/Random.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Fishing from Artisan down to Apprentice.
    uint32 const FISHING_SPELLS[] = { 18248, 7732, 7731, 7620 };

    uint32 const TICK_MS = 250;
    uint32 const SEARCH_MS = 1000;
    uint32 const PULL_TIMEOUT_MS = 20000;
    uint32 const BLACKLIST_MS = 60000;
    // Below this a caster or hunter backs off from a mob in melee range; above it, stands and fights.
    float const RUN_AWAY_HEALTH_PCT = 40.0f;

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        return std::sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
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

    template <class F>
    void ForEachBagItem(Player const* pPlayer, F f)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
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

    struct FishingHoleCheck
    {
        float x, y, range;
        bool operator()(GameObject* pGo) const
        {
            return pGo->GetGOInfo()->type == GAMEOBJECT_TYPE_FISHINGHOLE && pGo->isSpawned() &&
                pGo->GetDistance2d(x, y) <= range;
        }
    };
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

    pPlayer->SetAI(new SoloFarmAI(pPlayer, mode, radius));

    chat.PSendSysMessage("[solo] farm=%s %u", mode == SOLO_FARM_FISH ? "fish" : "grind", uint32(radius));
    if (mode == SOLO_FARM_FISH)
        chat.PSendSysMessage(zh ? "开始自动钓鱼：在这里 %u 码内找鱼群或水面，自动下钩、收竿、拾取。输入 .solo stop，或在角色站定时按移动键即可停止。"
                                : "Auto fishing within %u yards of here: finds a school or open water, casts, reels in and loots. Type .solo stop, or press a movement key while standing, to stop.",
            uint32(radius));
    else
        chat.PSendSysMessage(zh ? "开始自动打怪：以这里为中心 %u 码内游荡、找怪、战斗、拾取，血蓝低了会吃喝休息。输入 .solo stop，或在角色站定时按移动键即可停止。"
                                : "Auto grinding within %u yards of here: wanders, picks fights, loots, and eats or drinks when low. Type .solo stop, or press a movement key while standing, to stop.",
            uint32(radius));
    return true;
}

void SoloFarmAI::StopFor(Player* pPlayer, SoloFarmStop reason)
{
    if (SoloFarmAI* pAI = Of(pPlayer))
        pAI->Stop(reason);
}

void SoloFarmAI::OnManualMove(Player* pPlayer)
{
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
        if (m_mode == SOLO_FARM_FISH && me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            me->InterruptSpell(CURRENT_CHANNELED_SPELL);
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
        char const* what = m_mode == SOLO_FARM_FISH ? (zh ? "钓鱼" : "fishing") : (zh ? "打怪" : "grinding");
        char line[256] = {};
        switch (reason)
        {
            case SOLO_STOP_COMMAND:
                snprintf(line, sizeof(line), zh ? "已停止自动%s。" : "Auto %s stopped.", what);
                break;
            case SOLO_STOP_MOVED:
                snprintf(line, sizeof(line), zh ? "你移动了角色，自动%s已停止。" : "You moved, so auto %s stopped.", what);
                break;
            case SOLO_STOP_DIED:
                snprintf(line, sizeof(line), zh ? "角色死亡，自动%s已停止。" : "You died; auto %s stopped.", what);
                break;
            case SOLO_STOP_BAGS_FULL:
                snprintf(line, sizeof(line), zh ? "背包满了，自动%s已停止。" : "Your bags are full; auto %s stopped.", what);
                break;
            case SOLO_STOP_LEFT_AREA:
                snprintf(line, sizeof(line), zh ? "离开了区域，自动%s已停止。" : "You left the area; auto %s stopped.", what);
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
        Stop(SOLO_STOP_DIED);
        return;
    }

    if (me->GetMapId() != m_mapId || me->IsTaxiFlying() ||
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

    if (m_mode == SOLO_FARM_FISH)
    {
        Pet* pPet = me->GetPet();
        bool const attacked = !me->GetAttackers().empty() || (pPet && !pPet->GetAttackers().empty());
        if (attacked || me->GetVictim())
        {
            if (me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                me->InterruptSpell(CURRENT_CHANNELED_SPELL);
            RestoreWeapons();
            if (UpdateCombat())
                return;
        }
        UpdateFishing();
        return;
    }

    if (UpdateCombat())
        return;
    UpdateGrind();
}

// ------------------------------------------------------------------ combat --

// Alone, backing off only leads the mob on until it leashes home and drops the fight.
bool SoloFarmAI::RunAwayFromTarget(Unit* pEnemy)
{
    if (me->GetHealthPercent() >= RUN_AWAY_HEALTH_PCT)
        return false;
    return PartyBotAI::RunAwayFromTarget(pEnemy);
}

bool SoloFarmAI::UpdateCombat()
{
    Unit* pVictim = me->GetVictim();
    if (pVictim && !IsValidHostileTarget(pVictim))
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

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return true;

    if (me->GetHealthPercent() < 30.0f && UseConsumable(0, SPELL_EFFECT_HEAL))
        return true;
    if (me->GetHealthPercent() < 45.0f && HealSelf())
        return true;
    if (me->GetPowerType() == POWER_MANA && me->GetPowerPercent(POWER_MANA) < 15.0f &&
        UseConsumable(0, SPELL_EFFECT_ENERGIZE))
        return true;

    if (!me->IsMoving())
    {
        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
            (GetRole() == ROLE_MELEE_DPS || GetRole() == ROLE_TANK) &&
            AttackStart(pVictim))
            return true;

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

bool SoloFarmAI::Rest()
{
    bool const usesMana = me->GetPowerType() == POWER_MANA;
    bool const needHealth = me->GetHealthPercent() < (m_resting ? 95.0f : 60.0f);
    bool const needMana = usesMana && me->GetPowerPercent(POWER_MANA) < (m_resting ? 95.0f : 40.0f);

    if (!needHealth && !needMana)
    {
        if (m_resting)
        {
            m_resting = false;
            if (me->GetStandState() != UNIT_STAND_STATE_STAND)
                me->SetStandState(UNIT_STAND_STATE_STAND);
        }
        return false;
    }

    if (!m_resting)
    {
        m_resting = true;
        m_wandering = false;
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
        if (!me->movespline->Finalized())
            me->StopMoving();
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

    // Without food or drink, wait for the natural regeneration.
    return true;
}

bool SoloFarmAI::UseConsumable(uint32 auraType, uint32 effect)
{
    Item* pBest = nullptr;
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

            bool match = false;
            for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (auraType && pSpell->Effect[i] == SPELL_EFFECT_APPLY_AURA && pSpell->EffectApplyAuraName[i] == auraType)
                    match = true;
                if (effect && pSpell->Effect[i] == effect &&
                    (effect != SPELL_EFFECT_ENERGIZE || pSpell->EffectMiscValue[i] == POWER_MANA))
                    match = true;
            }
            if (match && (!pBest || pProto->ItemLevel > pBest->GetProto()->ItemLevel))
                pBest = pItem;
        }
    });

    return pBest && UseItemEffect(pBest);
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
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
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
        if (Creature* pTarget = FindTarget())
        {
            m_pullGuid = pTarget->GetObjectGuid();
            m_pullStart = m_now;
            m_wandering = false;
            AttackStart(pTarget);
            return;
        }
    }

    Wander();
}

Creature* SoloFarmAI::FindTarget()
{
    std::list<Creature*> creatures;
    AliveCreatureCheck check{ m_x, m_y, m_radius };
    MaNGOS::CreatureListSearcher<AliveCreatureCheck> searcher(creatures, check);
    Cell::VisitGridObjects(m_x, m_y, me->GetMap(), searcher, m_radius);

    uint32 const grayLevel = MaNGOS::XP::GetGrayLevel(me->GetLevel());
    uint32 const topLevel = me->GetLevel() + 2;

    std::vector<std::pair<float, Creature*>> picks;
    for (Creature* pCreature : creatures)
    {
        if (pCreature->IsPet() || pCreature->IsTotem() || !pCreature->GetOwnerGuid().IsEmpty())
            continue;
        if (pCreature->GetCreatureInfo()->type == CREATURE_TYPE_CRITTER || pCreature->IsCivilian() ||
            pCreature->IsGuard() || pCreature->IsElite() || pCreature->IsWorldBoss())
            continue;
        // Vendors, quest givers, trainers of the other side: never a grinding target.
        if (pCreature->GetUInt32Value(UNIT_NPC_FLAGS))
            continue;
        if (pCreature->GetLevel() <= grayLevel || pCreature->GetLevel() > topLevel)
            continue;
        if (pCreature->IsInEvadeMode() || IsBlacklisted(pCreature->GetObjectGuid()))
            continue;
        if (pCreature->HasLootRecipient() && !pCreature->IsTappedBy(me))
            continue;
        if (pCreature->GetVictim() && pCreature->GetVictim() != me && pCreature->GetVictim() != me->GetPet())
            continue;
        if (!IsValidHostileTarget(pCreature))
            continue;

        // A target with hostile company pulls more than one: it ranks as if further away.
        uint32 company = 0;
        for (Creature* pOther : creatures)
            if (pOther != pCreature && pOther->IsHostileTo(me) && pOther->IsWithinDist(pCreature, 10.0f))
                ++company;

        picks.emplace_back(me->GetDistance(pCreature) + 15.0f * company, pCreature);
    }

    std::sort(picks.begin(), picks.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    uint32 tries = 0;
    for (auto const& pick : picks)
    {
        if (++tries > 4)
            break;

        Creature* pCreature = pick.second;
        PathInfo path(me);
        path.calculate(pCreature->GetPositionX(), pCreature->GetPositionY(), pCreature->GetPositionZ(), false);
        if (!(path.getPathType() & PATHFIND_NORMAL) || path.Length() > m_radius * 3.0f)
        {
            Blacklist(pCreature->GetObjectGuid(), BLACKLIST_MS);
            continue;
        }
        return pCreature;
    }
    return nullptr;
}

void SoloFarmAI::Wander()
{
    if (!me->movespline->Finalized())
        return;

    if (m_wandering)
    {
        m_wandering = false;
        m_idleUntil = m_now + urand(1500, 4000);
        return;
    }
    if (m_now < m_idleUntil)
        return;

    float x = m_x;
    float y = m_y;
    float z = m_z;
    if (!me->GetMap()->GetWalkRandomPosition(nullptr, x, y, z, m_radius))
        return;

    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
    m_wandering = true;
}

// ------------------------------------------------------------------ fishing --

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
        Stop(SOLO_STOP_NO_POLE);
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

    if (me->CastSpell(me, m_fishingSpell, false) == SPELL_CAST_OK && me->GetGameObject(m_fishingSpell))
    {
        m_castFails = 0;
        ++m_casts;
        return;
    }

    // The bobber found no water where it fell (SPELL_FAILED_NOT_FISHABLE): another spot.
    if (++m_castFails >= 3)
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
    float const top = std::max(me->GetPositionZ(), m_z) + 10.0f;

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
    FishingHoleCheck holeCheck{ m_x, m_y, m_radius + m_castMax };
    MaNGOS::GameObjectListSearcher<FishingHoleCheck> holeSearcher(holes, holeCheck);
    Cell::VisitGridObjects(m_x, m_y, pMap, holeSearcher, m_radius + m_castMax);

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
                if (Dist2d(x, y, m_x, m_y) > m_radius || isBad(x, y) || !ground(x, y, z))
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
            float const x = m_x + dx;
            float const y = m_y + dy;
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
        return true;
    }
    return false;
}
