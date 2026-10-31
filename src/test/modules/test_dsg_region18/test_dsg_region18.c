/*-
 * PG18独立叶模块及显式TEST_ONLY四数组/AIO core测试；不授resize/Root许可。
 * 只显式管理员测试调用；真实映射/异常清理/数据观察，不输出VA供普通用户。
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/dsg_region.h"
#include "storage/dsg_buffer_arrays.h"
#include "utils/builtins.h"
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/wait.h>

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_dsg_region18_abi);
PG_FUNCTION_INFO_V1(test_dsg_region18_run);

static void
Admin(void)
{
	if (!superuser() || !superuser_arg(GetSessionUserId()))
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			errmsg("PG18 region tests require original administrator")));
}

static void
Ensure(bool ok, const char *message)
{
	if (!ok)
		elog(ERROR, "PG18 region test: %s", message);
}

Datum
test_dsg_region18_abi(PG_FUNCTION_ARGS)
{
	StringInfoData out;
	PgAioWaitRef reference;
#ifdef USE_LIBURING
	const char *uring = "true";
#else
	const char *uring = "false";
#endif
	Admin();
	pgaio_wref_clear(&reference);
	initStringInfo(&out);
	appendStringInfo(&out,
		"{\"pg_version_num\":%d,\"descriptor_bytes\":\"%zu\",\"descriptor_padded_bytes\":\"%zu\","
		"\"state_bytes\":\"%zu\",\"aio_wait_ref_bytes\":\"%zu\",\"io_wref_offset\":\"%zu\","
		"\"block_bytes\":\"%d\",\"iocv_bytes\":\"%zu\",\"checkpoint_bytes\":\"%zu\","
		"\"cleared_wref_invalid\":%s,\"io_uring_compiled\":%s,\"production_resize_hooked\":false}",
		PG_VERSION_NUM, sizeof(BufferDesc), sizeof(BufferDescPadded), sizeof(((BufferDesc *) 0)->state),
		sizeof(PgAioWaitRef), offsetof(BufferDesc, io_wref), BLCKSZ,
		sizeof(ConditionVariableMinimallyPadded), sizeof(CkptSortItem),
		!pgaio_wref_valid(&reference) ? "true" : "false", uring);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

Datum
test_dsg_region18_run(PG_FUNCTION_ARGS)
{
	/* 指针在PG_TRY前固定；映射寿命状态放heap，避免longjmp后读取改变的栈对象。 */
	DsgRegion *region;
	char *action;
	long actual_page;
	Size page;
	DsgMemoryResult result;
	Admin();
	action = text_to_cstring(PG_GETARG_TEXT_PP(0));
	actual_page = sysconf(_SC_PAGESIZE);
	Ensure(actual_page > 0, "actual page missing");
	page = (Size) actual_page;
	region = palloc0(sizeof(*region));
	PG_TRY();
	{
		if (strcmp(action, "invalid-zero") == 0)
			DsgRegionReserve(region, 0);
		DsgRegionReserve(region, 3 * page + 13);
		if (strcmp(action, "invalid-offset") == 0)
			(void) DsgRegionPopulate(region, region->maximum_bytes + 1, 1);
		else if (strcmp(action, "invalid-overflow") == 0)
			(void) DsgRegionPopulate(region, 1, SIZE_MAX);
		else if (strcmp(action, "share-fork") == 0)
		{
			pid_t child;
			pid_t waited;
			int status;
			result = DsgRegionPopulate(region, 0, region->maximum_bytes);
			Ensure(result.status == DSG_MEMORY_OK, "real populate failed");
			region->address[0] = 0x15;
			child = fork();
			Ensure(child >= 0, "test-only fork failed");
			if (child == 0)
			{
				/* 不调用PG锁/内存/退出回调，避免误清母backend的PGPROC资源。 */
				region->address[0] = 0x51;
				region->address[region->maximum_bytes - 1] = 0x37;
				_exit(0);
			}
			do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
			Ensure(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"exact test child reap failed");
			Ensure(region->address[0] == 0x51 && region->address[region->maximum_bytes - 1] == 0x37,
				"MAP_SHARED fork did not preserve common backing");
		}
		else if (strcmp(action, "release-boundary") == 0)
		{
			Size i;
			result = DsgRegionPopulate(region, 0, region->maximum_bytes);
			Ensure(result.status == DSG_MEMORY_OK, "initial populate failed");
			memset(region->address, 0x6a, region->mapped_bytes);
			/* 向内对齐的半页区间不得清掉任意边界对象。 */
			result = DsgRegionRelease(region, page + 7, page - 14);
			Ensure(result.status == DSG_MEMORY_NOOP, "partial-page release was widened");
			result = DsgRegionReleaseTail(region, page + 17);
			Ensure(result.physical_release_verified && result.affected_offset == 2 * page &&
				result.affected_bytes == 2 * page, "tail padding release not confirmed");
			for (i = 0; i < 2 * page; i++)
				Ensure((unsigned char) region->address[i] == 0x6a, "live prefix/boundary was overwritten");
			result = DsgRegionPopulate(region, 2 * page, region->maximum_bytes - 2 * page);
			Ensure(result.status == DSG_MEMORY_OK, "released tail repopulate failed");
			for (i = 2 * page; i < region->mapped_bytes; i++)
				Ensure(region->address[i] == 0, "tail/padding did not refault as zero");
		}
		else if (strcmp(action, "park-rearm") == 0)
		{
			char *address = region->address;
			int cycle;
			for (cycle = 0; cycle < 3; cycle++)
			{
				Size i;
				result = DsgRegionPopulate(region, 0, region->maximum_bytes);
				Ensure(result.status == DSG_MEMORY_OK, "pre-Park populate failed");
				memset(region->address, 0x3f, region->mapped_bytes);
				DsgRegionPark(region);
				result = DsgRegionPopulate(region, 0, region->maximum_bytes);
				Ensure(result.status == DSG_MEMORY_FAILED && result.allocation_may_be_partial,
					"PROT_NONE populate failure lost unknown marker");
				DsgRegionRearm(region);
				Ensure(region->address == address, "Rearm moved the stable address");
				result = DsgRegionPopulate(region, 0, region->maximum_bytes);
				Ensure(result.status == DSG_MEMORY_OK, "Rearm populate failed");
				for (i = 0; i < region->mapped_bytes; i++)
					Ensure(region->address[i] == 0, "new backing retained old bytes");
			}
		}
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown PG18 region action")));
		DsgRegionDestroy(region);
	}
	PG_CATCH();
	{
		DsgRegionDestroy(region);
		PG_RE_THROW();
	}
	PG_END_TRY();
	PG_RETURN_TEXT_P(cstring_to_text("{\"ok\":true,\"normal_permit\":false}"));
}

/* 原生四数组core测试仍要求当前与原session管理员；不把test profile当Root资格。 */
PG_FUNCTION_INFO_V1(test_dsg_arrays18_snapshot);
PG_FUNCTION_INFO_V1(test_dsg_arrays18_pause);
Datum
test_dsg_arrays18_snapshot(PG_FUNCTION_ARGS)
{
 DsgBufferArraysSnapshot s;
 StringInfoData out;
 int i;
 Admin();DsgBufferArraysSnapshotGet(&s);initStringInfo(&out);
 appendStringInfo(&out,"{\"enabled\":%s,\"maximum_buffers\":%d,\"startup_buffers\":%d,\"active_completions\":%u,\"paused_completions\":%u,\"completed_aio\":\"" UINT64_FORMAT "\",\"released_aio_pins\":\"" UINT64_FORMAT "\",\"completion_lock_available\":%s,\"resize_authorized\":false,\"normal_permit\":false,\"arrays\":[",s.enabled?"true":"false",s.maximum_buffers,s.startup_buffers,s.active_completions,s.paused_completions,s.completed_aio,s.released_aio_pins,s.completion_lock_available?"true":"false");
 for(i=0;i<DSG_NATIVE_ARRAY_COUNT;i++){if(i)appendStringInfoChar(&out,',');appendStringInfo(&out,"{\"base\":\"%zu\",\"maximum_bytes\":\"%zu\",\"reserved_bytes\":\"%zu\",\"committed_bytes\":\"%zu\"}",(Size)s.ranges[i].address,s.ranges[i].maximum_bytes,s.ranges[i].mapped_bytes,s.committed_bytes[i]);}
 appendStringInfoString(&out,"]}");PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
Datum
test_dsg_arrays18_pause(PG_FUNCTION_ARGS)
{
 Admin();DsgBufferArraysTestPause(PG_GETARG_BOOL(0));PG_RETURN_VOID();
}
