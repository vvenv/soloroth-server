/*
 * Seed table of classic 5-man bosses that need a positioning / interrupt nudge.
 * Expand this list as players report mop-ups; keep flags coarse.
 * HINTS must stay sorted by entry for binary search.
 */

#include "PartyBotEncounter.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Unit.h"

#include <algorithm>
#include <string>

namespace
{
    PartyBotEncounterHint const HINTS[] =
    {
        // Deadmines
        { 639,   PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Edwin VanCleef
        { 643,   PB_ENC_MELEE_BEHIND },                                     // Sneed
        { 644,   PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Rhahk'Zor
        { 645,   PB_ENC_INTERRUPT_HARD },                                   // Cookie
        { 646,   PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY | PB_ENC_SPREAD },  // Mr. Smite
        { 647,   PB_ENC_MELEE_BEHIND },                                     // Captain Greenskin

        // Stockade
        { 1716,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Bazil Thredd
        { 1717,  PB_ENC_INTERRUPT_HARD | PB_ENC_FEAR_WARD },                // Hamhock

        // Uldaman
        { 2748,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY | PB_ENC_SPREAD },  // Archaedas

        // Wailing Caverns
        { 3654,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY | PB_ENC_FEAR_WARD }, // Mutanus
        { 3669,  PB_ENC_MELEE_BEHIND },                                     // Lord Cobrahn
        { 3670,  PB_ENC_MELEE_BEHIND },                                     // Lord Pythas
        { 3671,  PB_ENC_INTERRUPT_HARD },                                   // Lady Anacondra
        { 3673,  PB_ENC_MELEE_BEHIND },                                     // Lord Serpentis

        // Shadowfang Keep
        { 3886,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Razorclaw
        { 3887,  PB_ENC_MELEE_BEHIND },                                     // Baron Silverlaine

        // Scarlet Monastery
        { 3975,  PB_ENC_KITE_WHIRLWIND | PB_ENC_SPREAD | PB_ENC_FACE_AWAY }, // Herod
        { 3976,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Mograine
        { 3977,  PB_ENC_INTERRUPT_HARD | PB_ENC_SPREAD },                   // Whitemane

        // Shadowfang Keep (cont.)
        { 4274,  PB_ENC_SPREAD },                                           // Fenrus
        { 4275,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD | PB_ENC_FEAR_WARD }, // Arugal
        { 4278,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Springvale

        // Razorfen Kraul
        { 4421,  PB_ENC_INTERRUPT_HARD | PB_ENC_SPREAD },                   // Charlga

        // Scarlet Monastery (cont.)
        { 4542,  PB_ENC_INTERRUPT_HARD },                                   // Fairbanks
        { 4543,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD },                   // Thalnos

        // Blackfathom Deeps
        { 4829,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Aku'mai
        { 4831,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD },                   // Lady Sarevess
        { 4832,  PB_ENC_INTERRUPT_HARD | PB_ENC_FEAR_WARD },                // Kelris
        { 4887,  PB_ENC_MELEE_BEHIND },                                     // Ghamoo-ra

        // Sunken Temple
        { 5709,  PB_ENC_FEAR_WARD | PB_ENC_SPREAD | PB_ENC_MELEE_BEHIND },  // Eranikus
        { 5710,  PB_ENC_INTERRUPT_HARD },                                   // Jammal'an

        // Gnomeregan
        { 6229,  PB_ENC_MELEE_BEHIND | PB_ENC_SPREAD },                     // Crowd Pummeler
        { 6235,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD },                   // Electrocutioner

        // Scarlet Monastery (cont.)
        { 6487,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD },                   // Arcanist Doan

        // Uldaman (cont.)
        { 7228,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Ironaya

        // Zul'Farrak
        { 7267,  PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Ukorz
        { 7271,  PB_ENC_INTERRUPT_HARD },                                   // Zum'rah
        { 7273,  PB_ENC_SPREAD },                                           // Gahz'rilla

        // Razorfen Downs
        { 7358,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD | PB_ENC_FEAR_WARD }, // Amnennar

        // Gnomeregan (cont.)
        { 7800,  PB_ENC_SPREAD | PB_ENC_MELEE_BEHIND },                     // Thermaplugg

        // Zul'Farrak (cont.)
        { 8127,  PB_ENC_SPREAD | PB_ENC_INTERRUPT_HARD },                   // Antu'sul

        // Ragefire Chasm
        { 11518, PB_ENC_INTERRUPT_HARD },                                   // Jergosh
        { 11520, PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Taragaman

        // Maraudon
        { 12201, PB_ENC_SPREAD | PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },  // Princess Theradras
        { 12203, PB_ENC_MELEE_BEHIND | PB_ENC_FACE_AWAY },                  // Landslide
        { 13282, PB_ENC_SPREAD },                                           // Noxxion
    };

    bool CompareHint(PartyBotEncounterHint const& a, uint32 entry)
    {
        return a.entry < entry;
    }
}

uint32 PartyBotEncounter::GetFlags(uint32 creatureEntry)
{
    if (!creatureEntry)
        return PB_ENC_NONE;

    auto const begin = std::begin(HINTS);
    auto const end = std::end(HINTS);
    auto it = std::lower_bound(begin, end, creatureEntry, CompareHint);
    if (it != end && it->entry == creatureEntry)
        return it->flags;
    return PB_ENC_NONE;
}

uint32 PartyBotEncounter::GetFlags(Unit const* pUnit)
{
    if (!pUnit || !pUnit->IsCreature())
        return PB_ENC_NONE;
    return GetFlags(pUnit->GetEntry());
}

bool PartyBotEncounter::HasWhirlwindAura(Unit const* pUnit)
{
    if (!pUnit)
        return false;

    for (auto const& pair : pUnit->GetSpellAuraHolderMap())
    {
        SpellAuraHolder const* holder = pair.second;
        if (!holder || !holder->GetSpellProto())
            continue;
        std::string const& name = holder->GetSpellProto()->SpellName[0];
        if (name.find("Whirlwind") != std::string::npos ||
            name.find("Blade Flurry") != std::string::npos ||
            name.find("Bladestorm") != std::string::npos)
            return true;
    }
    return false;
}
