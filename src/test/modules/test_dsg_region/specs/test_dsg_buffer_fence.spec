# 持 mappingX 的归还者，必须在另一个 Backend 持 accessX 时完成归还。
# 先确认两个真实暂停点都已进入，再依次唤醒，固定 mapping/access 锁交错。
setup
{
    CREATE EXTENSION test_dsg_region;
    CREATE EXTENSION injection_points;
    CREATE TABLE dsg_fence_data AS SELECT i FROM generate_series(1, 100) i;
    SELECT injection_points_attach('dsg-test-mapping-before-free', 'wait');
    SELECT injection_points_attach('dsg-test-fence-before-publish', 'wait');
}
teardown
{
    SELECT injection_points_detach('dsg-test-mapping-before-free');
    SELECT injection_points_detach('dsg-test-fence-before-publish');
    DROP TABLE dsg_fence_data;
    DROP EXTENSION injection_points;
    DROP EXTENSION test_dsg_region;
}

session returner
setup { PREPARE return_buffer AS SELECT test_dsg_mapping_free('dsg_fence_data'); }
step return_buffer { EXECUTE return_buffer; }

session fencer
setup { PREPARE fence AS SELECT test_dsg_allocation_limit(256) LIKE 'low=256 high=%' AS fenced; }
step fence { EXECUTE fence; }

session coordinator
setup
{
    PREPARE wake_returner AS SELECT injection_points_wakeup('dsg-test-mapping-before-free');
    PREPARE wake_fence AS SELECT injection_points_wakeup('dsg-test-fence-before-publish');
}
step wake_returner { EXECUTE wake_returner; }
step wake_fence { EXECUTE wake_fence; }
step verify { SELECT count(*), sum(i) FROM dsg_fence_data; }

permutation return_buffer fence wake_returner(return_buffer) wake_fence(fence) verify
