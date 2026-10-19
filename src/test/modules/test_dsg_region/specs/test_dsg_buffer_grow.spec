# 暂停真实扩容的发布前阶段，验证旧容量仍可见、竞争者有超时、取消
# 不发布容量。随后用新操作完成扩容，并验证注入错误保留未知分配预算。
setup
{
    CREATE EXTENSION test_dsg_region;
    CREATE EXTENSION injection_points;
    CREATE EXTENSION pg_buffercache;
    SELECT injection_points_attach('dsg-buffer-grow-before-publish', 'wait');
    SELECT injection_points_attach('dsg-buffercache-before-scan', 'wait');
}
teardown
{
    DROP EXTENSION injection_points;
    DROP EXTENSION pg_buffercache;
    DROP EXTENSION test_dsg_region;
}

session grower
setup { SET application_name = 'dsg-grow-test'; }
step grow { SELECT test_dsg_grow(1, 1, 1, 512, 60000); }

session observer
step preparing { SELECT test_dsg_grow_snapshot(); }
step competing { SELECT test_dsg_grow(2, 2, 1, 512, 20); }
step cancel_grow
{
    SELECT pg_cancel_backend(pid) FROM pg_stat_activity
    WHERE application_name = 'dsg-grow-test';
}
step denied { SELECT test_dsg_grow_snapshot(); }
step replay_denied { SELECT test_dsg_grow(1, 1, 1, 512); }
step detach { SELECT injection_points_detach('dsg-buffer-grow-before-publish'); }
step blocked_publish { SELECT test_dsg_grow(2, 2, 1, 512, 100); }
step publish_denied { SELECT test_dsg_grow_snapshot(); }
step wake_scan { SELECT injection_points_wakeup('dsg-buffercache-before-scan'); }
step detach_scan { SELECT injection_points_detach('dsg-buffercache-before-scan'); }
step retry { SELECT test_dsg_grow(3, 3, 1, 512); }
step inject_error { SELECT injection_points_attach('dsg-region-before-populate', 'error'); }
step failed { SELECT test_dsg_grow(4, 4, 2, 1024); }
step unknown { SELECT test_dsg_grow_snapshot(); }
step detach_error { SELECT injection_points_detach('dsg-region-before-populate'); }
step smaller_retry { SELECT test_dsg_grow(5, 5, 2, 768); }
step still_unknown { SELECT test_dsg_grow_snapshot(); }
step reconciled { SELECT test_dsg_grow(6, 6, 3, 1024); }
step known { SELECT test_dsg_grow_snapshot(); }
step inject_response_error { SELECT injection_points_attach('dsg-buffer-grow-after-publish', 'error'); }
step response_error { SELECT test_dsg_grow(7, 7, 4, 1280); }
step committed_without_response { SELECT test_dsg_grow_snapshot(); }
step detach_response_error { SELECT injection_points_detach('dsg-buffer-grow-after-publish'); }
step replay_committed { SELECT test_dsg_grow(7, 7, 4, 1280); }
step pause_response { SELECT injection_points_attach('dsg-buffer-grow-after-publish', 'wait'); }
step terminate_grower
{
    SELECT pg_terminate_backend(pid) FROM pg_stat_activity
    WHERE application_name = 'dsg-grow-response-test';
}
step committed_after_exit { SELECT test_dsg_grow_snapshot(); }
step replay_after_exit { SELECT test_dsg_grow(8, 8, 5, 1536); }
step detach_response_pause { SELECT injection_points_detach('dsg-buffer-grow-after-publish'); }

session scanner
step scan { SELECT count(*) > 0 AS scanned FROM pg_buffercache; }

session responder
setup { SET application_name = 'dsg-grow-response-test'; }
step grow_response { SELECT test_dsg_grow(8, 8, 5, 1536, 60000); }

permutation grow preparing competing cancel_grow(grow) denied replay_denied detach scan blocked_publish publish_denied wake_scan(scan) detach_scan retry inject_error failed unknown detach_error smaller_retry still_unknown reconciled known inject_response_error response_error committed_without_response detach_response_error replay_committed pause_response grow_response terminate_grower(grow_response) committed_after_exit replay_after_exit detach_response_pause
