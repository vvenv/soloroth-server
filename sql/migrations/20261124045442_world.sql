DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261124045442');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261124045442');
-- Add your query below.

-- Soloroth: Artisan Cookbook also at Dirge (Clamlette quest NPC) and capital
-- cooking suppliers — Expert-book vendors alone are easy to miss at 225.
DELETE FROM `npc_vendor` WHERE `item`=16073 AND `entry` IN (
  8125, 3027, 3400, 3957, 4223, 4265, 4553, 5160, 5483, 3085, 14738
);
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `itemflags`, `condition_id`) VALUES
(8125, 14, 16073, 0, 0, 0, 0),   -- Dirge Quikcleave, Gadgetzan
(3027, 9, 16073, 0, 0, 0, 0),    -- Naal Mistrunner, Thunder Bluff
(3400, 7, 16073, 0, 0, 0, 0),    -- Xen'to, Orgrimmar
(3957, 7, 16073, 0, 0, 0, 0),    -- Jainay Featherbreeze, Darnassus
(4223, 7, 16073, 0, 0, 0, 0),    -- Fyldan, Darnassus
(4265, 9, 16073, 0, 0, 0, 0),    -- Nyoma, Dolanaar
(4553, 9, 16073, 0, 0, 0, 0),    -- Ronald Burch, Undercity
(5160, 7, 16073, 0, 0, 0, 0),    -- Emrul Riknussun, Ironforge
(5483, 7, 16073, 0, 0, 0, 0),    -- Erika Tate, Stormwind
(3085, 7, 16073, 0, 0, 0, 0),    -- Gloria Femmel, Redridge
(14738, 7, 16073, 0, 0, 0, 0);   -- Otho Moji'ko, Revantusk

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
