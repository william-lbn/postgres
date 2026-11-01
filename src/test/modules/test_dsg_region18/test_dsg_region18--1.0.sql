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

CREATE FUNCTION test_dsg_birth_arrays18() RETURNS text
AS 'MODULE_PATHNAME','test_dsg_birth_arrays18' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_birth_arrays18() FROM PUBLIC;

CREATE FUNCTION test_dsg_birth_filter18() RETURNS text AS 'MODULE_PATHNAME','test_dsg_birth_filter18' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_filter_probe18(text) RETURNS text AS 'MODULE_PATHNAME','test_dsg_filter_probe18' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_birth_filter18(),test_dsg_filter_probe18(text) FROM PUBLIC;

CREATE FUNCTION test_dsg_work18_snapshot() RETURNS text AS 'MODULE_PATHNAME','test_dsg_work18_snapshot' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_work18_admission(boolean) RETURNS void AS 'MODULE_PATHNAME','test_dsg_work18_admission' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_work18_snapshot(),test_dsg_work18_admission(boolean) FROM PUBLIC;

CREATE FUNCTION test_dsg_connection18_snapshot() RETURNS text AS 'MODULE_PATHNAME','test_dsg_connection18_snapshot' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_connection18_snapshot() FROM PUBLIC;

CREATE FUNCTION test_dsg_prepared18_snapshot() RETURNS text AS 'MODULE_PATHNAME','test_dsg_prepared18_snapshot' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_prepared18_snapshot() FROM PUBLIC;

CREATE FUNCTION test_dsg_dsm18_snapshot() RETURNS text AS 'MODULE_PATHNAME','test_dsg_dsm18_snapshot' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_dsm18_create(bigint,boolean) RETURNS text AS 'MODULE_PATHNAME','test_dsg_dsm18_create' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_dsm18_touch(bigint,boolean) RETURNS text AS 'MODULE_PATHNAME','test_dsg_dsm18_touch' LANGUAGE C STRICT;
CREATE FUNCTION test_dsg_dsm18_collision() RETURNS text AS 'MODULE_PATHNAME','test_dsg_dsm18_collision' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_dsm18_snapshot(),test_dsg_dsm18_create(bigint,boolean),test_dsg_dsm18_touch(bigint,boolean),test_dsg_dsm18_collision() FROM PUBLIC;

CREATE FUNCTION test_dsg_dsm18_pgstat_area() RETURNS void AS 'MODULE_PATHNAME','test_dsg_dsm18_pgstat_area' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_dsg_dsm18_pgstat_area() FROM PUBLIC;
