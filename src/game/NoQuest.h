#ifndef MANGOS_NOQUEST_H
#define MANGOS_NOQUEST_H

class Player;
struct Loot;

namespace NoQuest
{
    // The quests a no-quest character completes by killing: every objective
    // a kill or an item creatures drop. Built once after quests and loot load.
    void LoadQuests();

    // Class epics that Classic only gave through long quest chains, put on
    // the matching boss's corpse for a no-quest character of that class.
    void FillBossEpics(Player* player, Loot& loot);
}

#endif
