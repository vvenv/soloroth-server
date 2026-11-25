/*
 * Party bots that join on their own in dungeons and raids (trio-scaled).
 * See DungeonBotMgr.h for the rules.
 */

#include "DungeonBotMgr.h"
#include "PartyBotAI.h"
#include "PlayerBotMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "Database/DatabaseEnv.h"
#include "Util.h"
#include "Utilities/Random.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include <algorithm>
#include <list>
#include <set>
#include <vector>

namespace
{
    // Every 1.12-legal race × class × talent tree on each side. Names are
    // short Chinese, flavoured by race. Slots 0–47 stay the original
    // indices; the rest are the missing races for each tree. A fill picks
    // at random; calling the same slot again brings that companion back.
    struct RosterBot
    {
        char const* name;
        Team team;
        uint8 botClass;
        uint8 race;
        uint8 gender;
        CombatBotRoles role;
        char const* specEn;
        char const* specZh;
        char const* specHint;   // part of the premade spec names to prefer
    };

    RosterBot const ROSTER[] =
    {
        // ---- original Alliance (slots 0–23) ----
        { "奥德",    ALLIANCE, CLASS_WARRIOR, RACE_HUMAN,    GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "布兰",   ALLIANCE, CLASS_WARRIOR, RACE_DWARF,    GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "杜根",    ALLIANCE, CLASS_WARRIOR, RACE_DWARF,    GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "艾莲",    ALLIANCE, CLASS_PALADIN, RACE_HUMAN,    GENDER_FEMALE, ROLE_HEALER,    "Holy Paladin",           "神圣骑士",     "holy" },
        { "哈姆",    ALLIANCE, CLASS_PALADIN, RACE_DWARF,    GENDER_MALE,   ROLE_TANK,      "Protection Paladin",     "防护骑士",     "prot" },
        { "塞德",    ALLIANCE, CLASS_PALADIN, RACE_HUMAN,    GENDER_MALE,   ROLE_MELEE_DPS, "Retribution Paladin",    "惩戒骑士",     "retribution" },
        { "希薇",  ALLIANCE, CLASS_HUNTER,  RACE_NIGHTELF, GENDER_FEMALE, ROLE_RANGE_DPS, "Beast Mastery Hunter",   "野兽控制猎人", "" },
        { "冈德",   ALLIANCE, CLASS_HUNTER,  RACE_DWARF,    GENDER_MALE,   ROLE_RANGE_DPS, "Marksmanship Hunter",    "射击猎人",     "mm" },
        { "费兰",    ALLIANCE, CLASS_HUNTER,  RACE_NIGHTELF, GENDER_MALE,   ROLE_RANGE_DPS, "Survival Hunter",        "生存猎人",     "sv" },
        { "薇珀",    ALLIANCE, CLASS_ROGUE,   RACE_HUMAN,    GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "菲兹",  ALLIANCE, CLASS_ROGUE,   RACE_GNOME,    GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "妮莎",     ALLIANCE, CLASS_ROGUE,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "布伦",    ALLIANCE, CLASS_PRIEST,  RACE_DWARF,    GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",      "戒律牧师",     "holy" },
        { "露茜",  ALLIANCE, CLASS_PRIEST,  RACE_HUMAN,    GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",            "神圣牧师",     "holy" },
        { "莫德",  ALLIANCE, CLASS_PRIEST,  RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",          "暗影牧师",     "shadow" },
        { "汀卡",   ALLIANCE, CLASS_MAGE,    RACE_GNOME,    GENDER_FEMALE, ROLE_RANGE_DPS, "Arcane Mage",            "奥术法师",     "arcane" },
        { "柯文",    ALLIANCE, CLASS_MAGE,    RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Fire Mage",              "火焰法师",     "fire" },
        { "伊索",    ALLIANCE, CLASS_MAGE,    RACE_HUMAN,    GENDER_FEMALE, ROLE_RANGE_DPS, "Frost Mage",             "冰霜法师",     "frost" },
        { "玛拉",  ALLIANCE, CLASS_WARLOCK, RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Affliction Warlock",     "痛苦术士",     "affli" },
        { "尼可",   ALLIANCE, CLASS_WARLOCK, RACE_GNOME,    GENDER_MALE,   ROLE_RANGE_DPS, "Demonology Warlock",     "恶魔学识术士", "demo" },
        { "瑟拉", ALLIANCE, CLASS_WARLOCK, RACE_HUMAN,    GENDER_FEMALE, ROLE_RANGE_DPS, "Destruction Warlock",    "毁灭术士",     "ruin" },
        { "艾雯",   ALLIANCE, CLASS_DRUID,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_RANGE_DPS, "Balance Druid",          "平衡德鲁伊",   "balance" },
        { "萨洛",    ALLIANCE, CLASS_DRUID,   RACE_NIGHTELF, GENDER_MALE,   ROLE_MELEE_DPS, "Feral Druid",            "野性德鲁伊",   "feral" },
        { "奈雯",    ALLIANCE, CLASS_DRUID,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_HEALER,    "Restoration Druid",      "恢复德鲁伊",   "resto" },

        // ---- original Horde (slots 24–47) ----
        { "格鲁",   HORDE,    CLASS_WARRIOR, RACE_ORC,      GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "拉克",    HORDE,    CLASS_WARRIOR, RACE_TROLL,    GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "塔霍",   HORDE,    CLASS_WARRIOR, RACE_TAUREN,   GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "莎卡",   HORDE,    CLASS_SHAMAN,  RACE_ORC,      GENDER_FEMALE, ROLE_RANGE_DPS, "Elemental Shaman",       "元素萨满",     "ele" },
        { "杜加",   HORDE,    CLASS_SHAMAN,  RACE_ORC,      GENDER_MALE,   ROLE_MELEE_DPS, "Enhancement Shaman",     "增强萨满",     "enha" },
        { "哈洛",    HORDE,    CLASS_SHAMAN,  RACE_TAUREN,   GENDER_FEMALE, ROLE_HEALER,    "Restoration Shaman",     "恢复萨满",     "resto" },
        { "格拉",  HORDE,    CLASS_HUNTER,  RACE_ORC,      GENDER_MALE,   ROLE_RANGE_DPS, "Beast Mastery Hunter",   "野兽控制猎人", "" },
        { "赞吉",   HORDE,    CLASS_HUNTER,  RACE_TROLL,    GENDER_FEMALE, ROLE_RANGE_DPS, "Marksmanship Hunter",    "射击猎人",     "mm" },
        { "科托",    HORDE,    CLASS_HUNTER,  RACE_TAUREN,   GENDER_MALE,   ROLE_RANGE_DPS, "Survival Hunter",        "生存猎人",     "sv" },
        { "茉雯",  HORDE,    CLASS_ROGUE,   RACE_UNDEAD,   GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "斯卡",     HORDE,    CLASS_ROGUE,   RACE_ORC,      GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "维金",    HORDE,    CLASS_ROGUE,   RACE_TROLL,    GENDER_MALE,   ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "扎拉",    HORDE,    CLASS_PRIEST,  RACE_TROLL,    GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",      "戒律牧师",     "holy" },
        { "埃丝",   HORDE,    CLASS_PRIEST,  RACE_UNDEAD,   GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",            "神圣牧师",     "holy" },
        { "沃拉",   HORDE,    CLASS_PRIEST,  RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",          "暗影牧师",     "shadow" },
        { "阿什",   HORDE,    CLASS_MAGE,    RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Arcane Mage",            "奥术法师",     "arcane" },
        { "叶娜",   HORDE,    CLASS_MAGE,    RACE_TROLL,    GENDER_FEMALE, ROLE_RANGE_DPS, "Fire Mage",              "火焰法师",     "fire" },
        { "哈尔",   HORDE,    CLASS_MAGE,    RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Frost Mage",             "冰霜法师",     "frost" },
        { "莫斯",   HORDE,    CLASS_WARLOCK, RACE_UNDEAD,   GENDER_MALE,   ROLE_RANGE_DPS, "Affliction Warlock",     "痛苦术士",     "affli" },
        { "格莉",   HORDE,    CLASS_WARLOCK, RACE_ORC,      GENDER_FEMALE, ROLE_RANGE_DPS, "Demonology Warlock",     "恶魔学识术士", "demo" },
        { "辛德",   HORDE,    CLASS_WARLOCK, RACE_UNDEAD,   GENDER_FEMALE, ROLE_RANGE_DPS, "Destruction Warlock",    "毁灭术士",     "ruin" },
        { "阿波",     HORDE,    CLASS_DRUID,   RACE_TAUREN,   GENDER_FEMALE, ROLE_RANGE_DPS, "Balance Druid",          "平衡德鲁伊",   "balance" },
        { "霍克",   HORDE,    CLASS_DRUID,   RACE_TAUREN,   GENDER_MALE,   ROLE_MELEE_DPS, "Feral Druid",            "野性德鲁伊",   "feral" },
        { "玛哈",    HORDE,    CLASS_DRUID,   RACE_TAUREN,   GENDER_FEMALE, ROLE_HEALER,    "Restoration Druid",      "恢复德鲁伊",   "resto" },

        // ---- Alliance race fills (1.12-legal races missing above) ----
        { "托瓦",   ALLIANCE, CLASS_WARRIOR, RACE_DWARF,    GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "凯蒙",   ALLIANCE, CLASS_WARRIOR, RACE_NIGHTELF, GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "比克",    ALLIANCE, CLASS_WARRIOR, RACE_GNOME,    GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",           "武器战士",     "arms" },
        { "萝薇",    ALLIANCE, CLASS_WARRIOR, RACE_HUMAN,    GENDER_FEMALE, ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "莉蕾",    ALLIANCE, CLASS_WARRIOR, RACE_NIGHTELF, GENDER_FEMALE, ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "金布",    ALLIANCE, CLASS_WARRIOR, RACE_GNOME,    GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",           "狂怒战士",     "fury" },
        { "奥斯",     ALLIANCE, CLASS_WARRIOR, RACE_HUMAN,    GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "瑟伦",    ALLIANCE, CLASS_WARRIOR, RACE_NIGHTELF, GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "齿轮",   ALLIANCE, CLASS_WARRIOR, RACE_GNOME,    GENDER_MALE,   ROLE_TANK,      "Protection Warrior",     "防护战士",     "protection" },
        { "英格",    ALLIANCE, CLASS_PALADIN, RACE_DWARF,    GENDER_FEMALE, ROLE_HEALER,    "Holy Paladin",           "神圣骑士",     "holy" },
        { "戈德",    ALLIANCE, CLASS_PALADIN, RACE_HUMAN,    GENDER_MALE,   ROLE_TANK,      "Protection Paladin",     "防护骑士",     "prot" },
        { "布洛",    ALLIANCE, CLASS_PALADIN, RACE_DWARF,    GENDER_MALE,   ROLE_MELEE_DPS, "Retribution Paladin",    "惩戒骑士",     "retribution" },
        { "玛格",     ALLIANCE, CLASS_HUNTER,  RACE_DWARF,    GENDER_FEMALE, ROLE_RANGE_DPS, "Beast Mastery Hunter",   "野兽控制猎人", "" },
        { "西尔",  ALLIANCE, CLASS_HUNTER,  RACE_NIGHTELF, GENDER_MALE,   ROLE_RANGE_DPS, "Marksmanship Hunter",    "射击猎人",     "mm" },
        { "博林",     ALLIANCE, CLASS_HUNTER,  RACE_DWARF,    GENDER_MALE,   ROLE_RANGE_DPS, "Survival Hunter",        "生存猎人",     "sv" },
        { "格蕾",    ALLIANCE, CLASS_ROGUE,   RACE_DWARF,    GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "塞琳",   ALLIANCE, CLASS_ROGUE,   RACE_NIGHTELF, GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "翠克",    ALLIANCE, CLASS_ROGUE,   RACE_GNOME,    GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",    "刺杀盗贼",     "assa" },
        { "达里",    ALLIANCE, CLASS_ROGUE,   RACE_HUMAN,    GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "柯古",    ALLIANCE, CLASS_ROGUE,   RACE_DWARF,    GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "芬里",    ALLIANCE, CLASS_ROGUE,   RACE_NIGHTELF, GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",           "战斗盗贼",     "combat" },
        { "玛丽",   ALLIANCE, CLASS_ROGUE,   RACE_HUMAN,    GENDER_FEMALE, ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "奥拉",      ALLIANCE, CLASS_ROGUE,   RACE_DWARF,    GENDER_FEMALE, ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "纳克",    ALLIANCE, CLASS_ROGUE,   RACE_GNOME,    GENDER_MALE,   ROLE_MELEE_DPS, "Subtlety Rogue",         "敏锐盗贼",     "" },
        { "奥黛",    ALLIANCE, CLASS_PRIEST,  RACE_HUMAN,    GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",      "戒律牧师",     "holy" },
        { "伊莉",    ALLIANCE, CLASS_PRIEST,  RACE_NIGHTELF, GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",      "戒律牧师",     "holy" },
        { "阿斯",    ALLIANCE, CLASS_PRIEST,  RACE_DWARF,    GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",            "神圣牧师",     "holy" },
        { "艾洛",    ALLIANCE, CLASS_PRIEST,  RACE_NIGHTELF, GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",            "神圣牧师",     "holy" },
        { "格里",   ALLIANCE, CLASS_PRIEST,  RACE_DWARF,    GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",          "暗影牧师",     "shadow" },
        { "瓦伦",    ALLIANCE, CLASS_PRIEST,  RACE_NIGHTELF, GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",          "暗影牧师",     "shadow" },
        { "珀西",  ALLIANCE, CLASS_MAGE,    RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Arcane Mage",            "奥术法师",     "arcane" },
        { "火花",   ALLIANCE, CLASS_MAGE,    RACE_GNOME,    GENDER_FEMALE, ROLE_RANGE_DPS, "Fire Mage",              "火焰法师",     "fire" },
        { "寒芯", ALLIANCE, CLASS_MAGE,    RACE_GNOME,    GENDER_MALE,   ROLE_RANGE_DPS, "Frost Mage",             "冰霜法师",     "frost" },
        { "赫克",    ALLIANCE, CLASS_WARLOCK, RACE_GNOME,    GENDER_MALE,   ROLE_RANGE_DPS, "Affliction Warlock",     "痛苦术士",     "affli" },
        { "莫丹",   ALLIANCE, CLASS_WARLOCK, RACE_HUMAN,    GENDER_MALE,   ROLE_RANGE_DPS, "Demonology Warlock",     "恶魔学识术士", "demo" },
        { "墨羽",     ALLIANCE, CLASS_WARLOCK, RACE_GNOME,    GENDER_FEMALE, ROLE_RANGE_DPS, "Destruction Warlock",    "毁灭术士",     "ruin" },

        // ---- Horde race fills ----
        { "髓刃",      HORDE, CLASS_WARRIOR, RACE_UNDEAD, GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",         "武器战士",     "arms" },
        { "石蹄",   HORDE, CLASS_WARRIOR, RACE_TAUREN, GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",         "武器战士",     "arms" },
        { "卡祖",      HORDE, CLASS_WARRIOR, RACE_TROLL,  GENDER_MALE,   ROLE_MELEE_DPS, "Arms Warrior",         "武器战士",     "arms" },
        { "血牙",   HORDE, CLASS_WARRIOR, RACE_ORC,    GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",         "狂怒战士",     "fury" },
        { "坟冢",      HORDE, CLASS_WARRIOR, RACE_UNDEAD, GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",         "狂怒战士",     "fury" },
        { "雷角", HORDE, CLASS_WARRIOR, RACE_TAUREN, GENDER_MALE,   ROLE_MELEE_DPS, "Fury Warrior",         "狂怒战士",     "fury" },
        { "铁颚",     HORDE, CLASS_WARRIOR, RACE_ORC,    GENDER_MALE,   ROLE_TANK,      "Protection Warrior",   "防护战士",     "protection" },
        { "墓卫",   HORDE, CLASS_WARRIOR, RACE_UNDEAD, GENDER_MALE,   ROLE_TANK,      "Protection Warrior",   "防护战士",     "protection" },
        { "咒牙",      HORDE, CLASS_WARRIOR, RACE_TROLL,  GENDER_MALE,   ROLE_TANK,      "Protection Warrior",   "防护战士",     "protection" },
        { "地歌",   HORDE, CLASS_SHAMAN,  RACE_TAUREN, GENDER_FEMALE, ROLE_RANGE_DPS, "Elemental Shaman",     "元素萨满",     "ele" },
        { "金扎",     HORDE, CLASS_SHAMAN,  RACE_TROLL,  GENDER_FEMALE, ROLE_RANGE_DPS, "Elemental Shaman",     "元素萨满",     "ele" },
        { "雷图",  HORDE, CLASS_SHAMAN,  RACE_TAUREN, GENDER_MALE,   ROLE_MELEE_DPS, "Enhancement Shaman",   "增强萨满",     "enha" },
        { "拉金",      HORDE, CLASS_SHAMAN,  RACE_TROLL,  GENDER_MALE,   ROLE_MELEE_DPS, "Enhancement Shaman",   "增强萨满",     "enha" },
        { "火舌",  HORDE, CLASS_SHAMAN,  RACE_ORC,    GENDER_FEMALE, ROLE_HEALER,    "Restoration Shaman",   "恢复萨满",     "resto" },
        { "扎塔",     HORDE, CLASS_SHAMAN,  RACE_TROLL,  GENDER_FEMALE, ROLE_HEALER,    "Restoration Shaman",   "恢复萨满",     "resto" },
        { "远蹄",  HORDE, CLASS_HUNTER,  RACE_TAUREN, GENDER_MALE,   ROLE_RANGE_DPS, "Beast Mastery Hunter", "野兽控制猎人", "" },
        { "赫卡",      HORDE, CLASS_HUNTER,  RACE_TROLL,  GENDER_MALE,   ROLE_RANGE_DPS, "Beast Mastery Hunter", "野兽控制猎人", "" },
        { "黑弓",    HORDE, CLASS_HUNTER,  RACE_ORC,    GENDER_FEMALE, ROLE_RANGE_DPS, "Marksmanship Hunter",  "射击猎人",     "mm" },
        { "风视",    HORDE, CLASS_HUNTER,  RACE_TAUREN, GENDER_FEMALE, ROLE_RANGE_DPS, "Marksmanship Hunter",  "射击猎人",     "mm" },
        { "赤斧",      HORDE, CLASS_HUNTER,  RACE_ORC,    GENDER_MALE,   ROLE_RANGE_DPS, "Survival Hunter",      "生存猎人",     "sv" },
        { "贾萨",      HORDE, CLASS_HUNTER,  RACE_TROLL,  GENDER_FEMALE, ROLE_RANGE_DPS, "Survival Hunter",      "生存猎人",     "sv" },
        { "裂爪",     HORDE, CLASS_ROGUE,   RACE_ORC,    GENDER_MALE,   ROLE_MELEE_DPS, "Assassination Rogue",  "刺杀盗贼",     "assa" },
        { "维尼",       HORDE, CLASS_ROGUE,   RACE_TROLL,  GENDER_FEMALE, ROLE_MELEE_DPS, "Assassination Rogue",  "刺杀盗贼",     "assa" },
        { "暮刃",   HORDE, CLASS_ROGUE,   RACE_UNDEAD, GENDER_MALE,   ROLE_MELEE_DPS, "Combat Rogue",         "战斗盗贼",     "combat" },
        { "希芙",      HORDE, CLASS_ROGUE,   RACE_TROLL,  GENDER_FEMALE, ROLE_MELEE_DPS, "Combat Rogue",         "战斗盗贼",     "combat" },
        { "夜痕",   HORDE, CLASS_ROGUE,   RACE_ORC,    GENDER_FEMALE, ROLE_MELEE_DPS, "Subtlety Rogue",       "敏锐盗贼",     "" },
        { "幽语",     HORDE, CLASS_ROGUE,   RACE_UNDEAD, GENDER_FEMALE, ROLE_MELEE_DPS, "Subtlety Rogue",       "敏锐盗贼",     "" },
        { "塞璃",      HORDE, CLASS_PRIEST,  RACE_UNDEAD, GENDER_FEMALE, ROLE_HEALER,    "Discipline Priest",    "戒律牧师",     "holy" },
        { "佐拉",      HORDE, CLASS_PRIEST,  RACE_TROLL,  GENDER_FEMALE, ROLE_HEALER,    "Holy Priest",          "神圣牧师",     "holy" },
        { "玛凯",     HORDE, CLASS_PRIEST,  RACE_TROLL,  GENDER_MALE,   ROLE_RANGE_DPS, "Shadow Priest",        "暗影牧师",     "shadow" },
        { "沃佳",     HORDE, CLASS_MAGE,    RACE_TROLL,  GENDER_FEMALE, ROLE_RANGE_DPS, "Arcane Mage",          "奥术法师",     "arcane" },
        { "灰烬",     HORDE, CLASS_MAGE,    RACE_UNDEAD, GENDER_MALE,   ROLE_RANGE_DPS, "Fire Mage",            "火焰法师",     "fire" },
        { "霜牙",   HORDE, CLASS_MAGE,    RACE_TROLL,  GENDER_MALE,   ROLE_RANGE_DPS, "Frost Mage",           "冰霜法师",     "frost" },
        { "灾笑",    HORDE, CLASS_WARLOCK, RACE_ORC,    GENDER_MALE,   ROLE_RANGE_DPS, "Affliction Warlock",   "痛苦术士",     "affli" },
        { "邪影",    HORDE, CLASS_WARLOCK, RACE_UNDEAD, GENDER_FEMALE, ROLE_RANGE_DPS, "Demonology Warlock",   "恶魔学识术士", "demo" },
        { "颅火",   HORDE, CLASS_WARLOCK, RACE_ORC,    GENDER_FEMALE, ROLE_RANGE_DPS, "Destruction Warlock",  "毁灭术士",     "ruin" },
    };

    uint32 LookSeed(char const* name)
    {
        uint32 h = 2166136261u;
        for (; *name; ++name)
            h = (h ^ uint8(*name)) * 16777619u;
        return h ? h : 1;
    }

    uint32 const UPDATE_EVERY_MS = 500;
    // The client finishes loading the dungeon before the bots appear.
    uint32 const FILL_DELAY_MS = 3000;
    // A fill that waits for a ghost to come back to life gives up after this.
    uint32 const FILL_GIVE_UP_MS = 120000;
    // A bot that never shows up in the group is forgotten after this.
    uint32 const BOT_LOGIN_GRACE_MS = 30000;
    // After AddBot fails (account still online), skip that roster slot a while.
    uint32 const SLOT_FAIL_COOLDOWN_MS = 15000;
    // Short-party repair: do not re-Fill every tick or chat spams new names.
    uint32 const REPAIR_RETRY_OK_MS = 4000;
    uint32 const REPAIR_RETRY_FAIL_MS = 10000;
    // After a full wipe, wait for mobs to evade before safe-spot revive.
    uint32 const WIPE_REVIVE_DELAY_MS = 8000;
    float const SAFE_AGGRO_MARGIN = 3.0f;
    float const SAFE_SEARCH_RANGE = 40.0f;
    uint32 const SPELL_HONORLESS_TARGET = 2479;
    uint32 const PARTY_SIZE = 3;


    enum : uint32
    {
        SPELL_SHIELD_SLAM      = 23922,
        SPELL_HOLY_SHIELD      = 20925,
        SPELL_HOLY_SHOCK       = 20473,
        SPELL_SHADOWFORM       = 15473,
        SPELL_MOONKIN_FORM     = 24858,
        SPELL_SWIFTMEND        = 18562,
        SPELL_NATURES_SWIFT_DR = 17116,
        SPELL_MANA_TIDE        = 16190,
        SPELL_STORMSTRIKE      = 17364,
    };

    bool IsDps(CombatBotRoles role)
    {
        return role == ROLE_MELEE_DPS || role == ROLE_RANGE_DPS || role == ROLE_INVALID;
    }

    bool HasShield(Player const* pPlayer)
    {
        Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return pItem && pItem->GetProto()->InventoryType == INVTYPE_SHIELD;
    }

    // The client's language. GetSessionDbcLocale() is the server's DBC locale
    // (enUS here), so the client's own locale comes from its text index.
    bool IsZh(WorldSession const* pSession)
    {
        if (!pSession)
            return false;
        int const index = pSession->GetSessionDbLocaleIndex();
        return index >= 0 && sObjectMgr.GetLocaleForIndex(index) == LOCALE_zhCN;
    }

    // The party bot AI that runs a player, if it is one.
    PartyBotAI* GetPartyBotAI(Player* pPlayer)
    {
        if (!pPlayer || !pPlayer->IsBot() || !pPlayer->AI())
            return nullptr;
        return dynamic_cast<PartyBotAI*>(pPlayer->AI());
    }
}

DungeonBotMgr& DungeonBotMgr::Instance()
{
    static DungeonBotMgr instance;
    return instance;
}

char const* DungeonBotMgr::RoleName(CombatBotRoles role, bool zh)
{
    switch (role)
    {
        case ROLE_TANK:      return zh ? "坦克" : "tank";
        case ROLE_HEALER:    return zh ? "治疗" : "healer";
        case ROLE_MELEE_DPS: return zh ? "近战输出" : "melee dps";
        case ROLE_RANGE_DPS: return zh ? "远程输出" : "ranged dps";
        default:             return zh ? "自动" : "auto";
    }
}

CombatBotRoles DungeonBotMgr::GuessRole(Player const* pPlayer)
{
    switch (pPlayer->GetClass())
    {
        case CLASS_WARRIOR:
            return (pPlayer->HasSpell(SPELL_SHIELD_SLAM) || HasShield(pPlayer)) ? ROLE_TANK : ROLE_MELEE_DPS;
        case CLASS_PALADIN:
            if (pPlayer->HasSpell(SPELL_HOLY_SHIELD))
                return ROLE_TANK;
            if (pPlayer->HasSpell(SPELL_HOLY_SHOCK))
                return ROLE_HEALER;
            return ROLE_MELEE_DPS;
        case CLASS_PRIEST:
            return pPlayer->HasSpell(SPELL_SHADOWFORM) ? ROLE_RANGE_DPS : ROLE_HEALER;
        case CLASS_DRUID:
            if (pPlayer->HasSpell(SPELL_SWIFTMEND) || pPlayer->HasSpell(SPELL_NATURES_SWIFT_DR))
                return ROLE_HEALER;
            return pPlayer->HasSpell(SPELL_MOONKIN_FORM) ? ROLE_RANGE_DPS : ROLE_MELEE_DPS;
        case CLASS_SHAMAN:
            if (pPlayer->HasSpell(SPELL_MANA_TIDE))
                return ROLE_HEALER;
            return pPlayer->HasSpell(SPELL_STORMSTRIKE) ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
        case CLASS_ROGUE:
            return ROLE_MELEE_DPS;
        default:
            return ROLE_RANGE_DPS;
    }
}

// ---------------------------------------------------------------- settings --

DungeonBotMgr::Settings& DungeonBotMgr::GetSettings(Player* pPlayer)
{
    uint32 const guid = pPlayer->GetGUIDLow();
    auto itr = m_settings.find(guid);
    if (itr != m_settings.end())
        return itr->second;

    Settings& s = m_settings[guid];
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT `role`, `auto_fill`, `engage` FROM `character_party_bots` WHERE `guid` = %u", guid))
    {
        Field* fields = result->Fetch();
        uint8 const role = fields[0].GetUInt8();
        s.role = role <= ROLE_HEALER ? CombatBotRoles(role) : ROLE_INVALID;
        s.autoFill = fields[1].GetUInt8() != 0;
        uint8 const engage = fields[2].GetUInt8();
        s.engage = engage == PARTYBOT_ENGAGE_AGGRESSIVE ? PARTYBOT_ENGAGE_AGGRESSIVE : PARTYBOT_ENGAGE_CALM;
    }
    return s;
}

void DungeonBotMgr::SaveSettings(Player* pPlayer)
{
    Settings const& s = GetSettings(pPlayer);
    CharacterDatabase.PExecute(
        "REPLACE INTO `character_party_bots` (`guid`, `role`, `auto_fill`, `engage`) VALUES (%u, %u, %u, %u)",
        pPlayer->GetGUIDLow(), uint32(s.role), s.autoFill ? 1u : 0u, uint32(s.engage));
}

CombatBotRoles DungeonBotMgr::GetChosenRole(Player* pPlayer)
{
    return GetSettings(pPlayer).role;
}

CombatBotRoles DungeonBotMgr::GetPlayerRole(Player* pPlayer)
{
    CombatBotRoles const chosen = GetChosenRole(pPlayer);
    return chosen != ROLE_INVALID ? chosen : GuessRole(pPlayer);
}

void DungeonBotMgr::SetChosenRole(Player* pPlayer, CombatBotRoles role)
{
    GetSettings(pPlayer).role = role;
    SaveSettings(pPlayer);
}

bool DungeonBotMgr::IsAutoFill(Player* pPlayer)
{
    return GetSettings(pPlayer).autoFill;
}

PartyBotEngageStyle DungeonBotMgr::GetEngageStyle(Player* pPlayer)
{
    return GetSettings(pPlayer).engage;
}

void DungeonBotMgr::SetEngageStyle(Player* pPlayer, PartyBotEngageStyle style)
{
    GetSettings(pPlayer).engage = style;
    SaveSettings(pPlayer);
}

char const* DungeonBotMgr::EngageStyleName(PartyBotEngageStyle style, bool zh)
{
    if (style == PARTYBOT_ENGAGE_AGGRESSIVE)
        return zh ? "奔放" : "aggressive";
    return zh ? "冷静" : "calm";
}

void DungeonBotMgr::SetAutoFill(Player* pPlayer, bool on)
{
    GetSettings(pPlayer).autoFill = on;
    SaveSettings(pPlayer);
}

// --------------------------------------------------------------- companions --

void DungeonBotMgr::LoadCompanions()
{
    if (m_companionsLoaded)
        return;
    m_companionsLoaded = true;
    if (std::unique_ptr<QueryResult> result = CharacterDatabase.Query(
            "SELECT `owner_guid`, `slot`, `bot_guid` FROM `character_companions`"))
    {
        do
        {
            Field* fields = result->Fetch();
            m_companions[fields[0].GetUInt32()][fields[1].GetUInt32()] = fields[2].GetUInt32();
            m_companionOwner[fields[2].GetUInt32()] = fields[0].GetUInt32();
        }
        while (result->NextRow());
    }
}

bool DungeonBotMgr::IsCompanion(uint32 botLow)
{
    LoadCompanions();
    return m_companionOwner.find(botLow) != m_companionOwner.end();
}

uint32 DungeonBotMgr::GetCompanion(uint32 ownerLow, uint32 slot)
{
    LoadCompanions();
    auto owner = m_companions.find(ownerLow);
    if (owner == m_companions.end())
        return 0;
    auto itr = owner->second.find(slot);
    return itr != owner->second.end() ? itr->second : 0;
}

void DungeonBotMgr::SetCompanion(uint32 ownerLow, uint32 slot, uint32 botLow)
{
    m_companions[ownerLow][slot] = botLow;
    m_companionOwner[botLow] = ownerLow;
    CharacterDatabase.PExecute("REPLACE INTO `character_companions` (`owner_guid`, `slot`, `bot_guid`) VALUES (%u, %u, %u)",
        ownerLow, slot, botLow);
}

void DungeonBotMgr::ForgetCompanion(uint32 ownerLow, uint32 slot)
{
    if (uint32 const botLow = GetCompanion(ownerLow, slot))
        m_companionOwner.erase(botLow);
    m_companions[ownerLow].erase(slot);
    CharacterDatabase.PExecute("DELETE FROM `character_companions` WHERE `owner_guid` = %u AND `slot` = %u", ownerLow, slot);
}

void DungeonBotMgr::DeleteCompanions(uint32 ownerLow)
{
    LoadCompanions();
    auto owner = m_companions.find(ownerLow);
    if (owner == m_companions.end())
        return;
    for (auto const& itr : owner->second)
    {
        m_companionOwner.erase(itr.second);
        Player::DeleteFromDB(ObjectGuid(HIGHGUID_PLAYER, itr.second), sObjectMgr.GetPlayerAccountIdByGUID(itr.second), false, true);
    }
    m_companions.erase(owner);
    CharacterDatabase.PExecute("DELETE FROM `character_companions` WHERE `owner_guid` = %u", ownerLow);
}

// -------------------------------------------------------------- fill/dismiss --

void DungeonBotMgr::ForgetGoneBots(uint32 leaderLow)
{
    auto itr = m_autoBots.find(leaderLow);
    if (itr == m_autoBots.end())
        return;

    Player* leader = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, leaderLow));
    Group* group = leader ? leader->GetGroup() : nullptr;

    for (auto bot = itr->second.begin(); bot != itr->second.end();)
    {
        ObjectGuid const guid(HIGHGUID_PLAYER, bot->first);
        bool const inGroup = group && group->IsMember(guid);
        PlayerBotEntry* entry = sPlayerBotMgr.GetBotEntry(bot->first);
        bool const loading = entry && entry->state == PB_STATE_LOADING;

        // Still in the party, or mid-login: keep the seat.
        if (inGroup || loading)
        {
            if (ObjectAccessor::FindPlayerNotInWorld(guid) || loading)
                bot->second.lastSeen = m_now;
            ++bot;
            continue;
        }

        // Between maps a bot is briefly not in the group list yet still online.
        if (entry && entry->state == PB_STATE_ONLINE &&
            ObjectAccessor::FindPlayerNotInWorld(guid) &&
            m_now - bot->second.lastSeen <= BOT_LOGIN_GRACE_MS)
        {
            ++bot;
            continue;
        }

        // Self-removed or never joined: free the seat now (was a 30s block).
        if (entry)
        {
            if (entry->state != PB_STATE_OFFLINE)
                entry->requestRemoval = true;
            else
                sPlayerBotMgr.DeleteBot(bot->first);
        }
        bot = itr->second.erase(bot);
    }
    if (itr->second.empty())
        m_autoBots.erase(itr);
}

void DungeonBotMgr::SetWantSize(uint32 leaderLow, uint32 size)
{
    if (size < 2)
        size = 2;
    if (size > PARTY_SIZE)
        size = PARTY_SIZE;
    m_wantSize[leaderLow] = size;
}

void DungeonBotMgr::ClearWantSize(uint32 leaderLow)
{
    m_wantSize.erase(leaderLow);
}

uint32 DungeonBotMgr::Fill(Player* pPlayer, std::string& who, std::string& why, uint32 target, uint32* removedOut)
{
    if (target < 2)
        target = 2;
    if (target > PARTY_SIZE)
        target = PARTY_SIZE;
    if (removedOut)
        *removedOut = 0;

    bool const zh = IsZh(pPlayer->GetSession());

    if (pPlayer->IsTaxiFlying() || pPlayer->InBattleGround() || pPlayer->IsBeingTeleported())
    {
        why = zh ? "现在不能叫机器人（飞行中、战场里或传送中）。" : "Bots cannot join now (flying, in a battleground or teleporting).";
        return 0;
    }
    // FindMap: GetMap asserts when m_currMap is null (e.g. mid-teleport).
    Map* pMap = pPlayer->FindMap();
    if (!pMap)
    {
        why = zh ? "现在不能叫机器人。" : "Bots cannot join now.";
        return 0;
    }

    uint32 const ownerLow = pPlayer->GetGUIDLow();
    ForgetGoneBots(ownerLow);
    SetWantSize(ownerLow, target);

    // A saved group can hold companions (or other stubs) that went offline
    // with the server. Drop anyone not in the world at all so they do not
    // take a fill slot.
    if (Group* pOld = pPlayer->GetGroup())
    {
        std::vector<ObjectGuid> gone;
        for (auto const& slot : pOld->GetMemberSlots())
            if (!ObjectAccessor::FindPlayerNotInWorld(slot.guid))
                gone.push_back(slot.guid);
        for (ObjectGuid const& guid : gone)
            if (Group* pNow = pPlayer->GetGroup())
                pNow->RemoveMember(guid, 0);
    }

    // Who is in the party already, and as what.
    std::vector<CombatBotRoles> roles;
    std::vector<uint8> classes;
    std::vector<uint8> races;
    std::vector<std::string> names;
    std::vector<uint32> guids;
    Group* pGroup = pPlayer->GetGroup();
    if (pGroup)
    {
        // Trio product stays a normal party (max 5). A raid group would let
        // AddMember keep going past three; Soloroth never creates raids.
        if (pGroup->isRaidGroup())
        {
            why = zh ? "请先解散团队再叫小队机器人。" : "Leave the raid group before calling party bots.";
            return 0;
        }
        if (pGroup->GetLeaderGuid() != pPlayer->GetObjectGuid())
        {
            why = zh ? "只有队长能叫机器人。" : "Only the party leader can call bots.";
            return 0;
        }
        for (auto const& slot : pGroup->GetMemberSlots())
        {
            Player* pMember = sObjectAccessor.FindPlayer(slot.guid);
            if (!pMember)
            {
                // Still loading / between maps: count the seat. Truly gone
                // members were stripped above.
                if (!ObjectAccessor::FindPlayerNotInWorld(slot.guid))
                    continue;
                guids.push_back(slot.guid.GetCounter());
                roles.push_back(ROLE_INVALID);
                continue;
            }
            guids.push_back(slot.guid.GetCounter());
            classes.push_back(pMember->GetClass());
            races.push_back(pMember->GetRace());
            names.push_back(pMember->GetName());
            if (PartyBotAI* pAI = GetPartyBotAI(pMember))
                roles.push_back(pAI->GetRole());
            else
                roles.push_back(GetPlayerRole(pMember));
        }
    }
    else
    {
        // Same order as a group slot, so guids[i] and roles[i] stay paired.
        guids.push_back(pPlayer->GetGUIDLow());
        roles.push_back(GetPlayerRole(pPlayer));
        classes.push_back(pPlayer->GetClass());
        races.push_back(pPlayer->GetRace());
        names.push_back(pPlayer->GetName());
    }

    // Bots still logging in (or online but not yet on the party roster) count
    // too, or a second fill would double them. Offline / self-removed entries
    // are dropped by ForgetGoneBots and must not report "already full".
    auto& mine = m_autoBots[ownerLow];
    for (auto const& bot : mine)
    {
        ObjectGuid const guid(HIGHGUID_PLAYER, bot.first);
        if (pGroup && pGroup->IsMember(guid))
            continue;
        PlayerBotEntry* entry = sPlayerBotMgr.GetBotEntry(bot.first);
        if (!entry || entry->state == PB_STATE_OFFLINE)
            continue;
        guids.push_back(bot.first);
        roles.push_back(bot.second.role);
        classes.push_back(bot.second.botClass);
        if (bot.second.race)
            races.push_back(bot.second.race);
        names.push_back(bot.second.name);
    }

    auto dropBot = [&](uint32 botLow)
    {
        ObjectGuid const guid(HIGHGUID_PLAYER, botLow);
        if (Group* pNow = pPlayer->GetGroup())
            if (pNow->IsMember(guid))
                pNow->RemoveMember(guid, 0);
        // In the world: the bot manager logs them out. Still logging in:
        // DeleteBot leaves the entry offline so the login does not finish.
        if (PartyBotAI* pAI = GetPartyBotAI(sObjectAccessor.FindPlayer(guid)))
        {
            if (pAI->botEntry)
                pAI->botEntry->requestRemoval = true;
        }
        else
            sPlayerBotMgr.DeleteBot(botLow);
        mine.erase(botLow);
    };
    // Prefer dropping damage bots, then a healer, then a tank.
    auto pickDropIndex = [&](bool dpsOnly) -> int
    {
        int best = -1;
        int bestScore = 99;
        for (uint32 i = 0; i < guids.size(); ++i)
        {
            if (guids[i] == pPlayer->GetGUIDLow())
                continue;
            ObjectGuid const guid(HIGHGUID_PLAYER, guids[i]);
            bool const bot = mine.find(guids[i]) != mine.end() || IsCompanion(guids[i]) ||
                             GetPartyBotAI(sObjectAccessor.FindPlayer(guid)) != nullptr;
            if (!bot)
                continue;
            int score = 0;
            CombatBotRoles const role = i < roles.size() ? roles[i] : ROLE_INVALID;
            if (role == ROLE_HEALER)
                score = 1;
            else if (role == ROLE_TANK)
                score = 2;
            if (dpsOnly && score != 0)
                continue;
            if (best < 0 || score < bestScore || (score == bestScore && int(i) > best))
            {
                best = int(i);
                bestScore = score;
            }
        }
        return best;
    };
    auto eraseSeat = [&](uint32 index)
    {
        guids.erase(guids.begin() + index);
        if (index < roles.size())
            roles.erase(roles.begin() + index);
        if (index < classes.size())
            classes.erase(classes.begin() + index);
        if (index < races.size())
            races.erase(races.begin() + index);
        if (index < names.size())
            names.erase(names.begin() + index);
    };

    uint32 removed = 0;

    // Too many: send bots home until the party is the size asked for. The
    // player is never removed.
    if (roles.size() > target)
    {
        while (roles.size() > target)
        {
            int const best = pickDropIndex(false);
            if (best < 0)
                break;
            dropBot(guids[best]);
            eraseSeat(uint32(best));
            ++removed;
        }
        if (removedOut)
            *removedOut = removed;
        if (roles.size() > target)
        {
            why = zh ? "队伍里还有别人，不能缩到这个人数。" : "The party has other players, so it cannot shrink to that size.";
            return 0;
        }
        // Shrinking done; composition may still lack a tank/healer below.
    }

    bool needTank = std::find(roles.begin(), roles.end(), ROLE_TANK) == roles.end();
    bool needHealer = std::find(roles.begin(), roles.end(), ROLE_HEALER) == roles.end();

    // Trio full but missing a tank/healer: free DPS seats so the fill below
    // can replace them. A dismiss/re-enter race used to leave only DPS
    // and then report "already full" forever. Duo is random — no role repair.
    if (target >= 3 && roles.size() >= target && (needTank || needHealer))
    {
        uint32 missing = (needTank ? 1u : 0u) + (needHealer ? 1u : 0u);
        while (missing && roles.size() == target)
        {
            int const best = pickDropIndex(true);
            if (best < 0)
                break;
            dropBot(guids[best]);
            eraseSeat(uint32(best));
            ++removed;
            --missing;
        }
        needTank = std::find(roles.begin(), roles.end(), ROLE_TANK) == roles.end();
        needHealer = std::find(roles.begin(), roles.end(), ROLE_HEALER) == roles.end();
    }
    if (removedOut)
        *removedOut = removed;

    if (roles.size() >= target)
    {
        // Shrink-only success: leave `why` empty so `.bots fill` reports the removals.
        if (!removed)
        {
            if (target == 2)
                why = zh ? "已经是两人。" : "Already a duo.";
            else if (target == 3)
                why = zh ? "已经是三人。" : "Already a trio.";
            else
                why = zh ? "队伍已经满了。" : "The party is already full.";
        }
        return 0;
    }

    uint32 melee = uint32(std::count(roles.begin(), roles.end(), ROLE_MELEE_DPS));

    Team const team = pPlayer->GetTeam();
    auto slotOf = [](RosterBot const& r) { return uint32(&r - ROSTER); };
    // Roster slots that failed to spawn this fill (stuck online session, etc.).
    std::set<uint32> blocked;
    // Only this character's own companion can be out already. Do not KickBot
    // here: logging out a LOADING session crashes the world update and left
    // accounts "already online", so Fill announced a new name every tick.
    auto present = [&](RosterBot const& r)
    {
        uint32 const slot = slotOf(r);
        if (blocked.find(slot) != blocked.end())
            return true;
        auto cool = m_slotCooldown.find(ownerLow);
        if (cool != m_slotCooldown.end())
        {
            auto when = cool->second.find(slot);
            if (when != cool->second.end() && m_now < when->second)
                return true;
        }
        uint32 const botLow = GetCompanion(ownerLow, slot);
        if (!botLow)
            return false;
        if (std::find(guids.begin(), guids.end(), botLow) != guids.end())
            return true;
        return ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, botLow)) != nullptr;
    };
    auto markSlotFail = [&](uint32 slot)
    {
        blocked.insert(slot);
        m_slotCooldown[ownerLow][slot] = m_now + SLOT_FAIL_COOLDOWN_MS;
    };
    // A free roster member for one of `wanted` roles. Prefer a class the
    // party lacks, then a race it lacks, else any. Each class gets the same
    // chance however many races or trees fit; within a class, each race the
    // same chance however many trees fit.
    auto pick = [&](std::vector<CombatBotRoles> const& wanted) -> RosterBot const*
    {
        std::vector<RosterBot const*> fit, freshClass, freshRace;
        for (RosterBot const& r : ROSTER)
        {
            if (r.team != team || std::find(wanted.begin(), wanted.end(), r.role) == wanted.end() || present(r))
                continue;
            fit.push_back(&r);
            if (std::find(classes.begin(), classes.end(), r.botClass) == classes.end())
                freshClass.push_back(&r);
            if (std::find(races.begin(), races.end(), r.race) == races.end())
                freshRace.push_back(&r);
        }
        std::vector<RosterBot const*> const& pool =
            !freshClass.empty() ? freshClass : !freshRace.empty() ? freshRace : fit;
        if (pool.empty())
            return nullptr;
        std::vector<uint8> poolClasses;
        for (RosterBot const* r : pool)
            if (std::find(poolClasses.begin(), poolClasses.end(), r->botClass) == poolClasses.end())
                poolClasses.push_back(r->botClass);
        uint8 const botClass = SelectRandomContainerElement(poolClasses);
        std::vector<RosterBot const*> ofClass;
        for (RosterBot const* r : pool)
            if (r->botClass == botClass)
                ofClass.push_back(r);
        std::vector<uint8> poolRaces;
        for (RosterBot const* r : ofClass)
            if (std::find(poolRaces.begin(), poolRaces.end(), r->race) == poolRaces.end())
                poolRaces.push_back(r->race);
        uint8 const botRace = SelectRandomContainerElement(poolRaces);
        std::vector<RosterBot const*> ofRace;
        for (RosterBot const* r : ofClass)
            if (r->race == botRace)
                ofRace.push_back(r);
        return SelectRandomContainerElement(ofRace);
    };

    // Keep trying until the party reaches `target`, or no roster left to try.
    // A failed pick/AddBot must not burn a seat (that used to leave fill-5 at 3).
    // Trio: do not spend the last seats on DPS while a tank/healer is
    // still missing — leave them open so Update can retry once companions
    // finish logging out. Duo: one random companion, any role.
    uint32 have = uint32(roles.size());
    uint32 added = 0;
    while (have < target)
    {
        RosterBot const* r = nullptr;
        if (target < 3)
        {
            static CombatBotRoles const kDuoRoles[] = {
                ROLE_TANK, ROLE_HEALER, ROLE_MELEE_DPS, ROLE_RANGE_DPS
            };
            CombatBotRoles const wantRole = kDuoRoles[urand(0, 3)];
            r = pick({ wantRole });
            if (!r)
                r = pick({ ROLE_TANK, ROLE_HEALER, ROLE_MELEE_DPS, ROLE_RANGE_DPS });
        }
        else
        {
            if (needTank)
                r = pick({ ROLE_TANK });
            if (!r && needHealer)
                r = pick({ ROLE_HEALER });
            // Tank/healer first: leave the seat open rather than plug it with DPS.
            if (!r && (needTank || needHealer))
                break;
            if (!r)
            {
                // At most two melee, so the tank is not crowded and cleaves hit fewer.
                r = melee >= 2 ? pick({ ROLE_RANGE_DPS }) : pick({ ROLE_MELEE_DPS, ROLE_RANGE_DPS });
            }
        }
        if (!r)
            break;

        float x, y, z;
        pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 3.0f, frand(0.0f, 2 * M_PI_F));
        uint32 const slot = slotOf(*r);
        uint32 botLow = GetCompanion(ownerLow, slot);
        std::string name;
        if (botLow && !sObjectMgr.GetPlayerNameByGUID(botLow, name))
        {
            ForgetCompanion(ownerLow, slot);    // its character is gone
            botLow = 0;
        }

        PartyBotAI* ai;
        if (botLow)
        {
            // Race and class 0: PartyBotAI logs the saved character in.
            ai = new PartyBotAI(pPlayer, nullptr, r->role, 0, 0, pPlayer->GetLevel(),
                pPlayer->GetMapId(), pMap->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
            ai->m_specHint = r->specHint;
            ai->m_companion = true;
            if (!sPlayerBotMgr.AddBot(botLow, false, ai))
            {
                // AddBot already kicks a stuck ONLINE bot session once. Do not
                // KickBot a LOADING session here (world update asserts) — cool
                // the slot down and try another roster member.
                delete ai;
                markSlotFail(slot);
                continue;
            }
        }
        else
        {
            name = sObjectMgr.GetPlayerGuidByName(r->name).IsEmpty() ? std::string(r->name) : sObjectMgr.GenerateFreePlayerName();
            normalizePlayerName(name);
            ai = new PartyBotAI(pPlayer, nullptr, r->role, r->race, r->botClass, pPlayer->GetLevel(),
                pPlayer->GetMapId(), pMap->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
            ai->SetIdentity(name, r->gender, LookSeed(name.c_str()));
            ai->m_specHint = r->specHint;
            ai->m_companion = true;
            if (!sPlayerBotMgr.AddBot(ai, true))
            {
                delete ai;
                markSlotFail(slot);
                continue;
            }
            botLow = uint32(ai->botEntry->playerGUID);
            SetCompanion(ownerLow, slot, botLow);
        }
        ai->botEntry->persistent = true;

        if (r->role == ROLE_TANK)
            needTank = false;
        else if (r->role == ROLE_HEALER)
            needHealer = false;
        else if (r->role == ROLE_MELEE_DPS)
            ++melee;

        AutoBot& bot = mine[botLow];
        bot.role = r->role;
        bot.botClass = r->botClass;
        bot.race = r->race;
        bot.name = name;
        bot.lastSeen = m_now;
        bot.dungeon = pMap->IsDungeon();
        roles.push_back(r->role);
        classes.push_back(r->botClass);
        races.push_back(r->race);
        names.push_back(name);
        guids.push_back(botLow);
        if (!who.empty())
            who += zh ? "、" : ", ";
        who += name;
        who += zh ? "（" : " (";
        who += zh ? r->specZh : r->specEn;
        who += zh ? "）" : ")";
        ++added;
        ++have;
    }

    if (!added)
        why = zh ? "机器人没能生成。" : "No bot could be spawned.";
    else if (have < target)
        why = zh ? "队伍没能补满。" : "The party could not be filled completely.";
    return added;
}

uint32 DungeonBotMgr::Dismiss(Player* pPlayer, bool autoOnly)
{
    uint32 count = 0;
    uint32 const leaderLow = pPlayer->GetGUIDLow();
    // Stop repair-fills (leaving a dungeon must not re-fill to three outdoors).
    ClearWantSize(leaderLow);

    // The bots this manager brought, wherever they are. Always drop the
    // listing: a stuck login left in m_autoBots used to shrink the next fill.
    auto mine = m_autoBots.find(leaderLow);
    if (mine != m_autoBots.end())
    {
        for (auto bot = mine->second.begin(); bot != mine->second.end();)
        {
            if (autoOnly && !bot->second.dungeon)
            {
                ++bot;
                continue;
            }
            ObjectGuid const guid(HIGHGUID_PLAYER, bot->first);
            if (Group* pGroup = pPlayer->GetGroup())
                if (pGroup->IsMember(guid))
                    pGroup->RemoveMember(guid, 0);
            PartyBotAI* pAI = GetPartyBotAI(sObjectAccessor.FindPlayer(guid));
            if (pAI && pAI->botEntry)
                pAI->botEntry->requestRemoval = true;
            else
                sPlayerBotMgr.DeleteBot(bot->first);
            ++count;
            bot = mine->second.erase(bot);
        }
        if (mine->second.empty())
            m_autoBots.erase(mine);
    }

    // Everything else this player leads: bots added with .partybot.
    if (!autoOnly)
    {
        if (Group* pGroup = pPlayer->GetGroup())
        {
            for (auto const& slot : pGroup->GetMemberSlots())
            {
                PartyBotAI* pAI = GetPartyBotAI(sObjectAccessor.FindPlayer(slot.guid));
                if (!pAI || pAI->m_leaderGuid != pPlayer->GetObjectGuid() || !pAI->botEntry || pAI->botEntry->requestRemoval)
                    continue;
                pAI->botEntry->requestRemoval = true;
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------- safe / wipe rez --

void DungeonBotMgr::ClearSafe(Watch& w)
{
    w.hasSafe = false;
    w.safeMap = 0;
    w.safeInstance = 0;
    w.safeX = w.safeY = w.safeZ = w.safeO = 0.f;
}

void DungeonBotMgr::SeedSafe(Watch& w, Player* pPlayer)
{
    if (!pPlayer || !pPlayer->FindMap())
        return;
    w.hasSafe = true;
    w.safeMap = pPlayer->GetMapId();
    w.safeInstance = pPlayer->GetInstanceId();
    w.safeX = pPlayer->GetPositionX();
    w.safeY = pPlayer->GetPositionY();
    w.safeZ = pPlayer->GetPositionZ();
    w.safeO = pPlayer->GetOrientation();
}

bool DungeonBotMgr::IsSafeSpot(Player* pPlayer) const
{
    if (!pPlayer || !pPlayer->IsInWorld())
        return false;

    std::list<Unit*> hostiles;
    MaNGOS::AnyHostileUnitInObjectRangeCheck u_check(pPlayer, pPlayer, SAFE_SEARCH_RANGE);
    MaNGOS::UnitListSearcher<MaNGOS::AnyHostileUnitInObjectRangeCheck> searcher(hostiles, u_check);
    Cell::VisitAllObjects(pPlayer, searcher, SAFE_SEARCH_RANGE);

    for (Unit* u : hostiles)
    {
        Creature* c = u ? u->ToCreature() : nullptr;
        if (!c || !c->IsAlive() || c->IsCivilian())
            continue;
        if (c->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1 | UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PLAYER))
            continue;
        float const aggro = c->GetAttackDistance(pPlayer) + SAFE_AGGRO_MARGIN;
        if (aggro > 0.f && pPlayer->IsWithinDistInMap(c, aggro))
            return false;
    }
    return true;
}

void DungeonBotMgr::MaybeUpdateSafe(Watch& w, Player* pPlayer)
{
    if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsInCombat())
        return;
    if (!IsSafeSpot(pPlayer))
        return;
    SeedSafe(w, pPlayer);
}

bool DungeonBotMgr::PartyWipedOnMap(Player* pPlayer) const
{
    if (!pPlayer || pPlayer->IsAlive())
        return false;
    Map* pMap = pPlayer->FindMap();
    if (!pMap)
        return false;

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
        return true;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        // Group still holds the Player* while a bot is between maps (ResetMap
        // already ran). GetMap() would assert; treat no-map as not on this wipe.
        if (!pMember || pMember->FindMap() != pMap)
            continue;
        if (pMember->IsAlive())
            return false;
    }
    return true;
}

void DungeonBotMgr::RevivePartyAtSafe(Player* pPlayer, Watch& w)
{
    if (!pPlayer || !w.hasSafe)
        return;

    uint32 const mapId = w.safeMap;
    float const x = w.safeX;
    float const y = w.safeY;
    float const z = w.safeZ;
    float const o = w.safeO;

    auto reviveOne = [&](Player* p)
    {
        if (!p || p->IsAlive() || !p->IsInWorld() || p->IsBeingTeleported())
            return;
        if (p->GetMapId() != mapId)
            return;

        p->TeleportTo(mapId, x, y, z, o);
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(0.5f);
            p->SpawnCorpseBones();
        }
        if (p->IsBot())
            p->CastSpell(p, SPELL_HONORLESS_TARGET, true);
    };

    reviveOne(pPlayer);
    if (Group* pGroup = pPlayer->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (pMember && pMember != pPlayer)
                reviveOne(pMember);
        }
    }

    bool const zh = IsZh(pPlayer->GetSession());
    ChatHandler(pPlayer).SendSysMessage(zh
        ? "团灭了。队伍已在最近的安全点复活。"
        : "Party wiped. The group resurrected at the last safe spot.");
}

// ------------------------------------------------------------------- update --

void DungeonBotMgr::Update(uint32 diff)
{
    m_now += diff;
    m_timer += diff;
    if (m_timer < UPDATE_EVERY_MS)
        return;
    m_timer = 0;

    std::vector<ObjectGuid> players;
    {
        HashMapHolder<Player>::ReadGuard guard(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
            if (itr.second && !itr.second->IsBot())
                players.push_back(itr.first);
    }

    for (ObjectGuid const& guid : players)
    {
        Player* pPlayer = sObjectAccessor.FindPlayer(guid);
        if (!pPlayer || !pPlayer->IsInWorld() || pPlayer->IsBeingTeleported())
            continue;
        Map* pMap = pPlayer->FindMap();
        if (!pMap)
            continue;

        Watch& w = m_watch[guid.GetCounter()];
        bool const inDungeon = pMap->IsDungeon();
        if (pMap->GetId() != w.lastMap)
        {
            w.lastMap = pMap->GetId();
            w.fillAt = (inDungeon && IsAutoFill(pPlayer)) ? m_now + FILL_DELAY_MS : 0;
            w.wipeAt = 0;
            ClearSafe(w);
            if (inDungeon && pPlayer->IsAlive())
                SeedSafe(w, pPlayer);
        }

        if (!inDungeon)
        {
            w.fillAt = 0;
            w.wipeAt = 0;
            ClearSafe(w);
            // Out of the dungeon on their feet: the dungeon's bots go home.
            // A ghost on its corpse run keeps them.
            auto mine = m_autoBots.find(guid.GetCounter());
            if (pPlayer->IsAlive() && mine != m_autoBots.end() && !mine->second.empty())
            {
                ForgetGoneBots(guid.GetCounter());
                if (uint32 gone = Dismiss(pPlayer, true))
                {
                    bool const zh = IsZh(pPlayer->GetSession());
                    ChatHandler(pPlayer).PSendSysMessage(zh ? "离开副本，%u 个机器人队友解散了。" : "Left the dungeon: %u bot(s) went home.", gone);
                }
            }
            continue;
        }

        // Safe checkpoint while alive and out of combat.
        if (pPlayer->IsAlive())
        {
            w.wipeAt = 0;
            if (!w.hasSafe)
                SeedSafe(w, pPlayer);
            else
                MaybeUpdateSafe(w, pPlayer);
        }
        else if (PartyWipedOnMap(pPlayer))
        {
            if (!w.wipeAt)
                w.wipeAt = m_now;
            else if (m_now - w.wipeAt >= WIPE_REVIVE_DELAY_MS)
            {
                // No walk recorded (e.g. died on the load-in): use the corpse.
                if (!w.hasSafe)
                    SeedSafe(w, pPlayer);
                RevivePartyAtSafe(pPlayer, w);
                w.wipeAt = 0;
            }
        }
        else
            w.wipeAt = 0;

        if (!w.fillAt || m_now < w.fillAt)
            continue;
        if (!pPlayer->IsAlive() || pPlayer->IsInCombat())
        {
            if (m_now - w.fillAt > FILL_GIVE_UP_MS)
                w.fillAt = 0;
            continue;
        }
        w.fillAt = 0;

        std::string who, why;
        bool const zh = IsZh(pPlayer->GetSession());
        if (Fill(pPlayer, who, why))
        {
            ChatHandler(pPlayer).PSendSysMessage(zh ? "队友加入了队伍：%s（你是%s）。人数在小地图下的组队按钮里改。" : "Joining your party: %s (you play %s). The party button under the minimap changes the size.",
                who.c_str(), RoleName(GetPlayerRole(pPlayer), zh));
            // Give the new bots time to finish login / join the roster before
            // the short-party repair can race another Fill.
            w.repairAt = m_now + REPAIR_RETRY_OK_MS;
        }
        else if (!why.empty())
            ChatHandler(pPlayer).SendSysMessage(why.c_str());
    }

    // Repair a short party, or a trio that still lacks tank/healer (e.g.
    // dismiss raced a re-fill and DPS took every seat). Duo stays random — no
    // role repair. Back off when fills fail so chat does not spam a new
    // random name every half-second.
    for (ObjectGuid const& guid : players)
    {
        Player* pPlayer = sObjectAccessor.FindPlayer(guid);
        if (!pPlayer || !pPlayer->IsInWorld() || pPlayer->IsBeingTeleported())
            continue;
        if (pPlayer->IsTaxiFlying() || pPlayer->InBattleGround() || !pPlayer->IsAlive())
            continue;
        uint32 const low = guid.GetCounter();
        auto want = m_wantSize.find(low);
        if (want == m_wantSize.end())
            continue;
        Watch& repairWatch = m_watch[low];
        if (repairWatch.repairAt && m_now < repairWatch.repairAt)
            continue;
        ForgetGoneBots(low);
        Group* group = pPlayer->GetGroup();
        uint32 have = group ? group->GetMembersCount() : 1;
        bool hasTank = GetPlayerRole(pPlayer) == ROLE_TANK;
        bool hasHealer = GetPlayerRole(pPlayer) == ROLE_HEALER;
        bool pendingLogin = false;
        if (group)
        {
            for (auto const& slot : group->GetMemberSlots())
            {
                if (slot.guid == pPlayer->GetObjectGuid())
                    continue;
                Player* pMember = sObjectAccessor.FindPlayer(slot.guid);
                CombatBotRoles role = ROLE_INVALID;
                if (pMember)
                {
                    if (PartyBotAI* pAI = GetPartyBotAI(pMember))
                        role = pAI->GetRole();
                    else
                        role = GetPlayerRole(pMember);
                }
                else
                {
                    auto mineRoles = m_autoBots.find(low);
                    if (mineRoles != m_autoBots.end())
                    {
                        auto bot = mineRoles->second.find(slot.guid.GetCounter());
                        if (bot != mineRoles->second.end())
                            role = bot->second.role;
                    }
                }
                if (role == ROLE_TANK)
                    hasTank = true;
                else if (role == ROLE_HEALER)
                    hasHealer = true;
            }
        }
        auto mine = m_autoBots.find(low);
        if (mine != m_autoBots.end())
        {
            for (auto const& bot : mine->second)
            {
                if (group && group->IsMember(ObjectGuid(HIGHGUID_PLAYER, bot.first)))
                    continue;
                PlayerBotEntry* entry = sPlayerBotMgr.GetBotEntry(bot.first);
                // LOADING or ONLINE-but-not-grouped yet: hold the seat. Counting
                // only LOADING let a second fill race in and push a trio to five.
                if (!entry || entry->state == PB_STATE_OFFLINE)
                    continue;
                ++have;
                pendingLogin = true;
                if (bot.second.role == ROLE_TANK)
                    hasTank = true;
                else if (bot.second.role == ROLE_HEALER)
                    hasHealer = true;
            }
        }
        bool const shortParty = have < want->second;
        // Trio needs tank+healer; duo is a random companion.
        bool const badComp = want->second >= 3 && (!hasTank || !hasHealer);
        if (!shortParty && !badComp)
        {
            repairWatch.repairAt = 0;
            continue;
        }
        // Wait for in-flight logins before starting another wave of names.
        if (pendingLogin)
        {
            repairWatch.repairAt = m_now + REPAIR_RETRY_OK_MS;
            continue;
        }
        std::string who, why;
        if (Fill(pPlayer, who, why, want->second) && !who.empty())
        {
            bool const zh = IsZh(pPlayer->GetSession());
            ChatHandler(pPlayer).PSendSysMessage(zh ? "队友正在赶来：%s。" : "On their way: %s.", who.c_str());
            repairWatch.repairAt = m_now + REPAIR_RETRY_OK_MS;
        }
        else
            repairWatch.repairAt = m_now + REPAIR_RETRY_FAIL_MS;
    }

    // Players who logged out: forget where they were, so logging back in
    // inside a dungeon counts as entering it (their bots left with them).
    // (A player between two maps is not in the world but still online.)
    for (auto itr = m_watch.begin(); itr != m_watch.end();)
    {
        if (!ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, itr->first)))
            itr = m_watch.erase(itr);
        else
            ++itr;
    }

    // Leaders who logged out: their bots leave on their own (PartyBotAI).
    for (auto itr = m_autoBots.begin(); itr != m_autoBots.end();)
    {
        if (!ObjectAccessor::FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, itr->first)))
        {
            ClearWantSize(itr->first);
            itr = m_autoBots.erase(itr);
        }
        else
            ++itr;
    }
}

// ----------------------------------------------------------------- commands --

static bool BotsZh(ChatHandler* handler)
{
    WorldSession* s = handler->GetSession();
    return IsZh(s);
}

bool ChatHandler::HandleBotsStatusCommand(char* args)
{
    // `.bots status quiet`: only the machine line, for the Bots addon.
    bool const quiet = args && std::string(args) == "quiet";
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    bool const zh = BotsZh(this);
    CombatBotRoles const chosen = sDungeonBotMgr.GetChosenRole(pPlayer);
    CombatBotRoles const playing = sDungeonBotMgr.GetPlayerRole(pPlayer);
    bool const autoFill = sDungeonBotMgr.IsAutoFill(pPlayer);
    PartyBotEngageStyle const engage = sDungeonBotMgr.GetEngageStyle(pPlayer);
    // The first line is also read by the Bots addon: keep its shape.
    PSendSysMessage("[bots] role=%s playing=%s auto=%u style=%s", DungeonBotMgr::RoleName(chosen, false),
        DungeonBotMgr::RoleName(playing, false), autoFill ? 1u : 0u,
        DungeonBotMgr::EngageStyleName(engage, false));
    if (quiet)
        return true;
    if (zh)
        PSendSysMessage("你的职责：%s%s；进副本自动补三人小队：%s；接战：%s。",
            DungeonBotMgr::RoleName(playing, true), chosen == ROLE_INVALID ? "（自动判断）" : "",
            autoFill ? "开" : "关", DungeonBotMgr::EngageStyleName(engage, true));
    else
        PSendSysMessage("You play %s%s; instances fill a trio with bots: %s; engage: %s.",
            DungeonBotMgr::RoleName(playing, false), chosen == ROLE_INVALID ? " (guessed)" : "",
            autoFill ? "on" : "off", DungeonBotMgr::EngageStyleName(engage, false));
    return true;
}

bool ChatHandler::HandleBotsEngageCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    std::string const arg = args ? args : "";
    PartyBotEngageStyle style;
    if (arg == "calm" || arg == "safe" || arg == "hold")
        style = PARTYBOT_ENGAGE_CALM;
    else if (arg == "aggressive" || arg == "aggro" || arg == "rush")
        style = PARTYBOT_ENGAGE_AGGRESSIVE;
    else
    {
        SendSysMessage(BotsZh(this)
            ? "用法：.bots engage calm|aggressive（冷静|奔放）"
            : "Usage: .bots engage calm|aggressive");
        SetSentErrorMessage(true);
        return false;
    }
    sDungeonBotMgr.SetEngageStyle(pPlayer, style);
    return HandleBotsStatusCommand(nullptr);
}

bool ChatHandler::HandleBotsRoleCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    std::string const arg = args ? args : "";
    CombatBotRoles role;
    if (arg == "tank")
        role = ROLE_TANK;
    else if (arg == "healer" || arg == "heal")
        role = ROLE_HEALER;
    else if (arg == "dps")
        role = CombatBotBaseAI::IsMeleeDamageClass(pPlayer->GetClass()) && pPlayer->GetClass() != CLASS_DRUID &&
               pPlayer->GetClass() != CLASS_SHAMAN ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
    else if (arg == "melee")
        role = ROLE_MELEE_DPS;
    else if (arg == "ranged")
        role = ROLE_RANGE_DPS;
    else if (arg == "auto")
        role = ROLE_INVALID;
    else
    {
        SendSysMessage(BotsZh(this) ? "用法：.bots role tank|healer|dps|auto" : "Usage: .bots role tank|healer|dps|auto");
        SetSentErrorMessage(true);
        return false;
    }
    sDungeonBotMgr.SetChosenRole(pPlayer, role);
    return HandleBotsStatusCommand(nullptr);
}

bool ChatHandler::HandleBotsAutoCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    bool on;
    if (!ExtractOnOff(&args, on))
    {
        SendSysMessage(BotsZh(this) ? "用法：.bots auto on|off" : "Usage: .bots auto on|off");
        SetSentErrorMessage(true);
        return false;
    }
    sDungeonBotMgr.SetAutoFill(pPlayer, on);
    return HandleBotsStatusCommand(nullptr);
}

bool ChatHandler::HandleBotsFillCommand(char* args)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;

    // No argument fills a trio. 2 is a duo. Instances are trio-scaled; five
    // is no longer offered.
    uint32 target = 3;
    if (args)
    {
        while (*args == ' ')
            ++args;
        if (*args)
        {
            char const digit = args[0];
            char* rest = args + 1;
            while (*rest == ' ')
                ++rest;
            if ((digit != '2' && digit != '3') || *rest)
            {
                SendSysMessage(BotsZh(this)
                    ? "用法：.bots fill [2|3]。两人小队或三人小队。"
                    : "Usage: .bots fill [2|3]. A duo or a trio.");
                SetSentErrorMessage(true);
                return false;
            }
            target = uint32(digit - '0');
        }
    }

    std::string who, why;
    uint32 removed = 0;
    if (!sDungeonBotMgr.Fill(pPlayer, who, why, target, &removed))
    {
        if (removed && why.empty())
        {
            PSendSysMessage(BotsZh(this) ? "解散了 %u 个机器人，现在是 %u 人。" : "Sent %u bot(s) home. The party is %u.",
                removed, target);
            return true;
        }
        SendSysMessage(why.c_str());
        // "Already a duo" is the answer, not a failure the client should print in red.
        if (why.find("已经") != std::string::npos || why.find("lready") != std::string::npos)
            return true;
        SetSentErrorMessage(true);
        return false;
    }
    PSendSysMessage(BotsZh(this) ? "队友正在赶来：%s。" : "On their way: %s.", who.c_str());
    if (!why.empty())
        SendSysMessage(why.c_str());
    return true;
}

bool ChatHandler::HandleBotsDismissCommand(char* /*args*/)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    uint32 const gone = sDungeonBotMgr.Dismiss(pPlayer, false);
    PSendSysMessage(BotsZh(this) ? "%u 个机器人队友解散了。" : "%u bot(s) went home.", gone);
    return true;
}
