/*
 * Lightweight party-bot encounter hints for classic 5-mans.
 * Flags nudge positioning / interrupt urgency; they are not full boss scripts.
 */

#ifndef MANGOS_PARTY_BOT_ENCOUNTER_H
#define MANGOS_PARTY_BOT_ENCOUNTER_H

#include "Common.h"

class Unit;

enum PartyBotEncounterFlags : uint32
{
    PB_ENC_NONE            = 0,
    // Melee DPS stay behind the boss (cleave / frontal).
    PB_ENC_MELEE_BEHIND    = 0x0001,
    // Tank stand opposite the party (cleave into the wall).
    PB_ENC_FACE_AWAY       = 0x0002,
    // Ranged / healers keep more spacing than usual.
    PB_ENC_SPREAD          = 0x0004,
    // Ranged / healers hug the tank (stack soak / avoid room AoE edges).
    PB_ENC_STACK_TANK      = 0x0008,
    // Treat every cast from this mob as high-priority interrupt.
    PB_ENC_INTERRUPT_HARD  = 0x0010,
    // Melee step out while the boss has a Whirlwind-like aura.
    PB_ENC_KITE_WHIRLWIND  = 0x0020,
    // Prefer Fear Ward up for the duration of the fight.
    PB_ENC_FEAR_WARD       = 0x0040,
};

struct PartyBotEncounterHint
{
    uint32 entry;
    uint32 flags;
};

namespace PartyBotEncounter
{
    // Flags for this creature entry (0 = no special hint).
    uint32 GetFlags(uint32 creatureEntry);
    uint32 GetFlags(Unit const* pUnit);

    // True when any aura on the unit looks like Whirlwind / Bladestorm.
    bool HasWhirlwindAura(Unit const* pUnit);
}

#endif
