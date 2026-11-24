DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261124031139');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261124031139');
-- Add your query below.

-- Soloroth: no quest log, so Artisan Cooking / First Aid / Fishing (quest-gated in
-- 1.12) are sold as books next to the Expert books, same path as Expert already uses.
DELETE FROM `npc_vendor` WHERE `item` IN (16073, 16082, 16085)
  AND `entry` IN (3955, 12033, 2626, 2805, 13476);
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `itemflags`, `condition_id`) VALUES
(3955, 30, 16073, 0, 0, 0, 0),   -- Shandrina: Artisan Cookbook
(12033, 10, 16073, 0, 0, 0, 0),  -- Wulan: Artisan Cookbook
(2626, 6, 16082, 0, 0, 0, 0),    -- Old Man Heming: Artisan Fishing
(2805, 21, 16085, 0, 0, 0, 0),   -- Deneb Walker: Artisan First Aid
(13476, 325, 16085, 0, 0, 0, 0); -- Balai Lok'Wein: Artisan First Aid

-- Stock zhCN locale for 16085 wrongly says fishing; match Expert First Aid wording.
UPDATE `locales_item` SET `name_loc4`='高级急救教材 - 自救手册' WHERE `entry`=16085;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
