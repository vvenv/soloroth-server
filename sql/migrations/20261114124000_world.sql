DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20261114124000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20261114124000');
-- Add your query below.

-- Fix zhCN mangos_string: game "character" was mistranslated as 字符 (glyph)
-- instead of 角色 (player character). Leave letter/length 字符 alone.
UPDATE `mangos_string` SET `content_loc4`='没有选择角色。' WHERE `entry`=116;
UPDATE `mangos_string` SET `content_loc4`='角色 %s 票已删除。' WHERE `entry`=295;
UPDATE `mangos_string` SET `content_loc4`='角色转储失败！' WHERE `entry`=482;
UPDATE `mangos_string` SET `content_loc4`='找不到角色 %s 的帐户' WHERE `entry`=809;
UPDATE `mangos_string` SET `content_loc4`='找不到角色。' WHERE `entry`=1019;
UPDATE `mangos_string` SET `content_loc4`='恢复以下角色：' WHERE `entry`=1020;
UPDATE `mangos_string` SET `content_loc4`='删除以下角色：' WHERE `entry`=1021;
UPDATE `mangos_string` SET `content_loc4`='错误：如果您只选择了一个角色，则只能分配一个新名称！' WHERE `entry`=1022;
UPDATE `mangos_string` SET `content_loc4`='帐户 %s (%u) 具有最大允许角色数（客户端限制）' WHERE `entry`=1113;
UPDATE `mangos_string` SET `content_loc4`='角色 GUID 无效！' WHERE `entry`=1116;
UPDATE `mangos_string` SET `content_loc4`='角色 guid %u 正在使用中！' WHERE `entry`=1117;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
