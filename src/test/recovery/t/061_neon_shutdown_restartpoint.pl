# Copyright (c) 2026, PostgreSQL Global Development Group

# Neon的主库关机前阶段不适用于备库restartpoint。备库必须在推进控制文件
# redo之前写回重放产生的脏Buffer；否则热缓存查询成功也可能在快关机后丢表。
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
shared_buffers = '16MB'
bgwriter_lru_maxpages = 0
checkpoint_timeout = '1h'
});
$primary->start;
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
my $initial_redo = $standby->safe_psql('postgres',
	'SELECT redo_lsn FROM pg_control_checkpoint()');

# 在basebackup之后创建真实catalog/heap/index，工作集远小于池且禁后台驱逐，
# 避免测试因偶然提前刷盘而虚绿。主库checkpoint为备库提供新的安全restartpoint。
$primary->safe_psql('postgres', qq{
CREATE TABLE restartpoint_data(i integer PRIMARY KEY, payload text);
INSERT INTO restartpoint_data
  SELECT i, repeat(md5(i::text), 20) FROM generate_series(1, 100) i;
});
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
my $checksum = q{SELECT count(*), sum(i), sum(octet_length(payload)),
                       sum(hashtextextended(payload, 0)) FROM restartpoint_data};
my $expected = $primary->safe_psql('postgres', $checksum);
is($standby->safe_psql('postgres', $checksum), $expected,
	'replayed catalog and data are visible before shutdown');

$standby->stop('fast');
$standby->start;
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'), 't',
	'node remains a physical standby after restart');
is($standby->safe_psql('postgres',
	"SELECT redo_lsn > '$initial_redo'::pg_lsn FROM pg_control_checkpoint()"), 't',
	'shutdown restartpoint advanced the persisted redo position');
is($standby->safe_psql('postgres', $checksum), $expected,
	'catalog, heap and payload survive shutdown restartpoint');

# 新的独立Backend查询index；不能只依靠原会话的relcache或热heap页证明完整。
is($standby->safe_psql('postgres',
	q{SET enable_seqscan=off;
      SELECT payload=repeat(md5('50'),20) FROM restartpoint_data WHERE i=50}), 't',
	'index and tuple remain usable after restart');
$standby->stop;
$primary->stop;
done_testing();
