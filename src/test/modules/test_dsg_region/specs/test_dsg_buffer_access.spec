# 暂停真实 pg_buffercache 扫描，确认另一 Backend 不能取得回收独占锁。
# 分别验证正常唤醒、query cancel 及连接终止后读侧锁均释放。
setup
{
    CREATE EXTENSION test_dsg_region;
    CREATE EXTENSION pg_buffercache;
    CREATE EXTENSION injection_points;
    SELECT injection_points_attach('dsg-buffercache-before-scan', 'wait');
}
teardown
{
    SELECT injection_points_detach('dsg-buffercache-before-scan');
    DROP EXTENSION injection_points;
    DROP EXTENSION pg_buffercache;
    DROP EXTENSION test_dsg_region;
}

session reader
setup { SET application_name = 'dsg-array-scan-test'; }
step scan { SELECT count(*) > 0 AS scanned FROM pg_buffercache; }

session coordinator
step denied { SELECT test_dsg_exclusive_available(); }
step wake { SELECT injection_points_wakeup('dsg-buffercache-before-scan'); }
step cancel_reader
{
    SELECT pg_cancel_backend(pid) FROM pg_stat_activity
    WHERE application_name = 'dsg-array-scan-test';
}
step available { SELECT test_dsg_exclusive_available(); }
step terminate_reader
{
    SELECT pg_terminate_backend(pid) FROM pg_stat_activity
    WHERE application_name = 'dsg-array-scan-test';
}

permutation scan denied wake(scan) available
permutation scan denied cancel_reader(scan) available
# 终止连接必须是最后一种排列，isolationtester 不会重连此 reader。
permutation scan denied terminate_reader(scan) available
