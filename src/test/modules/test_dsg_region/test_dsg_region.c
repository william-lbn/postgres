#include "postgres.h"

#include <unistd.h>
#include <sys/mman.h>

#include "access/table.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/dsg_region.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proclist.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/wait_event.h"

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
PG_FUNCTION_INFO_V1(test_dsg_shrink);
PG_FUNCTION_INFO_V1(test_dsg_shrink_snapshot);
PG_FUNCTION_INFO_V1(test_dsg_tail_block);
PG_FUNCTION_INFO_V1(test_dsg_invalid_tail);
PG_FUNCTION_INFO_V1(test_dsg_collision_unpin);
PG_FUNCTION_INFO_V1(test_dsg_grow_snapshot);
PG_FUNCTION_INFO_V1(test_dsg_grow_verify);
PG_FUNCTION_INFO_V1(test_dsg_allocation_limit);
PG_FUNCTION_INFO_V1(test_dsg_relation_below);
PG_FUNCTION_INFO_V1(test_dsg_pin);
PG_FUNCTION_INFO_V1(test_dsg_pin_control);
PG_FUNCTION_INFO_V1(test_dsg_unpin_probe);
PG_FUNCTION_INFO_V1(test_dsg_mapping_free);
PG_FUNCTION_INFO_V1(test_dsg_checkpoint_control);
PG_FUNCTION_INFO_V1(test_dsg_io);
PG_FUNCTION_INFO_V1(test_dsg_io_control);
PG_FUNCTION_INFO_V1(test_dsg_grow_control);
PG_FUNCTION_INFO_V1(test_dsg_admission);
PG_FUNCTION_INFO_V1(test_dsg_admission_snapshot);

void		_PG_init(void);

static DsgRegion inherited_region;

/*-
 * 测试值与 pin 门禁使用不同原子字段。门禁仅供临时集群确定性调度，
 * 不改 Buffer refcount/flags，也不伪造原生 cleanup waiter 的等待。
 */
typedef struct DsgTestSharedState
{
	pg_atomic_uint32 value;
	pg_atomic_uint32 pin_gate;	/* 0=未持有，1=真实持 pin，2=请求释放 */
	pg_atomic_uint32 checkpoint_gate;
	pg_atomic_uint32 io_writer_gate;
	pg_atomic_uint32 io_waiter_gate;
	pg_atomic_uint32 io_buffer;
	pg_atomic_uint32 io_audit;
	pg_atomic_uint32 fast_drop_found;
	pg_atomic_uint32 grow_exit_gate;
	pg_atomic_uint32 grow_prepare_gate;
} DsgTestSharedState;

static void test_dsg_wait_gate(pg_atomic_uint32 *gate, const char *event_name);

#ifdef USE_INJECTION_POINTS
extern PGDLLEXPORT void test_dsg_protect_descriptor(const char *name,
													const void *private_data);
static void *probe_page;
static Size probe_bytes;
static bool probe_armed;
static bool probe_protected;
static bool collision_armed;
extern PGDLLEXPORT void test_dsg_collision_pause(const char *name, const void *private_data);

extern PGDLLEXPORT void test_dsg_checkpoint_pause(const char *name, const void *private_data);
extern PGDLLEXPORT void test_dsg_checkpoint_fail(const char *name, const void *private_data);
extern PGDLLEXPORT void test_dsg_io_pause(const char *name, const void *private_data);
extern PGDLLEXPORT void test_dsg_io_audit(const char *name, const void *private_data);
extern PGDLLEXPORT void test_dsg_fast_drop(const char *name, const void *private_data);
extern PGDLLEXPORT void test_dsg_grow_pause(const char *name, const void *private_data);
static bool io_writer_armed;
static bool io_waiter_armed;
static int	grow_mode;
static int	release_probe_calls;

/*-
 * 此回调后登记、先执行，恰好暂停在 PG 原生已放锁、内核 Grow 清理之前。
 * 不修改被测操作；控制端必须观察本门禁后才启动下一真实协调者。
 */
static void
test_dsg_grow_exit_pause(int code, Datum arg)
{
	Assert(!LWLockHeldByMe(&DSBCtrl->resize_lock));
	test_dsg_wait_gate(&((DsgTestSharedState *) inherited_region.address)->grow_exit_gate,
					   "DsgTestGrowExit");
}

void
test_dsg_grow_pause(const char *name, const void *private_data)
{
	int			mode = grow_mode;

	grow_mode = 0;
	if (mode == 1)
	{
		before_shmem_exit(test_dsg_grow_exit_pause, (Datum) 0);
		ereport(FATAL, (errmsg("DSGA test Grow FATAL")));
	}
	else if (mode == 2)
	{
		/*-
		 * 新Guard夹具暂停的是实际读者，持accessSH；原有Grow/Shrink
		 * 注入点仍必须持resizeX。按注入点核验，不能放宽原有锁断言。
		 */
		if (strcmp(name, "dsg-test-control-guard-held") == 0)
			Assert(LWLockHeldByMeInMode(&DSBCtrl->access_lock, LW_SHARED));
		else
			Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
		test_dsg_wait_gate(&((DsgTestSharedState *) inherited_region.address)->grow_prepare_gate,
						   "DsgTestGrowPrepare");
	}
	else if (mode == 3)
		ereport(FATAL, (errmsg("DSGA test Grow FATAL")));
	else if (mode == 4)
		ereport(ERROR, (errmsg("DSGA test resize ERROR")));
	else if (mode == 5)
	{
		/*
		 * 第一数组真实调用Provider；第二数组调用前报错，不伪造前次回执。
		 */
		if (++release_probe_calls == 2)
			ereport(ERROR, (errmsg("DSGA test second array release ERROR")));
		grow_mode = 5;
	}
}

/*-
 * 只改本测试 Backend 的页表，不改共享 backing 和其他 Backend 的访问权。
 * 回调后直到 ReleaseBuffer 返回并恢复权限期间禁止再读 Descriptor。
 * 全局注入点在其他 Backend 的回调因本地 probe_armed=false 而无副作用。
 */
void
test_dsg_protect_descriptor(const char *name, const void *private_data)
{
	if (!probe_armed)
		return;
	probe_armed = false;
	if (mprotect(probe_page, probe_bytes, PROT_NONE) != 0)
		ereport(ERROR, (errmsg("could not protect DSGA test descriptor: %m")));
	probe_protected = true;
}

/*-
 * 原生BufferAlloc取得victim后暂停。测试集群无其他未赋tag的并发分配，
 * 扫描仍核验唯一性，避免误选；不写header、hash、refcount或IO状态。
 * 另一会话随后在L以下加载同页，真实制造BufTableInsert冲突。
 */
void
test_dsg_collision_pause(const char *name, const void *private_data)
{
	DsgBufferAccess access;
	int			i;
	int			found = 0;
	Size		page = (Size) sysconf(_SC_PAGESIZE);

	if (!collision_armed)
		return;
	collision_armed = false;
	access = DsgBufferAccessBegin();
	for (i = NBuffers; i < access.count; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		uint32		state = LockBufHdr(buf);

		if (!(state & BM_TAG_VALID) && BUF_STATE_GET_REFCOUNT(state) == 1)
		{
			found++;
			probe_page = (void *) TYPEALIGN_DOWN(page, (uintptr_t) buf);
			probe_bytes = page;
		}
		UnlockBufHdr(buf, state);
	}
	DsgBufferAccessEnd(&access);
	if (found != 1)
		ereport(ERROR, (errmsg("DSGA collision probe requires one real tail victim, found %d", found)));
	probe_armed = true;
	test_dsg_wait_gate(&((DsgTestSharedState *) inherited_region.address)->grow_prepare_gate,
					   "DsgTestCollisionVictim");
}

void
test_dsg_checkpoint_pause(const char *name, const void *private_data)
{
	Assert(DSBCtrl != NULL);
	Assert(LWLockHeldByMe(&DSBCtrl->checkpoint_lock));
	Assert(!LWLockHeldByMe(&DSBCtrl->access_lock));
	test_dsg_wait_gate(&((DsgTestSharedState *) inherited_region.address)->checkpoint_gate,
					   "DsgTestCheckpoint");
}

void
test_dsg_checkpoint_fail(const char *name, const void *private_data)
{
	Assert(DSBCtrl != NULL && LWLockHeldByMe(&DSBCtrl->checkpoint_lock));
	Assert(!LWLockHeldByMe(&DSBCtrl->access_lock));
	ereport(ERROR, (errmsg("DSGA test checkpoint failure")));
}

/* 全局注入点仅暂停本地已武装的测试会话，不影响 checkpointer 等进程。 */
void
test_dsg_io_pause(const char *name, const void *private_data)
{
	DsgTestSharedState *state = (DsgTestSharedState *) inherited_region.address;

	if (strcmp(name, "dsg-buffer-before-write") == 0)
	{
		if (io_writer_armed)
			test_dsg_wait_gate(&state->io_writer_gate, "DsgTestIOWriter");
	}
	else if (io_waiter_armed)
	{
		Buffer		buffer = (Buffer) pg_atomic_read_u32(&state->io_buffer);

		Assert(DSBCtrl != NULL && LWLockHeldByMe(&DSBCtrl->access_lock));
		CheckBufferIsPinnedOnce(buffer);
		test_dsg_wait_gate(&state->io_waiter_gate, "DsgTestIOWaiter");
	}
}

/* 真实 WAL 恢复快速路径必须在访问 Descriptor 前已持数组保护。 */
void
test_dsg_fast_drop(const char *name, const void *private_data)
{
	DsgTestSharedState *state = (DsgTestSharedState *) inherited_region.address;

	Assert(DSBCtrl != NULL && LWLockHeldByMe(&DSBCtrl->access_lock));
	pg_atomic_fetch_add_u32(&state->fast_drop_found, 1);
}
#endif

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
		DsgTestSharedState *state;

		DsgRegionReserve(&inherited_region, (Size) sysconf(_SC_PAGESIZE));
		state = (DsgTestSharedState *) inherited_region.address;
		pg_atomic_init_u32(&state->value, 0);
		pg_atomic_init_u32(&state->pin_gate, 0);
		pg_atomic_init_u32(&state->checkpoint_gate, 0);
		pg_atomic_init_u32(&state->io_writer_gate, 0);
		pg_atomic_init_u32(&state->io_waiter_gate, 0);
		pg_atomic_init_u32(&state->io_buffer, 0);
		pg_atomic_init_u32(&state->io_audit, 0);
		pg_atomic_init_u32(&state->fast_drop_found, 0);
		pg_atomic_init_u32(&state->grow_exit_gate, 0);
		pg_atomic_init_u32(&state->grow_prepare_gate, 0);
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
	return &((DsgTestSharedState *) inherited_region.address)->value;
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

/* 测试门禁只决定正常流程何时继续，不改原生 pin、checkpoint 或 IO 状态。 */
static void
test_dsg_wait_gate(pg_atomic_uint32 *gate, const char *event_name)
{
	uint32		wait_event = WaitEventExtensionNew(event_name);

	PG_TRY();
	{
		pg_atomic_write_u32(gate, 1);
		while (pg_atomic_read_u32(gate) != 2)
		{
			CHECK_FOR_INTERRUPTS();
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 10L, wait_event);
			ResetLatch(MyLatch);
		}
	}
	PG_FINALLY();
	{
		pg_atomic_write_u32(gate, 0);
	}
	PG_END_TRY();
}

Datum
test_dsg_boot_id(PG_FUNCTION_ARGS)
{
	(void) test_dsg_shared_value();
	if (DSBCtrl == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) DSBCtrl->boot_id);
}

/* 使用真实boot及单调revision测试门禁，生产入口还须由Neon管理接口包装。 */
Datum
test_dsg_admission(PG_FUNCTION_ARGS)
{
	DsgBufferSetAdmission((uint64) PG_GETARG_INT64(0),
						  (uint64) PG_GETARG_INT64(1),
						  (uint64) PG_GETARG_INT64(2), PG_GETARG_BOOL(3));
	PG_RETURN_BOOL(true);
}

Datum
test_dsg_admission_snapshot(PG_FUNCTION_ARGS)
{
	DsgBufferSnapshot snapshot;

	(void) test_dsg_shared_value();
	DsgBufferGetSnapshot(&snapshot);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("open=%s revision=" UINT64_FORMAT " high=%d shared=%zu",
											  snapshot.admission_open ? "yes" : "no",
											  snapshot.admission_revision, snapshot.high,
											  snapshot.startup_shared_upper_bytes)));
}

/*-
 * 有界试锁，不发布容量；用于跨 Backend 验证遍历与回收互斥。
 * 单次失败可能只是 BG writer 的短扫描，不能当作本会话泄锁的证明。
 * 因此先检查本会话没有遗留锁，再短暂重试；被注入点固定暂停的读者
 * 仍必须返回 false，不采用阻塞 LWLockAcquire 掩盖实际互斥状态。
 */
Datum
test_dsg_exclusive_available(PG_FUNCTION_ARGS)
{
	int			attempt;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	test_dsg_check(!LWLockHeldByMe(&DSBCtrl->access_lock), "caller leaked a buffer access lock");
	for (attempt = 0; attempt < 1000; attempt++)
	{
		if (LWLockConditionalAcquire(&DSBCtrl->access_lock, LW_EXCLUSIVE))
		{
			LWLockRelease(&DSBCtrl->access_lock);
			PG_RETURN_BOOL(true);
		}
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000L);
	}
	PG_RETURN_BOOL(false);
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

/* 测试始终调用真实Shrink，不能通过测试接口直接写H/L或伪造物理完成。 */
Datum
test_dsg_shrink(PG_FUNCTION_ARGS)
{
	DsgBufferResizeRequest request;
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
	committed = DsgBufferShrink(&request, PG_GETARG_INT32(4), &snapshot);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s high=%d generation=" UINT64_FORMAT " replay=%s",
											  committed ? "committed" : "denied", snapshot.high,
											  snapshot.generation, snapshot.replayed ? "true" : "false")));
}

/* 只读真实账本；Python可精确读取u64，不将SQL响应当作物理回收证据。 */
Datum
test_dsg_shrink_snapshot(PG_FUNCTION_ARGS)
{
	DsgBufferSnapshot snapshot;
	StringInfoData output;
	int			i;

	(void) test_dsg_shared_value();
	DsgBufferGetSnapshot(&snapshot);
	initStringInfo(&output);
	appendStringInfo(&output,
					 "{\"low\":%d,\"high\":%d,\"generation\":" UINT64_FORMAT
					 ",\"revision\":" UINT64_FORMAT ",\"kind\":%d,\"phase\":%d,\"reason\":%d,\"drain\":[",
					 snapshot.low, snapshot.high, snapshot.generation,
					 snapshot.operation.request.revision, snapshot.operation.kind,
					 snapshot.operation.phase, snapshot.operation.reason);
	for (i = 0; i < DSG_DRAIN_RESULT_COUNT; i++)
		appendStringInfo(&output, "%s%u", i ? "," : "", snapshot.operation.drain_counts[i]);
	appendStringInfoString(&output, "],\"arrays\":[");
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
		appendStringInfo(&output,
						 "%s{\"upper\":%zu,\"unknown\":%s,\"state\":%d,\"revision\":" UINT64_FORMAT
						 ",\"generation\":" UINT64_FORMAT ",\"verified\":%s,\"partial\":%s}",
						 i ? "," : "", snapshot.committed_upper_bytes[i],
						 snapshot.allocation_unknown[i] ? "true" : "false", snapshot.release_state[i],
						 snapshot.release_revision[i], snapshot.release_generation[i],
						 snapshot.release_result[i].physical_release_verified ? "true" : "false",
						 snapshot.release_result[i].release_may_be_partial ? "true" : "false");
	appendStringInfoString(&output, "]}");
	PG_RETURN_TEXT_P(cstring_to_text(output.data));
}

/* 找到真实驻留在待排水尾部的关系页，后续pin也必须走ReadBuffer，而非改refcount。 */
Datum
test_dsg_tail_block(PG_FUNCTION_ARGS)
{
	Relation	rel;
	DsgBufferAccess access;
	int			i;
	BlockNumber block = InvalidBlockNumber;

	(void) test_dsg_shared_value();
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	access = DsgBufferAccessBegin();
	for (i = Max(0, PG_GETARG_INT32(1)); i < access.count; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		uint32		state = LockBufHdr(buf);

		if ((state & (BM_VALID | BM_TAG_VALID)) == (BM_VALID | BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&buf->tag, &rel->rd_locator) &&
			BufTagGetForkNum(&buf->tag) == MAIN_FORKNUM)
			block = buf->tag.blockNum;
		UnlockBufHdr(buf, state);
		if (block != InvalidBlockNumber)
			break;
	}
	DsgBufferAccessEnd(&access);
	table_close(rel, AccessShareLock);
	if (block == InvalidBlockNumber)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) block);
}

/* 观察真实读失败留下的tagged-invalid尾部，不写header、hash、IO或refcount。 */
Datum
test_dsg_invalid_tail(PG_FUNCTION_ARGS)
{
	Relation	rel;
	DsgBufferAccess access;
	int			i;
	int			count = 0;

	(void) test_dsg_shared_value();
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	access = DsgBufferAccessBegin();
	for (i = Max(0, PG_GETARG_INT32(1)); i < access.count; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		uint32		state = LockBufHdr(buf);

		if ((state & (BM_TAG_VALID | BM_VALID | BM_IO_IN_PROGRESS)) == BM_TAG_VALID &&
			BUF_STATE_GET_REFCOUNT(state) == 0 &&
			BufTagMatchesRelFileLocator(&buf->tag, &rel->rd_locator))
			count++;
		UnlockBufHdr(buf, state);
	}
	DsgBufferAccessEnd(&access);
	table_close(rel, AccessShareLock);
	PG_RETURN_INT32(count);
}

/*-
 * 使用真实两会话hash冲突，在原victim最后unpin后保护其Descriptor页。
 * 胜者被分配在bootstrap前缀的另一页；ReadBuffer返回前若仍访问旧victim
 * 就会触发本Backend访问错误。正常返回后立即恢复权限，再释放胜者pin。
 */
Datum
test_dsg_collision_unpin(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	Relation	rel;
	bool		old_evict = neon_test_evict;

	(void) test_dsg_shared_value();
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	InjectionPointAttach("dsg-buffer-alloc-before-insert", "test_dsg_region",
						 "test_dsg_collision_pause", NULL, 0);
	InjectionPointAttach("dsg-buffer-after-unpin", "test_dsg_region",
						 "test_dsg_protect_descriptor", NULL, 0);
	collision_armed = true;
	probe_protected = false;
	neon_test_evict = true;
	PG_TRY();
	{
		Buffer		buffer = ReadBuffer(rel, 0);

		test_dsg_check(probe_protected, "native collision did not release its original victim");
		if (mprotect(probe_page, probe_bytes, PROT_READ | PROT_WRITE) != 0)
			ereport(FATAL, (errmsg("could not restore collision Descriptor probe: %m")));
		probe_protected = false;
		neon_test_evict = old_evict;
		ReleaseBuffer(buffer);
	}
	PG_FINALLY();
	{
		collision_armed = false;
		probe_armed = false;
		if (probe_protected && mprotect(probe_page, probe_bytes, PROT_READ | PROT_WRITE) != 0)
			ereport(FATAL, (errmsg("could not restore collision Descriptor probe: %m")));
		probe_protected = false;
		neon_test_evict = old_evict;
		InjectionPointDetach("dsg-buffer-alloc-before-insert");
		InjectionPointDetach("dsg-buffer-after-unpin");
	}
	PG_END_TRY();
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(true);
#else
	ereport(ERROR, (errmsg("DSGA collision test requires injection points")));
	PG_RETURN_NULL();
#endif
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

/* Grow 退出回归的调度接口，不写操作 phase/reason/预算或伪造锁所有权。 */
Datum
test_dsg_grow_control(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	DsgTestSharedState *state;
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *result = "ok";

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	state = (DsgTestSharedState *) inherited_region.address;
	if (strcmp(action, "attach_guard") == 0)
		InjectionPointAttach("dsg-test-control-guard-held",
							 "test_dsg_region", "test_dsg_grow_pause", NULL, 0);
	else if (strcmp(action, "attach") == 0 || strcmp(action, "attach_after_publish") == 0)
		InjectionPointAttach(strcmp(action, "attach") == 0 ?
							 "dsg-buffer-grow-before-publish" : "dsg-buffer-grow-after-publish",
							 "test_dsg_region", "test_dsg_grow_pause", NULL, 0);
	else if (strcmp(action, "arm_fatal") == 0)
		grow_mode = 1;
	else if (strcmp(action, "arm_pause") == 0)
		grow_mode = 2;
	else if (strcmp(action, "arm_fatal_unpaused") == 0)
		grow_mode = 3;
	else if (strcmp(action, "arm_error") == 0)
		grow_mode = 4;
	else if (strcmp(action, "arm_second_release_error") == 0)
	{
		grow_mode = 5;
		release_probe_calls = 0;
	}
	else if (strcmp(action, "attach_shrink_filter") == 0 || strcmp(action, "attach_release") == 0)
		InjectionPointAttach(strcmp(action, "attach_shrink_filter") == 0 ?
							 "dsg-buffer-shrink-filter-batch" : "dsg-region-before-release",
							 "test_dsg_region", "test_dsg_grow_pause", NULL, 0);
	else if (strcmp(action, "attach_issuer_register") == 0)
		InjectionPointAttach("dsg-buffer-before-operation-register",
							 "test_dsg_region", "test_dsg_grow_pause", NULL, 0);
	else if (strcmp(action, "attach_shrink_fence") == 0 ||
			 strcmp(action, "attach_shrink_prepare") == 0 ||
			 strcmp(action, "attach_shrink_publish") == 0)
		InjectionPointAttach(strcmp(action, "attach_shrink_fence") == 0 ?
							 "dsg-buffer-shrink-after-fence" :
							 (strcmp(action, "attach_shrink_prepare") == 0 ?
							  "dsg-buffer-shrink-before-publish" : "dsg-buffer-shrink-after-publish"),
							 "test_dsg_region", "test_dsg_grow_pause", NULL, 0);
	else if (strcmp(action, "detach_shrink") == 0)
	{
		InjectionPointDetach("dsg-buffer-shrink-after-fence");
		InjectionPointDetach("dsg-buffer-shrink-before-publish");
		InjectionPointDetach("dsg-buffer-shrink-after-publish");
		InjectionPointDetach("dsg-buffer-shrink-filter-batch");
		InjectionPointDetach("dsg-region-before-release");
	}
	else if (strcmp(action, "exit_waiting") == 0)
		result = pg_atomic_read_u32(&state->grow_exit_gate) == 1 ? "yes" : "no";
	else if (strcmp(action, "prepare_waiting") == 0)
		result = pg_atomic_read_u32(&state->grow_prepare_gate) == 1 ? "yes" : "no";
	else if (strcmp(action, "release_exit") == 0)
		pg_atomic_write_u32(&state->grow_exit_gate, 2);
	else if (strcmp(action, "release_prepare") == 0)
		pg_atomic_write_u32(&state->grow_prepare_gate, 2);
	else if (strcmp(action, "detach") == 0)
	{
		InjectionPointDetach("dsg-buffer-grow-before-publish");
		InjectionPointDetach("dsg-buffer-grow-after-publish");
	}
	else if (strcmp(action, "operation") == 0)
	{
		DsgBufferSnapshot snapshot;

		DsgBufferGetSnapshot(&snapshot);
		PG_RETURN_TEXT_P(cstring_to_text(psprintf("op=" UINT64_FORMAT " phase=%d reason=%d high=%d generation=" UINT64_FORMAT,
												  snapshot.operation.request.operation_id_low,
												  snapshot.operation.phase, snapshot.operation.reason,
												  snapshot.high, snapshot.generation)));
	}
	else
		ereport(ERROR, (errmsg("invalid DSGA Grow test action")));
	pfree(action);
	PG_RETURN_TEXT_P(cstring_to_text(result));
#else
	ereport(ERROR, (errmsg("DSGA Grow tests require injection points")));
	PG_RETURN_NULL();
#endif
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
 * 此测试入口只操作分配栅栏，H、代次与物理预算均保持不变。它不能被当作
 * Shrink 的控制入口。与生产协调器约定一样先串行 resize，再取得 accessX。
 */
Datum
test_dsg_allocation_limit(PG_FUNCTION_ARGS)
{
	int			limit = PG_GETARG_INT32(0);

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	LWLockAcquire(&DSBCtrl->resize_lock, LW_EXCLUSIVE);
	LWLockAcquire(&DSBCtrl->access_lock, LW_EXCLUSIVE);
	test_dsg_check(limit >= NBuffers && limit <= GetHighNBuffers(), "invalid allocation limit");
	INJECTION_POINT("dsg-test-fence-before-publish");
	StrategySetAllocationLimit(limit);
	LWLockRelease(&DSBCtrl->access_lock);
	LWLockRelease(&DSBCtrl->resize_lock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("low=%d high=%d", GetLowNBuffers(), GetHighNBuffers())));
}

/* 检查普通 SQL 为新关系分配的真实 Buffer ID，而非只读取 L 数字。 */
Datum
test_dsg_relation_below(PG_FUNCTION_ARGS)
{
	Relation	rel;
	DsgBufferAccess access;
	int			limit = PG_GETARG_INT32(1);
	bool		found = false;
	bool		below = true;
	int			i;

	(void) test_dsg_shared_value();
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	access = DsgBufferAccessBegin();
	for (i = 0; i < access.count; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		uint32		state = LockBufHdr(buf);

		if ((state & BM_TAG_VALID) &&
			RelFileLocatorEquals(BufTagGetRelFileLocator(&buf->tag), rel->rd_locator))
		{
			found = true;
			below &= i < limit;
		}
		UnlockBufHdr(buf, state);
	}
	DsgBufferAccessEnd(&access);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(found && below);
}

/*-
 * 复现 BufferAlloc hash 碰撞归还路径的锁上下文：持真实 mappingX 调用
 * StrategyFreeBuffer。另一个 Backend 可同时持 accessX；此处不得再取
 * access 锁，否则与持 accessX 等待 mapping 的回收者形成反向锁序。
 * 测试自身保留正常 pin，不修改 tag，也不伪造 Descriptor 的共享状态。
 */
Datum
test_dsg_mapping_free(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Buffer		buffer;
	BufferDesc *buf;
	LWLock	   *mapping;

	(void) test_dsg_shared_value();
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	buffer = ReadBuffer(rel, 0);
	test_dsg_check(!BufferIsLocal(buffer), "mapping tests require a shared relation");
	buf = GetBufferDescriptor(buffer - 1);
	mapping = BufMappingPartitionLock(BufTableHashCode(&buf->tag));
	LWLockAcquire(mapping, LW_EXCLUSIVE);
	INJECTION_POINT("dsg-test-mapping-before-free");
	StrategyFreeBuffer(buf);
	LWLockRelease(mapping);
	ReleaseBuffer(buffer);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(true);
}

/*-
 * 使用真实 ReadBuffer/LockBufferForCleanup，不伪造共享 refcount 或 waiter 位。
 * holder 仅持 pin、没有 content/header 锁；原生清理等待可以被取消或终止，
 * 由 PG 自身 UnlockBuffers/ResourceOwner 清理其 waiter 和 pin。
 */
Datum
test_dsg_pin(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Buffer		buffer;
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));

	(void) test_dsg_shared_value();
	test_dsg_check(strcmp(mode, "hold") == 0 || strcmp(mode, "tail_hold") == 0 || strcmp(mode, "cleanup") == 0 ||
				   strcmp(mode, "read") == 0, "invalid pin test mode");
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	buffer = ReadBuffer(rel, PG_GETARG_INT32(1));
	test_dsg_check(!BufferIsLocal(buffer), "pin tests require a shared relation");
	if (strcmp(mode, "hold") == 0 || strcmp(mode, "tail_hold") == 0)
	{
		pg_atomic_uint32 *gate = &((DsgTestSharedState *) inherited_region.address)->pin_gate;

		if (strcmp(mode, "tail_hold") == 0)
			test_dsg_check(buffer > NBuffers, "test pin did not reach the shrink tail");

		/*-
		 * 外部测试先观察真实 pin 已建立，再启动 cleanup waiter；只有看到
		 * 原生 BufferPin 等待后才释放门禁。期限由调用方 statement_timeout
		 * 保证，循环显式处理取消，异常后也清除测试状态。
		 */
		test_dsg_wait_gate(gate, "DsgTestPinHolder");
	}
	else if (strcmp(mode, "cleanup") == 0)
	{
		LockBufferForCleanup(buffer);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}
	ReleaseBuffer(buffer);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(true);
}

/* 只操作测试门禁；false 观察 holder，true 请求放 pin，实际释放仍走 PG 原生路径。 */
Datum
test_dsg_pin_control(PG_FUNCTION_ARGS)
{
	pg_atomic_uint32 *gate;

	(void) test_dsg_shared_value();
	gate = &((DsgTestSharedState *) inherited_region.address)->pin_gate;
	if (PG_GETARG_BOOL(0))
	{
		pg_atomic_write_u32(gate, 2);
		PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(pg_atomic_read_u32(gate) == 1);
}

/* 仅供真实 checkpointer 的暂停/错误及独立工作区租约验证。 */
Datum
test_dsg_checkpoint_control(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(0));
	pg_atomic_uint32 *gate;
	bool		result = true;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	gate = &((DsgTestSharedState *) inherited_region.address)->checkpoint_gate;
	if (strcmp(action, "attach") == 0 || strcmp(action, "attach_error") == 0 ||
		strcmp(action, "attach_write_error") == 0)
	{
		bool		write_error = strcmp(action, "attach_write_error") == 0;

		InjectionPointAttach(write_error ? "dsg-checkpoint-before-write" : "dsg-checkpoint-after-scan",
							 "test_dsg_region",
							 strcmp(action, "attach") == 0 ?
							 "test_dsg_checkpoint_pause" : "test_dsg_checkpoint_fail", NULL, 0);
	}
	else if (strcmp(action, "release") == 0)
		pg_atomic_write_u32(gate, 2);
	else if (strcmp(action, "waiting") == 0)
		result = pg_atomic_read_u32(gate) == 1;
	else if (strcmp(action, "detach") == 0)
	{
		result = InjectionPointDetach("dsg-checkpoint-after-scan");
		result = InjectionPointDetach("dsg-checkpoint-before-write") || result;
	}
	else if (strcmp(action, "available") == 0)
	{
		result = LWLockConditionalAcquire(&DSBCtrl->checkpoint_lock, LW_EXCLUSIVE);
		if (result)
			LWLockRelease(&DSBCtrl->checkpoint_lock);
	}
	else
		ereport(ERROR, (errmsg("invalid DSGA checkpoint test action")));
	pfree(action);
	PG_RETURN_BOOL(result);
#else
	ereport(ERROR, (errmsg("DSGA checkpoint tests require injection points")));
	PG_RETURN_BOOL(false);
#endif
}

/*-
 * 在生产WaitIO已经撤CV、尚未放专属owner的pin时观察。ERROR时应仍持Guard，
 * FATAL时shmem_exit已放下Guard；两者均须真实持pin且不在CV队列。不能在
 * 外层清理已经放pin后再读Descriptor，也不替被测代码撤CV或释放资源。
 */
#ifdef USE_INJECTION_POINTS
void
test_dsg_io_audit(const char *name, const void *private_data)
{
	DsgTestSharedState *state = (DsgTestSharedState *) inherited_region.address;
	Buffer		buffer = (Buffer) pg_atomic_read_u32(&state->io_buffer);
	uint32		result = 0;

	if (!io_waiter_armed)
		return;
	CheckBufferIsPinnedOnce(buffer);
	{
		ConditionVariable *cv = BufferDescriptorGetIOCV(GetBufferDescriptor(buffer - 1));

		result |= 1;
		SpinLockAcquire(&cv->mutex);
		if (!proclist_contains(&cv->wakeup, MyProcNumber, cvWaitLink))
			result |= 2;
		SpinLockRelease(&cv->mutex);
	}
	if (LWLockHeldByMe(&DSBCtrl->access_lock))
		result |= 4;
	pg_atomic_write_u32(&state->io_audit, result);
	io_waiter_armed = false;
}

typedef struct DsgReleasingIOTest
{
	ResourceOwner owner;
	SMgrRelation smgr;
	bool		pending;
} DsgReleasingIOTest;

/*-
 * 由原生ResourceOwnerRelease设置真实releasing状态后触发Drop，与事务
 * 回滚删除relation时相同；不修改owner内部标志、不伪造IO/pin或CV状态。
 * 只进入一次，排除清理异常后再次递归Drop；smgr由外层真实关系锁保护。
 */
static void
test_dsg_releasing_io(ResourceReleasePhase phase, bool isCommit,
					  bool isTopLevel, void *arg)
{
	DsgReleasingIOTest *test = (DsgReleasingIOTest *) arg;

	if (phase == RESOURCE_RELEASE_BEFORE_LOCKS &&
		CurrentResourceOwner == test->owner && test->pending)
	{
		ForkNumber	forknum = MAIN_FORKNUM;
		BlockNumber first = 0;

		test->pending = false;
		DropRelationBuffers(test->smgr, &forknum, 1, &first);
	}
}
#endif

/*-
 * writer 使用真实 pin/content lock/StartBufferIO/写回；invalidate 持关系
 * 排他锁调用原生 Drop，不删文件、不改数据。仅把同一页面再次标脏以制造
 * 真实写回，测试不直接改 refcount、IO 位或 CV 队列。
 */
Datum
test_dsg_io(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	DsgTestSharedState *state = (DsgTestSharedState *) inherited_region.address;
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Relation	rel;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	if (strcmp(mode, "writer") == 0)
	{
		Buffer		buffer;

		rel = table_open(PG_GETARG_OID(0), AccessShareLock);
		buffer = ReadBuffer(rel, PG_NARGS() >= 3 ? PG_GETARG_INT32(2) : 0);
		test_dsg_check(!BufferIsLocal(buffer), "IO tests require a shared relation");
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		MarkBufferDirty(buffer);
		pg_atomic_write_u32(&state->io_buffer, (uint32) buffer);

		/*
		 * 原生写回不需要关系锁；让 invalidator 能像
		 * checkpointer 并发写时一样进入。
		 */
		table_close(rel, AccessShareLock);
		io_writer_armed = true;
		PG_TRY();
		{
			FlushOneBuffer(buffer);
		}
		PG_FINALLY();
		{
			io_writer_armed = false;
		}
		PG_END_TRY();
		UnlockReleaseBuffer(buffer);
	}
	else if (strcmp(mode, "invalidate") == 0 || strcmp(mode, "invalidate_release") == 0)
	{
		ForkNumber	forknum = MAIN_FORKNUM;
		BlockNumber first = 0;
		DsgReleasingIOTest *releasing = NULL;
		ResourceOwner saved_owner = CurrentResourceOwner;

		if (strcmp(mode, "invalidate_release") == 0)
			releasing = palloc0(sizeof(*releasing));

		rel = table_open(PG_GETARG_OID(0), AccessExclusiveLock);
		test_dsg_check(rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP,
					   "IO tests require a shared relation");
		io_waiter_armed = true;
		PG_TRY();
		{
			if (releasing != NULL)
			{
				releasing->owner = ResourceOwnerCreate(NULL, "DSGA releasing IO test");
				releasing->smgr = RelationGetSmgr(rel);
				releasing->pending = true;
				RegisterResourceReleaseCallback(test_dsg_releasing_io, releasing);
				ResourceOwnerRelease(releasing->owner, RESOURCE_RELEASE_BEFORE_LOCKS,
									 false, false);
			}
			else
				DropRelationBuffers(RelationGetSmgr(rel), &forknum, 1, &first);
		}
		PG_FINALLY();
		{
			io_waiter_armed = false;
			CurrentResourceOwner = saved_owner;
			if (releasing != NULL)
			{
				UnregisterResourceReleaseCallback(test_dsg_releasing_io, releasing);
				if (releasing->owner != NULL)
					ResourceOwnerDelete(releasing->owner);
			}
		}
		PG_END_TRY();
		if (releasing != NULL)
			pfree(releasing);

		/*
		 * 若 WaitIO 遗留自身 pin，InvalidateBuffer 的原生检查已报错。
		 */
		table_close(rel, AccessExclusiveLock);
	}
	else
		ereport(ERROR, (errmsg("invalid DSGA IO test mode")));
	pfree(mode);
	PG_RETURN_BOOL(true);
#else
	ereport(ERROR, (errmsg("DSGA IO tests require injection points")));
	PG_RETURN_BOOL(false);
#endif
}

Datum
test_dsg_io_control(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	DsgTestSharedState *state = (DsgTestSharedState *) inherited_region.address;
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(0));
	uint32		result = 1;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	if (strcmp(action, "attach") == 0)
	{
		InjectionPointAttach("dsg-buffer-before-write", "test_dsg_region",
							 "test_dsg_io_pause", NULL, 0);
		InjectionPointAttach("dsg-buffer-io-wait-prepared", "test_dsg_region",
							 "test_dsg_io_pause", NULL, 0);
		InjectionPointAttach("dsg-buffer-io-wait-cleaned", "test_dsg_region",
							 "test_dsg_io_audit", NULL, 0);
	}
	else if (strcmp(action, "writer_waiting") == 0)
		result = pg_atomic_read_u32(&state->io_writer_gate) == 1;
	else if (strcmp(action, "waiter_waiting") == 0)
		result = pg_atomic_read_u32(&state->io_waiter_gate) == 1;
	else if (strcmp(action, "release_writer") == 0)
		pg_atomic_write_u32(&state->io_writer_gate, 2);
	else if (strcmp(action, "release_waiter") == 0)
		pg_atomic_write_u32(&state->io_waiter_gate, 2);
	else if (strcmp(action, "audit") == 0)
		result = pg_atomic_read_u32(&state->io_audit);
	else if (strcmp(action, "waiter_alone") == 0)
	{
		DsgBufferAccess access = DsgBufferAccessBegin();
		Buffer		buffer = (Buffer) pg_atomic_read_u32(&state->io_buffer);

		result = 0;
		if (buffer > 0 && buffer <= access.count)
		{
			BufferDesc *buf = GetBufferDescriptor(buffer - 1);
			uint32		buf_state = LockBufHdr(buf);

			result = BUF_STATE_GET_REFCOUNT(buf_state) == 1 && !(buf_state & BM_IO_IN_PROGRESS);
			UnlockBufHdr(buf, buf_state);
		}
		DsgBufferAccessEnd(&access);
	}
	else if (strcmp(action, "attach_fast_drop") == 0)
		InjectionPointAttach("dsg-buffer-fast-drop-found", "test_dsg_region",
							 "test_dsg_fast_drop", NULL, 0);
	else if (strcmp(action, "fast_drop_found") == 0)
		result = pg_atomic_read_u32(&state->fast_drop_found);
	else if (strcmp(action, "detach") == 0)
	{
		InjectionPointDetach("dsg-buffer-before-write");
		InjectionPointDetach("dsg-buffer-io-wait-prepared");
		InjectionPointDetach("dsg-buffer-io-wait-cleaned");
		InjectionPointDetach("dsg-buffer-fast-drop-found");
	}
	else
		ereport(ERROR, (errmsg("invalid DSGA IO test action")));
	pfree(action);
	PG_RETURN_INT32((int32) result);
#else
	ereport(ERROR, (errmsg("DSGA IO tests require injection points")));
	PG_RETURN_INT32(0);
#endif
}

/*-
 * 选取扩容后普通 SQL 已使用的干净尾部页，正常 pin 后开启 Neon test evict。
 * 最后 unpin 的注入点将整张 Descriptor 页设为不可访问；ReleaseBuffer
 * 返回后立即恢复。若释放后路径仍碰 Descriptor，真实进程会触发访问错误。
 * 该探针验证访问寿命，不代表 Provider 或 Linux/VM 已完成物理释放。
 */
Datum
test_dsg_unpin_probe(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	DsgBufferAccess access;
	Buffer		buffer = InvalidBuffer;
	bool		old_evict = neon_test_evict;
	volatile bool attached = false;
	int			i;

	(void) test_dsg_shared_value();
	test_dsg_check(DSBCtrl != NULL, "dynamic buffers are disabled");
	probe_bytes = DSBCtrl->arrays[DSG_BUFFER_DESCRIPTORS].region.page_bytes;
	access = DsgBufferAccessBegin();
	for (i = NBuffers; i < access.count; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		uint32		state = LockBufHdr(buf);
		BufferTag	tag = buf->tag;
		bool		eligible = (state & (BM_VALID | BM_TAG_VALID | BM_DIRTY | BM_IO_IN_PROGRESS)) ==
			(BM_VALID | BM_TAG_VALID) && BUF_STATE_GET_REFCOUNT(state) == 0 &&
			((char *) buf - (char *) BufferDescriptors) >=
			TYPEALIGN(probe_bytes, mul_size(NBuffers, sizeof(BufferDescPadded)));

		UnlockBufHdr(buf, state);
		if (eligible && ReadRecentBuffer(BufTagGetRelFileLocator(&tag), tag.forkNum,
										 tag.blockNum, i + 1))
		{
			buffer = i + 1;
			probe_page = (void *) ((uintptr_t) buf & ~(uintptr_t) (probe_bytes - 1));
			break;
		}
	}
	DsgBufferAccessEnd(&access);
	test_dsg_check(BufferIsValid(buffer), "no clean grown descriptor available for probe");
	PG_TRY();
	{
		InjectionPointAttach("dsg-buffer-after-unpin", "test_dsg_region",
							 "test_dsg_protect_descriptor", NULL, 0);
		attached = true;
		probe_protected = false;
		probe_armed = true;
		neon_test_evict = true;
		ReleaseBuffer(buffer);
	}
	PG_FINALLY();
	{
		/*
		 * 恢复权限必须在任何进一步的 PG 资源/目录操作之前完成。
		 */
		probe_armed = false;
		if (probe_protected && mprotect(probe_page, probe_bytes, PROT_READ | PROT_WRITE) != 0)
			ereport(FATAL, (errmsg("could not restore DSGA test descriptor: %m")));
		neon_test_evict = old_evict;
		if (attached)
			InjectionPointDetach("dsg-buffer-after-unpin");
	}
	PG_END_TRY();
	test_dsg_check(probe_protected, "after-unpin probe was not reached");
	probe_protected = false;
	PG_RETURN_BOOL(true);
#else
	ereport(ERROR, (errmsg("DSGA unpin probe requires injection points")));
	PG_RETURN_BOOL(false);
#endif
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
