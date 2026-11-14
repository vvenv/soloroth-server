DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261114160000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261114160000');
-- Add your query below.

-- A character's companions (DungeonBotMgr): the saved party bot that stands
-- for one roster member (`slot`, its index in the roster) for that character.
CREATE TABLE IF NOT EXISTS `character_companions` (
  `owner_guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `slot` tinyint(3) unsigned NOT NULL COMMENT 'Roster index',
  `bot_guid` int(11) unsigned NOT NULL COMMENT 'The companion''s character guid',
  PRIMARY KEY (`owner_guid`, `slot`),
  UNIQUE KEY `bot_guid` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
