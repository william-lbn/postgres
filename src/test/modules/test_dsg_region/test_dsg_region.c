#include "postgres.h"

#include <unistd.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/dsg_region.h"
#include "storage/ipc.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_dsg_region);
PG_FUNCTION_INFO_V1(test_dsg_shared_write);
PG_FUNCTION_INFO_V1(test_dsg_shared_read);

void		_PG_init(void);

static DsgRegion inherited_region;

/*-
 * 测试映射在 Postmaster preload 阶段创建，两个隔离测试会话由 Postmaster
 * 分别 fork，验证的是实际继承同一 backing，而非某个连接的私有副本。
 * 无文件/持久数据；映射随各进程退出由操作系统回收。
 */
void
_PG_init(void)
{
	if (process_shared_preload_libraries_in_progress && !IsUnderPostmaster)
	{
		DsgRegionReserve(&inherited_region, (Size) sysconf(_SC_PAGESIZE));
		pg_atomic_init_u32((pg_atomic_uint32 *) inherited_region.address, 0);
	}
}

static pg_atomic_uint32 *
test_dsg_shared_value(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("DSGA provider tests require superuser")));
	if (inherited_region.address == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("test_dsg_region must be loaded via shared_preload_libraries")));
	return (pg_atomic_uint32 *) inherited_region.address;
}

Datum
test_dsg_shared_write(PG_FUNCTION_ARGS)
{
	pg_atomic_write_u32(test_dsg_shared_value(), (uint32) PG_GETARG_INT32(0));
	PG_RETURN_VOID();
}

Datum
test_dsg_shared_read(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) pg_atomic_read_u32(test_dsg_shared_value()));
}

static void
test_dsg_cleanup(int code, Datum arg)
{
	DsgRegionDestroy((DsgRegion *) DatumGetPointer(arg));
}

static void
test_dsg_check(bool condition, const char *message)
{
	if (!condition)
		elog(ERROR, "DSGA provider test failed: %s", message);
}

Datum
test_dsg_region(PG_FUNCTION_ARGS)
{
	char	   *scenario = text_to_cstring(PG_GETARG_TEXT_PP(0));
	DsgRegion  *region = palloc0(sizeof(DsgRegion));
	Size		page = (Size) sysconf(_SC_PAGESIZE);
	DsgMemoryResult result;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("DSGA provider tests require superuser")));

	DsgRegionReserve(region, 4 * page);
	PG_ENSURE_ERROR_CLEANUP(test_dsg_cleanup, PointerGetDatum(region));
	{
		if (strcmp(scenario, "populate_preserves_prefix") == 0)
		{
			Size		offset = page - 17;
			Size		i;

			memset(region->address, 0x5a, offset);
			result = DsgRegionPopulate(region, offset, page + 33);
			test_dsg_check(result.status == DSG_MEMORY_OK, "populate must succeed");
			test_dsg_check(!result.physical_release_verified, "populate is not release proof");
			for (i = 0; i < offset; i++)
				test_dsg_check(region->address[i] == 0x5a, "active boundary prefix changed");
		}
		else if (strcmp(scenario, "release_preserves_boundary_pages") == 0)
		{
			Size		i;

			result = DsgRegionPopulate(region, 0, 4 * page);
			test_dsg_check(result.status == DSG_MEMORY_OK, "initial populate failed");
			memset(region->address, 0x5a, 4 * page);
			result = DsgRegionRelease(region, page + 17, 2 * page + 10);
			for (i = 0; i < 2 * page; i++)
				test_dsg_check(region->address[i] == 0x5a, "left boundary page changed");
			for (i = 3 * page; i < 4 * page; i++)
				test_dsg_check(region->address[i] == 0x5a, "right boundary page changed");
			if (DsgRegionCanRelease())
			{
				test_dsg_check(result.status == DSG_MEMORY_OK, "release failed");
				test_dsg_check(result.physical_release_verified, "missing release proof");
				test_dsg_check(result.affected_offset == 2 * page &&
							   result.affected_bytes == page, "wrong physical release span");
				for (i = 2 * page; i < 3 * page; i++)
					test_dsg_check(region->address[i] == 0, "released shared bytes not cleared");
			}
			else
			{
				test_dsg_check(result.status == DSG_MEMORY_UNSUPPORTED, "must deny unsupported release");
				test_dsg_check(!result.physical_release_verified && result.affected_bytes == 0,
							   "unsupported provider invented physical release");
				for (i = 2 * page; i < 3 * page; i++)
					test_dsg_check(region->address[i] == 0x5a, "unsupported release modified memory");
			}
		}
		else if (strcmp(scenario, "partial_page_noop") == 0)
		{
			result = DsgRegionRelease(region, page + 1, page - 2);
			test_dsg_check(result.status == DSG_MEMORY_NOOP && result.affected_bytes == 0 &&
						   !result.physical_release_verified, "partial page was reclaimed");
		}
		else if (strcmp(scenario, "zero_at_end") == 0)
		{
			result = DsgRegionPopulate(region, 4 * page, 0);
			test_dsg_check(result.status == DSG_MEMORY_NOOP, "zero-length end populate failed");
			result = DsgRegionRelease(region, 4 * page, 0);
			test_dsg_check(result.status == DSG_MEMORY_NOOP, "zero-length end release failed");
		}
		else if (strcmp(scenario, "overflow") == 0)
			(void) DsgRegionPopulate(region, SIZE_MAX - 3, 8);
		else if (strcmp(scenario, "past_end") == 0)
			(void) DsgRegionRelease(region, 4 * page, 1);
		else if (strcmp(scenario, "reserve_twice") == 0)
			DsgRegionReserve(region, page);
		else if (strcmp(scenario, "reserve_zero") == 0)
		{
			DsgRegionDestroy(region);
			DsgRegionReserve(region, 0);
		}
		else if (strcmp(scenario, "reserve_overflow") == 0)
		{
			DsgRegionDestroy(region);
			DsgRegionReserve(region, SIZE_MAX);
		}
		else if (strcmp(scenario, "unmapped_populate") == 0)
		{
			DsgRegionDestroy(region);
			(void) DsgRegionPopulate(region, 0, 0);
		}
		else if (strcmp(scenario, "rounded_mapping_limit") == 0)
		{
			DsgRegionDestroy(region);
			DsgRegionReserve(region, page + 1);
			test_dsg_check(region->mapped_bytes == 2 * page &&
						   region->maximum_bytes == page + 1,
						   "mapping rounding changed accessible limit");
			(void) DsgRegionPopulate(region, page + 1, 1);
		}
		else if (strcmp(scenario, "destroy_twice") == 0)
		{
			DsgRegionDestroy(region);
			DsgRegionDestroy(region);
			test_dsg_check(region->address == NULL && region->mapped_bytes == 0,
						   "destroy retained stale metadata");
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unknown DSGA provider test scenario")));
	}
	PG_END_ENSURE_ERROR_CLEANUP(test_dsg_cleanup, PointerGetDatum(region));
	test_dsg_cleanup(0, PointerGetDatum(region));
	pfree(region);
	pfree(scenario);
	PG_RETURN_BOOL(true);
}
