DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261115220000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261115220000');
-- Add your query below.

-- Quests are no longer a per-character switch. Kill progress stays in
-- character_noquest_kills.
DROP TABLE IF EXISTS `character_noquest`;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
