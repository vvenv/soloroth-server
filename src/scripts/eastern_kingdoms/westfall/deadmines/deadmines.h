/* Copyright (C) 2006 - 2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 * This program is free software licensed under GPL version 2
 * Please see the included DOCS/LICENSE.TXT for more information */

#ifndef DEF_DEADMINES_H
#define DEF_DEADMINES_H

enum
{
    // Doors after Rhahk'Zor / Sneed / Gilnid must persist across unload;
    // previously only the iron-clad end door was saved.
    INSTANCE_DM_MAX_ENCOUNTER           = 4,

    TYPE_RHAHKZOR_DOOR      = 0,
    TYPE_SNEED_DOOR         = 1,
    TYPE_GILNID_DOOR        = 2,
    TYPE_DEFIAS_ENDDOOR     = 3,
    DATA_DEFIAS_DOOR        = 4,

    INST_SAY_ALARM1         = 1148,
    INST_SAY_ALARM2         = 1149,

    /** Doors which need to be opened automatically once the boss before died */
    GO_DOOR1                = 13965,
    GO_DOOR2                = 16400,
    GO_DOOR3                = 16399,

    GUN_POWDER_EVENT        = 5000,

    GO_DOOR_LEVER           = 101833,
    GO_IRON_CLAD            = 16397,
    GO_DEFIAS_CANNON        = 16398,
    GO_DMF_CHEST            = 180024,
    NPC_MR_SMITE            = 646,
    NPC_PIRATE              = 657,
    NPC_SNEED               = 643,
    NPC_RHAHKZOR            = 644,
    NPC_GILDNID             = 1763,

    QUEST_FORTUNE_AWAITS    = 7938
};

#endif
