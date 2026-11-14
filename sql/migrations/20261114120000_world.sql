DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261114120000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261114120000');
-- Add your query below.

-- Undercity canals (1497) and Ruins of Lordaeron outside (153) had no fishing
-- base skill, so zone_skill stayed 0 and low-skill characters almost always got
-- FISH_ESCAPED on a bobber click. Capitals use -20 (Stormwind 1519, Orgrimmar
-- 1637, …); match that here.
INSERT INTO `skill_fishing_base_level` (`entry`, `skill`) VALUES
(1497, -20),
(153, -20);

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
