DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261114140000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261114140000');
-- Add your query below.

-- The player's own experience rate (`.xprate`), times the server's rate:
-- 0 stops experience, 1 takes all of it. No row means 1.
CREATE TABLE IF NOT EXISTS `character_xp_rate` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `rate` float NOT NULL DEFAULT '1',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
