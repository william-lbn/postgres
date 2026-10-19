/*-------------------------------------------------------------------------
 * dsg_buffer_grow.c -- 普通页 Buffer Adapter 的串行扩容协调。
 *
 * 只增加已初始化前缀，不移动共享地址，不改变现有页面/脏页/WAL 语义。
 * 操作日志是共享内存事实，不随调用事务回滚；丢失响应须用同一请求重放。
 * 本模块没有 VM 许可，也不实现缩容；外层仍须先证明实际资源已增加。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/ipc.h"
#include "utils/injection_point.h"
#include "utils/wait_event.h"

static void DsgGrowDeny(DsgBufferOperationReason reason);
static bool DsgGrowSameRequest(const DsgBufferGrowRequest *a, const DsgBufferGrowRequest *b);

/*-
 * 回调只能清理本次真正建立的操作，不能把共享目录的当前操作当成自己。
 * 上下文在 PG_TRY 之前分配，指针此后不变；将可变标志放在堆上，避免
 * longjmp 后读取已修改的非 volatile 自动变量。事务内存清理晚于本回调。
 */
typedef struct DsgGrowCleanup
{
	DsgBufferGrowRequest request;
	bool		operation_started;
} DsgGrowCleanup;

void
DsgBufferGetSnapshot(DsgBufferSnapshot *snapshot)
{
	int			i;

	if (DSBCtrl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dynamic shared buffers are disabled")));
	MemSet(snapshot, 0, sizeof(*snapshot));
	SpinLockAcquire(&DSBCtrl->status_lock);
	snapshot->operation = DSBCtrl->operation;
	snapshot->boot_id = DSBCtrl->boot_id;
	snapshot->generation = pg_atomic_read_u64(&DSBCtrl->generation);
	snapshot->low = GetLowNBuffers();
	snapshot->high = GetHighNBuffers();
	snapshot->maximum = DSBCtrl->maximum;
	snapshot->admission_open = pg_atomic_read_u32(&DSBCtrl->admission_open) != 0;
	snapshot->admission_revision = DSBCtrl->admission_revision;
	snapshot->startup_shared_upper_bytes = DSBCtrl->startup_shared_upper_bytes;
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		snapshot->committed_upper_bytes[i] = DSBCtrl->arrays[i].committed_upper_bytes;
		snapshot->allocation_unknown[i] = DSBCtrl->arrays[i].allocation_unknown;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
}

/*-
 * 用 monotonic 时钟限时，避免墙钟校时改变等待预算。不能阻塞式取得
 * LWLock 后才检查取消；所有竞争等待都用条件取得及可中断 Latch。
 * 成功返回时保留原生 LWLock 的 holdoff，调用者明确决定何时抵消。
 */
static bool
DsgGrowExpired(instr_time started, int timeout_ms)
{
	instr_time	elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed) >= timeout_ms;
}

static bool
DsgGrowAcquire(LWLock *lock, instr_time started, int timeout_ms)
{
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (DsgGrowExpired(started, timeout_ms))
			return false;
		if (LWLockConditionalAcquire(lock, LW_EXCLUSIVE))
			return true;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L, WAIT_EVENT_DSG_BUFFER_RESIZE);
		ResetLatch(MyLatch);
	}
}

/*-
 * 调用者持 resize_lock，Granule 目录由该锁串行保护；状态观察者不能
 * 在只持 status_lock 时扫描整个目录。只修改本操作涉及的范围，避免
 * 在短 spinlock 内遍历全部 M，也避免每次小扩容扫描整个预留目录。
 */
static void
DsgGrowGranules(int active, int prepared, DsgBufferGranuleState pending,
				uint64 generation)
{
	int			first = DSBCtrl->operation.original_buffers / DSG_BUFFER_GRANULE_BUFFERS;
	int			end = 1 + (DSBCtrl->operation.request.target_buffers - 1) / DSG_BUFFER_GRANULE_BUFFERS;
	int			i;

	for (i = first; i < end; i++)
	{
		DsgBufferGranule *granule = &DSBCtrl->granules[i];
		int			nactive = Min(granule->maximum_buffers, Max(0, active - granule->first_buffer));
		int			nprepared = Min(granule->maximum_buffers, Max(0, prepared - granule->first_buffer));

		granule->active_buffers = nactive;
		granule->prepared_buffers = nprepared;
		granule->generation = generation;
		granule->state = nprepared > nactive ? pending :
			(nactive > 0 ? DSG_GRANULE_ACTIVE : DSG_GRANULE_RESERVED);
	}
}

/*-
 * ERROR/FATAL 都会经过此清理，SIGKILL 则由 Postmaster 重建整个 boot。
 * PREPARING 时 H/L 没变，只撤销目录中的“待发布”，不虚报物理释放。
 * Provider 调用前已写入预算；中断可能留下未知分配，必须保留这些事实。
 * COMMITTED 后绝不把已发布的容量回退，客户端须查询结果或幂等重放。
 */
static void
DsgGrowInterrupted(int code, Datum arg)
{
	DsgGrowCleanup *cleanup = (DsgGrowCleanup *) DatumGetPointer(arg);
	bool		acquired = false;
	bool		own_preparing;

	if (!cleanup->operation_started)
		return;

	/*-
	 * 普通 ERROR 局部展开时仍持串行锁；FATAL 的 shmem_exit 却先释放
	 * 所有 LWLock，再运行退出回调。后者必须重新条件取得串行权，不能
	 * 等待正在执行的新操作，也不能在别人持锁时更改其目录/终态。
	 * 取得失败就保留旧预算事实；下一协调者按 revision/当前事实对账。
	 */
	if (!LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE))
	{
		if (!LWLockConditionalAcquire(&DSBCtrl->resize_lock, LW_EXCLUSIVE))
			return;
		acquired = true;
	}
	SpinLockAcquire(&DSBCtrl->status_lock);
	own_preparing = DSBCtrl->operation.phase == DSG_OP_PREPARING &&
		DsgGrowSameRequest(&DSBCtrl->operation.request, &cleanup->request);
	SpinLockRelease(&DSBCtrl->status_lock);
	if (own_preparing)
		DsgGrowDeny(DSG_REASON_INTERRUPTED);
	if (acquired)
		LWLockRelease(&DSBCtrl->resize_lock);
}

static bool
DsgGrowSameRequest(const DsgBufferGrowRequest *a, const DsgBufferGrowRequest *b)
{
	return a->boot_id == b->boot_id && a->revision == b->revision &&
		a->operation_id_high == b->operation_id_high && a->operation_id_low == b->operation_id_low &&
		a->expected_generation == b->expected_generation && a->target_buffers == b->target_buffers;
}

static void
DsgGrowDeny(DsgBufferOperationReason reason)
{
	Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
	DsgGrowGranules(GetHighNBuffers(), GetHighNBuffers(), DSG_GRANULE_RESERVED,
					pg_atomic_read_u64(&DSBCtrl->generation));
	SpinLockAcquire(&DSBCtrl->status_lock);
	DSBCtrl->operation.phase = DSG_OP_DENIED;
	DSBCtrl->operation.reason = reason;
	SpinLockRelease(&DSBCtrl->status_lock);
}

/*-
 * 所有分配、预提交及对象初始化都在数组独占锁外。新 ID 尚未发布，
 * 现有查询可继续访问旧前缀。独占发布只追加 freelist 并改变 H/L，
 * 其中不能分配内存、做 IO、等待、调用注入点或报告可恢复 ERROR。
 */
static bool
DsgGrowPrepareAndPublish(const DsgBufferGrowRequest *request,
						 instr_time started, int timeout_ms)
{
	int			old_high = GetHighNBuffers();
	int			target = request->target_buffers;
	uint64		generation = pg_atomic_read_u64(&DSBCtrl->generation);
	int			i;

	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		DsgBufferArray *array = &DSBCtrl->arrays[i];
		int			first = old_high;

		/*-
		 * 缩容可因旧 checkpoint 的租约而保留较大的排序工作区。该前缀
		 * 已准备且可能正在排序/读取，不能从较小 H 起再次 memset。只为
		 * 尚未证明准备的连续尾部调用 Provider，保留其原有预算与内容。
		 */
		if (i == DSG_BUFFER_CHECKPOINT)
			first = Max(first, (int) pg_atomic_read_u32(&DSBCtrl->checkpoint_prepared_buffers));

		/*
		 * 按 Granule 分批，避免大请求在一次预触页中消耗全部等待预算。
		 */
		while (first < target)
		{
			int			count = Min(target - first,
									DSG_BUFFER_GRANULE_BUFFERS - first % DSG_BUFFER_GRANULE_BUFFERS);
			Size		start = mul_size(first, array->element_bytes);
			Size		end = mul_size(first + count, array->element_bytes);
			Size		upper = TYPEALIGN(array->region.page_bytes, end);
			Size		previous_upper;
			DsgMemoryResult result;

			CHECK_FOR_INTERRUPTS();
			if (DsgGrowExpired(started, timeout_ms))
			{
				DsgGrowDeny(DSG_REASON_TIMEOUT);
				return false;
			}
			/*-
			 * 系统调用前先计入最坏成本，错误/取消不能留下已触页却零成本。
			 * 只有本轮从旧 H 起连续覆盖全部未知尾部，才可清除 unknown；
			 * 较小重试不得抹掉先前较大失败请求的预算。这里不是 RSS。
			 */
			SpinLockAcquire(&DSBCtrl->status_lock);
			previous_upper = array->committed_upper_bytes;
			array->committed_upper_bytes = Max(previous_upper, upper);
			array->allocation_unknown = true;
			SpinLockRelease(&DSBCtrl->status_lock);
			result = DsgRegionPopulate(&array->region, start, end - start);
			if (result.status != DSG_MEMORY_OK)
			{
				DsgGrowDeny(DSG_REASON_POPULATE_FAILED);
				return false;
			}
			SpinLockAcquire(&DSBCtrl->status_lock);
			if (upper >= previous_upper)
				array->allocation_unknown = false;
			if (i == DSG_BUFFER_CHECKPOINT)
				pg_atomic_write_u32(&DSBCtrl->checkpoint_prepared_buffers, first + count);
			SpinLockRelease(&DSBCtrl->status_lock);
			first += count;
		}
	}

	for (i = old_high; i < target; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);

		CHECK_FOR_INTERRUPTS();
		if (i % 128 == 0 && DsgGrowExpired(started, timeout_ms))
		{
			DsgGrowDeny(DSG_REASON_TIMEOUT);
			return false;
		}
		ClearBufferTag(&buf->tag);
		pg_atomic_init_u32(&buf->state, 0);
		buf->wait_backend_pgprocno = INVALID_PROC_NUMBER;
		buf->buf_id = i;
		buf->freeNext = i + 1 < target ? i + 1 : FREENEXT_END_OF_LIST;
		LWLockInitialize(BufferDescriptorGetContentLock(buf), LWTRANCHE_BUFFER_CONTENT);
		ConditionVariableInit(BufferDescriptorGetIOCV(buf));
	}
	DsgGrowGranules(old_high, target, DSG_GRANULE_READY, generation);
	INJECTION_POINT("dsg-buffer-grow-before-publish");
	if (!DsgGrowAcquire(&DSBCtrl->access_lock, started, timeout_ms))
	{
		DsgGrowDeny(DSG_REASON_TIMEOUT);
		return false;
	}

	StrategyAddBufferRange(old_high, target - old_high);
	DsgGrowGranules(target, target, DSG_GRANULE_ACTIVE, generation + 1);
	SpinLockAcquire(&DSBCtrl->status_lock);

	/*
	 * H 先于 L 发布；对象已完成初始化，选择器仍被独占锁排除。
	 */
	pg_write_barrier();
	pg_atomic_write_u32(&DSBCtrl->high, target);
	pg_atomic_write_u32(&DSBCtrl->low, target);
	pg_atomic_write_u64(&DSBCtrl->generation, generation + 1);
	DSBCtrl->operation.phase = DSG_OP_COMMITTED;
	SpinLockRelease(&DSBCtrl->status_lock);
	LWLockRelease(&DSBCtrl->access_lock);

	/*
	 * 发布后响应丢失也必须保留 COMMITTED，不能按 SQL ERROR 回退容量。
	 */
	INJECTION_POINT("dsg-buffer-grow-after-publish");
	return true;
}

bool
DsgBufferGrow(const DsgBufferGrowRequest *request, int timeout_ms, DsgBufferSnapshot *snapshot)
{
	instr_time	started;
	DsgBufferOperation previous;
	DsgGrowCleanup *cleanup;
	uint64		generation;
	bool		committed = false;
	bool		replayed = false;

	if (DSBCtrl == NULL || !IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dynamic buffer growth requires an enabled running server")));
	if (request->boot_id != DSBCtrl->boot_id || request->revision == 0 ||
		(request->operation_id_high == 0 && request->operation_id_low == 0) ||
		request->target_buffers < 16 || request->target_buffers > GetMaxNBuffers() ||
		timeout_ms < 1 || timeout_ms > 60000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid or stale DSGA grow request")));

	/*
	 * 不允许读侧锁升级，也不能由已经持有本协调锁的路径递归调用。
	 */
	if (LWLockHeldByMe(&DSBCtrl->access_lock) || LWLockHeldByMe(&DSBCtrl->resize_lock))
		elog(ERROR, "DSGA grow cannot run while holding buffer coordination locks");
	INSTR_TIME_SET_CURRENT(started);
	if (!DsgGrowAcquire(&DSBCtrl->resize_lock, started, timeout_ms))
	{
		DsgBufferGetSnapshot(snapshot);
		return false;
	}

	cleanup = palloc0(sizeof(*cleanup));
	cleanup->request = *request;
	/*- 与长遍历 Guard 相同：协调期间允许取消，释放时再成对 HOLD。 */
	RESUME_INTERRUPTS();
	PG_ENSURE_ERROR_CLEANUP(DsgGrowInterrupted, PointerGetDatum(cleanup));
	{
		SpinLockAcquire(&DSBCtrl->status_lock);
		previous = DSBCtrl->operation;
		generation = pg_atomic_read_u64(&DSBCtrl->generation);
		SpinLockRelease(&DSBCtrl->status_lock);
		if (previous.phase != DSG_OP_IDLE &&
			previous.request.operation_id_high == request->operation_id_high &&
			previous.request.operation_id_low == request->operation_id_low)
		{
			if (!DsgGrowSameRequest(&previous.request, request))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("DSGA operation identity was reused with a different request")));
			replayed = true;
			committed = previous.phase == DSG_OP_COMMITTED;
		}
		else
		{
			if (request->revision <= previous.request.revision ||
				request->expected_generation != generation || generation == PG_UINT64_MAX ||
				request->target_buffers < GetHighNBuffers() || GetLowNBuffers() != GetHighNBuffers())
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("DSGA grow request conflicts with current capacity or revision")));
			SpinLockAcquire(&DSBCtrl->status_lock);
			DSBCtrl->operation.request = *request;
			DSBCtrl->operation.original_buffers = GetHighNBuffers();
			DSBCtrl->operation.phase = DSG_OP_PREPARING;
			DSBCtrl->operation.reason = DSG_REASON_NONE;
			cleanup->operation_started = true;
			SpinLockRelease(&DSBCtrl->status_lock);
			DsgGrowGranules(GetHighNBuffers(), request->target_buffers, DSG_GRANULE_PREPARING, generation);
			if (request->target_buffers == GetHighNBuffers())
			{
				SpinLockAcquire(&DSBCtrl->status_lock);
				DSBCtrl->operation.phase = DSG_OP_COMMITTED;
				SpinLockRelease(&DSBCtrl->status_lock);
				committed = true;
			}
			else
				committed = DsgGrowPrepareAndPublish(request, started, timeout_ms);
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(DsgGrowInterrupted, PointerGetDatum(cleanup));
	pfree(cleanup);
	DsgBufferGetSnapshot(snapshot);
	snapshot->replayed = replayed;
	HOLD_INTERRUPTS();
	LWLockRelease(&DSBCtrl->resize_lock);
	return committed;
}
