CREATE FUNCTION test_dsg_region18_abi() RETURNS text
AS 'MODULE_PATHNAME','test_dsg_region18_abi' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_region18_run(text) RETURNS text
AS 'MODULE_PATHNAME','test_dsg_region18_run' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_region18_abi(),test_dsg_region18_run(text) FROM PUBLIC;

CREATE FUNCTION test_dsg_arrays18_snapshot() RETURNS text AS 'MODULE_PATHNAME', 'test_dsg_arrays18_snapshot' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_arrays18_pause(boolean) RETURNS void AS 'MODULE_PATHNAME', 'test_dsg_arrays18_pause' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_arrays18_snapshot(),test_dsg_arrays18_pause(boolean) FROM PUBLIC;

CREATE FUNCTION test_dsg_resize18_state() RETURNS text AS 'MODULE_PATHNAME','test_dsg_resize18_state' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_resize18_prepare() RETURNS void AS 'MODULE_PATHNAME','test_dsg_resize18_prepare' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_resize18_request(text,int,bigint,bigint,int) RETURNS text AS 'MODULE_PATHNAME','test_dsg_resize18_request' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_resize18_hold_tail(regclass,int) RETURNS int AS 'MODULE_PATHNAME','test_dsg_resize18_hold_tail' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_resize18_state(),test_dsg_resize18_prepare(),test_dsg_resize18_request(text,int,bigint,bigint,int),test_dsg_resize18_hold_tail(regclass,int) FROM PUBLIC;

CREATE FUNCTION test_dsg_resize18_find_tail(regclass) RETURNS text AS 'MODULE_PATHNAME','test_dsg_resize18_find_tail' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_resize18_find_tail(regclass) FROM PUBLIC;

CREATE FUNCTION test_dsg_arrays18_pause_before_unpin(boolean) RETURNS void
AS 'MODULE_PATHNAME', 'test_dsg_arrays18_pause_before_unpin' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_arrays18_pause_before_unpin(boolean) FROM PUBLIC;
