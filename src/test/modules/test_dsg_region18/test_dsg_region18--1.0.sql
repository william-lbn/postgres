CREATE FUNCTION test_dsg_region18_abi() RETURNS text
AS 'MODULE_PATHNAME','test_dsg_region18_abi' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_region18_run(text) RETURNS text
AS 'MODULE_PATHNAME','test_dsg_region18_run' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_region18_abi(),test_dsg_region18_run(text) FROM PUBLIC;

CREATE FUNCTION test_dsg_arrays18_snapshot() RETURNS text AS 'MODULE_PATHNAME', 'test_dsg_arrays18_snapshot' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_arrays18_pause(boolean) RETURNS void AS 'MODULE_PATHNAME', 'test_dsg_arrays18_pause' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_arrays18_snapshot(),test_dsg_arrays18_pause(boolean) FROM PUBLIC;
