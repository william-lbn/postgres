# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# 中文说明：currtid2 直接访问 relation，不经过普通 SELECT 的计划阶段。因此必须用
# 真实物理备库验证恢复期拒绝发生在访问 unlogged 存储之前；仅检查源码/错误字符串
# 不能证明已绕开“文件不存在”的底层错误。主库、永久表和临时表正常路径同时保留。
my $primary = PostgreSQL::Test::Cluster->new('currtid_primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->safe_psql('postgres', q{
    CREATE TABLE currtid_logged(a int);
    INSERT INTO currtid_logged VALUES (1);
    CREATE UNLOGGED TABLE currtid_unlogged(a int);
    INSERT INTO currtid_unlogged VALUES (1);
    CREATE VIEW currtid_unlogged_view AS SELECT ctid, a FROM currtid_unlogged;
});
for my $table ('currtid_logged', 'currtid_unlogged')
{
    is($primary->safe_psql('postgres',
        "SELECT currtid2('$table', '(0,1)'::tid)"),
        '(0,1)', "primary $table remains accessible");
}
is($primary->safe_psql('postgres', q{
    CREATE TEMP TABLE currtid_temp(a int);
    INSERT INTO currtid_temp VALUES (1);
    SELECT currtid2('currtid_temp', '(0,1)'::tid);
}), '(0,1)', 'primary temporary relation remains accessible');

# 主备由一次真实 pg_basebackup 建立，不通过创建同名空表模拟恢复期。Cluster 管理
# 临时目录和子进程；测试结束或断言失败均由框架收尾，不接触用户数据库。
# 本批其余两个内核修复同时验证：空 tsquery 的二进制收发不能触发断言；
# bigint 阶段号不能被截成 int32 后误认为一个合法 btree 阶段。服务端 COPY 文件只
# 放在框架的独占临时目录，转义 SQL 字符串，不使用任何预置用户数据或持久目录。
my $copy = $primary->basedir . '/empty-tsquery.bin';
$copy =~ s/'/''/g;
is($primary->safe_psql('postgres', qq{
    COPY (SELECT ''::tsquery AS q) TO '$copy' (FORMAT binary);
    CREATE TABLE empty_tsquery_roundtrip(q tsquery);
    COPY empty_tsquery_roundtrip FROM '$copy' (FORMAT binary);
    SELECT q = ''::tsquery FROM empty_tsquery_roundtrip;
}), 't', 'empty tsquery survives binary COPY round trip');
is($primary->safe_psql('postgres', q{
    SELECT pg_indexam_progress_phasename(
        (SELECT oid FROM pg_am WHERE amname = 'btree'), 4294967297::bigint) IS NULL;
}), 't', 'index progress phase keeps all bigint bits');

$primary->backup('currtid_backup');
my $standby = PostgreSQL::Test::Cluster->new('currtid_standby');
$standby->init_from_backup($primary, 'currtid_backup', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
    't', 'actual physical standby is in recovery');
is($standby->safe_psql('postgres',
    "SELECT currtid2('currtid_logged', '(0,1)'::tid)"),
    '(0,1)', 'standby permanent relation remains accessible');

# 同时核对退出状态、SQLSTATE 与语义错误，防止权限错误、未找到表或崩溃被误计为
# 正确拒绝。视图递归也必须到达同一防护。一次错误后备库仍能执行普通查询。
for my $table ('currtid_unlogged', 'currtid_unlogged_view')
{
    my ($ret, $out, $err) = $standby->psql('postgres',
        "SELECT currtid2('$table', '(0,1)'::tid)",
        extra_params => ['-v', 'VERBOSITY=verbose']);
    is($ret, 3, "standby $table fails with a SQL error");
    is($out, '', "standby $table returns no tuple");
    like($err, qr/ERROR:\s+0A000: cannot access temporary or unlogged relations during recovery/,
        "standby $table uses feature-not-supported before storage access");
}
is($standby->safe_psql('postgres', 'SELECT 1'), '1',
    'standby remains usable after rejected currtid calls');
$standby->stop;
$primary->stop;
done_testing();
