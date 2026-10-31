/*-------------------------------------------------------------------------
 * dsg_buffer_shrink.c -- DSGA尾部排水、逻辑收缩和逐数组保守回收。
 *
 * resize_lock覆盖完整操作和全部同步Provider调用；access_lock仅用于L栅栏
 * 与H发布/读侧排水，不包住数据写回或物理回收。PG逻辑提交和物理回收分开，
 * 本模块永远不直接授权VM动作。Mac可验证真实逻辑收缩，不能虚报物理完成。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/dsg_buffer_arrays.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/injection_point.h"
#include "utils/wait_event.h"

typedef struct DsgShrinkCleanup
{
	DsgBufferResizeRequest request;
	bool		operation_started;
} DsgShrinkCleanup;

static bool
DsgShrinkSameRequest(const DsgBufferResizeRequest *a, const DsgBufferResizeRequest *b)
{
	return DsgBufferSameRequest(a, b);
}

static bool
DsgShrinkExpired(instr_time started, int timeout_ms)
{
	instr_time	elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed) >= timeout_ms;
}

/* 条件取锁+可中断Latch，不为等待串行权/长Guard吞掉取消或期限。 */
static bool
DsgShrinkAcquire(LWLock *lock, instr_time started, int timeout_ms,
				 const DsgBufferResizeRequest *request)
{
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * resize串行等待尚未接受；发布等待已绑定本操作，可以响应业务取消。
		 */
		if (request != NULL && DsgBufferCancelRequested(request))
			return false;
		if (DsgShrinkExpired(started, timeout_ms))
			return false;
		if (LWLockConditionalAcquire(lock, LW_EXCLUSIVE))
			return true;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}
}

/* 目录只在resizeX下访问；physical/unknown事实另存数组账本，不能由RESERVED推断。 */
static void
DsgShrinkGranules(int high, DsgBufferGranuleState state, uint64 generation)
{
	int			first = DSBCtrl->operation.request.target_buffers / DSG_BUFFER_GRANULE_BUFFERS;
	int			end = 1 + (DSBCtrl->operation.original_buffers - 1) / DSG_BUFFER_GRANULE_BUFFERS;
	int			i;

	for (i = first; i < end; i++)
	{
		DsgBufferGranule *granule = &DSBCtrl->granules[i];

		granule->active_buffers = Min(granule->maximum_buffers, Max(0, high - granule->first_buffer));
		granule->prepared_buffers = granule->active_buffers;
		granule->generation = generation;
		granule->state = granule->active_buffers > 0 ? state : DSG_GRANULE_RESERVED;
	}
}

/* H尚未改变，可以恢复L；已正常驱逐的页不必回装，数据继续由原生存储承载。 */
static void
DsgShrinkDeny(DsgBufferOperationReason reason)
{
	Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
	StrategyRestoreAllocationLimit();
	DsgShrinkGranules(GetHighNBuffers(), DSG_GRANULE_ACTIVE,
					  pg_atomic_read_u64(&DSBCtrl->generation));
	SpinLockAcquire(&DSBCtrl->status_lock);
	DSBCtrl->operation.phase = DSG_OP_DENIED;
	DSBCtrl->operation.reason = reason;
	SpinLockRelease(&DSBCtrl->status_lock);
}

/*-
 * 已取得resizeX而共享状态仍PREPARING，说明原Shrink执行者已退出并放锁。
 * FATAL原生放锁先于退出回调，旧回调可能抢不到串行权。新协调者须主动
 * 恢复遗留L，不能永久卡在L<H；也不能将旧PREPARING当成仍在执行的承诺。
 * 同步Provider尚未开始时H未变；H发布后phase已COMMITTED，不走此补偿。
 */
void
DsgBufferReconcileInterruptedShrink(void)
{
	bool		abandoned;

	Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
	SpinLockAcquire(&DSBCtrl->status_lock);
	abandoned = DSBCtrl->operation.kind == DSG_OP_SHRINK &&
		DSBCtrl->operation.phase == DSG_OP_PREPARING;
	SpinLockRelease(&DSBCtrl->status_lock);
	if (abandoned)
		DsgShrinkDeny(DSG_REASON_INTERRUPTED);
}

/*-
 * 与Grow相同，堆上上下文避免longjmp读取已改变的自动变量。清理绑定完整
 * 身份；FATAL放锁后只条件重获，禁止阻塞新操作或修改别人的PREPARING。
 * COMMITTED后不恢复H、不补放页，不允许旧退出回调回收新Grow刚使用的尾部。
 */
static void
DsgShrinkInterrupted(int code, Datum arg)
{
	DsgShrinkCleanup *cleanup = (DsgShrinkCleanup *) DatumGetPointer(arg);
	bool		acquired = false;
	bool		own_preparing;

	if (!cleanup->operation_started)
		return;
	if (!LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE))
	{
		if (!LWLockConditionalAcquire(&DSBCtrl->resize_lock, LW_EXCLUSIVE))
			return;
		acquired = true;
	}
	SpinLockAcquire(&DSBCtrl->status_lock);
	own_preparing = DSBCtrl->operation.kind == DSG_OP_SHRINK &&
		DSBCtrl->operation.phase == DSG_OP_PREPARING &&
		DsgShrinkSameRequest(&DSBCtrl->operation.request, &cleanup->request);
	SpinLockRelease(&DSBCtrl->status_lock);
	if (own_preparing)
		DsgShrinkDeny(DSG_REASON_INTERRUPTED);
	if (acquired)
		LWLockRelease(&DSBCtrl->resize_lock);
}

/*-
 * 每页正常写净/撤tag后，在L栅栏下不能再成为新victim。所有页面都成功的
 * 一轮扫描构成单调排水证明；最后accessX等待旧Guard/未持pin的CV退出，
 * 不在发布锁内重复全池长扫描。hash-hit重pin由mapping锁与header核验排除。
 * 本轮的分类计数是进度，不是未来工作/私有内存的预算证明。
 */
static bool
DsgShrinkDrain(const DsgBufferResizeRequest *request, int target, int original, instr_time started, int timeout_ms)
{
	for (;;)
	{
		uint32		counts[DSG_DRAIN_RESULT_COUNT] = {0};
		int			i;

		for (i = target; i < original; i++)
		{
			DsgBufferDrainResult result;

			CHECK_FOR_INTERRUPTS();
			if (i % 128 == 0 && DsgBufferCancelRequested(request))
				return false;
			if (i % 128 == 0 && DsgShrinkExpired(started, timeout_ms))
				return false;
			result = DsgBufferDrainOne(i);
			counts[result]++;
			if ((i + 1) % 128 == 0 || i == original - 1)
			{
				SpinLockAcquire(&DSBCtrl->status_lock);
				memcpy(DSBCtrl->operation.drain_counts, counts, sizeof(counts));
				SpinLockRelease(&DSBCtrl->status_lock);
			}
		}
		if (counts[DSG_DRAINED] == original - target)
			return true;
		if (DsgShrinkExpired(started, timeout_ms))
			return false;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}
}

/*-
 * 物理调用在accessX外，但完整保留resizeX，防止旧Release与regrow竞争。
 * 系统调用前保留原上界并记pending/未知；取消、错误或响应丢失都不能把已
 * 计入成本抹掉。只有ReleaseTail的全安全尾部回执可降低预算；边界页始终保留。
 * checkpoint独占租约仅条件取得，忙则保留工作区和预算，由新revision重试。
 */
static void
DsgShrinkRelease(int target)
{
	int			i;

	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		DsgBufferArray *array = &DSBCtrl->arrays[i];
		Size		offset = mul_size(target, array->element_bytes);
		Size		upper = TYPEALIGN(array->region.page_bytes, offset);
		bool		checkpoint = i == DSG_BUFFER_CHECKPOINT;
		DsgMemoryResult result;

		CHECK_FOR_INTERRUPTS();
		if (checkpoint && !LWLockConditionalAcquire(&DSBCtrl->checkpoint_lock, LW_EXCLUSIVE))
		{
			SpinLockAcquire(&DSBCtrl->status_lock);
			array->release_state = DSG_RELEASE_WAIT_CHECKPOINT;
			SpinLockRelease(&DSBCtrl->status_lock);
			continue;
		}
		SpinLockAcquire(&DSBCtrl->status_lock);

		/*
		 * 已获独占租约，后续regrow必须重新准备未知/已释放尾部，不复用旧内容。
		 */
		if (checkpoint)
			pg_atomic_write_u32(&DSBCtrl->checkpoint_prepared_buffers, target);
		if (array->committed_upper_bytes <= upper && !array->allocation_unknown)
		{
			MemSet(&result, 0, sizeof(result));
			result.status = DSG_MEMORY_NOOP;
			array->release_state = DSG_RELEASE_VERIFIED;
			array->release_result = result;
			SpinLockRelease(&DSBCtrl->status_lock);
		}
		else
		{
			SpinLockRelease(&DSBCtrl->status_lock);
			result = DsgRegionReleaseTail(&array->region, offset);
			SpinLockAcquire(&DSBCtrl->status_lock);
			array->release_result = result;
			if (result.physical_release_verified)
			{
				array->release_state = DSG_RELEASE_VERIFIED;
				array->committed_upper_bytes = Min(array->committed_upper_bytes, upper);
				array->allocation_unknown = false;
			}
			else
				array->release_state = result.status == DSG_MEMORY_UNSUPPORTED ?
					DSG_RELEASE_UNSUPPORTED : DSG_RELEASE_FAILED;
			SpinLockRelease(&DSBCtrl->status_lock);
		}
		if (checkpoint)
			LWLockRelease(&DSBCtrl->checkpoint_lock);
	}
}


/*-
 * PREPARING前后只有resizeX串行权；H发布区保持固定次数状态写入。
 * 目录遍历、正常IO与Provider均在accessX之外，发布失败只由外层恢复L。
 */
static bool
DsgShrinkPrepareAndPublish(const DsgBufferResizeRequest *request,
						   instr_time started, int timeout_ms)
{
	int			original = GetHighNBuffers();
	uint64		generation = pg_atomic_read_u64(&DSBCtrl->generation);
	int			i;

	if (!DsgShrinkAcquire(&DSBCtrl->access_lock, started, timeout_ms, request))
		return false;
	StrategyBeginAllocationFence(request->target_buffers);
	LWLockRelease(&DSBCtrl->access_lock);
	while (!StrategyFilterAllocationFence(128))
	{
		CHECK_FOR_INTERRUPTS();
		if (DsgBufferCancelRequested(request))
			return false;
		if (DsgShrinkExpired(started, timeout_ms))
			return false;
		INJECTION_POINT("dsg-buffer-shrink-filter-batch", NULL);
	}
	INJECTION_POINT("dsg-buffer-shrink-after-fence", NULL);
	if (!DsgShrinkDrain(request, request->target_buffers, original, started, timeout_ms))
		return false;
	INJECTION_POINT("dsg-buffer-shrink-before-publish", NULL);
	if (!DsgShrinkAcquire(&DSBCtrl->access_lock, started, timeout_ms, request))
		return false;

	/* 18 AIO的pin/wref已清不等于最后IOCV/waiter已结束。accessX下只try，
	 * 忙立即退回原H/成本，外层按原协议否决/恢复L；绝不能持X等callback。 */
	if(!DsgBufferArraysTryCompletionBarrier()){
	LWLockRelease(&DSBCtrl->access_lock);
	return false;
	}
	if (!DsgBufferBeginCommit(request))
	{
	DsgBufferArraysEndCompletionBarrier();
		LWLockRelease(&DSBCtrl->access_lock);
		return false;
	}
	if (original != request->target_buffers)
		generation++;
	SpinLockAcquire(&DSBCtrl->status_lock);
	pg_atomic_write_u32(&DSBCtrl->high, request->target_buffers);
	pg_atomic_write_u64(&DSBCtrl->generation, generation);
	DSBCtrl->operation.phase = DSG_OP_COMMITTED;
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		DsgBufferArray *array = &DSBCtrl->arrays[i];

		array->release_state = DSG_RELEASE_PENDING;
		array->release_issuer_epoch = request->issuer_epoch;
		array->release_revision = request->revision;
		array->release_operation_id_high = request->operation_id_high;
		array->release_operation_id_low = request->operation_id_low;
		array->release_generation = generation;
		MemSet(&array->release_result, 0, sizeof(array->release_result));
		array->release_result.release_may_be_partial = true;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	DsgBufferArraysEndCompletionBarrier();
	LWLockRelease(&DSBCtrl->access_lock);
	DsgShrinkGranules(request->target_buffers, DSG_GRANULE_ACTIVE, generation);
	INJECTION_POINT("dsg-buffer-shrink-after-publish", NULL);
	DsgShrinkRelease(request->target_buffers);
	return true;
}

bool
DsgBufferShrinkWithReceipt(const DsgBufferResizeRequest *request, int timeout_ms,
						   DsgBufferSnapshot *snapshot, DsgBufferReceipt *receipt)
{
	instr_time	started;
	DsgBufferOperation previous;
	DsgShrinkCleanup *cleanup;
	uint64		generation;
	int			original;
	int			replay_match;
	bool		committed = false;
	bool		replayed = false;

	if (DSBCtrl == NULL || !IsUnderPostmaster)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("dynamic buffer shrink requires an enabled running server")));
	if (request->boot_id != DSBCtrl->boot_id || request->revision == 0 ||
		(request->operation_id_high == 0 && request->operation_id_low == 0) ||
		request->target_buffers < NBuffers || request->target_buffers > GetMaxNBuffers() ||
		timeout_ms < 1 || timeout_ms > 60000)
	{
		if (receipt != NULL)
		{
			receipt->disposition = request->boot_id != DSBCtrl->boot_id ?
				DSG_RECEIPT_STALE : DSG_RECEIPT_REJECTED;
			DsgBufferGetSnapshot(snapshot);
			return false;
		}
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid or stale DSGA shrink request")));
	}
	if (LWLockHeldByMe(&DSBCtrl->access_lock) || LWLockHeldByMe(&DSBCtrl->resize_lock))
		elog(ERROR, "DSGA shrink cannot run while holding buffer coordination locks");
	INSTR_TIME_SET_CURRENT(started);
	if (!DsgShrinkAcquire(&DSBCtrl->resize_lock, started, timeout_ms, NULL))
	{
		if (receipt != NULL)
			receipt->disposition = DSG_RECEIPT_BUSY;
		DsgBufferGetSnapshot(snapshot);
		return false;
	}
	cleanup = palloc0(sizeof(*cleanup));
	cleanup->request = *request;
	RESUME_INTERRUPTS();
	PG_ENSURE_ERROR_CLEANUP(DsgShrinkInterrupted, PointerGetDatum(cleanup));
	{
		DsgBufferReconcileInterruptedShrink();
		SpinLockAcquire(&DSBCtrl->status_lock);
		previous = DSBCtrl->operation;
		generation = pg_atomic_read_u64(&DSBCtrl->generation);
		original = GetHighNBuffers();
		SpinLockRelease(&DSBCtrl->status_lock);
		replay_match = DsgBufferFindReplay(request, DSG_OP_SHRINK, snapshot);
		if (replay_match != 0)
		{
			if (replay_match < 0)
			{
				if (receipt != NULL)
					receipt->disposition = DSG_RECEIPT_CONFLICT;
				else
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									errmsg("DSGA operation identity was reused with a different request")));
			}
			else
			{
				replayed = true;
				committed = snapshot->operation.phase == DSG_OP_COMMITTED;
			}
		}
		else if (!DsgBufferRequestNewer(request, &previous.request) ||
				 request->expected_generation != generation || GetLowNBuffers() != original ||
				 request->target_buffers > original ||
				 (generation == PG_UINT64_MAX && request->target_buffers != original))
		{
			if (receipt != NULL)
				receipt->disposition = DSG_RECEIPT_STALE;
			else
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("DSGA shrink request conflicts with current capacity or revision")));
		}
		else
		{
			/*
			 * 新请求校验成功后才归档旧Grow，拒绝不能改变另一操作的状态。
			 */
			DsgBufferReconcileInterruptedGrow();
			INJECTION_POINT("dsg-buffer-before-operation-register", NULL);
			if (!DsgBufferStartOperation(request, DSG_OP_SHRINK, original))
			{
				if (receipt != NULL)
					receipt->disposition = DSG_RECEIPT_STALE;
				else
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									errmsg("DSGA issuer epoch was fenced before operation registration")));
			}
			else
			{
				cleanup->operation_started = true;
				DsgShrinkGranules(original, DSG_GRANULE_DRAINING, generation);
				committed = DsgShrinkPrepareAndPublish(request, started, timeout_ms);
				if (!committed)
					DsgShrinkDeny(DsgBufferCancelRequested(request) ? DSG_REASON_CANCELLED : DSG_REASON_TIMEOUT);
			}
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(DsgShrinkInterrupted, PointerGetDatum(cleanup));
	if (!replayed)
		DsgBufferGetSnapshot(snapshot);
	snapshot->replayed = replayed;
	if (receipt != NULL && (cleanup->operation_started || replayed))
	{
		receipt->disposition = replayed ? DSG_RECEIPT_REPLAYED : DSG_RECEIPT_ACCEPTED;
		receipt->has_result = true;
		receipt->result = *snapshot;
	}
	pfree(cleanup);
	HOLD_INTERRUPTS();
	LWLockRelease(&DSBCtrl->resize_lock);
	return committed;
}

bool
DsgBufferShrink(const DsgBufferResizeRequest *request, int timeout_ms, DsgBufferSnapshot *snapshot)
{
	return DsgBufferShrinkWithReceipt(request, timeout_ms, snapshot, NULL);
}
