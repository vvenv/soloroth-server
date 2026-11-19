/* Copyright (C) 2006 - 2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
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

/* ScriptData
SDName: Instance_Deadmines
SD%Complete: 0
SDComment: Placeholder
SDCategory: Deadmines
EndScriptData */

#include "scriptPCH.h"
#include "deadmines.h"

struct instance_deadmines : public ScriptedInstance
{
    instance_deadmines(Map* pMap) : ScriptedInstance(pMap)
    {
        Initialize();
    };

    uint32 m_auiEncounter[INSTANCE_DM_MAX_ENCOUNTER];
    std::string strInstData;

    uint64 m_uiIronCladGUID;
    uint64 m_uiCannonGUID;
    uint64 m_uiSmiteGUID;
    uint64 m_uiRhahkGUID;
    uint64 m_uiGilnidGUID;

    uint64 m_uiDoor1GUID;
    uint64 m_uiDoor2GUID;
    uint64 m_uiDoor3GUID;

    uint32 m_uiSpawnPatrolOnRhahkDeath;
    bool   m_isRhahkDead;
    uint32 m_isGunPowderEventDone;
    bool   m_isGilnidDead;
    uint32 m_uiSpawnPatrolOnGilnidDeath;

    uint32 m_uiIronDoorTimer;
    uint32 m_uiIronDoorStep;

    void Initialize() override
    {
        memset(&m_auiEncounter, 0, sizeof(m_auiEncounter));
        strInstData.clear();

        m_uiIronCladGUID = 0;
        m_uiCannonGUID = 0;
        m_uiSmiteGUID = 0;
        m_uiRhahkGUID = 0;
        m_uiGilnidGUID = 0;
        m_uiSpawnPatrolOnRhahkDeath = 30000;
        m_isRhahkDead = false;
        m_isGunPowderEventDone = 0;
        m_isGilnidDead = false;
        m_uiSpawnPatrolOnGilnidDeath = 30000;

        m_uiIronDoorTimer = 0;
        m_uiIronDoorStep = 0;

        m_uiDoor1GUID = 0;
        m_uiDoor2GUID = 0;
        m_uiDoor3GUID = 0;
    }

    void OnCreatureCreate(Creature* pCreature) override
    {
        if (pCreature->GetEntry() == NPC_RHAHKZOR)
            m_uiRhahkGUID = pCreature->GetGUID();

        if (pCreature->GetEntry() == NPC_GILDNID)
            m_uiGilnidGUID = pCreature->GetGUID();

        if (pCreature->GetEntry() == NPC_MR_SMITE)
            m_uiSmiteGUID = pCreature->GetGUID();

        /** Initialize Rhahz Patrol */
        if (pCreature->GetRespawnDelay() == 43199)
        {
            pCreature->SetVisibility(VISIBILITY_OFF);
            pCreature->SetFactionTemplateId(35);
        }

        /** Initialize Gilnid Patrol */
        if (pCreature->GetRespawnDelay() == 43201)
        {
            pCreature->SetVisibility(VISIBILITY_OFF);
            pCreature->SetFactionTemplateId(35);
        }
    }

    void OpenDoorIfDone(uint32 encounter, uint64 doorGuid)
    {
        if (m_auiEncounter[encounter] != DONE || !doorGuid)
            return;
        if (GameObject* pGo = instance->GetGameObject(doorGuid))
            if (pGo->GetGoState() != GO_STATE_ACTIVE)
                DoUseDoorOrButton(doorGuid);
    }

    void NoteDoorBossDone(uint32 encounter, uint64 doorGuid)
    {
        m_auiEncounter[encounter] = DONE;
        OpenDoorIfDone(encounter, doorGuid);

        OUT_SAVE_INST_DATA;
        std::ostringstream saveStream;
        saveStream << m_auiEncounter[0] << " " << m_auiEncounter[1] << " "
                   << m_auiEncounter[2] << " " << m_auiEncounter[3];
        strInstData = saveStream.str();
        SaveToDB();
        OUT_SAVE_INST_DATA_COMPLETE;
    }

    void OnCreatureDeath(Creature *who) override
    {
        switch (who->GetEntry())
        {
            case NPC_RHAHKZOR :
                m_isRhahkDead = true;
                m_uiSpawnPatrolOnRhahkDeath = 60000;
                NoteDoorBossDone(TYPE_RHAHKZOR_DOOR, m_uiDoor1GUID);
                break;
            case NPC_SNEED :
                NoteDoorBossDone(TYPE_SNEED_DOOR, m_uiDoor2GUID);
                break;
            case NPC_GILDNID :
                m_isGilnidDead = true;
                m_uiSpawnPatrolOnGilnidDeath = 30000;
                NoteDoorBossDone(TYPE_GILNID_DOOR, m_uiDoor3GUID);
                break;
        }
    }

    void OnObjectCreate(GameObject* pGo) override
    {
        if (pGo->GetEntry() == GO_IRON_CLAD)
            m_uiIronCladGUID = pGo->GetGUID();

        if (pGo->GetEntry() == GO_DEFIAS_CANNON)
            m_uiCannonGUID = pGo->GetGUID();

        if (pGo->GetEntry() == GO_DOOR1)
        {
            m_uiDoor1GUID = pGo->GetGUID();
            if (m_auiEncounter[TYPE_RHAHKZOR_DOOR] == DONE)
                pGo->SetGoState(GO_STATE_ACTIVE);
        }
        if (pGo->GetEntry() == GO_DOOR2 && pGo->GetPositionX() > -291.0f && pGo->GetPositionX() < -290.0f)
        {
            m_uiDoor2GUID = pGo->GetGUID();
            if (m_auiEncounter[TYPE_SNEED_DOOR] == DONE)
                pGo->SetGoState(GO_STATE_ACTIVE);
        }
        if (pGo->GetEntry() == GO_DOOR3 && pGo->GetPositionX() > -169.0f && pGo->GetPositionX() < -168.0f)
        {
            m_uiDoor3GUID = pGo->GetGUID();
            if (m_auiEncounter[TYPE_GILNID_DOOR] == DONE)
                pGo->SetGoState(GO_STATE_ACTIVE);
        }

        if (pGo->GetEntry() == GO_DMF_CHEST)
            pGo->SetVisible(false);
    }

    void SetData(uint32 uiType, uint32 uiData) override
    {
        if (uiType == TYPE_DEFIAS_ENDDOOR)
        {
            if (uiData == IN_PROGRESS)
            {
                if (GameObject* pGo = instance->GetGameObject(m_uiIronCladGUID))
                {
                    // no breaking the door if it's open
                    if (!(pGo->GetGoState() == GO_STATE_ACTIVE))
                        pGo->UseDoorOrButton(0, true);
                    m_uiIronDoorTimer = 3000;
                }
            }
            m_auiEncounter[TYPE_DEFIAS_ENDDOOR] = uiData;
            if (uiData == DONE || uiData == SPECIAL || uiData == FAIL)
            {
                OUT_SAVE_INST_DATA;
                std::ostringstream saveStream;
                saveStream << m_auiEncounter[0] << " " << m_auiEncounter[1] << " "
                           << m_auiEncounter[2] << " " << m_auiEncounter[3];
                strInstData = saveStream.str();
                SaveToDB();
                OUT_SAVE_INST_DATA_COMPLETE;
            }
        }
        else if (uiType == GUN_POWDER_EVENT)
            m_isGunPowderEventDone = uiData;
    }

    uint32 GetData(uint32 uiType) override
    {
        if (uiType == TYPE_DEFIAS_ENDDOOR)
            return m_auiEncounter[TYPE_DEFIAS_ENDDOOR];
        else if (uiType == GUN_POWDER_EVENT)
            return m_isGunPowderEventDone;
        else if (uiType < INSTANCE_DM_MAX_ENCOUNTER)
            return m_auiEncounter[uiType];
        return 0;
    }

    uint64 GetData64(uint32 uiData) override
    {
        if (uiData == DATA_DEFIAS_DOOR)
            return m_uiIronCladGUID;

        return 0;
    }

    char const* Save() override
    {
        return strInstData.c_str();
    }

    void Load(char const* chrIn) override
    {
        if (!chrIn)
        {
            OUT_LOAD_INST_DATA_FAIL;
            return;
        }

        OUT_LOAD_INST_DATA(chrIn);

        std::istringstream loadStream(chrIn);
        // Old saves only stored the iron-clad door in one field.
        loadStream >> m_auiEncounter[0];
        if (!(loadStream >> m_auiEncounter[1] >> m_auiEncounter[2] >> m_auiEncounter[3]))
        {
            m_auiEncounter[TYPE_DEFIAS_ENDDOOR] = m_auiEncounter[0];
            m_auiEncounter[TYPE_RHAHKZOR_DOOR] = NOT_STARTED;
            m_auiEncounter[TYPE_SNEED_DOOR] = NOT_STARTED;
            m_auiEncounter[TYPE_GILNID_DOOR] = NOT_STARTED;
        }

        for (uint32 & i : m_auiEncounter)
        {
            if (i == IN_PROGRESS)
                i = NOT_STARTED;
        }

        if (m_auiEncounter[TYPE_RHAHKZOR_DOOR] == DONE)
            m_isRhahkDead = true;
        if (m_auiEncounter[TYPE_GILNID_DOOR] == DONE)
            m_isGilnidDead = true;

        OUT_LOAD_INST_DATA_COMPLETE;
    }

    void Update(uint32 uiDiff) override
    {
        if (m_isRhahkDead && m_uiSpawnPatrolOnRhahkDeath)
        {
            if (m_uiSpawnPatrolOnRhahkDeath <= uiDiff)
            {
                if (Creature* pRhahk = instance->GetCreature(m_uiRhahkGUID))
                {
                    std::list<Creature*> escortList;

                    GetCreatureListWithEntryInGrid(escortList, pRhahk, 634, 400.0f);
                    for (const auto& it : escortList)
                        if (it->GetRespawnDelay() == 43199)
                        {
                            it->SetVisibility(VISIBILITY_ON);
                            it->SetFactionTemplateId(17);
                        }
                    escortList.clear();

                    GetCreatureListWithEntryInGrid(escortList, pRhahk, 1729, 400.0f);
                    for (const auto& it : escortList)
                        if (it->GetRespawnDelay() == 43199)
                        {
                            it->SetVisibility(VISIBILITY_ON);
                            it->SetFactionTemplateId(17);
                        }
                    escortList.clear();

                    m_uiSpawnPatrolOnRhahkDeath = 0;
                }
            }
            else
                m_uiSpawnPatrolOnRhahkDeath -= uiDiff;
        }

        if (m_isGilnidDead && m_uiSpawnPatrolOnGilnidDeath)
        {
            if (m_uiSpawnPatrolOnGilnidDeath <= uiDiff)
            {
                if (Creature* pGilnid = instance->GetCreature(m_uiGilnidGUID))
                {
                    std::list<Creature*> escortList;

                    GetCreatureListWithEntryInGrid(escortList, pGilnid, 4417, 400.0f);
                    for (const auto& it : escortList)
                        if (it->GetRespawnDelay() == 43201)
                        {
                            it->SetVisibility(VISIBILITY_ON);
                            it->SetFactionTemplateId(17);
                        }
                    escortList.clear();

                    GetCreatureListWithEntryInGrid(escortList, pGilnid, 4418, 400.0f);
                    for (const auto& it : escortList)
                        if (it->GetRespawnDelay() == 43201)
                        {
                            it->SetVisibility(VISIBILITY_ON);
                            it->SetFactionTemplateId(17);
                        }
                    escortList.clear();

                    m_uiSpawnPatrolOnGilnidDeath = 0;
                }
            }
            else
                m_uiSpawnPatrolOnGilnidDeath -= uiDiff;
        }

        if (m_uiIronDoorTimer)
        {
            if (m_uiIronDoorTimer <= uiDiff)
            {
                if (Creature* pMrSmite = instance->GetCreature(m_uiSmiteGUID))
                {
                    std::list<Creature*> escortList;

                    switch (m_uiIronDoorStep)
                    {
                        case 0:
                            DoScriptText(INST_SAY_ALARM1, pMrSmite);
                            GetCreatureListWithEntryInGrid(escortList, pMrSmite, 657, 400.0f);
                            for (const auto& it : escortList)
                                if (it->GetRespawnDelay() == 43202)
                                    it->GetMotionMaster()->MovePoint(0, -99.6611f, -671.071655f, 7.42241f, MOVE_PATHFINDING | MOVE_RUN_MODE);
                            escortList.clear();
                            ++m_uiIronDoorStep;
                            m_uiIronDoorTimer = 15000;
                            break;
                        case 1:
                            DoScriptText(INST_SAY_ALARM2, pMrSmite);
                            m_uiIronDoorStep = 0;
                            m_uiIronDoorTimer = 0;
                            sLog.Out(LOG_SCRIPTS, LOG_LVL_DEBUG, "Instance Deadmines: Iron door event reached end.");
                            break;
                    }
                }
                else
                    m_uiIronDoorTimer = 0;
            }
            else
                m_uiIronDoorTimer -= uiDiff;
        }
    }
};

InstanceData* GetInstanceData_instance_deadmines(Map* pMap)
{
    return new instance_deadmines(pMap);
}

bool AreaTrigger_at_dmf_chest_dm(Player* pPlayer, AreaTriggerEntry const* pAt)
{
    // Darkmoon chest visible only for players on the quest
    if (pPlayer->GetQuestStatus(QUEST_FORTUNE_AWAITS) == QUEST_STATUS_COMPLETE)
    {
        if (GameObject* pGo = pPlayer->FindNearestGameObject(GO_DMF_CHEST, 100.0f))
            pGo->SetVisible(true);
    }

    return false;
}

void AddSC_instance_deadmines()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "instance_deadmines";
    newscript->GetInstanceData = &GetInstanceData_instance_deadmines;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "at_dmf_chest_dm";
    newscript->pAreaTrigger = &AreaTrigger_at_dmf_chest_dm;
    newscript->RegisterSelf();
}
