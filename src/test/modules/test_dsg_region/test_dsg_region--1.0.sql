\echo Use "CREATE EXTENSION test_dsg_region" to load this file. \quit

CREATE FUNCTION test_dsg_region(text) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- 测试函数能触及真实地址空间，只允许显式安装扩展的管理员调用。
REVOKE ALL ON FUNCTION test_dsg_region(text) FROM PUBLIC;

CREATE FUNCTION test_dsg_shared_write(integer) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_shared_read() RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;
REVOKE ALL ON FUNCTION test_dsg_shared_write(integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_shared_read() FROM PUBLIC;
