DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261119013701');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261119013701');
-- Add your query below.

-- Last in-dungeon position so re-entry (entrance or /gd) resumes there.
-- instance must still match the character bind; a reset clears the row.
CREATE TABLE IF NOT EXISTS `character_dungeon_resume` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `map` smallint(5) unsigned NOT NULL,
  `instance` int(11) unsigned NOT NULL,
  `position_x` float NOT NULL,
  `position_y` float NOT NULL,
  `position_z` float NOT NULL,
  `orientation` float NOT NULL,
  PRIMARY KEY (`guid`, `map`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
