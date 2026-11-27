DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261127160000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261127160000');
-- Add your query below.

-- Soloroth: reverse 20261116143000 bag/quiver ×2 (cap 36). Vanilla max
-- before the double was 18; values that hit the 36 cap restore to 18.
UPDATE `item_template`
SET `container_slots` = IF(`container_slots` >= 36, 18, `container_slots` DIV 2)
WHERE `class` IN (1, 11) AND `container_slots` > 0;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
