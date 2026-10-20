
# Copyright (c) 2023-2024, PostgreSQL Global Development Group

# Test logical replication slots are always flushed to disk during a shutdown
# checkpoint.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub compare_confirmed_flush
{
	my ($node, $confirmed_flush_from_log, $saved_confirmed_flush) = @_;

	# Neon在PreCheckPointGuts保存slot并为其写入neon-file WAL，所以最终
	# checkpoint位置会在已确认LSN之后。必须精确验证重启后恢复的是关机时
	# 最后保存的confirmed_flush，不能把正确条件降成“任意较旧LSN都可以”。
	is($confirmed_flush_from_log, $saved_confirmed_flush,
		"slot confirmed_flush survives shutdown without retreat");

	# Fetch Latest checkpoint location from the control file
	my ($stdout, $stderr) =
	  run_command([ 'pg_controldata', $node->data_dir ]);
	my @control_data = split("\n", $stdout);
	my $latest_checkpoint = undef;
	foreach (@control_data)
	{
		if ($_ =~ /^Latest checkpoint location:\s*(.*)$/mg)
		{
			$latest_checkpoint = $1;
			last;
		}
	}
	die "Latest checkpoint location not found in control file\n"
	  unless defined($latest_checkpoint);

	# 额外验证该确认点不越过已持久的checkpoint。比较pg_lsn而非字符串排序。
	is($node->safe_psql('postgres',
		"SELECT '$confirmed_flush_from_log'::pg_lsn <= '$latest_checkpoint'::pg_lsn"), 't',
		"slot confirmed_flush does not exceed the durable shutdown checkpoint");

	return;
}

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('pub');
$node_publisher->init(allows_streaming => 'logical');
# Avoid checkpoint during the test, otherwise, the latest checkpoint location
# will change.
$node_publisher->append_conf(
	'postgresql.conf', q{
checkpoint_timeout = 1h
autovacuum = off
});
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('sub');
$node_subscriber->init;
$node_subscriber->start;

# Create tables
$node_publisher->safe_psql('postgres', "CREATE TABLE test_tbl (id int)");
$node_subscriber->safe_psql('postgres', "CREATE TABLE test_tbl (id int)");

# To avoid a shutdown checkpoint WAL record (that gets generated as part of
# the publisher restart below) falling into a new page, advance the WAL
# segment. Otherwise, the confirmed_flush_lsn and shutdown_checkpoint
# location won't match.
$node_publisher->advance_wal(1);

# Insert some data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tbl VALUES (generate_series(1, 5));");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher_connstr' PUBLICATION pub"
);

$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub');

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM test_tbl");

is($result, qq(5), "check initial copy was done");

my $offset = -s $node_publisher->logfile;

# Note: Don't insert any data on the publisher that may cause the shutdown
# checkpoint to fall into a new WAL file. See the comments atop advance_wal()
# above.

# Restart the publisher to ensure that the slot will be flushed if required
$node_publisher->restart();

# Wait until the walsender creates decoding context
$node_publisher->wait_for_log(
	qr/Streaming transactions committing after ([A-F0-9]+\/[A-F0-9]+), reading WAL from ([A-F0-9]+\/[A-F0-9]+)./,
	$offset);

# Extract confirmed_flush from the logfile
my $log_contents = slurp_file($node_publisher->logfile, $offset);
$log_contents =~
  qr/Streaming transactions committing after ([A-F0-9]+\/[A-F0-9]+), reading WAL from ([A-F0-9]+\/[A-F0-9]+)./
  or die "could not get confirmed_flush_lsn";

my $confirmed_flush = $1;

# 只取本次关机阶段的最后一次真实保存日志，不能拿重启后的新保存掩盖回退。
# 未强制刷新的旧缺陷若重现，保存日志缺失或新解码起点不匹配都会失败。
my ($shutdown_log) = split(/database system is shut down/, $log_contents, 2);
my @saved = $shutdown_log =~
  /Save replication slot at pg_replslot\/sub\/state restart_lsn=[A-F0-9]+\/[A-F0-9]+ confirmed_flush=([A-F0-9]+\/[A-F0-9]+)/g;
ok(scalar @saved > 0, 'logical slot was explicitly saved during shutdown');
compare_confirmed_flush($node_publisher, $confirmed_flush,
	@saved ? $saved[-1] : '0/0');

done_testing();
