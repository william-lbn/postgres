CREATE OR REPLACE FUNCTION pg17_dsm_cycle() RETURNS boolean AS '/evidence/shim17.so','pg17_dsm_cycle' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_snapshot() RETURNS text AS '/evidence/shim17.so','pg17_dsm_snapshot' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_hold(bigint) RETURNS bigint AS '/evidence/shim17.so','pg17_dsm_hold' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_release(bigint) RETURNS boolean AS '/evidence/shim17.so','pg17_dsm_release' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_pgstat_area() RETURNS void AS '/evidence/shim17.so','pg17_dsm_pgstat_area' LANGUAGE C;
SELECT CASE WHEN pg17_dsm_cycle() THEN 'PG17_STANDALONE_ACTUAL_DSM_OK' ELSE 'FAIL' END;
SELECT pg17_dsm_snapshot();
