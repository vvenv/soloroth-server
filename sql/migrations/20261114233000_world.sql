DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261114233000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261114233000');
-- Add your query below.

-- Class abilities that Classic only taught through quests, offered by the
-- matching class trainers so a no-quest character can learn them there.
-- Spell ids are the teach spells (SPELL_EFFECT_LEARN_SPELL); race filters
-- for the priest racials come from SkillLineAbility as usual.

-- Warrior trainers (templates 22, 23): Defensive Stance, Berserker Stance.
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 22 AS entry UNION SELECT 23) t,
  (SELECT 8121 AS spell, 10 AS lvl, 1000 AS cost UNION ALL
   SELECT 8616, 30, 3000) s
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Paladin (28, 29): Redemption, Sense Undead, Warhorse, Charger.
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 28 AS entry UNION SELECT 29) t,
  (SELECT 7329 AS spell, 12 AS lvl, 1200 AS cost UNION ALL
   SELECT 5503, 20, 2000 UNION ALL
   SELECT 13820, 40, 4000 UNION ALL
   SELECT 23215, 60, 6000) s
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Hunter (19, 20): Tame Beast, Beast Training.
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 19 AS entry UNION SELECT 20) t,
  (SELECT 1579 AS spell, 10 AS lvl, 1000 AS cost UNION ALL
   SELECT 5300, 10, 1000) s
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Rogue (25, 26): Poisons.
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 25 AS entry UNION SELECT 26) t,
  (SELECT 2995 AS spell, 20 AS lvl, 2000 AS cost) s
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Priest (7, 8): race-gated racials (SkillLineAbility filters who sees them).
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 7 AS entry UNION SELECT 8) t,
  (SELECT 19350 AS spell, 10 AS lvl, 1000 AS cost UNION ALL  -- Starshards (Night Elf)
   SELECT 19338, 10, 1000 UNION ALL                          -- Desperate Prayer (Human/Dwarf)
   SELECT 19325, 10, 1000 UNION ALL                          -- Hex of Weakness (Troll)
   SELECT 19318, 10, 1000 UNION ALL                          -- Touch of Weakness (Undead)
   SELECT 19337, 20, 2000 UNION ALL                          -- Fear Ward (Dwarf)
   SELECT 19331, 20, 2000 UNION ALL                          -- Shadowguard (Troll)
   SELECT 2946,  20, 2000 UNION ALL                          -- Devouring Plague (Undead)
   SELECT 19357, 20, 2000 UNION ALL                          -- Elune's Grace (Night Elf)
   SELECT 19345, 20, 2000) s                                 -- Feedback (Human)
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Shaman (10, 11): the four element totems' first ranks (totem items are granted in code).
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 10 AS entry UNION SELECT 11) t,
  (SELECT 8073 AS spell, 4 AS lvl, 400 AS cost UNION ALL     -- Stoneskin Totem
   SELECT 2075, 10, 1000 UNION ALL                           -- Searing Totem
   SELECT 5396, 20, 2000) s                                  -- Healing Stream Totem
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Warlock (13, 14): Voidwalker, Succubus, Felhunter, Felsteed, Inferno, Ritual of Doom, Dreadsteed.
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 13 AS entry UNION SELECT 14) t,
  (SELECT 11520 AS spell, 10 AS lvl, 1000 AS cost UNION ALL  -- Voidwalker
   SELECT 11519, 20, 2000 UNION ALL                          -- Succubus
   SELECT 1373,  30, 3000 UNION ALL                          -- Felhunter
   SELECT 1710,  40, 4000 UNION ALL                          -- Felsteed
   SELECT 1413,  50, 5000 UNION ALL                          -- Inferno
   SELECT 20700, 60, 6000 UNION ALL                          -- Ritual of Doom
   SELECT 23160, 60, 6000) s                                 -- Dreadsteed
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- Druid (16, 17): Bear Form, Aquatic Form, Cure Poison.
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`, `build_min`, `build_max`)
SELECT t.entry, s.spell, s.cost, 0, 0, s.lvl, 0, 5875 FROM
  (SELECT 16 AS entry UNION SELECT 17) t,
  (SELECT 19179 AS spell, 10 AS lvl, 1000 AS cost UNION ALL  -- Bear Form
   SELECT 8947,  14, 1400 UNION ALL                          -- Cure Poison
   SELECT 1446,  16, 1600) s                                 -- Aquatic Form
WHERE NOT EXISTS (SELECT 1 FROM `npc_trainer_template` x WHERE x.`entry`=t.entry AND x.`spell`=s.spell);

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
