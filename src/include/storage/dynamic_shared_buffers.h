/*-------------------------------------------------------------------------
 * dynamic_shared_buffers.h -- Buffer Pool 的固定地址上限及共享容量状态。
 *
 * L（low）只约束新 victim；H（high）是已初始化且允许访问的上限；M（maximum）
 * 在本轮共享内存生命周期固定。已有 pin 的有效性不能使用 L 判断。
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_SHARED_BUFFERS_H
#define DYNAMIC_SHARED_BUFFERS_H

#include "port/atomics.h"
#include "storage/dsg_region.h"
#include "storage/lwlock.h"
#include "storage/spin.h"

/* Neon 扩展按能力宏适配，不能由 PG 大版本号推断本 fork 的内核接口。 */
#define NEON_DSG_BUFFER_API_VERSION 5
#define DSG_BUFFER_GRANULE_BUFFERS 2048
#define DSG_BUFFER_HISTORY_SIZE 16
#define DSG_BUFFER_CONTEXT_BYTES 32

/* 回收事实独立于逻辑H；pending/unsupported/failed都不能据此扣除VM预算。 */
typedef enum DsgBufferReleaseState
{
	DSG_RELEASE_NONE,
	DSG_RELEASE_PENDING,
	DSG_RELEASE_WAIT_CHECKPOINT,
	DSG_RELEASE_VERIFIED,
	DSG_RELEASE_UNSUPPORTED,
	DSG_RELEASE_FAILED
} DsgBufferReleaseState;

typedef enum DsgBufferArrayId
{
	DSG_BUFFER_DESCRIPTORS,
	DSG_BUFFER_BLOCKS,
	DSG_BUFFER_IOCV,
	DSG_BUFFER_CHECKPOINT,
	DSG_BUFFER_ARRAY_COUNT
} DsgBufferArrayId;

typedef struct DsgBufferArray
{
	DsgRegion	region;
	Size		element_bytes;

	/*
	 * 预算保守上界，不是 RSS 采样值；未发布部分也必须计入。
	 */
	Size		committed_upper_bytes;
	bool		allocation_unknown;
	DsgBufferReleaseState release_state;

	/*-
	 * 完整绑定释放意图，不能只用换epoch后可重复的revision/generation。
	 */
	uint64		release_issuer_epoch;
	uint64		release_revision;
	uint64		release_operation_id_high;
	uint64		release_operation_id_low;
	uint64		release_generation;
	DsgMemoryResult release_result;
} DsgBufferArray;

/*-
 * Granule 是固定 Buffer ID 范围，元数据永不随数组尾部回收。
 * 初值或 M 可以形成部分 Granule；active_buffers 才是已发布数量。
 * RESERVED 仅表示逻辑未发布，不声称 backing 已经释放。
 * PREPARING/READY 可以保留原有 active_buffers，新增部分不可见。
 */
typedef enum DsgBufferGranuleState
{
	DSG_GRANULE_RESERVED,
	DSG_GRANULE_PREPARING,
	DSG_GRANULE_READY,
	DSG_GRANULE_ACTIVE,
	DSG_GRANULE_DRAINING
} DsgBufferGranuleState;

typedef struct DsgBufferGranule
{
	int			first_buffer;
	int			maximum_buffers;
	int			active_buffers;
	int			prepared_buffers;
	uint64		generation;
	DsgBufferGranuleState state;
} DsgBufferGranule;

typedef enum DsgBufferOperationPhase
{
	DSG_OP_IDLE,
	DSG_OP_PREPARING,
	DSG_OP_COMMITTED,
	DSG_OP_DENIED
} DsgBufferOperationPhase;

typedef enum DsgBufferOperationReason
{
	DSG_REASON_NONE,
	DSG_REASON_TIMEOUT,
	DSG_REASON_POPULATE_FAILED,
	DSG_REASON_INTERRUPTED,
	DSG_REASON_CANCELLED
} DsgBufferOperationReason;

/*-
 * boot 防止跨共享内存重建重放，revision 拒绝过期请求，operation_id
 * 用于同一请求重试，expected_generation 防止据过期容量执行新请求。
 * 外层 VM/Compute 身份仍须由 Neon/Agent 栅栏核验，本结构不能替代它。
 */
typedef struct DsgBufferResizeRequest
{
	uint64		boot_id;

	/*
	 * epoch允许外层领导权换代后重新开始revision；0仅保留内部旧调用兼容。
	 */
	uint64		issuer_epoch;
	uint64		revision;
	uint64		operation_id_high;
	uint64		operation_id_low;
	uint64		expected_generation;
	int			target_buffers;

	/*
	 * 全部外层意图的规范化SHA-256绑定；不是认证或资源承诺本身。
	 */
	unsigned char context_hash[DSG_BUFFER_CONTEXT_BYTES];
} DsgBufferResizeRequest;

/* 保留已有Grow调用点的源代码兼容；两种操作共享身份/范围格式，由kind区分。 */
typedef DsgBufferResizeRequest DsgBufferGrowRequest;

typedef enum DsgBufferOperationKind
{
	DSG_OP_GROW,
	DSG_OP_SHRINK
} DsgBufferOperationKind;

/* 每轮真实排水的分类计数。不是当前仍被pin的全库采样，也不授权VM动作。 */
typedef enum DsgBufferDrainResult
{
	DSG_DRAINED,
	DSG_DRAIN_PINNED,
	DSG_DRAIN_IO,
	DSG_DRAIN_DIRTY,
	DSG_DRAIN_CONTENT_LOCK,
	DSG_DRAIN_MAPPING_LOCK,
	DSG_DRAIN_RESULT_COUNT
} DsgBufferDrainResult;

typedef struct DsgBufferOperation
{
	DsgBufferResizeRequest request;
	DsgBufferOperationKind kind;
	int			original_buffers;
	DsgBufferOperationPhase phase;
	DsgBufferOperationReason reason;
	uint32		drain_counts[DSG_DRAIN_RESULT_COUNT];
} DsgBufferOperation;

typedef struct DsgBufferSnapshot
{
	DsgBufferOperation operation;
	uint64		boot_id;
	uint64		generation;
	int			low;
	int			high;
	int			maximum;
	Size		committed_upper_bytes[DSG_BUFFER_ARRAY_COUNT];
	bool		allocation_unknown[DSG_BUFFER_ARRAY_COUNT];
	DsgBufferReleaseState release_state[DSG_BUFFER_ARRAY_COUNT];
	uint64		release_issuer_epoch[DSG_BUFFER_ARRAY_COUNT];
	uint64		release_revision[DSG_BUFFER_ARRAY_COUNT];
	uint64		release_operation_id_high[DSG_BUFFER_ARRAY_COUNT];
	uint64		release_operation_id_low[DSG_BUFFER_ARRAY_COUNT];
	uint64		release_generation[DSG_BUFFER_ARRAY_COUNT];
	DsgMemoryResult release_result[DSG_BUFFER_ARRAY_COUNT];
	bool		replayed;
	bool		admission_open;
	bool		cancel_requested;
	bool		cancel_closed;
	uint64		admission_revision;
	Size		startup_shared_upper_bytes;
} DsgBufferSnapshot;

/*-
 * 回执属于本请求；result只有has_result时有效，是该操作的历史事实。
 * observed另取当前快照，二者不得混用，尤其不能把别人的COMMITTED当本次结果。
 * 历史只在本boot有界保留，淘汰后拒绝重新执行，外层负责持久历史。
 */
typedef struct DsgBufferRequestKey
{
	uint64		boot_id;
	uint64		issuer_epoch;
	uint64		revision;
	uint64		operation_id_high;
	uint64		operation_id_low;
} DsgBufferRequestKey;

typedef enum DsgBufferReceiptDisposition
{
	DSG_RECEIPT_ACCEPTED,
	DSG_RECEIPT_REPLAYED,
	DSG_RECEIPT_BUSY,
	DSG_RECEIPT_STALE,
	DSG_RECEIPT_CONFLICT,
	DSG_RECEIPT_RESULT_EXPIRED,
	DSG_RECEIPT_NOT_FOUND,
	DSG_RECEIPT_CANCEL_REQUESTED,
	DSG_RECEIPT_TOO_LATE,
	DSG_RECEIPT_REJECTED
} DsgBufferReceiptDisposition;

typedef struct DsgBufferReceipt
{
	DsgBufferRequestKey key;
	DsgBufferReceiptDisposition disposition;
	bool		has_result;
	DsgBufferSnapshot result;
} DsgBufferReceipt;

typedef struct DsgBufferControlResponse
{
	DsgBufferReceipt receipt;
	DsgBufferSnapshot observed;
} DsgBufferControlResponse;

typedef struct DynamicSharedBuffersControl
{
	pg_atomic_uint32 low;
	pg_atomic_uint32 high;
	int			maximum;

	/*
	 * 每次重建共享内存都换身份，包括同一 Postmaster 的
	 * crash recovery。
	 */
	uint64		boot_id;

	/*
	 * 新 boot 不继承旧许可；受管模式必须经管理链核对预算后重新开放。
	 */
	pg_atomic_uint32 admission_open;
	uint64		admission_revision;
	Size		startup_shared_upper_bytes;
	LWLock		access_lock;
	/*-
	 * checkpoint 只长期持此共享租约，保护排序工作区，不阻塞容量发布。
	 * 回收工作区只能条件取得独占租约；失败则保留 backing/预算，禁止
	 * 持 accessX 等待该锁。prepared 是已真实准备的连续对象前缀；Grow
	 * 不得重新清零其中可能仍被旧 checkpoint 使用的内容。
	 */
	LWLock		checkpoint_lock;
	pg_atomic_uint32 checkpoint_prepared_buffers;
	/*-
	 * resize_lock 只串行协调者。status_lock 仅保护短的状态复制，
	 * 禁止在 spinlock 内分配、调用 Provider、等待或报告 ERROR。
	 * 锁序：resize -> access -> status；普通观察者只取 status。
	 */
	LWLock		resize_lock;
	slock_t		status_lock;
	pg_atomic_uint64 generation;
	DsgBufferOperation operation;

	/*
	 *
	 * status_lock仅做固定上限16个键扫描/单条快照复制，不分配/IO/报错。
	 */
	bool		cancel_requested;
	bool		cancel_closed;
	int			history_count;
	int			history_next;
	DsgBufferSnapshot history[DSG_BUFFER_HISTORY_SIZE];
	DsgBufferArray arrays[DSG_BUFFER_ARRAY_COUNT];

	/*
	 * 目录遍历需持 resize_lock；禁止在 status spinlock 内做长遍历。
	 */
	int			granule_count;
	DsgBufferGranule granules[FLEXIBLE_ARRAY_MEMBER];
} DynamicSharedBuffersControl;

/*-
 * Guard 持有期间 H 及其数组 backing 不会被缩容回收。嵌套调用复用本进程
 * 已持有的锁；每个 Guard 只释放自己取得的锁，避免重复登记和错误释放，
 * 也允许独占协调者复用已取得的保护。Guard 不阻止查询取消；ERROR/FATAL 路径由 PG 的
 * LWLockReleaseAll 清理。Begin/End 对原生 holdoff 的调整必须严格成对。
 */
typedef struct DsgBufferAccess
{
	int			count;
	bool		acquired;
} DsgBufferAccess;

extern PGDLLIMPORT bool enable_dynamic_shared_buffers;
extern PGDLLIMPORT int max_shared_buffers;
extern PGDLLIMPORT bool dynamic_shared_buffers_require_admission;
extern PGDLLIMPORT int dynamic_shared_buffers_startup_budget;
extern PGDLLIMPORT DynamicSharedBuffersControl *DSBCtrl;

extern int	GetMaxNBuffers(void);
extern int	GetLowNBuffers(void);
extern int	GetHighNBuffers(void);
extern Size DynamicSharedBuffersShmemSize(void);
extern void DynamicSharedBuffersInit(void);
extern DsgBufferAccess DsgBufferAccessBegin(void);
extern void DsgBufferAccessEnd(DsgBufferAccess *access);
extern void DsgBufferGetSnapshot(DsgBufferSnapshot *snapshot);
extern bool DsgBufferParseUint64(const char *value, Size length, uint64 *result);
extern void DsgBufferCheckStartupBudget(Size main_segment_bytes);
extern void DsgBufferCheckAdmission(void);
extern void DsgBufferSetAdmission(uint64 boot_id, uint64 revision,
								  uint64 expected_generation, bool open);

/* 内核协调接口；调用者须先完成权限/外层资源预算检查，尚无用户 SQL 入口。 */
extern bool DsgBufferGrow(const DsgBufferGrowRequest *request, int timeout_ms,
						  DsgBufferSnapshot *snapshot);
extern bool DsgBufferShrink(const DsgBufferResizeRequest *request, int timeout_ms,
							DsgBufferSnapshot *snapshot);

/* 受管C入口；外层必须另做认证、实例/意图绑定与预算核验，不发放VM许可。 */
extern void DsgBufferControlResize(const DsgBufferResizeRequest *request,
								   DsgBufferOperationKind kind, int timeout_ms,
								   DsgBufferControlResponse *response);
extern void DsgBufferControlStatus(const DsgBufferRequestKey *key,
								   DsgBufferControlResponse *response);
extern void DsgBufferControlCancel(const DsgBufferRequestKey *key,
								   DsgBufferControlResponse *response);

/* 以下为协调器内部接口，不能由扩展绕开上述受管入口。 */
extern bool DsgBufferSameRequest(const DsgBufferResizeRequest *a,
								 const DsgBufferResizeRequest *b);
extern bool DsgBufferRequestNewer(const DsgBufferResizeRequest *a,
								  const DsgBufferResizeRequest *b);
extern void DsgBufferCopySnapshotLocked(DsgBufferSnapshot *snapshot);
extern void DsgBufferArchiveOperation(void);
extern int	DsgBufferFindReplay(const DsgBufferResizeRequest *request,
								DsgBufferOperationKind kind, DsgBufferSnapshot *snapshot);
extern bool DsgBufferCancelRequested(const DsgBufferResizeRequest *request);
extern bool DsgBufferBeginCommit(const DsgBufferResizeRequest *request);
extern void DsgBufferReconcileInterruptedGrow(void);
extern bool DsgBufferGrowWithReceipt(const DsgBufferResizeRequest *request, int timeout_ms,
									 DsgBufferSnapshot *snapshot, DsgBufferReceipt *receipt);
extern bool DsgBufferShrinkWithReceipt(const DsgBufferResizeRequest *request, int timeout_ms,
									   DsgBufferSnapshot *snapshot, DsgBufferReceipt *receipt);

/* 以下内部原语只允许持resize独占锁的协调者调用。 */
extern DsgBufferDrainResult DsgBufferDrainOne(int buffer_index);
extern void DsgBufferReconcileInterruptedShrink(void);
extern void StrategyRestoreAllocationLimit(void);

#endif							/* DYNAMIC_SHARED_BUFFERS_H */
