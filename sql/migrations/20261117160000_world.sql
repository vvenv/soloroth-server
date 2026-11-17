DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261117160000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261117160000');
-- Add your query below.

-- Soloroth: outdoor quest-summoned bosses get a normal world spawn so
-- NoQuest players can find them. Coordinates from the summon scripts (or
-- the remains / den for Kroshius and Grenka). Respawn 5 minutes.
-- Dungeon/raid summons (Gahz'rilla, Hakkar avatar, …) stay instance-only.

-- Attackable as a permanent spawn (quest scripts normally flip these).
UPDATE `creature_template` SET `faction` = 834 WHERE `entry` = 14435; -- Thunderaan: was Friendly until script
UPDATE `creature_template`
SET `faction` = 14,
    `static_flags1` = (`static_flags1` & ~0x60)
WHERE `entry` = 14467; -- Kroshius: was immune until Fel Fire
UPDATE `creature_template`
SET `static_flags1` = (`static_flags1` & ~0x20)
WHERE `entry` = 2755; -- Myzrael: was immune to players until summoned

INSERT INTO `creature`
(`guid`, `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`,
 `spawntimesecsmin`, `spawntimesecsmax`, `wander_distance`, `health_percent`,
 `mana_percent`, `movement_type`, `spawn_flags`, `visibility_mod`, `patch_min`, `patch_max`)
VALUES
-- Eastern Kingdoms
(310001,   314, 0, -10270.00,    53.92,   41.89, 6.154, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Eliza (Duskwood)
(310002,  1494, 0, -14598.60,    76.06,  -11.25, 0.925, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Negolash (STV)
(310003,  2757, 0,  -6718.00, -4125.00,  263.89, 2.683, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Blacklash (Badlands)
(310004,  2759, 0,  -6703.00, -4094.00,  264.23, 2.830, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Hematus (Badlands)
(310005,  2763, 0,  -1522.30, -2166.64,   17.36, 5.960, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Thenan (Arathi)
(310006,  2755, 0,   -943.43, -3119.69,   49.05, 4.468, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Myzrael (Arathi)
(310007,  2937, 0,  -2405.19, -4172.46,   -7.06, 0.977, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Dagun (Wetlands)
(310008,  2707, 0,   -407.00, -2862.00,   77.31, 5.870, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Shadra (Hinterlands)
(310009,  6239, 0,    332.82, -1473.05,   42.27, 4.000, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Cyclonian (Alterac)
(310010,  8391, 0,  -6455.00, -1260.00,  180.50, 3.543, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Lathoric (Searing Gorge, by Obsidion)
(310011,  7664, 0, -11234.40, -2842.68,  157.92, 1.420, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Razelikh (Blasted Lands)
(310012, 12339, 0,   1629.34, -5492.59,  100.73, 1.088, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Demetria (EPL)
-- Kalimdor
(310013, 10882, 1,  -5002.89, -2103.46,   83.81, 1.365, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Arikara (Thousand Needles)
(310014, 10581, 1,  -5443.85, -2399.18,   89.33, 5.585, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Young Arikara
(310015,  9684, 1,  -7201.95, -2354.85, -225.15, 4.847, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Lar'korwi (Un'Goro)
(310016,  8075, 1,  -2764.68,  2623.21,   70.42, 2.336, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Edana Hatetalon (Feralas)
(310017,  5402, 1,  -1129.90,  2896.08,  195.91, 3.150, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Khan Hratha (Desolace)
(310018, 10373, 1,   6891.92,  -477.76,   44.40, 3.805, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Xabraxxis (Ashenvale)
(310019,  9456, 1,   -297.62, -1839.22,   93.88, 4.817, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Warlord Krom'zar (Barrens)
(310020,  9453, 1,  -8179.46, -5169.84,    3.99, 1.344, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Aquementas (Tanaris)
(310021, 10737, 1,   8072.38, -3833.81,  690.03, 4.560, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Shy-Rotam (Winterspring)
(310022, 15623, 1,   6724.96, -5275.13,  778.23, 1.820, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Xandivious (Winterspring)
(310023, 14435, 1,  -6242.95,  1715.08,    4.34, 0.593, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Prince Thunderaan (Silithus)
(310024, 14467, 1,   5780.34,  -964.84,  412.69, 0.000, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10), -- Kroshius (Felwood remains)
(310025,  4490, 1,  -5484.00, -1625.00,   11.00, 0.000, 300, 300, 0, 100, 100, 0, 0, 0, 0, 10); -- Grenka (Roguefeather Den)

-- Bangalash already has a world spawn; keep it short for solo.
UPDATE `creature` SET `spawntimesecsmin` = 300, `spawntimesecsmax` = 300 WHERE `id` = 731;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
