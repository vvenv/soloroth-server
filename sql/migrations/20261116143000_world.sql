DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261116143000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261116143000');
-- Add your query below.

-- Soloroth: bag capacity is twice the template, capped at the 36 slots a
-- container object can carry. The default backpack is not an item; the
-- server and client add its extra 16 slots beside this.
UPDATE `item_template`
SET `container_slots` = LEAST(`container_slots` * 2, 36)
WHERE `class` IN (1, 11) AND `container_slots` > 0 AND `container_slots` < 36;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
