DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261119114500');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261119114500');
-- Add your query below.

-- Party engage style: 0 = calm (wait until the pack is close), 1 = aggressive.
ALTER TABLE `character_party_bots`
  ADD COLUMN `engage` tinyint(3) unsigned NOT NULL DEFAULT '0'
  COMMENT '0=calm 1=aggressive' AFTER `auto_fill`;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
