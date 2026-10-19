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

CREATE FUNCTION test_dsg_buffer_layout() RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION test_dsg_boot_id() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C;
REVOKE ALL ON FUNCTION test_dsg_buffer_layout() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_boot_id() FROM PUBLIC;

CREATE FUNCTION test_dsg_exclusive_available() RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION test_dsg_access_error() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION test_dsg_stale_hint() RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;
REVOKE ALL ON FUNCTION test_dsg_exclusive_available() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_access_error() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_stale_hint() FROM PUBLIC;

CREATE FUNCTION test_dsg_grow(bigint, bigint, bigint, integer, integer DEFAULT 1000, boolean DEFAULT false)
RETURNS text AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_grow_snapshot() RETURNS text AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION test_dsg_grow_verify() RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C;
REVOKE ALL ON FUNCTION test_dsg_grow(bigint, bigint, bigint, integer, integer, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_grow_snapshot() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_grow_verify() FROM PUBLIC;

-- 仅供独立临时集群验证分配栅栏、真实 pin 等待和释放后的访问边界。
CREATE FUNCTION test_dsg_allocation_limit(integer) RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_relation_below(regclass, integer) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_pin(regclass, integer, text) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_pin_control(boolean) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_unpin_probe() RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;
REVOKE ALL ON FUNCTION test_dsg_allocation_limit(integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_relation_below(regclass, integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_pin(regclass, integer, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_pin_control(boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION test_dsg_unpin_probe() FROM PUBLIC;

CREATE FUNCTION test_dsg_mapping_free(regclass) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_mapping_free(regclass) FROM PUBLIC;

CREATE FUNCTION test_dsg_checkpoint_control(text) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_checkpoint_control(text) FROM PUBLIC;
