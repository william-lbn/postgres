#include "postgres.h"

#include <unistd.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/dsg_region.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/ipc.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_dsg_region);
PG_FUNCTION_INFO_V1(test_dsg_shared_write);
PG_FUNCTION_INFO_V1(test_dsg_shared_read);
PG_FUNCTION_INFO_V1(test_dsg_buffer_layout);
PG_FUNCTION_INFO_V1(test_dsg_boot_id);
PG_FUNCTION_INFO_V1(test_dsg_exclusive_available);
PG_FUNCTION_INFO_V1(test_dsg_access_error);
PG_FUNCTION_INFO_V1(test_dsg_stale_hint);
PG_FUNCTION_INFO_V1(test_dsg_grow);
PG_FUNCTION_INFO_V1(test_dsg_grow_snapshot);
PG_FUNCTION_INFO_V1(test_dsg_grow_verify);

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
test_dsg_boot_id(PG_FUNCTION_ARGS)
{
	(void) test_dsg_shared_value();
	if (DSBCtrl == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) DSBCtrl->boot_id);
}

/* 只试锁，不发布容量；用于跨 Backend 证明真实遍历与回收者互斥。 */
Datum
test_dsg_exclusive_available(PG_FUNCTION_ARGS)
{
	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	if (!LWLockConditionalAcquire(&DSBCtrl->access_lock, LW_EXCLUSIVE))
		PG_RETURN_BOOL(false);
	LWLockRelease(&DSBCtrl->access_lock);
	PG_RETURN_BOOL(true);
}

Datum
test_dsg_access_error(PG_FUNCTION_ARGS)
{
	(void) test_dsg_shared_value();

	/*
	 * 故意不调用 End，验证 SQL ERROR 的 PG 原生 LWLock 清理路径。
	 */
	(void) DsgBufferAccessBegin();
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("DSGA test error while protecting buffer arrays")));
	PG_RETURN_VOID();
}

Datum
test_dsg_stale_hint(PG_FUNCTION_ARGS)
{
	RelFileLocator locator = {1, 1, 1};

	(void) test_dsg_shared_value();
	/*-
	 * 先测仍在预留 M 内、但超出当前 H 的提示，再测远超 M 的提示。
	 * 两者都必须在解引用 Descriptor 前正常拒绝，不能触发断言/触页。
	 */
	test_dsg_check(!ReadRecentBuffer(locator, MAIN_FORKNUM, 0, GetHighNBuffers() + 1),
				   "inactive tail hint was accepted");
	test_dsg_check(!ReadRecentBuffer(locator, MAIN_FORKNUM, 0, INT_MAX),
				   "out-of-reservation hint was accepted");
	PG_RETURN_BOOL(true);
}

/* 调用真实协调器；不直接改 H/L，也不伪造 Provider 或成功状态。 */
Datum
test_dsg_grow(PG_FUNCTION_ARGS)
{
	DsgBufferGrowRequest request;
	DsgBufferSnapshot snapshot;
	bool		committed;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	MemSet(&request, 0, sizeof(request));
	request.boot_id = DSBCtrl->boot_id;
	request.revision = (uint64) PG_GETARG_INT64(0);
	request.operation_id_low = (uint64) PG_GETARG_INT64(1);
	request.expected_generation = (uint64) PG_GETARG_INT64(2);
	request.target_buffers = PG_GETARG_INT32(3);
	if (PG_GETARG_BOOL(5))
		request.boot_id ^= UINT64CONST(1);
	committed = DsgBufferGrow(&request, PG_GETARG_INT32(4), &snapshot);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s high=%d generation=" UINT64_FORMAT " replay=%s",
											  committed ? "committed" : "denied", snapshot.high,
											  snapshot.generation, snapshot.replayed ? "true" : "false")));
}

Datum
test_dsg_grow_snapshot(PG_FUNCTION_ARGS)
{
	DsgBufferSnapshot snapshot;
	int			unknown = 0;
	int			charged_tail = 0;
	int			i;

	(void) test_dsg_shared_value();
	DsgBufferGetSnapshot(&snapshot);
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		DsgBufferArray *array = &DSBCtrl->arrays[i];

		unknown += snapshot.allocation_unknown[i] ? 1 : 0;
		charged_tail += snapshot.committed_upper_bytes[i] >
			TYPEALIGN(array->region.page_bytes, mul_size(snapshot.high, array->element_bytes)) ? 1 : 0;
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("phase=%d reason=%d high=%d generation=" UINT64_FORMAT " unknown=%d charged_tail=%d",
											  snapshot.operation.phase, snapshot.operation.reason, snapshot.high,
											  snapshot.generation, unknown, charged_tail)));
}

/*-
 * SQL 数据读写后检查新增 ID 的实际描述符及永久 Granule 目录；至少一个
 * 新增 Buffer 必须已被普通 BufferAlloc 使用，避免只验证 H 的数字变化。
 * 仅扫描实际 Guard 允许的前缀，不用测试自身触摸预留尾部来冒充扩容。
 */
Datum
test_dsg_grow_verify(PG_FUNCTION_ARGS)
{
	DsgBufferAccess access;
	bool		used_tail = false;
	int			i;
	int			active = 0;

	(void) test_dsg_shared_value();
	LWLockAcquire(&DSBCtrl->resize_lock, LW_SHARED);
	access = DsgBufferAccessBegin();
	for (i = NBuffers; i < access.count; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		uint32		state = LockBufHdr(buf);

		bool		correct_id = buf->buf_id == i;

		if ((state & (BM_VALID | BM_TAG_VALID)) == (BM_VALID | BM_TAG_VALID))
			used_tail = true;
		UnlockBufHdr(buf, state);
		test_dsg_check(correct_id, "grown descriptor has wrong ID");
	}
	for (i = 0; i < DSBCtrl->granule_count; i++)
		active += DSBCtrl->granules[i].active_buffers;
	test_dsg_check(active == access.count, "granule active count differs from protected capacity");
	DsgBufferAccessEnd(&access);
	LWLockRelease(&DSBCtrl->resize_lock);
	test_dsg_check(used_tail, "ordinary buffer allocation never used the grown range");
	PG_RETURN_BOOL(true);
}

/*-
 * 断言实际 Buffer Manager 指针来自目录中的共享映射，并校验只提交初始
 * 前缀、四数组分别按 M 预留。嵌套 Guard 测试覆盖重复读锁的释放责任。
 * 这里不触摸未发布尾部，避免测试自身把整个 M 变成实际占用。
 */
Datum
test_dsg_buffer_layout(PG_FUNCTION_ARGS)
{
	DsgBufferAccess outer;
	DsgBufferAccess inner;
	int			i;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffer layout is disabled");
	outer = DsgBufferAccessBegin();
	inner = DsgBufferAccessBegin();
	test_dsg_check(outer.acquired && !inner.acquired, "nested guard acquired twice");
	test_dsg_check(outer.count == inner.count && outer.count == GetLowNBuffers() &&
				   outer.count == NBuffers && outer.count < GetMaxNBuffers(),
				   "invalid initial L/H/M");
	test_dsg_check(BufferDescriptors ==
				   (BufferDescPadded *) DSBCtrl->arrays[DSG_BUFFER_DESCRIPTORS].region.address &&
				   BufferBlocks == DSBCtrl->arrays[DSG_BUFFER_BLOCKS].region.address &&
				   BufferIOCVArray ==
				   (ConditionVariableMinimallyPadded *) DSBCtrl->arrays[DSG_BUFFER_IOCV].region.address &&
				   CkptBufferIds ==
				   (CkptSortItem *) DSBCtrl->arrays[DSG_BUFFER_CHECKPOINT].region.address,
				   "buffer arrays do not use directory mappings");
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		DsgBufferArray *array = &DSBCtrl->arrays[i];
		Size		active = mul_size(outer.count, array->element_bytes);

		test_dsg_check(array->region.maximum_bytes ==
					   mul_size(GetMaxNBuffers(), array->element_bytes), "incorrect reservation");
		test_dsg_check(array->committed_upper_bytes ==
					   TYPEALIGN(array->region.page_bytes, active) &&
					   !array->allocation_unknown, "incorrect initial commitment budget");
	}
	test_dsg_check(GetBufferDescriptor(outer.count - 1)->buf_id == outer.count - 1,
				   "last active descriptor is not initialized");
	DsgBufferAccessEnd(&inner);
	test_dsg_check(LWLockHeldByMe(&DSBCtrl->access_lock), "inner guard released outer lock");
	DsgBufferAccessEnd(&outer);
	test_dsg_check(!LWLockHeldByMe(&DSBCtrl->access_lock), "outer guard leaked lock");
	PG_RETURN_BOOL(true);
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
		else if (strcmp(scenario, "tail_terminal_padding") == 0)
		{
			Size		offset = page + 17;
			Size		i;

			DsgRegionDestroy(region);
			DsgRegionReserve(region, 3 * page + 1);
			result = DsgRegionPopulate(region, 0, region->maximum_bytes);
			test_dsg_check(result.status == DSG_MEMORY_OK, "tail test populate failed");
			memset(region->address, 0x5a, region->maximum_bytes);
			/*-
			 * 普通区间不能释放末页；随后整段尾部才可以涵盖该页 padding。
			 * 检查真实共享内容，只读 maximum 内的合法地址，不靠越界访问
			 * padding 来验证。Mac 两次均必须保留原内容且不给物理证明。
			 */
			result = DsgRegionRelease(region, offset, region->maximum_bytes - offset);
			test_dsg_check(region->address[3 * page] == 0x5a,
						   "ordinary release discarded terminal boundary page");
			if (DsgRegionCanRelease())
				test_dsg_check(result.physical_release_verified &&
							   result.affected_offset == 2 * page && result.affected_bytes == page,
							   "ordinary release expanded to terminal padding");
			result = DsgRegionReleaseTail(region, offset);
			test_dsg_check(result.requested_offset == offset &&
						   result.requested_bytes == region->maximum_bytes - offset,
						   "tail logical request changed");
			for (i = 0; i < 2 * page; i++)
				test_dsg_check(region->address[i] == 0x5a, "tail release damaged live boundary page");
			if (DsgRegionCanRelease())
			{
				test_dsg_check(result.status == DSG_MEMORY_OK && result.physical_release_verified &&
							   result.affected_offset == 2 * page && result.affected_bytes == 2 * page,
							   "tail release did not include owned terminal padding");
				for (i = 2 * page; i < region->maximum_bytes; i++)
					test_dsg_check(region->address[i] == 0, "tail shared bytes were not reclaimed");
			}
			else
			{
				test_dsg_check(result.status == DSG_MEMORY_UNSUPPORTED &&
							   !result.physical_release_verified && result.affected_bytes == 0 &&
							   !result.release_may_be_partial, "unsupported tail release invented evidence");
				for (i = 2 * page; i < region->maximum_bytes; i++)
					test_dsg_check(region->address[i] == 0x5a, "unsupported tail release changed contents");
			}
		}
		else if (strcmp(scenario, "tail_at_end_noop") == 0)
		{
			DsgRegionDestroy(region);
			DsgRegionReserve(region, 3 * page + 1);
			region->address[3 * page] = 0x5a;
			result = DsgRegionReleaseTail(region, region->maximum_bytes);
			test_dsg_check(result.status == DSG_MEMORY_NOOP && result.affected_bytes == 0 &&
						   !result.physical_release_verified && region->address[3 * page] == 0x5a,
						   "empty tail discarded a live partial terminal page");
		}
		else if (strcmp(scenario, "tail_past_end") == 0)
			(void) DsgRegionReleaseTail(region, SIZE_MAX);
		else if (strcmp(scenario, "tail_unmapped") == 0)
		{
			DsgRegionDestroy(region);
			(void) DsgRegionReleaseTail(region, 0);
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
