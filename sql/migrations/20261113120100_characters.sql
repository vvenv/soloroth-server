DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261113120100');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261113120100');
-- Add your query below.

-- Achievements a character earned (AchievementMgr), with the unix time.
CREATE TABLE IF NOT EXISTS `character_achievement` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `achievement` int(11) unsigned NOT NULL,
  `date` int(11) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`, `achievement`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Progress the character cannot tell again at login (boss kills), by
-- achievement_criteria id.
CREATE TABLE IF NOT EXISTS `character_achievement_progress` (
  `guid` int(11) unsigned NOT NULL COMMENT 'Character guid',
  `criteria` int(11) unsigned NOT NULL,
  `counter` int(11) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`, `criteria`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
