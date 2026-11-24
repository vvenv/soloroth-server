#ifndef MANGOS_COMBAT_BOT_BASE_H
#define MANGOS_COMBAT_BOT_BASE_H

#include "PlayerBotAI.h"
#include "SpellEntry.h"
#include "Player.h"

struct HealSpellCompare
{
    bool operator() (SpellEntry const* const lhs, SpellEntry const* const rhs) const
    {
        uint32 spell1dmg = 0;
        uint32 spell2dmg = 0;

        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (lhs->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    spell1dmg += lhs->EffectBasePoints[i];
                    break;
            }
        }
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (rhs->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    spell2dmg += rhs->EffectBasePoints[i];
                    break;
            }
        }

        return spell1dmg > spell2dmg;
    }
};

struct HealAuraCompare
{
    bool operator() (SpellEntry const* const lhs, SpellEntry const* const rhs) const
    {
        uint32 spell1dmg = 0;
        uint32 spell2dmg = 0;

        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (lhs->Effect[i])
            {
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    if (lhs->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                        spell1dmg += lhs->EffectBasePoints[i];
                    break;
            }
        }
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (rhs->Effect[i])
            {
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    if (rhs->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                        spell2dmg += rhs->EffectBasePoints[i];
                    break;
            }
        }

        return spell1dmg > spell2dmg;
    }
};

class CombatBotBaseAI : public PlayerBotAI
{
public:

    CombatBotBaseAI() : PlayerBotAI(nullptr)
    {
        for (auto& ptr : m_spells.raw.spells)
            ptr = nullptr;
    }

    virtual void OnPacketReceived(WorldPacket const* packet) override;
    void SendBattlefieldPortPacket();
    void SendBattlemasterJoinPacket(uint8 battlegroundId);
    void SendAreaTriggerPacket(uint32 areaTriggerId);
    void ActivateNearbyAreaTrigger();

    void AutoAssignRole();
    void PopulateSpellData();
    void ResetSpellData();
    void AddAllSpellReagents();
    void SummonPetIfNeeded();
    void LearnArmorProficiencies();
    void LearnPremadeSpecForClass();
    void EquipPremadeGearTemplate();
    void EquipRandomGearInEmptySlots();
    // Swaps worn pieces for random ones of a higher item level, then fills empty slots.
    void UpgradeGear();
    void EquipRandomGear(bool upgrade);
    void AutoEquipGear(uint32 option);
    // Bow / gun / crossbow in the ranged slot, with matching ammo. Wands and thrown break Auto Shot.
    void EnsureHunterRangedWeapon();
    void LearnRandomTalents();
    
    uint8 GetAttackersInRangeCount(float range) const;
    // Hostiles near pTarget that are on our / the party's threat — not idle
    // camp trash or a neighbor pull that is merely IsInCombat.
    uint8 GetCombatEnemyCountInRadiusAround(Unit const* pTarget, float radius) const;
    Unit* SelectAttackerDifferentFrom(Unit const* pExcept) const;
    Unit* SelectHealTarget(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f) const;
    Unit* SelectPeriodicHealTarget(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f) const;
    Player* SelectBuffTarget(SpellEntry const* pSpellEntry) const;
    Player* SelectBuffTarget(SpellEntry const* pSingleSpellEntry, SpellEntry const* pGroupSpellEntry, SpellEntry const*& pSelectedSpellEntry) const;
    Player* SelectDispelTarget(SpellEntry const* pSpellEntry) const;
    bool IsValidBuffTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool IsValidHealTarget(Unit const* pTarget, float healthPercent = 100.0f) const;
    bool IsValidHostileTarget(Unit const* pTarget) const;
    bool IsValidDispelTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    // Higher = strip sooner. Fear/charm/silence first; tank beats DPS for the same aura.
    int GetFriendlyDispelScore(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    // Higher = Purge / offensive Dispel Magic first (absorbs, damage buffs, shields).
    int GetEnemyBuffStripScore(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool FindAndPreHealTarget();
    bool FindAndHealInjuredAlly(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f);
    bool HealInjuredTarget(Unit* pTarget);
    bool HealInjuredTargetDirect(Unit* pTarget);
    bool HealInjuredTargetPeriodic(Unit* pTarget);
    template <class T>
    SpellEntry const* SelectMostEfficientHealingSpell(Unit const* pTarget, std::set<SpellEntry const*, T>& spellList) const;
    template <class T>
    SpellEntry const* SelectMostEfficientHealingSpell(Unit const* pTarget, int32 missingHealth, std::set<SpellEntry const*, T>& spellList) const;
    int32 GetIncomingdamage(Unit const* pTarget) const;
    bool AreOthersOnSameTarget(ObjectGuid guid, bool checkMelee = true, bool checkSpells = true) const;
    // myThreat / tankOrVictimThreat. 1.0 = about to rip; 0 = safe.
    float GetThreatRatio(Unit* pEnemy) const;
    bool IsNearTankThreat(Unit* pEnemy, float ratio = 0.80f) const;
    uint32 GetAuraStackCount(Unit const* pTarget, uint32 spellId) const;
    // Party role of a member (tank / healer), including the human leader guess.
    bool IsPartyTank(Player const* pMember) const;
    bool IsPartyHealer(Player const* pMember) const;
    // Elite / boss / multi-pull / long fight — burn Recklessness, Combustion, etc.
    bool IsDangerousFight(Unit const* pVictim) const;
    bool ShouldBurnMajorCooldown(Unit const* pVictim) const;

    SpellCastResult DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry);
    // Right-drag / keyboard turn from the player: pause AI facing broadcasts briefly.
    void NotePlayerFacing();
    // Face pTarget when outside the front cone. Broadcasts MSG_MOVE_SET_FACING unless
    // this is an own character whose player just looked away.
    void EnsureFacing(Unit* pTarget);
    virtual bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool IsWearingShield(Player* pPlayer) const;
    bool IsInDuel() const;
    CombatBotRoles GetRole() const;

    void EquipOrUseNewItem();
    void AddItemToInventory(uint32 itemId, uint32 count = 1);
    void AddHunterAmmo();
    // Level-appropriate health / mana potions (and keep a few in the bags).
    void EnsureCombatPotions();
    // Instant heal / mana from a potion or healthstone / mana gem. Prefer stones.
    bool UseCombatConsumable(uint32 effect);
    // True when bags already hold a conjured stone/gem for this effect.
    bool HasConjuredConsumable(uint32 effect) const;
    // Free Action / Limited Invulnerability when rooted, snared, or nearly dead.
    bool TryUtilityConsumable();
    // Blood Fury / Berserking / War Stomp when an elite or packed fight needs it.
    bool TryRacialCombatAbility(Unit* pVictim);
    uint8 GetHighestHonorRankFromEquippedItems() const;
    void UpdateVisualHonorRankBasedOnItems();
    // PartyBotAI overrides to refuse bee-lining through idle packs.
    virtual void BeginChasing(Unit* pVictim) const;
    // Melee DPS and healers approach from behind; ranged hold a band instead.
    virtual bool ChaseFromBehind(Unit* /*pVictim*/) const
    {
        return m_role == ROLE_MELEE_DPS || m_role == ROLE_HEALER;
    }
    bool SummonShamanTotems();
    // True when a party member is feared / charmed / asleep — drop Tremor.
    bool PartyNeedsTremorTotem() const;
    SpellCastResult CastWeaponBuff(SpellEntry const* pSpellEntry, EquipmentSlots slot);
    bool UseTrinketEffects(bool onlyToBreakCC = false);
    bool UseItemEffect(Item* pItem, bool onlyToBreakCC = false);
    void BreakCrowdControlEffects();

    virtual void UpdateInCombatAI() = 0;
    virtual void UpdateOutOfCombatAI() = 0;
    virtual void UpdateInCombatAI_Paladin() = 0;
    virtual void UpdateOutOfCombatAI_Paladin() = 0;
    virtual void UpdateInCombatAI_Shaman() = 0;
    virtual void UpdateOutOfCombatAI_Shaman() = 0;
    virtual void UpdateInCombatAI_Hunter() = 0;
    virtual void UpdateOutOfCombatAI_Hunter() = 0;
    virtual void UpdateInCombatAI_Mage() = 0;
    virtual void UpdateOutOfCombatAI_Mage() = 0;
    virtual void UpdateInCombatAI_Priest() = 0;
    virtual void UpdateOutOfCombatAI_Priest() = 0;
    virtual void UpdateInCombatAI_Warlock() = 0;
    virtual void UpdateOutOfCombatAI_Warlock() = 0;
    virtual void UpdateInCombatAI_Warrior() = 0;
    virtual void UpdateOutOfCombatAI_Warrior() = 0;
    virtual void UpdateInCombatAI_Rogue() = 0;
    virtual void UpdateOutOfCombatAI_Rogue() = 0;
    virtual void UpdateInCombatAI_Druid() = 0;
    virtual void UpdateOutOfCombatAI_Druid() = 0;

    static bool IsPhysicalDamageClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_HUNTER:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsRangedDamageClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_HUNTER:
            case CLASS_PRIEST:
            case CLASS_SHAMAN:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsMeleeDamageClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsMeleeWeaponClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_SHAMAN:
                return true;
        }
        return false;
    }
    static bool IsShieldClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_SHAMAN:
                return true;
        }
        return false;
    }
    static bool IsTankClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsHealerClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_PALADIN:
            case CLASS_PRIEST:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsStealthClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_ROGUE:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }

    SpellEntry const* GetCrowdControlSpell() const
    {
        switch (me->GetClass())
        {
            case CLASS_PALADIN:
                return m_spells.paladin.pHammerOfJustice;
            case CLASS_MAGE:
                return m_spells.mage.pPolymorph;
            case CLASS_PRIEST:
                return m_spells.priest.pShackleUndead;
            case CLASS_WARLOCK:
                return m_spells.warlock.pBanish;
            case CLASS_ROGUE:
                return m_spells.rogue.pBlind;
            case CLASS_DRUID:
                return m_spells.druid.pHibernate;
        }
        return nullptr;
    }

    SpellEntry const* m_resurrectionSpell = nullptr;
    std::vector<SpellEntry const*> m_spellListTaunt;
    std::set<SpellEntry const*, HealAuraCompare> m_spellListPeriodicHeal;
    std::set<SpellEntry const*, HealSpellCompare> m_spellListDirectHeal;
    union
    {
        struct
        {
            SpellEntry const* spells[45];
        } raw;
        struct
        {
            SpellEntry const* pAura;
            SpellEntry const* pSeal;
            SpellEntry const* pBlessingBuff;
            SpellEntry const* pBlessingOfProtection;
            SpellEntry const* pBlessingOfFreedom;
            SpellEntry const* pBlessingOfSacrifice;
            SpellEntry const* pHammerOfJustice;
            SpellEntry const* pJudgement;
            SpellEntry const* pExorcism;
            SpellEntry const* pConsecration;
            SpellEntry const* pHammerOfWrath;
            SpellEntry const* pCleanse;
            SpellEntry const* pDivineShield;
            SpellEntry const* pLayOnHands;
            SpellEntry const* pRighteousFury;
            SpellEntry const* pHolyShock;
            SpellEntry const* pDivineFavor;
            SpellEntry const* pHolyWrath;
            SpellEntry const* pTurnEvil;
            SpellEntry const* pHolyShield;
        } paladin;
        struct
        {
            SpellEntry const* pLightningBolt;
            SpellEntry const* pChainLightning;
            SpellEntry const* pEarthShock;
            SpellEntry const* pFlameShock;
            SpellEntry const* pFrostShock;
            SpellEntry const* pPurge;
            SpellEntry const* pStormstrike;
            SpellEntry const* pElementalMastery;
            SpellEntry const* pLightningShield;
            SpellEntry const* pGhostWolf;
            SpellEntry const* pCureDisease;
            SpellEntry const* pCurePoison;
            SpellEntry const* pAirTotem;
            SpellEntry const* pEarthTotem;
            SpellEntry const* pFireTotem;
            SpellEntry const* pWaterTotem;
            SpellEntry const* pManaTideTotem;
            SpellEntry const* pWeaponBuff;
            // Dropped on demand when the party is feared (overrides the earth pick).
            SpellEntry const* pTremorTotem;
        } shaman;
        struct
        {
            SpellEntry const* pAspectOfTheCheetah;
            SpellEntry const* pAspectOfTheMonkey;
            SpellEntry const* pAspectOfTheHawk;
            SpellEntry const* pSerpentSting;
            SpellEntry const* pArcaneShot;
            SpellEntry const* pAimedShot;
            SpellEntry const* pMultiShot;
            SpellEntry const* pConcussiveShot;
            SpellEntry const* pWingClip;
            SpellEntry const* pHuntersMark;
            SpellEntry const* pMongooseBite;
            SpellEntry const* pRaptorStrike;
            SpellEntry const* pDisengage;
            SpellEntry const* pFeignDeath;
            SpellEntry const* pScareBeast;
            SpellEntry const* pVolley;
            // Long CD haste — burn on elites / packed pulls only.
            SpellEntry const* pRapidFire;
        } hunter;
        struct
        {
            SpellEntry const* pIceArmor;
            SpellEntry const* pArcaneIntellect;
            SpellEntry const* pArcaneBrilliance;
            SpellEntry const* pIceBarrier;
            SpellEntry const* pManaShield;
            SpellEntry const* pPolymorph;
            SpellEntry const* pFrostbolt;
            SpellEntry const* pFireBlast;
            SpellEntry const* pFireball;
            SpellEntry const* pArcaneExplosion;
            SpellEntry const* pFrostNova;
            SpellEntry const* pConeofCold;
            SpellEntry const* pBlink;
            SpellEntry const* pCounterspell;
            SpellEntry const* pPresenceOfMind;
            SpellEntry const* pArcanePower;
            SpellEntry const* pRemoveLesserCurse;
            SpellEntry const* pScorch;
            SpellEntry const* pPyroblast;
            SpellEntry const* pEvocation;
            SpellEntry const* pIceBlock;
            SpellEntry const* pBlizzard;
            SpellEntry const* pBlastWave;
            SpellEntry const* pCombustion;
            // Highest Conjure Mana * gem the mage knows.
            SpellEntry const* pConjureManaGem;
        } mage;
        struct
        {
            SpellEntry const* pPowerWordFortitude;
            SpellEntry const* pDivineSpirit;
            SpellEntry const* pPrayerofSpirit;
            SpellEntry const* pPrayerofFortitude;
            SpellEntry const* pPrayerofShadowProtection;
            SpellEntry const* pInnerFire;
            SpellEntry const* pShadowProtection;
            SpellEntry const* pPowerWordShield;
            SpellEntry const* pHolyNova;
            SpellEntry const* pHolyFire;
            SpellEntry const* pMindBlast;
            SpellEntry const* pMindFlay;
            SpellEntry const* pShadowWordPain;
            SpellEntry const* pInnerFocus;
            SpellEntry const* pAbolishDisease;
            SpellEntry const* pDispelMagic;
            SpellEntry const* pManaBurn;
            SpellEntry const* pDevouringPlague;
            SpellEntry const* pPsychicScream;
            SpellEntry const* pShadowform;
            SpellEntry const* pVampiricEmbrace;
            SpellEntry const* pSilence;
            SpellEntry const* pFade;
            SpellEntry const* pShackleUndead;
            SpellEntry const* pSmite;
            // Pre-fight on the tank; renews when it falls off.
            SpellEntry const* pFearWard;
        } priest;
        struct
        {
            SpellEntry const* pDemonArmor;
            SpellEntry const* pDeathCoil;
            SpellEntry const* pDetectInvisibility;
            SpellEntry const* pShadowWard;
            SpellEntry const* pShadowBolt;
            SpellEntry const* pCorruption;
            SpellEntry const* pConflagrate;
            SpellEntry const* pShadowburn;
            SpellEntry const* pSearingPain;
            SpellEntry const* pImmolate;
            SpellEntry const* pRainOfFire;
            SpellEntry const* pDemonicSacrifice;
            SpellEntry const* pDrainLife;
            SpellEntry const* pSiphonLife;
            SpellEntry const* pBanish;
            SpellEntry const* pFear;
            SpellEntry const* pHowlofTerror;
            SpellEntry const* pCurseofAgony;
            SpellEntry const* pCurseofDoom;
            SpellEntry const* pCurseoftheElements;
            SpellEntry const* pCurseofShadow;
            SpellEntry const* pCurseofRecklessness;
            SpellEntry const* pCurseofTongues;
            SpellEntry const* pCurseofExhaustion;
            SpellEntry const* pLifeTap;
            // Highest Create Healthstone rank — restock between pulls.
            SpellEntry const* pCreateHealthstone;
        } warlock;
        struct
        {
            SpellEntry const* pBattleStance;
            SpellEntry const* pBerserkerStance;
            SpellEntry const* pDefensiveStance;
            SpellEntry const* pCharge;
            SpellEntry const* pIntercept;
            SpellEntry const* pOverpower;
            SpellEntry const* pHeroicStrike;
            SpellEntry const* pCleave;
            SpellEntry const* pExecute;
            SpellEntry const* pMortalStrike;
            SpellEntry const* pBloodthirst;
            SpellEntry const* pBloodrage;
            SpellEntry const* pBerserkerRage;
            SpellEntry const* pRecklessness;
            SpellEntry const* pRetaliation;
            SpellEntry const* pDeathWish;
            SpellEntry const* pIntimidatingShout;
            SpellEntry const* pPummel;
            SpellEntry const* pRend;
            SpellEntry const* pDisarm;
            SpellEntry const* pWhirlwind;
            SpellEntry const* pBattleShout;
            SpellEntry const* pDemoralizingShout;
            SpellEntry const* pHamstring;
            SpellEntry const* pThunderClap;
            SpellEntry const* pSweepingStrikes;
            SpellEntry const* pLastStand;
            SpellEntry const* pShieldBlock;
            SpellEntry const* pShieldWall;
            SpellEntry const* pShieldBash;
            SpellEntry const* pShieldSlam;
            SpellEntry const* pSunderArmor;
            SpellEntry const* pConcussionBlow;
            SpellEntry const* pPiercingHowl;
            // Tank: after dodge/parry/block. Arms filler (not Shield Slam).
            SpellEntry const* pRevenge;
            SpellEntry const* pSlam;
        } warrior;
        struct
        {
            SpellEntry const* pSliceAndDice;
            SpellEntry const* pSinisterStrike;
            SpellEntry const* pAdrenalineRush;
            SpellEntry const* pEviscerate;
            SpellEntry const* pStealth;
            SpellEntry const* pGarrote;
            SpellEntry const* pAmbush;
            SpellEntry const* pCheapShot;
            SpellEntry const* pPremeditation;
            SpellEntry const* pBackstab;
            SpellEntry const* pHemorrhage;
            SpellEntry const* pGhostlyStrike;
            SpellEntry const* pGouge;
            SpellEntry const* pRupture;
            SpellEntry const* pExposeArmor;
            SpellEntry const* pKidneyShot;
            SpellEntry const* pColdBlood;
            SpellEntry const* pBladeFlurry;
            SpellEntry const* pVanish;
            SpellEntry const* pBlind;
            SpellEntry const* pPreparation;
            SpellEntry const* pEvasion;
            SpellEntry const* pRiposte;
            SpellEntry const* pKick;
            SpellEntry const* pSprint;
            SpellEntry const* pMainHandPoison;
            SpellEntry const* pOffHandPoison;
        } rogue;
        struct
        {
            SpellEntry const* pBearForm;
            SpellEntry const* pCatForm;
            SpellEntry const* pTravelForm;
            SpellEntry const* pAquaticForm;
            SpellEntry const* pMoonkinForm;
            SpellEntry const* pWrath;
            SpellEntry const* pMoonfire;
            SpellEntry const* pStarfire;
            SpellEntry const* pHurricane;
            SpellEntry const* pInsectSwarm;
            SpellEntry const* pBarkskin;
            SpellEntry const* pNaturesGrasp;
            SpellEntry const* pMarkoftheWild;
            SpellEntry const* pGiftoftheWild;
            SpellEntry const* pThorns;
            SpellEntry const* pRemoveCurse;
            SpellEntry const* pCurePoison;
            SpellEntry const* pAbolishPoison;
            SpellEntry const* pRebirth;
            SpellEntry const* pFaerieFire;
            SpellEntry const* pInnervate;
            SpellEntry const* pNaturesSwiftness;
            SpellEntry const* pEntanglingRoots;
            SpellEntry const* pHibernate;
            // Cat
            SpellEntry const* pProwl;
            SpellEntry const* pPounce;
            SpellEntry const* pRavage;
            SpellEntry const* pClaw;
            SpellEntry const* pShred;
            SpellEntry const* pRake;
            SpellEntry const* pRip;
            SpellEntry const* pFerociousBite;
            SpellEntry const* pTigersFury;
            SpellEntry const* pDash;
            SpellEntry const* pFaerieFireFeral;
            SpellEntry const* pCower;
            // Bear
            SpellEntry const* pGrowl;
            SpellEntry const* pChallengingRoar;
            SpellEntry const* pDemoralizingRoar;
            SpellEntry const* pEnrage;
            SpellEntry const* pFrenziedRegeneration;
            SpellEntry const* pSwipe;
            SpellEntry const* pMaul;
            SpellEntry const* pBash;
            SpellEntry const* pFeralCharge;
        } druid;
    } m_spells;

    bool m_initialized = false;
    bool m_isBuffing = false;
    bool m_preventCasting = false;
    bool m_receivedBgInvite = false;
    // Drives a person's own character (`.solo grind`, `.solo fish`): no free items, reagents or pets.
    bool m_ownCharacter = false;
    // Last MSG_MOVE_SET_FACING / turn the player sent (own character only).
    uint32 m_playerFacingAt = 0;
    uint8 m_visualHonorRank = 0;
    CombatBotRoles m_role = ROLE_INVALID;
    // Part of a premade spec name (e.g. "frost") preferred among the role's
    // templates of the same level.
    std::string m_specHint;
};

#endif
