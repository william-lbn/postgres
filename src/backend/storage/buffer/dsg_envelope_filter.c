/*-
 * dsg_envelope_filter.c -- 四数组不可腾空VA范围与唯一Postmaster fork边界。
 *
 * 仅显式NEON_DSGA_EARLY_ARRAYS_FILTER=test-v1启用，必须已有完整早期R。
 * 此代码安装真实不可放宽cBPF，不把调用者env或/proc Seccomp=2当Root资格。
 * 普通memory操作只拒与R相交的完整页范围；不能只查起点，必须核64bit
 * 加法、页ceil、跨界和溢出。PM只可对准确四区做两种原Park/Rearm替换，
 * child叠加禁止任何覆盖。其余文件域/平台来源仍须独立证明，normal拒绝不变。
 */
/* Linux syscall标志须在任何系统头之前启用，不能依赖外部make命令偶然带GNU宏。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "postgres.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include "storage/dsg_envelope_birth.h"
#include "storage/dsg_envelope_filter.h"
#include "storage/dsg_native_birth_wire.h"
#include "storage/dsg_filter_audit.h"

static DsgBirthFilterSnapshot state = {.protocol_version = NEON_DSG_BIRTH_FILTER_API_VERSION};
static pid_t postmaster_pid;
static bool early_seen;
/* 仅原PM leader写这些私有状态；非leader先核真实TID再拒绝，不竞争读取。 */
static bool fork_active;
static bool reinitializing;
static pid_t pending_pid;
static int ready_pipe[2] = {-1,-1};

static pg_attribute_noreturn() void
DsgFilterFail(void)
{
	const char *text = "DSGA birth filter could not establish its exact native policy\n";
	(void) write(STDERR_FILENO, text, strlen(text));
	_exit(1);
}

#if defined(__linux__) && defined(__x86_64__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <poll.h>

#define FILTER_MAX 1024
#define LABEL_MAX 1024
#define X32_BIT 0x40000000U
typedef struct DsgFilterBuilder
{
	struct sock_filter code[FILTER_MAX];
	unsigned labels[LABEL_MAX];
	struct { unsigned at; unsigned label; } fixups[FILTER_MAX];
	unsigned count;
	unsigned label_count;
	unsigned fixup_count;
} DsgFilterBuilder;

static unsigned
NewLabel(DsgFilterBuilder *b)
{
	unsigned label = b->label_count++;
	if (label >= LABEL_MAX)
		DsgFilterFail();
	b->labels[label] = UINT_MAX;
	return label;
}
static void
Mark(DsgFilterBuilder *b, unsigned label)
{
	if (label >= b->label_count || b->labels[label] != UINT_MAX)
		DsgFilterFail();
	b->labels[label] = b->count;
}
static void
Emit(DsgFilterBuilder *b, unsigned short code, uint32 k)
{
	if (b->count >= FILTER_MAX)
		DsgFilterFail();
	b->code[b->count++] = (struct sock_filter) {code, 0, 0, k};
}
static void
Go(DsgFilterBuilder *b, unsigned label)
{
	unsigned f = b->fixup_count++;
	if (f >= FILTER_MAX)
		DsgFilterFail();
	b->fixups[f].at = b->count;
	b->fixups[f].label = label;
	Emit(b, BPF_JMP | BPF_JA, 0);
}
/* 条件只跳0/1，再用32bit JA前跳；避免大策略超过cBPF的8bit分支距离。 */
static void
If(DsgFilterBuilder *b, unsigned short test, uint32 k, unsigned yes, unsigned no)
{
	Emit(b, BPF_JMP | test, k);
	b->code[b->count - 1].jf = 1;
	Go(b, yes);
	Go(b, no);
}
static void
Arg(DsgFilterBuilder *b, unsigned arg, bool high)
{
	Emit(b, BPF_LD | BPF_W | BPF_ABS,
		 offsetof(struct seccomp_data, args) + 8 * arg + (high ? 4 : 0));
}
static void
ArgEquals(DsgFilterBuilder *b, unsigned arg, uint64 value, unsigned yes, unsigned no)
{
	unsigned low = NewLabel(b);
	Arg(b, arg, true);
	If(b, BPF_JEQ | BPF_K, value >> 32, low, no);
	Mark(b, low);
	Arg(b, arg, false);
	If(b, BPF_JEQ | BPF_K, (uint32) value, yes, no);
}
static void
MemLess(DsgFilterBuilder *b, unsigned lo, unsigned hi, uint64 value, unsigned yes, unsigned no)
{
	unsigned le = NewLabel(b), equal = NewLabel(b);
	Emit(b, BPF_LD | BPF_MEM, hi);
	If(b, BPF_JGT | BPF_K, value >> 32, no, le);
	Mark(b, le);
	If(b, BPF_JGE | BPF_K, value >> 32, equal, yes);
	Mark(b, equal);
	Emit(b, BPF_LD | BPF_MEM, lo);
	If(b, BPF_JGE | BPF_K, (uint32) value, no, yes);
}
static void
MemGreater(DsgFilterBuilder *b, unsigned lo, unsigned hi, uint64 value, unsigned yes, unsigned no)
{
	unsigned le = NewLabel(b), equal = NewLabel(b);
	Emit(b, BPF_LD | BPF_MEM, hi);
	If(b, BPF_JGT | BPF_K, value >> 32, yes, le);
	Mark(b, le);
	If(b, BPF_JGE | BPF_K, value >> 32, equal, no);
	Mark(b, equal);
	Emit(b, BPF_LD | BPF_MEM, lo);
	If(b, BPF_JGT | BPF_K, (uint32) value, yes, no);
}

/* M[0:1]=start，M[2:3]=ceil(len)，M[4:5]=end，M[6]=low carry。 */
static void
Span(DsgFilterBuilder *b, Size page, unsigned allow, unsigned deny)
{
	unsigned aligned=NewLabel(b), nonzero=NewLabel(b), carry=NewLabel(b);
	unsigned no_carry=NewLabel(b), rounded=NewLabel(b), high_ok=NewLabel(b);
	unsigned end_carry=NewLabel(b), end_no_carry=NewLabel(b), end_high=NewLabel(b);
	unsigned sum_ok=NewLabel(b), add_carry=NewLabel(b), done=NewLabel(b), increment=NewLabel(b);
	Arg(b,0,false);Emit(b,BPF_ST,0);
	Arg(b,0,true);Emit(b,BPF_ST,1);
	Arg(b,1,false);Emit(b,BPF_ST,2);
	Arg(b,1,true);Emit(b,BPF_ST,3);
	Emit(b,BPF_LD|BPF_MEM,0);Emit(b,BPF_ALU|BPF_AND|BPF_K,page-1);
	If(b,BPF_JEQ|BPF_K,0,aligned,deny);
	Mark(b,aligned);
	Emit(b,BPF_LD|BPF_MEM,2);Emit(b,BPF_LDX|BPF_MEM,3);Emit(b,BPF_ALU|BPF_OR|BPF_X,0);
	If(b,BPF_JEQ|BPF_K,0,allow,nonzero);
	Mark(b,nonzero);
	Emit(b,BPF_LD|BPF_MEM,2);Emit(b,BPF_MISC|BPF_TAX,0);
	Emit(b,BPF_ALU|BPF_ADD|BPF_K,page-1);Emit(b,BPF_ST,2);
	If(b,BPF_JGE|BPF_X,0,no_carry,carry);
	Mark(b,carry);Emit(b,BPF_LD|BPF_MEM,3);
	If(b,BPF_JEQ|BPF_K,UINT_MAX,deny,high_ok);
	Mark(b,high_ok);Emit(b,BPF_ALU|BPF_ADD|BPF_K,1);Emit(b,BPF_ST,3);Go(b,rounded);
	Mark(b,no_carry);Go(b,rounded);
	Mark(b,rounded);Emit(b,BPF_LD|BPF_MEM,2);Emit(b,BPF_ALU|BPF_AND|BPF_K,~((uint32)page-1));Emit(b,BPF_ST,2);
	Emit(b,BPF_LD|BPF_MEM,0);Emit(b,BPF_LDX|BPF_MEM,2);Emit(b,BPF_ALU|BPF_ADD|BPF_X,0);Emit(b,BPF_ST,4);
	Emit(b,BPF_LDX|BPF_MEM,0);If(b,BPF_JGE|BPF_X,0,end_no_carry,end_carry);
	Mark(b,end_carry);Emit(b,BPF_LD|BPF_IMM,1);Emit(b,BPF_ST,6);Go(b,end_high);
	Mark(b,end_no_carry);Emit(b,BPF_LD|BPF_IMM,0);Emit(b,BPF_ST,6);Go(b,end_high);
	Mark(b,end_high);Emit(b,BPF_LD|BPF_MEM,1);Emit(b,BPF_LDX|BPF_MEM,3);Emit(b,BPF_ALU|BPF_ADD|BPF_X,0);Emit(b,BPF_ST,5);
	Emit(b,BPF_LDX|BPF_MEM,1);If(b,BPF_JGE|BPF_X,0,sum_ok,deny);
	Mark(b,sum_ok);Emit(b,BPF_LD|BPF_MEM,6);If(b,BPF_JEQ|BPF_K,0,done,add_carry);
	Mark(b,add_carry);Emit(b,BPF_LD|BPF_MEM,5);If(b,BPF_JEQ|BPF_K,UINT_MAX,deny,increment);
	Mark(b,increment);Emit(b,BPF_ALU|BPF_ADD|BPF_K,1);Emit(b,BPF_ST,5);Go(b,done);
	Mark(b,done);
}

static void
ExactReplacement(DsgFilterBuilder *b, const DsgRegion *r, unsigned allow, unsigned next)
{
	unsigned length=NewLabel(b), offset=NewLabel(b), fd=NewLabel(b), prot=NewLabel(b);
	unsigned park=NewLabel(b), shared=NewLabel(b), park_flags=NewLabel(b), shared_prot=NewLabel(b), shared_flags=NewLabel(b);
	ArgEquals(b,0,(uint64)(uintptr_t)r->address,length,next);Mark(b,length);
	ArgEquals(b,1,r->mapped_bytes,offset,next);Mark(b,offset);
	ArgEquals(b,5,0,fd,next);Mark(b,fd);
	Arg(b,4,false);If(b,BPF_JEQ|BPF_K,UINT_MAX,prot,next);Mark(b,prot);
	Arg(b,2,false);If(b,BPF_JEQ|BPF_K,PROT_NONE,park,shared);
	Mark(b,park);ArgEquals(b,2,PROT_NONE,park_flags,next);Mark(b,park_flags);
	ArgEquals(b,3,MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE,allow,next);
	Mark(b,shared);ArgEquals(b,2,PROT_READ|PROT_WRITE,shared_prot,next);Mark(b,shared_prot);
	Go(b,shared_flags);Mark(b,shared_flags);
	ArgEquals(b,3,MAP_FIXED|MAP_ANONYMOUS|MAP_SHARED,allow,next);
}

static unsigned
Build(DsgFilterBuilder *b, bool child, bool birth_open, const DsgBirthArraysSnapshot *input)
{
	DsgBirthArraysSnapshot arrays;
	unsigned arch=NewLabel(b), nr=NewLabel(b), native=NewLabel(b);
	unsigned allow=NewLabel(b), deny=NewLabel(b), nosys=NewLabel(b), kill=NewLabel(b);
	unsigned clone=NewLabel(b), mmap_rule=NewLabel(b), fixed=NewLabel(b), span=NewLabel(b);
	unsigned shmat_rule=NewLabel(b), sendto_rule=NewLabel(b), sendmsg_rule=NewLabel(b);
	unsigned setsockopt_rule=NewLabel(b), sock_level=NewLabel(b), sock_option=NewLabel(b);
	unsigned open_rule=NewLabel(b), openat_rule=NewLabel(b), fcntl_rule=NewLabel(b), setfl_rule=NewLabel(b);
	unsigned advice_rule=NewLabel(b), advice_other=NewLabel(b), receive_rule=NewLabel(b);
	unsigned advice_list=NewLabel(b);
	unsigned prctl_rule=NewLabel(b), dumpable_rule=NewLabel(b);
	unsigned mmap_shared=NewLabel(b), mmap_fixed_check=NewLabel(b);
	int i;
	static const int forbidden[]={
		__NR_execve,__NR_execveat,__NR_vfork,__NR_mremap,__NR_remap_file_pages,
		__NR_unshare,__NR_setns,__NR_memfd_create,__NR_userfaultfd,__NR_ptrace,
		__NR_process_vm_readv,__NR_process_vm_writev,__NR_pidfd_getfd,
		__NR_io_uring_setup,__NR_io_uring_enter,__NR_io_uring_register,
		__NR_vmsplice,__NR_splice,__NR_tee,__NR_sendfile,
		__NR_mlock,__NR_mlock2,__NR_mlockall,__NR_recvmmsg,
		__NR_mbind,__NR_set_mempolicy,__NR_move_pages,__NR_migrate_pages,
		/* home_node不直接迁移，但仍改变未来NUMA策略；受限profile不能漂移。 */
		__NR_set_mempolicy_home_node,
		__NR_process_madvise,__NR_process_mrelease,
		-1};
	static const int other_advice[]={MADV_NORMAL,MADV_RANDOM,MADV_SEQUENTIAL,
		MADV_WILLNEED,MADV_DONTNEED,MADV_FREE,MADV_DONTDUMP,MADV_DODUMP,
		MADV_NOHUGEPAGE,MADV_DONTFORK,MADV_DOFORK,MADV_WIPEONFORK,
		MADV_KEEPONFORK,MADV_POPULATE_READ,-1};
	if (input == NULL)
		DsgFilterFail();
	arrays = *input;
	if (!arrays.enabled || arrays.page_size_bytes>UINT_MAX)
		DsgFilterFail();
	Emit(b,BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch));
	If(b,BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,arch,kill);
	Mark(b,arch);Emit(b,BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
	If(b,BPF_JSET|BPF_K,X32_BIT,kill,nr);Mark(b,nr);
	for(i=0;forbidden[i]>=0;i++)
	{
		unsigned next=NewLabel(b);
		If(b,BPF_JEQ|BPF_K,forbidden[i],deny,next);Mark(b,next);
	}
	/* clone3结构在用户地址，cBPF不能读它；ENOSYS让真实pthread退回已审clone。 */
	If(b,BPF_JEQ|BPF_K,__NR_clone3,nosys,native);Mark(b,native);
#define ROUTE(number,target) do { unsigned next=NewLabel(b); If(b,BPF_JEQ|BPF_K,(number),(target),next);Mark(b,next); } while(0)
	ROUTE(__NR_fork,child?deny:allow);
	ROUTE(__NR_clone,clone);
	ROUTE(__NR_mmap,mmap_rule);
	ROUTE(__NR_munmap,span);ROUTE(__NR_mprotect,span);ROUTE(__NR_pkey_mprotect,span);
	ROUTE(__NR_madvise,advice_rule);
	ROUTE(__NR_shmat,shmat_rule);
	ROUTE(__NR_recvmsg,receive_rule);ROUTE(__NR_prctl,prctl_rule);
	ROUTE(__NR_sendto,sendto_rule);ROUTE(__NR_sendmmsg,sendto_rule);ROUTE(__NR_sendmsg,sendmsg_rule);
	ROUTE(__NR_setsockopt,setsockopt_rule);
	ROUTE(__NR_open,open_rule);ROUTE(__NR_openat,openat_rule);
	ROUTE(__NR_openat2,nosys);ROUTE(__NR_fcntl,fcntl_rule);
	Go(b,allow);
#undef ROUTE
	Mark(b,clone);
	/* cBPF按AND继承：不能PM禁线程再由child放行。两者均准准确pthread，
	 * PM独立mm只准glibc fork签名；唯一实际forker由可信原生leader门禁
	 * 与reinit状态共同生产，绝不冒称纯kernel限制PM任意代码的线程行为。 */
	{
		uint64 flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
			CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID;
		unsigned alternative=NewLabel(b), process=NewLabel(b);
		ArgEquals(b,0,flags,allow,alternative);Mark(b,alternative);
		ArgEquals(b,0,flags|CLONE_CHILD_SETTID,allow,process);Mark(b,process);
		if(child)
			Go(b,deny);
		else
			ArgEquals(b,0,CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD,allow,deny);
	}
	/* DONTFORK等会使后继任务失去R；R相交只保留原Provider两种原生操作。 */
	Mark(b,advice_rule);ArgEquals(b,2,MADV_REMOVE,allow,advice_other);
	Mark(b,advice_other);ArgEquals(b,2,MADV_POPULATE_WRITE,allow,advice_list);
	Mark(b,advice_list);
	/* 非R也不泛放PAGEOUT/COLD/COLLAPSE或未来advice；其在途成本必须逐项有源。 */
	for(i=0;other_advice[i]>=0;i++)
	{
		unsigned next=NewLabel(b);
		ArgEquals(b,2,other_advice[i],span,next);Mark(b,next);
	}
	Go(b,deny);
	/* 只在早期Root握手阶段给固定FD接收精确ACK，Seal后及所有child禁FD导入。 */
	Mark(b,receive_rule);
	if(!child && birth_open)
		ArgEquals(b,0,DSG_BIRTH_WIRE_FD,allow,deny);
	else
		Go(b,deny);
	Mark(b,prctl_rule);Arg(b,0,false);If(b,BPF_JEQ|BPF_K,PR_SET_DUMPABLE,dumpable_rule,allow);
	Mark(b,dumpable_rule);ArgEquals(b,1,0,allow,deny);
	Mark(b,shmat_rule);Arg(b,2,false);If(b,BPF_JSET|BPF_K,SHM_REMAP,deny,allow);
	Mark(b,sendto_rule);Arg(b,3,false);If(b,BPF_JSET|BPF_K,MSG_ZEROCOPY,deny,allow);
	Mark(b,sendmsg_rule);Arg(b,2,false);If(b,BPF_JSET|BPF_K,MSG_ZEROCOPY,deny,allow);
	Mark(b,setsockopt_rule);Arg(b,1,false);If(b,BPF_JEQ|BPF_K,SOL_SOCKET,sock_level,allow);
	Mark(b,sock_level);Arg(b,2,false);If(b,BPF_JEQ|BPF_K,SO_ZEROCOPY,deny,sock_option);
	Mark(b,sock_option);Go(b,allow);
	Mark(b,open_rule);Arg(b,1,false);If(b,BPF_JSET|BPF_K,O_DIRECT,deny,allow);
	Mark(b,openat_rule);Arg(b,2,false);If(b,BPF_JSET|BPF_K,O_DIRECT,deny,allow);
	Mark(b,fcntl_rule);Arg(b,1,false);If(b,BPF_JEQ|BPF_K,F_SETFL,setfl_rule,allow);
	Mark(b,setfl_rule);Arg(b,2,false);If(b,BPF_JSET|BPF_K,O_DIRECT,deny,allow);
	Mark(b,mmap_rule);
	if(child)
	{
		/* R外共享匿名backing不受固定tmpfs额度约束；多线程final fput
		 * 还可能落后VA解除，因此child只能继承，不能自行创建这种对象。 */
		Arg(b,3,false);If(b,BPF_JSET|BPF_K,MAP_ANONYMOUS,mmap_shared,mmap_fixed_check);
		Mark(b,mmap_shared);Arg(b,3,false);If(b,BPF_JSET|BPF_K,MAP_SHARED,deny,mmap_fixed_check);
	}
	else
		Go(b,mmap_fixed_check);
	Mark(b,mmap_fixed_check);Arg(b,3,false);If(b,BPF_JSET|BPF_K,MAP_FIXED,fixed,allow);
	Mark(b,fixed);
	if(!child)
		for(i=0;i<DSG_BUFFER_ARRAY_COUNT;i++)
		{
			unsigned next=NewLabel(b);
			ExactReplacement(b,&arrays.ranges[i].region,allow,next);Mark(b,next);
		}
	Go(b,span);
	Mark(b,span);Span(b,arrays.page_size_bytes,allow,deny);
	for(i=0;i<DSG_BUFFER_ARRAY_COUNT;i++)
	{
		DsgRegion *r=&arrays.ranges[i].region;
		uint64 left=(uint64)(uintptr_t)r->address;
		unsigned examine_end=NewLabel(b),next=NewLabel(b);
		if(left>PG_UINT64_MAX-r->mapped_bytes)
			DsgFilterFail();
		MemLess(b,0,1,left+r->mapped_bytes,examine_end,next);Mark(b,examine_end);
		MemGreater(b,4,5,left,deny,next);Mark(b,next);
	}
	Go(b,allow);
	Mark(b,allow);Emit(b,BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
	Mark(b,deny);Emit(b,BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM);
	Mark(b,nosys);Emit(b,BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS);
	Mark(b,kill);Emit(b,BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS);
	for(i=0;i<b->fixup_count;i++)
	{
		unsigned at=b->fixups[i].at,pos=b->labels[b->fixups[i].label];
		if(pos==UINT_MAX || pos<=at)
			DsgFilterFail();
		b->code[at].k=pos-at-1;
	}
	return b->count;
}

static unsigned
Install(bool child, bool birth_open)
{
	DsgFilterBuilder builder={0};
	struct sock_fprog program;
	DsgBirthArraysSnapshot arrays;
	DsgBirthArraysGetSnapshot(&arrays);
	program.len=Build(&builder,child,birth_open,&arrays);
	program.filter=builder.code;
	if(prctl(PR_GET_NO_NEW_PRIVS,0L,0L,0L,0L)!=1 ||
	   prctl(PR_SET_SECCOMP,(unsigned long)SECCOMP_MODE_FILTER,&program,0L,0L)!=0)
		DsgFilterFail();
	return program.len;
}
#endif

void
DsgBirthFilterEarlyInit(void)
{
	const char *raw=getenv(DSG_BIRTH_FILTER_ENV);
	if(early_seen)
		DsgFilterFail();
	early_seen=true;
	if(raw==NULL)
		return;
	if ((strcmp(raw,"test-v1")!=0 && strcmp(raw,"root-v1")!=0) || !DsgBirthArraysEnabled())
		DsgFilterFail();
	if (strcmp(raw,"root-v1")==0 && !DsgNativeBirthWireRootPrepared())
		DsgFilterFail();
#if defined(__linux__) && defined(__x86_64__)
	postmaster_pid=getpid();
	state.postmaster_instructions=Install(false,true);
	state.enabled=true;
	state.postmaster_installed=true;
#else
	DsgFilterFail();
#endif
}

void
DsgBirthFilterSealPostmaster(void)
{
	if(!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	if(getpid()!=postmaster_pid || state.finalized)
		DsgFilterFail();
	if (strcmp(getenv(DSG_BIRTH_FILTER_ENV),"root-v1")==0 && !DsgNativeBirthWireRootACKClosed())
		DsgFilterFail();
	state.final_postmaster_instructions=Install(false,false);
	state.finalized=true;
#else
	DsgFilterFail();
#endif
}

void
DsgBirthFilterInstallChild(void)
{
	if(!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	if(getpid()==postmaster_pid || state.child_installed)
		DsgFilterFail();
	if(!state.finalized)
		DsgFilterFail();
	state.child_instructions=Install(true,false);
	state.child_installed=true;
#else
	DsgFilterFail();
#endif
}

void
DsgBirthFilterGetSnapshot(DsgBirthFilterSnapshot *snapshot)
{
	*snapshot=state;
}


bool
DsgBirthFilterEnabled(void)
{
	return state.enabled;
}

#if defined(__linux__) && defined(__x86_64__)
static bool
OriginalLeader(void)
{
	return getpid()==postmaster_pid && syscall(SYS_gettid)==(long)postmaster_pid;
}
static void
CloseReady(int which)
{
	if(ready_pipe[which]>=0)
	{
		close(ready_pipe[which]);
		ready_pipe[which]=-1;
	}
}
#define READY_MAGIC 0x44534752U
#endif

bool
DsgBirthFilterForkBegin(void)
{
	if(!state.enabled)
		return true;
#if defined(__linux__) && defined(__x86_64__)
	if(!OriginalLeader())
	{
		errno=EPERM;
		return false;
	}
	if(fork_active || reinitializing || pending_pid!=0 || ready_pipe[0]>=0 || ready_pipe[1]>=0)
	{
		errno=EBUSY;
		return false;
	}
	fork_active=true;
	if(pipe2(ready_pipe,O_CLOEXEC)!=0)
	{
		fork_active=false;
		return false;
	}
	return true;
#else
	DsgFilterFail();
#endif
}

pid_t
DsgBirthFilterForkFinish(pid_t result, int original_errno)
{
	if(!state.enabled)
	{
		errno=original_errno;
		return result;
	}
#if defined(__linux__) && defined(__x86_64__)
	if(!fork_active)
		DsgFilterFail();
	if(result==0)
	{
		/* child自己的私有拷贝不是母PM许可；写端只留到Park/filter完成。 */
		CloseReady(0);
		fork_active=false;
		pending_pid=0;
		errno=original_errno;
		return 0;
	}
	if(!OriginalLeader())
		DsgFilterFail();
	if(result<0)
	{
		/* syscall失败已同步清理其未出生mm，不通过ctx取消提前放门禁。 */
		CloseReady(0);CloseReady(1);
		fork_active=false;pending_pid=0;
		errno=original_errno;
		return result;
	}
	{
		struct pollfd descriptor;
		uint32 marker=0;
		int polled;
		ssize_t received=-1;
		pid_t waited;
		pending_pid=result;
		CloseReady(1);
		descriptor.fd=ready_pipe[0];descriptor.events=POLLIN;descriptor.revents=0;
		do { polled=poll(&descriptor,1,10000); } while(polled<0 && errno==EINTR);
		if(polled>0)
			do { received=read(ready_pipe[0],&marker,sizeof(marker)); } while(received<0 && errno==EINTR);
		CloseReady(0);
		if(received==(ssize_t)sizeof(marker) && marker==READY_MAGIC)
		{
			/* 成功仍保active/pending：PG实际名单或辅助全局PID落下后才注册清门。 */
			errno=original_errno;
			return result;
		}
		/* EOF/短读/超时/中断错误不能丢未登记PID。只杀不等不构成无旧R证明。 */
		if(kill(result,SIGKILL)!=0 && errno!=ESRCH)
			DsgFilterFail();
		do { waited=waitpid(result,NULL,0); } while(waited<0 && errno==EINTR);
		if(waited!=result)
			DsgFilterFail();
		fork_active=false;pending_pid=0;
		errno=EIO;
		return -1;
	}
#else
	DsgFilterFail();
#endif
}

void
DsgBirthFilterForkRegistered(pid_t pid)
{
	if(!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	if(!OriginalLeader())
		DsgFilterFail();
	if(pid<=0 && !fork_active && pending_pid==0)
		return;
	if(!fork_active || pending_pid!=pid || ready_pipe[0]>=0 || ready_pipe[1]>=0)
		DsgFilterFail();
	fork_active=false;pending_pid=0;
#else
	DsgFilterFail();
#endif
}

void
DsgBirthFilterChildReady(void)
{
	if(!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	{
		uint32 marker=READY_MAGIC;
		ssize_t sent;
		if(getpid()==postmaster_pid || !state.child_installed || !state.finalized || ready_pipe[1]<0)
			DsgFilterFail();
		do { sent=write(ready_pipe[1],&marker,sizeof(marker)); } while(sent<0 && errno==EINTR);
		CloseReady(1);
		if(sent!=(ssize_t)sizeof(marker))
			DsgFilterFail();
	}
#else
	DsgFilterFail();
#endif
}

#ifdef USE_INJECTION_POINTS
int
DsgBirthFilterTestReadyFD(void)
{
#if defined(__linux__) && defined(__x86_64__)
	const char *mode = getenv(DSG_BIRTH_FILTER_ENV);
	/* 只读getter不把母PM写端、已ready FD或未来root-v1暴露给fixture。 */
	if (state.enabled && mode != NULL && strcmp(mode, "test-v1") == 0 &&
		getpid() != postmaster_pid && ready_pipe[1] >= 0)
		return ready_pipe[1];
#endif
	errno = EPERM;
	return -1;
}
#endif

bool
DsgBirthFilterReinitializeBegin(void)
{
	if(!state.enabled)
		return true;
#if defined(__linux__) && defined(__x86_64__)
	if(!OriginalLeader())
	{
		errno=EPERM;return false;
	}
	if(fork_active || pending_pid!=0 || reinitializing)
	{
		errno=EBUSY;return false;
	}
	reinitializing=true;
	return true;
#else
	DsgFilterFail();
#endif
}

void
DsgBirthFilterReinitializeEnd(void)
{
	if(!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	if(!OriginalLeader() || !reinitializing || fork_active || pending_pid!=0)
		DsgFilterFail();
	reinitializing=false;
#else
	DsgFilterFail();
#endif
}

/* 纯编译入口与Install共用唯一Build；绝不将输出当内核已安装证明。
 * 只允许OS真实root，调用者仍须锁定本二进制和ELF/FS来源并逐完整指令复核。
 */
void
DsgBirthFilterCompileAudit(const DsgBirthArraysSnapshot *arrays,
		bool child, bool birth_open, DsgFilterAuditProgram *program)
{
#if defined(__linux__) && defined(__x86_64__)
	DsgFilterBuilder builder = {0};
	uint32 i;
	StaticAssertStmt(FILTER_MAX == DSG_FILTER_AUDIT_MAX_INSTRUCTIONS, "audit builder capacity must match");
	if (getuid() != 0 || geteuid() != 0 || program == NULL)
		DsgFilterFail();
	program->count = Build(&builder, child, birth_open, arrays);
	for (i = 0; i < program->count; i++)
	{
		program->instructions[i].code = builder.code[i].code;
		program->instructions[i].jt = builder.code[i].jt;
		program->instructions[i].jf = builder.code[i].jf;
		program->instructions[i].k = builder.code[i].k;
	}
#else
	DsgFilterFail();
#endif
}
