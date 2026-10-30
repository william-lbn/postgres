CREATE FUNCTION test_dsg_region18_abi() RETURNS text
AS 'MODULE_PATHNAME','test_dsg_region18_abi' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_region18_run(text) RETURNS text
AS 'MODULE_PATHNAME','test_dsg_region18_run' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_region18_abi(),test_dsg_region18_run(text) FROM PUBLIC;
