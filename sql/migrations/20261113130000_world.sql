DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261113130000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261113130000');
-- Add your query below.

-- Solo: new characters start with four 8-slot bags (Brown Leather Satchel)
-- so primary professions do not instantly fill the backpack.
INSERT INTO `playercreateinfo_item` (`race`, `class`, `itemid`, `amount`)
SELECT `race`, `class`, 4498, 4 FROM `playercreateinfo`;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
