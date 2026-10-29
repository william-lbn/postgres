/*-------------------------------------------------------------------------
 * dsg_buffer_control.c -- 本boot有界回执、历史查找和身份绑定的业务取消。
 *
 * 不提供VM权限或外层预算认证。resize_lock串行写入，status_lock仅复制
 * 固定大小事实；历史淘汰/换boot后拒绝重新执行，持久历史由外层保存。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/dynamic_shared_buffers.h"

/*-
 * wire的uint64用唯一十进制字符串表示；不借signed bigint/浮点/strtoull
 * 的宽松语法，避免高位截断、符号/空白归一化及不同语言的身份碰撞。
 * 长度由调用方实际字节串提供（含内嵌NUL也拒绝），失败不修改输出。
 */
bool
DsgBufferParseUint64(const char *value, Size length, uint64 *result)
{
	uint64		parsed = 0;
	Size		i;

	if (length == 0 || length > 20 || (length > 1 && value[0] == '0'))
		return false;
	for (i = 0; i < length; i++)
	{
		unsigned int digit;

		if (value[i] < '0' || value[i] > '9')
			return false;
		digit = value[i] - '0';
		if (parsed > (PG_UINT64_MAX - digit) / 10)
			return false;
		parsed = parsed * 10 + digit;
	}
	*result = parsed;
	return true;
}

static DsgBufferRequestKey
DsgRequestKey(const DsgBufferResizeRequest *request)
{
	DsgBufferRequestKey key;

	MemSet(&key, 0, sizeof(key));
	key.boot_id = request->boot_id;
	key.issuer_epoch = request->issuer_epoch;
	key.revision = request->revision;
	key.operation_id_high = request->operation_id_high;
	key.operation_id_low = request->operation_id_low;
	return key;
}

static bool
DsgKeyMatches(const DsgBufferRequestKey *key, const DsgBufferResizeRequest *request)
{
	return key->boot_id == request->boot_id && key->issuer_epoch == request->issuer_epoch &&
		key->revision == request->revision &&
		key->operation_id_high == request->operation_id_high &&
		key->operation_id_low == request->operation_id_low;
}

bool
DsgBufferSameRequest(const DsgBufferResizeRequest *a, const DsgBufferResizeRequest *b)
{
	DsgBufferRequestKey key = DsgRequestKey(a);

	return DsgKeyMatches(&key, b) && a->expected_generation == b->expected_generation &&
		a->target_buffers == b->target_buffers &&
		memcmp(a->context_hash, b->context_hash, DSG_BUFFER_CONTEXT_BYTES) == 0;
}

/* 显式二元排序，不通过加法拼接/截断epoch和revision，避免溢出或换代倒退。 */
bool
DsgBufferRequestNewer(const DsgBufferResizeRequest *a, const DsgBufferResizeRequest *b)
{
	return a->issuer_epoch > b->issuer_epoch ||
		(a->issuer_epoch == b->issuer_epoch && a->revision > b->revision);
}

/* 调用方持status_lock；只扫描固定上限16个键，不分配、不IO、不报错。 */
static const DsgBufferSnapshot *
DsgFindHistory(const DsgBufferRequestKey *key)
{
	int			i;

	for (i = 0; i < DSBCtrl->history_count; i++)
	{
		const DsgBufferSnapshot *entry = &DSBCtrl->history[i];
		const DsgBufferResizeRequest *request = &entry->operation.request;

		if (request->operation_id_high == key->operation_id_high &&
			request->operation_id_low == key->operation_id_low)
			return entry;
	}
	return NULL;
}

/*-
 * 旧终态必须在新操作覆盖共享operation之前保存。只归档终态；异常退出遗留
 * PREPARING先由resize所有者对账，不能把无执行者的状态伪装成永久RUNNING。
 */
static void
DsgArchiveOperationLocked(void)
{
	Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
	Assert(DSBCtrl->operation.phase != DSG_OP_PREPARING);
	if (DSBCtrl->operation.phase != DSG_OP_IDLE)
	{
		DsgBufferCopySnapshotLocked(&DSBCtrl->history[DSBCtrl->history_next]);
		DSBCtrl->history_next = (DSBCtrl->history_next + 1) % DSG_BUFFER_HISTORY_SIZE;
		DSBCtrl->history_count = Min(DSBCtrl->history_count + 1, DSG_BUFFER_HISTORY_SIZE);
	}
}

/*-
 * 调用方持resize独占锁，但接管只需status短锁就能推进floor。因此floor比较、
 * 旧终态归档、新操作登记必须同一次status锁内完成，不能先检查再无条件覆盖。
 * 若接管先到，拒绝不消耗历史槽、不改另一操作/预算；若登记先到，接管会保留
 * 原意图并请求其取消，且在resize/Provider退出前只能返回PENDING。旧内部epoch0
 * 只在从未见过非零epoch的boot兼容；正式操作亦单调推进floor，不能被旧接口绕过。
 */
bool
DsgBufferStartOperation(const DsgBufferResizeRequest *request,
						DsgBufferOperationKind kind, int original_buffers)
{
	bool		allowed;

	Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
	SpinLockAcquire(&DSBCtrl->status_lock);
	allowed = request->issuer_epoch >= DSBCtrl->issuer_floor_epoch;
	if (allowed)
	{
		DsgArchiveOperationLocked();
		DSBCtrl->issuer_floor_epoch = request->issuer_epoch;
		MemSet(&DSBCtrl->operation, 0, sizeof(DSBCtrl->operation));
		DSBCtrl->operation.request = *request;
		DSBCtrl->operation.kind = kind;
		DSBCtrl->operation.original_buffers = original_buffers;
		DSBCtrl->operation.phase = DSG_OP_PREPARING;
		DSBCtrl->cancel_requested = false;
		DSBCtrl->cancel_closed = false;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	return allowed;
}

/* 0无此ID、1同一完整意图、-1 ID被改参数；只有1会填写本操作的结果。 */
int
DsgBufferFindReplay(const DsgBufferResizeRequest *request, DsgBufferOperationKind kind,
					DsgBufferSnapshot *snapshot)
{
	DsgBufferRequestKey key = DsgRequestKey(request);
	const DsgBufferSnapshot *history;
	const DsgBufferOperation *operation = NULL;
	int			result = 0;

	SpinLockAcquire(&DSBCtrl->status_lock);
	if (DSBCtrl->operation.phase != DSG_OP_IDLE &&
		DSBCtrl->operation.request.operation_id_high == key.operation_id_high &&
		DSBCtrl->operation.request.operation_id_low == key.operation_id_low)
		operation = &DSBCtrl->operation;
	history = operation == NULL ? DsgFindHistory(&key) : NULL;
	if (history != NULL)
		operation = &history->operation;
	if (operation != NULL)
	{
		result = operation->kind == kind && DsgBufferSameRequest(request, &operation->request) ? 1 : -1;
		if (result == 1)
		{
			if (history != NULL)
				*snapshot = *history;
			else
				DsgBufferCopySnapshotLocked(snapshot);
			snapshot->replayed = true;
		}
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	return result;
}

bool
DsgBufferCancelRequested(const DsgBufferResizeRequest *request)
{
	bool		cancelled;

	SpinLockAcquire(&DSBCtrl->status_lock);
	cancelled = DSBCtrl->cancel_requested &&
		DsgBufferSameRequest(request, &DSBCtrl->operation.request);
	SpinLockRelease(&DSBCtrl->status_lock);
	return cancelled;
}

/*-
 * 改变容量的调用方已持accessX，即将进入无IO/等待/可恢复错误的发布区。
 * H不变的Grow只提交幂等操作终态，不改数组，因此无需取得accessX。
 * 与取消共用status_lock：取消先到则拒绝发布；此栅栏先到则取消返回TOO_LATE。
 * 此处关闭的是取消窗口，不是VM完成证明；SIGKILL仍会换boot并使旧许可失效。
 */
bool
DsgBufferBeginCommit(const DsgBufferResizeRequest *request)
{
	bool		allowed;

	Assert(LWLockHeldByMeInMode(&DSBCtrl->resize_lock, LW_EXCLUSIVE));
	SpinLockAcquire(&DSBCtrl->status_lock);
	Assert(DsgBufferSameRequest(request, &DSBCtrl->operation.request));
	allowed = !DSBCtrl->cancel_requested && request->issuer_epoch >= DSBCtrl->issuer_floor_epoch;
	if (allowed)
		DSBCtrl->cancel_closed = true;
	SpinLockRelease(&DSBCtrl->status_lock);
	return allowed;
}

/*-
 * FATAL先放原生LWLock再运行退出回调。查询只能条件取得串行权，对账确认
 * 已无执行者的PREPARING；绝不等待仍在Provider中的正常操作，不取accessX。
 * 允许取消查询；即使中断，对账可重入，不会据部分目录遍历发放许可。
 */
static void
DsgReconcileForQuery(void)
{
	bool		preparing;

	SpinLockAcquire(&DSBCtrl->status_lock);
	preparing = DSBCtrl->operation.phase == DSG_OP_PREPARING;
	SpinLockRelease(&DSBCtrl->status_lock);
	if (!preparing || LWLockHeldByMe(&DSBCtrl->resize_lock) ||
		!LWLockConditionalAcquire(&DSBCtrl->resize_lock, LW_EXCLUSIVE))
		return;
	RESUME_INTERRUPTS();
	PG_TRY();
	{
		DsgBufferReconcileInterruptedGrow();
		DsgBufferReconcileInterruptedShrink();
	}
	PG_FINALLY();
	{
		HOLD_INTERRUPTS();
		LWLockRelease(&DSBCtrl->resize_lock);
	}
	PG_END_TRY();
}

static void
DsgStatusLocked(const DsgBufferRequestKey *key, DsgBufferControlResponse *response,
				bool cancel)
{
	const DsgBufferSnapshot *history = NULL;
	const DsgBufferResizeRequest *current = &DSBCtrl->operation.request;
	bool		current_id;

	MemSet(response, 0, sizeof(*response));
	response->receipt.key = *key;
	DsgBufferCopySnapshotLocked(&response->observed);
	if (key->boot_id != DSBCtrl->boot_id)
	{
		response->receipt.disposition = DSG_RECEIPT_STALE;
		return;
	}
	current_id = DSBCtrl->operation.phase != DSG_OP_IDLE &&
		key->operation_id_high == current->operation_id_high &&
		key->operation_id_low == current->operation_id_low;
	if (!current_id)
		history = DsgFindHistory(key);
	if (current_id || history != NULL)
	{
		const DsgBufferResizeRequest *found = current_id ? current : &history->operation.request;

		if (!DsgKeyMatches(key, found))
		{
			response->receipt.disposition = DSG_RECEIPT_CONFLICT;
			return;
		}
		response->receipt.has_result = true;
		response->receipt.disposition = DSG_RECEIPT_REPLAYED;
		if (current_id)
			DsgBufferCopySnapshotLocked(&response->receipt.result);
		else
			response->receipt.result = *history;
		response->receipt.result.replayed = true;
		if (cancel)
		{
			if (current_id && DSBCtrl->operation.phase == DSG_OP_PREPARING && !DSBCtrl->cancel_closed)
			{
				DSBCtrl->cancel_requested = true;
				response->receipt.disposition = DSG_RECEIPT_CANCEL_REQUESTED;
				response->receipt.result.cancel_requested = true;
				response->observed.cancel_requested = true;
			}
			else
				response->receipt.disposition = DSG_RECEIPT_TOO_LATE;
		}
	}
	else
	{
		/*
		 *
		 * RESULT_EXPIRED表示本boot无法确证旧结果，不证明这个ID曾被接受。
		 */
		response->receipt.disposition = key->issuer_epoch < DSBCtrl->issuer_floor_epoch ?
			DSG_RECEIPT_STALE :
			(key->issuer_epoch < current->issuer_epoch ||
			 (key->issuer_epoch == current->issuer_epoch && key->revision <= current->revision)) ?
			DSG_RECEIPT_RESULT_EXPIRED : DSG_RECEIPT_NOT_FOUND;
	}
}

static void
DsgControlLookup(const DsgBufferRequestKey *key, DsgBufferControlResponse *response,
				 bool cancel)
{
	if (DSBCtrl == NULL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("dynamic shared buffers are disabled")));
	/*-
	 * 查询可能条件取得resizeX进行遗留对账，也不能由持accessSH的调用方
	 * 反向进入。所有受管入口均在数组/协调锁外调用；锁内观察用原快照原语。
	 */
	if (LWLockHeldByMe(&DSBCtrl->access_lock) || LWLockHeldByMe(&DSBCtrl->resize_lock))
		elog(ERROR, "DSGA control cannot run while holding buffer coordination locks");
	if (key->revision == 0 || (key->operation_id_high == 0 && key->operation_id_low == 0))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid DSGA request key")));
	DsgReconcileForQuery();
	SpinLockAcquire(&DSBCtrl->status_lock);
	DsgStatusLocked(key, response, cancel);
	SpinLockRelease(&DSBCtrl->status_lock);
}

void
DsgBufferControlStatus(const DsgBufferRequestKey *key, DsgBufferControlResponse *response)
{
	DsgControlLookup(key, response, false);
}

void
DsgBufferControlCancel(const DsgBufferRequestKey *key, DsgBufferControlResponse *response)
{
	DsgControlLookup(key, response, true);
}

/*-
 * 接管先用status短锁推进floor并请求尚未封闭窗口的旧操作取消，不等待正在IO的
 * 执行者。绝不反向持status等待resize；之后仅条件取得resize，失败即PENDING。
 * COMMITTED仍可能在同步Provider中，不能按phase推断旧执行者退出。取得串行权
 * 后收敛FATAL遗留，再核验floor未被更高代次取代，才返回FENCED。整个过程中不
 * 清旧意图/预算、不回滚已提交H、不发放VM许可。调用者另须认证真实issuer权属。
 */
void
DsgBufferFenceIssuer(uint64 boot_id, uint64 issuer_epoch, DsgBufferIssuerFenceResponse *response)
{
	if (DSBCtrl == NULL || !IsUnderPostmaster)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("DSGA issuer fencing requires an enabled running server")));
	if (boot_id == 0 || issuer_epoch == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid DSGA issuer fence identity")));
	if (LWLockHeldByMe(&DSBCtrl->access_lock) || LWLockHeldByMe(&DSBCtrl->resize_lock))
		elog(ERROR, "DSGA issuer fencing cannot run while holding buffer coordination locks");
	MemSet(response, 0, sizeof(*response));
	response->requested_boot_id = boot_id;
	response->requested_epoch = issuer_epoch;
	SpinLockAcquire(&DSBCtrl->status_lock);
	if (boot_id != DSBCtrl->boot_id || issuer_epoch < DSBCtrl->issuer_floor_epoch)
		response->disposition = DSG_ISSUER_STALE;
	else
	{
		DSBCtrl->issuer_floor_epoch = issuer_epoch;
		DsgBufferAdmissionLeaseFenceLocked(issuer_epoch);
		if (DSBCtrl->operation.phase == DSG_OP_PREPARING &&
			DSBCtrl->operation.request.issuer_epoch < issuer_epoch && !DSBCtrl->cancel_closed)
			DSBCtrl->cancel_requested = true;
		response->disposition = DSG_ISSUER_PENDING;
	}
	DsgBufferCopySnapshotLocked(&response->observed);
	SpinLockRelease(&DSBCtrl->status_lock);
	if (response->disposition == DSG_ISSUER_STALE ||
		!LWLockConditionalAcquire(&DSBCtrl->resize_lock, LW_EXCLUSIVE))
		return;
	RESUME_INTERRUPTS();
	PG_TRY();
	{
		DsgBufferReconcileInterruptedShrink();
		DsgBufferReconcileInterruptedGrow();
		SpinLockAcquire(&DSBCtrl->status_lock);
		response->disposition = issuer_epoch == DSBCtrl->issuer_floor_epoch ?
			DSG_ISSUER_FENCED : DSG_ISSUER_STALE;
		DsgBufferCopySnapshotLocked(&response->observed);
		SpinLockRelease(&DSBCtrl->status_lock);
	}
	PG_FINALLY();
	{
		HOLD_INTERRUPTS();
		LWLockRelease(&DSBCtrl->resize_lock);
	}
	PG_END_TRY();
}

void
DsgBufferControlResize(const DsgBufferResizeRequest *request, DsgBufferOperationKind kind,
					   int timeout_ms, DsgBufferControlResponse *response)
{
	DsgBufferRequestKey key = DsgRequestKey(request);
	DsgBufferSnapshot own_result;

	if (kind != DSG_OP_GROW && kind != DSG_OP_SHRINK)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid DSGA operation kind")));
	DsgBufferControlStatus(&key, response);
	if (response->receipt.has_result)
	{
		if (response->receipt.result.operation.kind != kind ||
			!DsgBufferSameRequest(request, &response->receipt.result.operation.request))
		{
			response->receipt.disposition = DSG_RECEIPT_CONFLICT;
			response->receipt.has_result = false;
		}
		return;
	}
	if (response->receipt.disposition == DSG_RECEIPT_STALE ||
		response->receipt.disposition == DSG_RECEIPT_CONFLICT)
		return;
	if (response->receipt.disposition == DSG_RECEIPT_RESULT_EXPIRED)
	{
		response->receipt.disposition = DSG_RECEIPT_STALE;
		return;
	}
	if (timeout_ms == 0)
	{
		response->receipt.disposition = DSG_RECEIPT_REJECTED;
		return;
	}
	response->receipt.disposition = DSG_RECEIPT_ACCEPTED;
	if (kind == DSG_OP_GROW)
		(void) DsgBufferGrowWithReceipt(request, timeout_ms, &own_result, &response->receipt);
	else
		(void) DsgBufferShrinkWithReceipt(request, timeout_ms, &own_result, &response->receipt);
	DsgBufferGetSnapshot(&response->observed);
}
