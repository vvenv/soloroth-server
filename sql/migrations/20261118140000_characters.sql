DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261118140000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261118140000');
-- Add your query below.

-- How many times this character has killed a wing's last boss (DungeonProgress).
CREATE TABLE IF NOT EXISTS `character_dungeon_clear` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `map` smallint(5) unsigned NOT NULL,
  `wing` tinyint(3) unsigned NOT NULL,
  `clears` int(11) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`, `map`, `wing`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Bosses down in one instance. A new instance id (a reset) starts empty.
CREATE TABLE IF NOT EXISTS `character_dungeon_boss` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `instance` int(11) unsigned NOT NULL,
  `entry` int(11) unsigned NOT NULL COMMENT 'creature entry',
  PRIMARY KEY (`guid`, `instance`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
