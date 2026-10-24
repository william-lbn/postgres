/*-------------------------------------------------------------------------
 * dsg_work.c -- 固定PGPROC槽的真实工作登记，不分配可回收区元数据。
 *
 * PGPROC计数和admission共用status_lock；启动包前计数以原生原子从Postmaster交接。
 * 不扫描PGPROC、不查目录、不检测PID存活、不等待、不分配、不调用ERROR。
 * PROC生命周期的注册/注销发生在ProcStructLock之外，避免建立反向锁序。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/dsg_work.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"

/* 私有布局；不修改PGPROC或已发布Buffer ABI6的结构大小/字段偏移。 */
typedef enum DsgWorkKind
{
	DSG_WORK_UNUSED,
	DSG_WORK_CLIENT,
	DSG_WORK_BACKGROUND,
	DSG_WORK_AUXILIARY
} DsgWorkKind;

typedef enum DsgWorkPhase
{
	DSG_WORK_STARTUP,
	DSG_WORK_IDLE,
	DSG_WORK_READING,
	DSG_WORK_EXECUTING,
	DSG_WORK_BOUNDED_INPUT,
	DSG_WORK_PHASE_COUNT
} DsgWorkPhase;

typedef struct DsgWorkSlot
{
	int pid;
	DsgWorkKind kind;
	DsgWorkPhase phase;
	bool retained_transaction;
} DsgWorkSlot;

typedef struct DsgWorkControl
{
	uint64 boot_id;
	uint32 slot_count;
	uint32 phases[DSG_WORK_PHASE_COUNT];
	uint32 retained_transactions;
	uint32 backgrounds;
	uint32 auxiliaries;
	pg_atomic_uint32 prestart_clients;
	DsgWorkSlot slots[FLEXIBLE_ARRAY_MEMBER];
} DsgWorkControl;

static DsgWorkControl *work_control;
static bool work_registered;
/*-
 * 启动前槽放在PGPROC槽尾部，指针/容量由Postmaster本地初始化并通过fork继承。
 * Postmaster绝不读取共享区里的长度来寻址，也不等待status_lock等backend锁。
 * 0=未登记，1=待建立PGPROC，2=已在status_lock内交接到PGPROC。
 */
static pg_atomic_uint32 *prestart_slots;
static int prestart_slot_count;

static Size
DsgWorkProcBytes(void)
{
	return MAXALIGN(add_size(offsetof(DsgWorkControl, slots),
		mul_size(add_size(MaxBackends, NUM_AUXILIARY_PROCS), sizeof(DsgWorkSlot))));
}

Size
DsgWorkShmemSize(void)
{
	if (!enable_dynamic_shared_buffers)
		return 0;
	/* 预备事务不是活进程；其资源仍需完整Quiesce策略另行审查。 */
	return add_size(DsgWorkProcBytes(),
		mul_size(MaxLivePostmasterChildren(), sizeof(pg_atomic_uint32)));
}

void
DsgWorkShmemInit(void)
{
	bool found;
	Size bytes;
	int i;

	if (!enable_dynamic_shared_buffers)
		return;
#if defined(PG_HAVE_ATOMIC_U32_SIMULATION) || defined(PG_HAVE_MEMORY_BARRIER_EMULATION)
	/* Postmaster不能退化到持锁模拟原子，否则backend崩溃会拖死恢复。 */
	elog(FATAL, "DSGA prestart registry requires native 32-bit atomics and memory barriers");
#endif
	if (DSBCtrl == NULL || ProcGlobal == NULL)
		elog(FATAL, "DSGA work registry requires initialized buffer and process state");
	bytes = DsgWorkShmemSize();
	work_control = ShmemInitStruct("DSGA Work Registry", bytes, &found);
	prestart_slot_count = MaxLivePostmasterChildren();
	prestart_slots = (pg_atomic_uint32 *) ((char *) work_control + DsgWorkProcBytes());
	if (!found)
	{
		MemSet(work_control, 0, bytes);
		work_control->boot_id = DSBCtrl->boot_id;
		work_control->slot_count = ProcGlobal->allProcCount;
		pg_atomic_init_u32(&work_control->prestart_clients, 0);
		for (i = 0; i < prestart_slot_count; i++)
			pg_atomic_init_u32(&prestart_slots[i], 0);
	}
	if (work_control->boot_id != DSBCtrl->boot_id ||
		work_control->slot_count != ProcGlobal->allProcCount)
		elog(FATAL, "DSGA work registry does not match this PG boot");
}

/*-
 * 仅Postmaster调用。登记先于fork：子进程即使停在DNS/SSL/GSS/半个startup包，
 * 甚至未执行任何指令，也已计入。它的早期SIGTERM/超时仍可直接_exit，不新增
 * child共享清理责任。硬件原子不等待backend，不用共享状态决定槽分配或数组界。
 * fork失败由原ReleasePostmasterChildSlot统一回收；未知/损坏共享计数最多阻止
 * 管理观察成功，不能让Postmaster持backend锁或因共享槽内容触发PANIC。
 */
void
DsgWorkPreStart(int child_slot)
{
	if (!enable_dynamic_shared_buffers)
		return;
	Assert(!IsUnderPostmaster);
	Assert(child_slot > 0 && child_slot <= prestart_slot_count);
	pg_atomic_fetch_add_u32(&work_control->prestart_clients, 1);
	pg_atomic_write_u32(&prestart_slots[child_slot - 1], 1);
}

/*-
 * 只在真实waitpid确认退出或fork失败后调用。原PMSignal回收可调用两次，exchange
 * 使第二次无效果；已交接状态2不再扣启动计数。早期child本身不参与清理，不能
 * 把SIGTERM的发送、连接关闭或一次采样当作已退出。
 */
void
DsgWorkPreForget(int child_slot)
{
	uint32 previous;

	if (!enable_dynamic_shared_buffers)
		return;
	Assert(!IsUnderPostmaster);
	Assert(child_slot > 0 && child_slot <= prestart_slot_count);
	previous = pg_atomic_exchange_u32(&prestart_slots[child_slot - 1], 0);
	if (previous == 1)
		pg_atomic_fetch_sub_u32(&work_control->prestart_clients, 1);
}

/* 调用前确认基础指针；持锁后还须验证本进程拥有槽，不根据外部自报PID认领。 */
static DsgWorkSlot *
DsgWorkOwnSlot(void)
{
	if (work_control == NULL || DSBCtrl == NULL || MyProc == NULL ||
		MyProcNumber < 0 || MyProcNumber >= work_control->slot_count)
		elog(PANIC, "DSGA work registry is unavailable for this process");
	return &work_control->slots[MyProcNumber];
}

/* 槽只由本进程写入，注册后的身份不可改变；断言关闭构建也不能默许漏登记。 */
static DsgWorkSlot *
DsgWorkClientSlot(void)
{
	DsgWorkSlot *slot = DsgWorkOwnSlot();

	if (!work_registered || slot->pid != MyProcPid || slot->kind != DSG_WORK_CLIENT)
		elog(PANIC, "DSGA frontend work has no owned client registration");
	return slot;
}

void
DsgWorkAttach(bool auxiliary)
{
	DsgWorkSlot *slot;
	DsgWorkKind kind;
	bool valid;

	if (!enable_dynamic_shared_buffers)
		return;
	slot = DsgWorkOwnSlot();
	kind = auxiliary ? DSG_WORK_AUXILIARY :
		(MyBackendType == B_BACKEND || MyBackendType == B_WAL_SENDER ||
		 MyBackendType == B_STANDALONE_BACKEND ?
		 DSG_WORK_CLIENT : DSG_WORK_BACKGROUND);
	SpinLockAcquire(&DSBCtrl->status_lock);
	valid = !work_registered && slot->kind == DSG_WORK_UNUSED;
	if (valid && kind == DSG_WORK_CLIENT && IsUnderPostmaster)
	{
		uint32 expected = 1;

		/*-
		 * 与PGPROC STARTUP计数在同一status_lock内交接；观察者不可能看到
		 * 两者同时为零。Postmaster仅在本进程终止后回收该槽，因此正常情况下
		 * 没有竞争写者；缺原登记须失败关闭，不能当作首次无在途自动补造。
		 */
		valid = MyPMChildSlot > 0 && MyPMChildSlot <= prestart_slot_count &&
			pg_atomic_compare_exchange_u32(&prestart_slots[MyPMChildSlot - 1], &expected, 2);
	}
	if (valid)
	{
		slot->pid = MyProcPid;
		slot->kind = kind;
		slot->phase = DSG_WORK_STARTUP;
		slot->retained_transaction = false;
		if (kind == DSG_WORK_CLIENT)
		{
			work_control->phases[DSG_WORK_STARTUP]++;
			if (IsUnderPostmaster)
				pg_atomic_fetch_sub_u32(&work_control->prestart_clients, 1);
		}
		else if (kind == DSG_WORK_BACKGROUND)
			work_control->backgrounds++;
		else
			work_control->auxiliaries++;
		work_registered = true;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	/* 未知旧槽不能清零后假装无在途；PANIC重建共享内存会生成关闭的新boot。 */
	if (!valid)
		elog(PANIC, "DSGA work slot already belongs to another registration");
}

void
DsgWorkDetach(void)
{
	DsgWorkSlot *slot;
	bool valid;

	/* InitProcess在注册前失败时，已安装的ProcKill也会调用这里。 */
	if (!work_registered)
		return;
	slot = DsgWorkOwnSlot();
	SpinLockAcquire(&DSBCtrl->status_lock);
	valid = slot->pid == MyProcPid && slot->kind != DSG_WORK_UNUSED;
	if (valid)
	{
		if (slot->kind == DSG_WORK_CLIENT)
		{
			Assert(work_control->phases[slot->phase] > 0);
			work_control->phases[slot->phase]--;
			if (slot->retained_transaction)
				work_control->retained_transactions--;
		}
		else if (slot->kind == DSG_WORK_BACKGROUND)
			work_control->backgrounds--;
		else
			work_control->auxiliaries--;
		MemSet(slot, 0, sizeof(*slot));
		work_registered = false;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	if (!valid)
		elog(PANIC, "DSGA work registration lost ownership before cleanup");
}

/* 持status_lock；槽只能由拥有它的进程更新，每项计数受固定槽总数约束。 */
static void
DsgWorkSetPhase(DsgWorkSlot *slot, DsgWorkPhase phase)
{
	Assert(slot->pid == MyProcPid && slot->kind == DSG_WORK_CLIENT);
	Assert(work_control->phases[slot->phase] > 0);
	work_control->phases[slot->phase]--;
	slot->phase = phase;
	work_control->phases[phase]++;
}

/*-
 * 第一个类型字节已通过官方校验，正文尚未分配。关门和登记使用同一把锁，
 * 因而先进入者必为READING，关门后被拒绝者必为BOUNDED_INPUT，不能漏在
 * 两次检查之间。SU管理工作也登记；只是普通角色拒绝而不是终止恢复通道。
 * BOUNDED只用于现有固定缓冲丢弃路径，不代表任意低内存或SQL执行安全。
 */
bool
DsgWorkStartMessage(bool ordinary_work, bool bounded_discard)
{
	DsgWorkSlot *slot;
	bool reject;

	if (!enable_dynamic_shared_buffers)
		return false;
	slot = DsgWorkClientSlot();
	SpinLockAcquire(&DSBCtrl->status_lock);
	reject = ordinary_work && !current_role_is_superuser &&
		pg_atomic_read_u32(&DSBCtrl->admission_open) == 0;
	DsgWorkSetPhase(slot, bounded_discard || reject ?
					DSG_WORK_BOUNDED_INPUT : DSG_WORK_READING);
	SpinLockRelease(&DSBCtrl->status_lock);
	return reject;
}

void
DsgWorkStartExecution(void)
{
	DsgWorkSlot *slot;

	if (!enable_dynamic_shared_buffers)
		return;
	slot = DsgWorkClientSlot();
	SpinLockAcquire(&DSBCtrl->status_lock);
	if (slot->phase == DSG_WORK_READING || slot->phase == DSG_WORK_IDLE)
		DsgWorkSetPhase(slot, DSG_WORK_EXECUTING);
	SpinLockRelease(&DSBCtrl->status_lock);
}

void
DsgWorkIdle(bool retained_transaction)
{
	DsgWorkSlot *slot;

	if (!enable_dynamic_shared_buffers)
		return;
	slot = DsgWorkClientSlot();
	SpinLockAcquire(&DSBCtrl->status_lock);
	DsgWorkSetPhase(slot, DSG_WORK_IDLE);
	if (slot->retained_transaction != retained_transaction)
	{
		if (retained_transaction)
			work_control->retained_transactions++;
		else
			work_control->retained_transactions--;
		slot->retained_transaction = retained_transaction;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
}

/*-
 * 同一次锁内复制PG boot/floor/generation/门禁和所有计数，不能拼接两次采样。
 * 仅排除当前只读观察命令本身；显式/失败事务或上轮保留事务中的观察不排除，
 * 防止BEGIN;SELECT观察函数把自身长期事务藏起来。其他SU命令同样计为在途。
 * 背景/辅助这里只登记真实生存期，不把“存活”或“没有前台”冒充后台已排空。
 */
void
DsgWorkGetSnapshot(DsgWorkSnapshot *snapshot)
{
	DsgWorkSlot *slot;
	uint32 phases[DSG_WORK_PHASE_COUNT];
	uint32 prestart;
	bool may_exclude = !IsTransactionBlock() && !IsAbortedTransactionBlockState();
	int i;

	if (!enable_dynamic_shared_buffers || !work_registered)
		elog(ERROR, "DSGA work observation requires a registered running server");
	slot = DsgWorkOwnSlot();
	MemSet(snapshot, 0, sizeof(*snapshot));
	SpinLockAcquire(&DSBCtrl->status_lock);
	DsgBufferCopySnapshotLocked(&snapshot->observed);
	memcpy(phases, work_control->phases, sizeof(phases));
	snapshot->retained_transactions = work_control->retained_transactions;
	if (may_exclude && slot->kind == DSG_WORK_CLIENT &&
		slot->pid == MyProcPid && slot->phase == DSG_WORK_EXECUTING &&
		!slot->retained_transaction)
	{
		phases[DSG_WORK_EXECUTING]--;
		snapshot->observer_excluded = true;
	}
	snapshot->background_processes = work_control->backgrounds;
	snapshot->auxiliary_processes = work_control->auxiliaries;
	/* PGPROC交接持同锁；Postmaster只有原子加/退出减，固定此读取点即可。 */
	prestart = pg_atomic_read_u32(&work_control->prestart_clients);
	SpinLockRelease(&DSBCtrl->status_lock);
	if (prestart > (uint32) prestart_slot_count)
		elog(ERROR, "DSGA prestart registry count is inconsistent");
	phases[DSG_WORK_STARTUP] += prestart;
	snapshot->startup_clients = phases[DSG_WORK_STARTUP];
	snapshot->idle_clients = phases[DSG_WORK_IDLE];
	snapshot->frontend_reads = phases[DSG_WORK_READING];
	snapshot->frontend_executes = phases[DSG_WORK_EXECUTING];
	snapshot->bounded_inputs = phases[DSG_WORK_BOUNDED_INPUT];
	for (i = 0; i < DSG_WORK_PHASE_COUNT; i++)
		snapshot->client_processes += phases[i];
	snapshot->foreground_idle = !snapshot->observed.admission_open &&
		snapshot->startup_clients == 0 && snapshot->frontend_reads == 0 &&
		snapshot->frontend_executes == 0 && snapshot->retained_transactions == 0;
}
