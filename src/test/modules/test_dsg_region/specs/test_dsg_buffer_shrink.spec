# 真实L/H两阶段收缩：发布L后竞争者不取得串行权，尾部按原生协议写净，
# H提交后业务数据完整，随后允许新revision重新扩容。不声明Mac物理完成。
setup
{
    CREATE EXTENSION test_dsg_region;
    CREATE EXTENSION injection_points;
    -- isolation组共享Postmaster，前面的Grow/fence可能留下更大H和revision。
    -- 从真实快照重新建立测试基线，不重写共享状态或假定每个spec都有新boot。
    CREATE TABLE shrink_baseline AS SELECT
        (s->>'generation')::bigint AS generation,
        (s->>'revision')::bigint AS revision,
        (s->>'high')::integer AS original
        FROM (SELECT test_dsg_shrink_snapshot()::json AS s) q;
    SELECT test_dsg_allocation_limit(original) FROM shrink_baseline;
    SELECT test_dsg_grow(revision+1, revision+1, generation, greatest(original,1024)) FROM shrink_baseline;
    TRUNCATE shrink_baseline;
    INSERT INTO shrink_baseline SELECT
        (s->>'generation')::bigint, (s->>'revision')::bigint, (s->>'high')::integer
        FROM (SELECT test_dsg_shrink_snapshot()::json AS s) q;
    CREATE TABLE shrink_data(i integer, pad text);
    ALTER TABLE shrink_data ALTER pad SET STORAGE PLAIN;
    INSERT INTO shrink_data SELECT i, repeat(md5(i::text),150) FROM generate_series(1,400) i;
    SELECT injection_points_attach('dsg-buffer-shrink-after-fence','wait');
}
teardown
{
    DROP TABLE shrink_data;
    DROP TABLE shrink_baseline;
    DROP EXTENSION injection_points;
    DROP EXTENSION test_dsg_region;
}

session shrinker
step shrink
{
    SELECT test_dsg_shrink(revision+1, revision+1, generation, 256, 60000)
        LIKE 'committed high=256 generation=% replay=false' AS shrink_ok FROM shrink_baseline;
}

session observer
step fenced
{
    SELECT (s->>'low')::integer = 256 AND (s->>'high')::integer = b.original
        AND (s->>'phase')::integer = 1 AS fenced
    FROM (SELECT test_dsg_shrink_snapshot()::json AS s) q CROSS JOIN shrink_baseline b;
}
step competing
{
    SELECT test_dsg_grow(revision+2, revision+2, generation, original, 20)
        LIKE 'denied high=% replay=false' AS busy FROM shrink_baseline;
}
step wake { SELECT injection_points_wakeup('dsg-buffer-shrink-after-fence'); }
step shrunk
{
    SELECT (s->>'low')::integer = 256 AND (s->>'high')::integer = 256
        AND (s->>'generation')::bigint = b.generation+1 AND (s->>'phase')::integer = 2 AS shrunk
    FROM (SELECT test_dsg_shrink_snapshot()::json AS s) q CROSS JOIN shrink_baseline b;
}
step data { SELECT count(*) = 400 AND sum(i) = 80200 AS data_ok FROM shrink_data; }
step detach { SELECT injection_points_detach('dsg-buffer-shrink-after-fence'); }
step regrow
{
    SELECT test_dsg_grow(revision+2, revision+2, generation+1, 768)
        LIKE 'committed high=768 generation=% replay=false' AS regrow_ok FROM shrink_baseline;
}

permutation shrink fenced competing wake(shrink) shrunk data detach regrow
