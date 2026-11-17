DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261117150000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261117150000');
-- Add your query below.

-- Soloroth: Humar the Pridelord (轶事「狮王修玛」) already has a world
-- spawn under the tree NW of Ratchet; vanilla's ~15 h timer makes him
-- effectively uncampable in solo play. Shorten it so the kill is reachable
-- without any quest summon (he never needed one).
UPDATE `creature`
SET `spawntimesecsmin` = 300,
    `spawntimesecsmax` = 300
WHERE `id` = 5828;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
