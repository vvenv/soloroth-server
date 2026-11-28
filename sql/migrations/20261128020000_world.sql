DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261128020000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261128020000');
-- Add your query below.

-- Soloroth: clams stack to 20 (Wrath-era QoL). Opening still consumes one
-- per click (LootHandler DoLootRelease DestroyItemCount).
UPDATE `item_template` SET `stackable` = 20
WHERE `entry` IN (
  5523,  -- Small Barnacled Clam
  5524,  -- Thick-shelled Clam
  7973,  -- Big-mouth Clam
  15874  -- Soft-shelled Clam
);

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
