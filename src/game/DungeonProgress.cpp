/*
 * Dungeon and raid boss progress. See DungeonProgress.h.
 */

#include "DungeonProgress.h"
#include "AchievementMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MapPersistentStateMgr.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"

namespace
{
    char const* const ADDON_PREFIX = "SoloDun";

    // map, wing, creature entry, true = killing this one is a clear.
    // Order inside a wing is the order the addon shows.
    struct BossRow
    {
        uint32 map;
        uint32 wing;
        uint32 entry;
        bool finalBoss;
    };

    BossRow const BOSS_LIST[] =
    {
        { 389, 0, 11517, false }, // Oggleflint
        { 389, 0, 11520, true  }, // Taragaman the Hungerer
        { 389, 0, 11518, false }, // Jergosh the Invoker
        { 389, 0, 11519, false }, // Bazzalan

        { 43, 0, 3671, false },   // Lady Anacondra
        { 43, 0, 3669, false },   // Lord Cobrahn
        { 43, 0, 3670, false },   // Lord Pythas
        { 43, 0, 3674, false },   // Skum
        { 43, 0, 3673, false },   // Lord Serpentis
        { 43, 0, 5775, false },   // Verdan the Everliving
        { 43, 0, 3654, true  },   // Mutanus the Devourer

        { 36, 0, 644, false },    // Rhahk'Zor
        { 36, 0, 643, false },    // Sneed
        { 36, 0, 1763, false },   // Gilnid
        { 36, 0, 646, false },    // Mr. Smite
        { 36, 0, 645, false },    // Cookie
        { 36, 0, 647, false },    // Captain Greenskin
        { 36, 0, 639, true  },    // Edwin VanCleef

        { 33, 0, 3914, false },   // Rethilgore
        { 33, 0, 3886, false },   // Razorclaw the Butcher
        { 33, 0, 3887, false },   // Baron Silverlaine
        { 33, 0, 4278, false },   // Commander Springvale
        { 33, 0, 4279, false },   // Odo the Blindwatcher
        { 33, 0, 4274, false },   // Fenrus the Devourer
        { 33, 0, 3927, false },   // Wolf Master Nandos
        { 33, 0, 4275, true  },   // Archmage Arugal

        { 48, 0, 4887, false },   // Ghamoo-ra
        { 48, 0, 4831, false },   // Lady Sarevess
        { 48, 0, 6243, false },   // Gelihast
        { 48, 0, 4832, false },   // Twilight Lord Kelris
        { 48, 0, 4829, true  },   // Aku'mai

        { 34, 0, 1696, false },   // Targorr the Dread
        { 34, 0, 1666, false },   // Kam Deepfury
        { 34, 0, 1717, false },   // Hamhock
        { 34, 0, 1663, false },   // Dextren Ward
        { 34, 0, 1716, true  },   // Bazil Thredd

        { 189, 1, 3983, false },  // Interrogator Vishas
        { 189, 1, 4543, true  },  // Bloodmage Thalnos
        { 189, 2, 3974, false },  // Houndmaster Loksey
        { 189, 2, 6487, true  },  // Arcanist Doan
        { 189, 3, 3975, true  },  // Herod
        { 189, 4, 4542, false },  // High Inquisitor Fairbanks
        { 189, 4, 3976, false },  // Scarlet Commander Mograine
        { 189, 4, 3977, true  },  // High Inquisitor Whitemane

        { 90, 0, 7079, false },   // Viscous Fallout
        { 90, 0, 7361, false },   // Grubbis
        { 90, 0, 6235, false },   // Electrocutioner 6000
        { 90, 0, 6229, false },   // Crowd Pummeler 9-60
        { 90, 0, 7800, true  },   // Mekgineer Thermaplugg

        { 47, 0, 6168, false },   // Roogug
        { 47, 0, 4424, false },   // Aggem Thorncurse
        { 47, 0, 4428, false },   // Death Speaker Jargba
        { 47, 0, 4420, false },   // Overlord Ramtusk
        { 47, 0, 4422, false },   // Agathelos the Raging
        { 47, 0, 4421, true  },   // Charlga Razorflank

        { 129, 0, 7355, false },  // Tuten'kash
        { 129, 0, 7357, false },  // Mordresh Fire Eye
        { 129, 0, 8567, false },  // Glutton
        { 129, 0, 7358, true  },  // Amnennar the Coldbringer

        { 70, 0, 6910, false },   // Revelosh
        { 70, 0, 7023, false },   // Obsidian Sentinel
        { 70, 0, 7206, false },   // Ancient Stone Keeper
        { 70, 0, 7228, false },   // Ironaya
        { 70, 0, 7291, false },   // Galgann Firehammer
        { 70, 0, 4854, false },   // Grimlok
        { 70, 0, 2748, true  },   // Archaedas

        { 209, 0, 7272, false },  // Theka the Martyr
        { 209, 0, 8127, false },  // Antu'sul
        { 209, 0, 7271, false },  // Witch Doctor Zum'rah
        { 209, 0, 7796, false },  // Nekrum Gutchewer
        { 209, 0, 7275, false },  // Shadowpriest Sezz'ziz
        { 209, 0, 7795, false },  // Hydromancer Velratha
        { 209, 0, 7797, false },  // Ruuzlu
        { 209, 0, 7267, true  },  // Chief Ukorz Sandscalp
        { 209, 0, 7273, false },  // Gahz'rilla, summoned

        { 349, 1, 13282, false }, // Noxxion
        { 349, 1, 12258, true  }, // Razorlash
        { 349, 2, 12236, false }, // Lord Vyletongue
        { 349, 2, 12225, true  }, // Celebras the Cursed
        { 349, 3, 12203, false }, // Landslide
        { 349, 3, 13596, false }, // Rotgrip
        { 349, 3, 12201, true  }, // Princess Theradras
        { 349, 3, 13601, false }, // Tinkerer Gizlock

        { 109, 0, 5710, false },  // Jammal'an the Prophet
        { 109, 0, 5711, false },  // Ogom the Wretched
        { 109, 0, 5721, false },  // Dreamscythe
        { 109, 0, 5720, false },  // Weaver
        { 109, 0, 5719, false },  // Morphaz
        { 109, 0, 5722, false },  // Hazzas
        { 109, 0, 5709, true  },  // Shade of Eranikus
        { 109, 0, 8443, false },  // Avatar of Hakkar, summoned

        { 230, 0, 9018, false },  // High Interrogator Gerstahn
        { 230, 0, 9025, false },  // Lord Roccor
        { 230, 0, 9319, false },  // Houndmaster Grebmar
        { 230, 0, 9024, false },  // Pyromancer Loregrain
        { 230, 0, 9017, false },  // Lord Incendius
        { 230, 0, 9041, false },  // Warder Stilgiss
        { 230, 0, 9056, false },  // Fineous Darkvire
        { 230, 0, 9016, false },  // Bael'Gar
        { 230, 0, 9033, false },  // General Angerforge
        { 230, 0, 8983, false },  // Golem Lord Argelmach
        { 230, 0, 9537, false },  // Hurley Blackbreath
        { 230, 0, 9502, false },  // Phalanx
        { 230, 0, 9543, false },  // Ribbly Screwspigot
        { 230, 0, 9499, false },  // Plugger Spazzring
        { 230, 0, 9156, false },  // Ambassador Flamelash
        { 230, 0, 9938, false },  // Magmus
        { 230, 0, 9019, true  },  // Emperor Dagran Thaurissan

        { 229, 1, 9196, false },  // Highlord Omokk
        { 229, 1, 9236, false },  // Shadow Hunter Vosh'gajin
        { 229, 1, 9237, false },  // War Master Voone
        { 229, 1, 10596, false }, // Mother Smolderweb
        { 229, 1, 9736, false },  // Quartermaster Zigris
        { 229, 1, 10220, false }, // Halycon
        { 229, 1, 10268, false }, // Gizrul the Slavener
        { 229, 1, 9568, true  },  // Overlord Wyrmthalak
        { 229, 2, 9816, false },  // Pyroguard Emberseer
        { 229, 2, 10264, false }, // Solakar Flamewreath
        { 229, 2, 10429, false }, // Warchief Rend Blackhand
        { 229, 2, 10339, false }, // Gyth
        { 229, 2, 10430, false }, // The Beast
        { 229, 2, 10363, true  }, // General Drakkisath

        { 429, 1, 14354, false }, // Pusillin
        { 429, 1, 14327, false }, // Lethtendris
        { 429, 1, 13280, false }, // Hydrospawn
        { 429, 1, 11490, false }, // Zevrim Thornhoof
        { 429, 1, 11492, true  }, // Alzzin the Wildshaper
        { 429, 2, 11489, false }, // Tendris Warpwood
        { 429, 2, 11488, false }, // Illyanna Ravenoak
        { 429, 2, 11487, false }, // Magister Kalendris
        { 429, 2, 11496, false }, // Immol'thar
        { 429, 2, 11486, true  }, // Prince Tortheldrin
        { 429, 3, 14326, false }, // Guard Mol'dar
        { 429, 3, 14322, false }, // Stomper Kreeg
        { 429, 3, 14321, false }, // Guard Fengus
        { 429, 3, 14323, false }, // Guard Slip'kik
        { 429, 3, 14325, false }, // Captain Kromcrush
        { 429, 3, 14324, false }, // Cho'Rush the Observer
        { 429, 3, 11501, true  }, // King Gordok

        { 289, 0, 10506, false }, // Kirtonos the Herald
        { 289, 0, 10503, false }, // Jandice Barov
        { 289, 0, 11622, false }, // Rattlegore
        { 289, 0, 10433, false }, // Marduk Blackpool
        { 289, 0, 10432, false }, // Vectus
        { 289, 0, 10508, false }, // Ras Frostwhisper
        { 289, 0, 10505, false }, // Instructor Malicia
        { 289, 0, 11261, false }, // Doctor Theolen Krastinov
        { 289, 0, 10901, false }, // Lorekeeper Polkelt
        { 289, 0, 10507, false }, // The Ravenian
        { 289, 0, 10504, false }, // Lord Alexei Barov
        { 289, 0, 10502, false }, // Lady Illucia Barov
        { 289, 0, 1853, true  },  // Darkmaster Gandling

        { 329, 1, 10516, false }, // The Unforgiven
        { 329, 1, 10808, false }, // Timmy the Cruel
        { 329, 1, 10997, false }, // Cannon Master Willey
        { 329, 1, 10811, false }, // Archivist Galford
        { 329, 1, 10813, true  }, // Balnazzar
        { 329, 2, 10435, false }, // Magistrate Barthilas
        { 329, 2, 10437, false }, // Nerub'enkan
        { 329, 2, 10436, false }, // Baroness Anastari
        { 329, 2, 11121, false }, // Black Guard Swordsmith
        { 329, 2, 10438, false }, // Maleki the Pallid
        { 329, 2, 10439, false }, // Ramstein the Gorger
        { 329, 2, 10440, true  }, // Baron Rivendare

        { 409, 0, 12118, false }, // Lucifron
        { 409, 0, 11982, false }, // Magmadar
        { 409, 0, 12259, false }, // Gehennas
        { 409, 0, 12057, false }, // Garr
        { 409, 0, 12264, false }, // Shazzrah
        { 409, 0, 12056, false }, // Baron Geddon
        { 409, 0, 11988, false }, // Golemagg the Incinerator
        { 409, 0, 12098, false }, // Sulfuron Harbinger
        { 409, 0, 12018, false }, // Majordomo Executus
        { 409, 0, 11502, true  }, // Ragnaros

        { 469, 0, 12435, false }, // Razorgore the Untamed
        { 469, 0, 13020, false }, // Vaelastrasz the Corrupt
        { 469, 0, 12017, false }, // Broodlord Lashlayer
        { 469, 0, 11983, false }, // Firemaw
        { 469, 0, 14601, false }, // Ebonroc
        { 469, 0, 11981, false }, // Flamegor
        { 469, 0, 14020, false }, // Chromaggus
        { 469, 0, 11583, true  }, // Nefarian

        { 309, 0, 14517, false }, // High Priestess Jeklik
        { 309, 0, 14507, false }, // High Priest Venoxis
        { 309, 0, 14510, false }, // High Priestess Mar'li
        { 309, 0, 11382, false }, // Bloodlord Mandokir
        { 309, 0, 14509, false }, // High Priest Thekal
        { 309, 0, 14515, false }, // High Priestess Arlokk
        { 309, 0, 11380, false }, // Jin'do the Hexxer
        { 309, 0, 14834, true  }, // Hakkar

        { 509, 0, 15348, false }, // Kurinnaxx
        { 509, 0, 15341, false }, // General Rajaxx
        { 509, 0, 15340, false }, // Moam
        { 509, 0, 15370, false }, // Buru the Gorger
        { 509, 0, 15369, false }, // Ayamiss the Hunter
        { 509, 0, 15339, true  }, // Ossirian the Unscarred

        { 531, 0, 15263, false }, // The Prophet Skeram
        { 531, 0, 15543, false }, // Princess Yauj
        { 531, 0, 15544, false }, // Vem
        { 531, 0, 15511, false }, // Lord Kri
        { 531, 0, 15516, false }, // Battleguard Sartura
        { 531, 0, 15510, false }, // Fankriss the Unyielding
        { 531, 0, 15299, false }, // Viscidus
        { 531, 0, 15509, false }, // Princess Huhuran
        { 531, 0, 15276, false }, // Emperor Vek'lor
        { 531, 0, 15275, false }, // Emperor Vek'nilash
        { 531, 0, 15517, false }, // Ouro
        { 531, 0, 15727, true  }, // C'Thun

        { 533, 0, 16028, false }, // Patchwerk
        { 533, 0, 15931, false }, // Grobbulus
        { 533, 0, 15932, false }, // Gluth
        { 533, 0, 15928, false }, // Thaddius
        { 533, 0, 15956, false }, // Anub'Rekhan
        { 533, 0, 15953, false }, // Grand Widow Faerlina
        { 533, 0, 15952, false }, // Maexxna
        { 533, 0, 15954, false }, // Noth the Plaguebringer
        { 533, 0, 15936, false }, // Heigan the Unclean
        { 533, 0, 16011, false }, // Loatheb
        { 533, 0, 16061, false }, // Instructor Razuvious
        { 533, 0, 16060, false }, // Gothik the Harvester
        { 533, 0, 16063, false }, // Sir Zeliek
        { 533, 0, 16064, false }, // Thane Korth'azz
        { 533, 0, 16065, false }, // Lady Blaumeux
        { 533, 0, 16062, false }, // Highlord Mograine
        { 533, 0, 15990, true  }, // Kel'Thuzad

        { 249, 0, 10184, true  }, // Onyxia
    };

    struct WingInfo
    {
        uint32 map;
        uint32 wing;
        char const* name;
        char const* nameZh;
    };

    WingInfo const WING_LIST[] =
    {
        { 389, 0, "Ragefire Chasm", "怒焰裂谷" },
        { 43, 0, "Wailing Caverns", "哀嚎洞穴" },
        { 36, 0, "The Deadmines", "死亡矿井" },
        { 33, 0, "Shadowfang Keep", "影牙城堡" },
        { 48, 0, "Blackfathom Deeps", "黑暗深渊" },
        { 34, 0, "The Stockade", "暴风城监狱" },
        { 189, 1, "Scarlet Monastery: Graveyard", "血色修道院：墓地" },
        { 189, 2, "Scarlet Monastery: Library", "血色修道院：图书馆" },
        { 189, 3, "Scarlet Monastery: Armory", "血色修道院：军械库" },
        { 189, 4, "Scarlet Monastery: Cathedral", "血色修道院：大教堂" },
        { 90, 0, "Gnomeregan", "诺莫瑞根" },
        { 47, 0, "Razorfen Kraul", "剃刀沼泽" },
        { 129, 0, "Razorfen Downs", "剃刀高地" },
        { 70, 0, "Uldaman", "奥达曼" },
        { 209, 0, "Zul'Farrak", "祖尔法拉克" },
        { 349, 1, "Maraudon: Orange", "玛拉顿：橙色入口" },
        { 349, 2, "Maraudon: Purple", "玛拉顿：紫色入口" },
        { 349, 3, "Maraudon: Princess", "玛拉顿：瑟莱德丝" },
        { 109, 0, "Sunken Temple", "沉没的神庙" },
        { 230, 0, "Blackrock Depths", "黑石深渊" },
        { 229, 1, "Lower Blackrock Spire", "黑石塔下层" },
        { 229, 2, "Upper Blackrock Spire", "黑石塔上层" },
        { 429, 1, "Dire Maul: East", "厄运之槌：东" },
        { 429, 2, "Dire Maul: West", "厄运之槌：西" },
        { 429, 3, "Dire Maul: North", "厄运之槌：北" },
        { 289, 0, "Scholomance", "通灵学院" },
        { 329, 1, "Stratholme: Main Gate", "斯坦索姆：正门" },
        { 329, 2, "Stratholme: Service Gate", "斯坦索姆：后门" },
        { 409, 0, "Molten Core", "熔火之心" },
        { 469, 0, "Blackwing Lair", "黑翼之巢" },
        { 309, 0, "Zul'Gurub", "祖尔格拉布" },
        { 509, 0, "Ruins of Ahn'Qiraj", "安其拉废墟" },
        { 531, 0, "Temple of Ahn'Qiraj", "安其拉神殿" },
        { 533, 0, "Naxxramas", "纳克萨玛斯" },
        { 249, 0, "Onyxia's Lair", "奥妮克希亚的巢穴" },
    };

    std::string Clean(std::string s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r')
                s[i] = ' ';
        return s;
    }
}

DungeonProgress& DungeonProgress::Instance()
{
    static DungeonProgress instance;
    return instance;
}

void DungeonProgress::Load()
{
    m_bosses.clear();
    m_bosses.reserve(sizeof(BOSS_LIST) / sizeof(BOSS_LIST[0]));
    for (BossRow const& src : BOSS_LIST)
        m_bosses.push_back(Boss{src.map, src.wing, src.entry, src.finalBoss});
    m_wings.clear();
    m_byEntry.clear();
    m_spawns.clear();
    m_bossXY.clear();

    for (WingInfo const& src : WING_LIST)
    {
        Wing wing;
        wing.map = src.map;
        wing.wing = src.wing;
        wing.name = src.name;
        wing.nameZh = src.nameZh;
        m_wings.push_back(std::move(wing));
    }

    std::unordered_map<uint32, uint32> expectMap;
    for (Boss const& boss : m_bosses)
    {
        m_byEntry[boss.entry] = &boss;
        expectMap[boss.entry] = boss.map;
        for (Wing& wing : m_wings)
        {
            if (wing.map != boss.map || wing.wing != boss.wing)
                continue;
            wing.bosses.push_back(&boss);
            if (boss.finalBoss)
                wing.finalBoss = &boss;
            break;
        }
    }

    for (Wing const& wing : m_wings)
        if (!wing.finalBoss || wing.bosses.empty())
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "DungeonProgress: wing %u/%u has no last boss", wing.map, wing.wing);

    std::unordered_map<uint32, float> sumX, sumY;
    std::unordered_map<uint32, uint32> sumN;

    auto worker = [&](CreatureDataPair const& pair) -> bool
    {
        CreatureData const& data = pair.second;
        for (uint32 id : data.creature_id)
        {
            if (!id)
                continue;
            auto expect = expectMap.find(id);
            if (expect == expectMap.end() || data.position.mapId != expect->second)
                continue;
            m_spawns[id].push_back(pair.first);
            sumX[id] += data.position.x;
            sumY[id] += data.position.y;
            sumN[id] += 1;
            Boss const* boss = m_byEntry[id];
            for (Wing& wing : m_wings)
            {
                if (wing.map != boss->map || wing.wing != boss->wing)
                    continue;
                wing.ax += data.position.x;
                wing.ay += data.position.y;
                wing.az += data.position.z;
                wing.anchors += 1;
                break;
            }
            break;
        }
        return false;
    };
    sObjectMgr.DoCreatureData(worker);

    for (auto const& n : sumN)
    {
        if (!n.second)
            continue;
        BossXY xy;
        xy.x = sumX[n.first] / float(n.second);
        xy.y = sumY[n.first] / float(n.second);
        m_bossXY[n.first] = xy;
    }

    for (Wing& wing : m_wings)
    {
        if (!wing.anchors)
            continue;
        wing.ax /= float(wing.anchors);
        wing.ay /= float(wing.anchors);
        wing.az /= float(wing.anchors);
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Dungeon progress: %u bosses, %u wings",
        uint32(m_bosses.size()), uint32(m_wings.size()));
}

DungeonProgress::Wing const* DungeonProgress::WingFor(Player* pPlayer) const
{
    uint32 const mapId = pPlayer->GetMap()->GetId();
    Wing const* only = nullptr;
    uint32 count = 0;
    for (Wing const& wing : m_wings)
    {
        if (wing.map != mapId)
            continue;
        only = &wing;
        ++count;
    }
    if (!count)
        return nullptr;
    if (count == 1)
        return only;

    float const px = pPlayer->GetPositionX();
    float const py = pPlayer->GetPositionY();
    float const pz = pPlayer->GetPositionZ();
    Wing const* best = nullptr;
    float bestD = 0;
    for (Wing const& wing : m_wings)
    {
        if (wing.map != mapId || !wing.anchors)
            continue;
        float const dx = px - wing.ax;
        float const dy = py - wing.ay;
        float const dz = pz - wing.az;
        float const d = dx * dx + dy * dy + dz * dz;
        if (!best || d < bestD)
        {
            best = &wing;
            bestD = d;
        }
    }
    return best ? best : only;
}

bool DungeonProgress::SpawnDown(Map* pMap, uint32 lowguid) const
{
    MapPersistentState* pState = pMap->GetPersistentState();
    if (pState && pState->GetCreatureRespawnTime(lowguid) > time(nullptr))
        return true;
    // Still a corpse: the respawn time is written when the grid unloads, so a
    // boss killed a moment ago is not in creature_respawn yet.
    CreatureData const* data = sObjectMgr.GetCreatureData(lowguid);
    if (!data)
        return false;
    Creature* c = pMap->GetCreature(data->GetObjectGuid(lowguid));
    return c && c->IsDead();
}

std::unordered_set<uint32> DungeonProgress::LoadDown(uint32 guid, uint32 instanceId) const
{
    std::unordered_set<uint32> down;
    std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
        "SELECT `entry` FROM `character_dungeon_boss` WHERE `guid` = %u AND `instance` = %u",
        guid, instanceId));
    if (!result)
        return down;
    do
    {
        down.insert(result->Fetch()[0].GetUInt32());
    } while (result->NextRow());
    return down;
}

void DungeonProgress::Remember(uint32 guid, uint32 instanceId, uint32 entry)
{
    CharacterDatabase.PExecute(
        "INSERT IGNORE INTO `character_dungeon_boss` (`guid`, `instance`, `entry`) VALUES (%u, %u, %u)",
        guid, instanceId, entry);
}

uint32 DungeonProgress::LoadClears(uint32 guid, Wing const* pWing, bool& exists) const
{
    exists = false;
    std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
        "SELECT `clears` FROM `character_dungeon_clear` WHERE `guid` = %u AND `map` = %u AND `wing` = %u",
        guid, pWing->map, pWing->wing));
    if (!result)
        return 0;
    exists = true;
    return result->Fetch()[0].GetUInt32();
}

void DungeonProgress::SaveClears(uint32 guid, Wing const* pWing, uint32 clears)
{
    CharacterDatabase.PExecute(
        "REPLACE INTO `character_dungeon_clear` (`guid`, `map`, `wing`, `clears`) VALUES (%u, %u, %u, %u)",
        guid, pWing->map, pWing->wing, clears);
}

int DungeonProgress::Locale(Player* pPlayer)
{
    if (!pPlayer->GetSession())
        return -1;
    return pPlayer->GetSession()->GetSessionDbLocaleIndex();
}

std::string DungeonProgress::BossName(uint32 entry, int loc) const
{
    CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);
    if (!info)
        return std::to_string(entry);
    if (loc >= 0)
    {
        if (CreatureLocale const* cl = sObjectMgr.GetCreatureLocale(entry))
            if (cl->Name.size() > size_t(loc) && !cl->Name[loc].empty())
                return Clean(cl->Name[loc]);
    }
    return Clean(info->name);
}

std::string DungeonProgress::WingName(Wing const* pWing, int loc) const
{
    if (loc >= 0 && sObjectMgr.GetLocaleForIndex(loc) == LOCALE_zhCN && !pWing->nameZh.empty())
        return pWing->nameZh;
    return pWing->name;
}

void DungeonProgress::SendAddon(Player* pPlayer, std::string const& text)
{
    if (!pPlayer->GetSession())
        return;
    std::string const message = std::string(ADDON_PREFIX) + "\t" + text;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, message.c_str(), LANG_ADDON, CHAT_TAG_NONE, pPlayer->GetObjectGuid());
    pPlayer->GetSession()->SendPacket(&data);
}

DungeonProgress::Run& DungeonProgress::Touch(uint32 guid, uint32 instanceId)
{
    Run& run = m_runs[guid];
    // A reset binds a new instance. The clear count stays; the boss list does not.
    if (!run.downLoaded || run.instanceId != instanceId)
    {
        run.instanceId = instanceId;
        run.down = LoadDown(guid, instanceId);
        run.downLoaded = true;
    }
    return run;
}

uint32 DungeonProgress::ClearsOf(uint32 guid, Wing const* pWing, Run& run, bool& stored)
{
    uint32 const key = (pWing->map << 8) | pWing->wing;
    if (run.clearsKnown.find(key) == run.clearsKnown.end())
    {
        bool row = false;
        run.clears[key] = LoadClears(guid, pWing, row);
        run.clearsKnown.insert(key);
        if (row)
            run.clearsStored.insert(key);
    }
    stored = run.clearsStored.find(key) != run.clearsStored.end();
    return run.clears[key];
}

void DungeonProgress::NoteKill(Player* pPlayer, Boss const* pBoss, uint32 instanceId)
{
    uint32 const guid = pPlayer->GetGUIDLow();
    std::lock_guard<std::mutex> guard(m_lock);
    Run& run = Touch(guid, instanceId);
    if (!run.down.insert(pBoss->entry).second)
        return;
    Remember(guid, instanceId, pBoss->entry);
    if (!pBoss->finalBoss)
        return;
    Wing const* pWing = nullptr;
    for (Wing const& wing : m_wings)
        if (wing.map == pBoss->map && wing.wing == pBoss->wing)
            pWing = &wing;
    if (!pWing)
        return;
    bool stored = false;
    uint32 const clears = ClearsOf(guid, pWing, run, stored) + 1;
    uint32 const key = (pWing->map << 8) | pWing->wing;
    run.clears[key] = clears;
    run.clearsKnown.insert(key);
    run.clearsStored.insert(key);
    SaveClears(guid, pWing, clears);
}

void DungeonProgress::OnCreatureKilled(Creature* pVictim, Player* /*pTapper*/, Group* /*pGroupTap*/)
{
    if (!pVictim)
        return;
    auto found = m_byEntry.find(pVictim->GetEntry());
    if (found == m_byEntry.end())
        return;
    Map* pMap = pVictim->GetMap();
    if (!pMap || !pMap->IsDungeon())
        return;

    // The tap is often a bot. Every real player on this map gets the kill,
    // then a snapshot from memory so the panel moves before the async write.
    uint32 const instanceId = pMap->GetInstanceId();
    std::vector<Player*> players;
    for (auto const& itr : pMap->GetPlayers())
    {
        Player* pPlayer = itr.getSource();
        if (!pPlayer || !pPlayer->IsInWorld() || !pPlayer->GetSession() || pPlayer->IsBot())
            continue;
        players.push_back(pPlayer);
    }
    for (Player* pPlayer : players)
    {
        NoteKill(pPlayer, found->second, instanceId);
        Push(pPlayer, false);
    }
}

void DungeonProgress::Push(Player* pPlayer, bool backfill)
{
    Map* pMap = pPlayer->GetMap();
    if (!pMap || !pMap->IsDungeon())
    {
        SendAddon(pPlayer, "X");
        return;
    }
    Wing const* pWing = WingFor(pPlayer);
    if (!pWing)
    {
        SendAddon(pPlayer, "X");
        return;
    }

    uint32 const guid = pPlayer->GetGUIDLow();
    uint32 const instanceId = pMap->GetInstanceId();
    int const loc = Locale(pPlayer);
    uint32 clears = 0;
    std::vector<std::pair<bool, std::string>> rows;

    {
        std::lock_guard<std::mutex> guard(m_lock);
        Run& run = Touch(guid, instanceId);
        for (Boss const* boss : pWing->bosses)
        {
            if (run.down.find(boss->entry) != run.down.end())
                continue;
            auto spawns = m_spawns.find(boss->entry);
            if (spawns == m_spawns.end())
                continue;
            for (uint32 lowguid : spawns->second)
            {
                if (!SpawnDown(pMap, lowguid))
                    continue;
                Remember(guid, instanceId, boss->entry);
                run.down.insert(boss->entry);
                break;
            }
        }

        bool stored = false;
        clears = ClearsOf(guid, pWing, run, stored);
        if (backfill && !stored)
        {
            bool lastDown = pWing->finalBoss && run.down.find(pWing->finalBoss->entry) != run.down.end();
            bool credited = pWing->finalBoss && sAchievementMgr.HasKillCredit(pPlayer, pWing->finalBoss->entry);
            if (lastDown || credited)
            {
                clears = 1;
                uint32 const key = (pWing->map << 8) | pWing->wing;
                run.clears[key] = clears;
                run.clearsKnown.insert(key);
                run.clearsStored.insert(key);
                SaveClears(guid, pWing, clears);
            }
        }

        rows.reserve(pWing->bosses.size());
        for (Boss const* boss : pWing->bosses)
            rows.push_back(std::make_pair(run.down.find(boss->entry) != run.down.end(), BossName(boss->entry, loc)));
    }

    float const px = pPlayer->GetPositionX();
    float const py = pPlayer->GetPositionY();
    std::string const header = "H " + std::to_string(clears) + "\t" + WingName(pWing, loc);
    SendAddon(pPlayer, "S");
    SendAddon(pPlayer, header);
    SendAddon(pPlayer, "P " + std::to_string(px) + "\t" + std::to_string(py));
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto const& row = rows[i];
        std::string line = std::string("K ") + (row.first ? "1" : "0") + "\t" + row.second;
        Boss const* boss = pWing->bosses[i];
        auto xy = m_bossXY.find(boss->entry);
        if (xy != m_bossXY.end())
            line += "\t" + std::to_string(xy->second.x) + "\t" + std::to_string(xy->second.y);
        SendAddon(pPlayer, line);
    }
    SendAddon(pPlayer, "D");
}

void DungeonProgress::Send(Player* pPlayer)
{
    if (!pPlayer || !pPlayer->GetSession() || pPlayer->IsBot())
        return;
    Push(pPlayer, true);
}

void DungeonProgress::DeleteCharacter(uint32 guidLow)
{
    CharacterDatabase.PExecute("DELETE FROM `character_dungeon_clear` WHERE `guid` = %u", guidLow);
    CharacterDatabase.PExecute("DELETE FROM `character_dungeon_boss` WHERE `guid` = %u", guidLow);
}

bool ChatHandler::HandleDungeonProgressCommand(char* /*args*/)
{
    Player* pPlayer = m_session ? m_session->GetPlayer() : nullptr;
    if (!pPlayer)
        return false;
    sDungeonProgress.Send(pPlayer);
    return true;
}
