DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261114230000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261114230000');
-- Add your query below.

-- Characters in no-quest mode (`.noquest on`): a row means on.
CREATE TABLE IF NOT EXISTS `character_noquest` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- What a no-quest character has toward each quest it never sees: kills per
-- objective, and quest items counted from loot instead of handed over.
CREATE TABLE IF NOT EXISTS `character_noquest_kills` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `quest` int(11) unsigned NOT NULL COMMENT 'Quest id',
  `count1` int(11) unsigned NOT NULL DEFAULT '0',
  `count2` int(11) unsigned NOT NULL DEFAULT '0',
  `count3` int(11) unsigned NOT NULL DEFAULT '0',
  `count4` int(11) unsigned NOT NULL DEFAULT '0',
  `item1` int(11) unsigned NOT NULL DEFAULT '0',
  `item2` int(11) unsigned NOT NULL DEFAULT '0',
  `item3` int(11) unsigned NOT NULL DEFAULT '0',
  `item4` int(11) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`, `quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
