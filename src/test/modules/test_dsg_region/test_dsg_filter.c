/*-
 * 真实syscall范围测试，只操作当前独占PG/test-v1。原管理员双门禁+SQL REVOKE，
 * 不提供任意地址输入，不为Root/FS/normal资格签发任何证明。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "postgres.h"
#include "postmaster/fork_process.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
PG_FUNCTION_INFO_V1(test_dsg_birth_filter);
PG_FUNCTION_INFO_V1(test_dsg_filter_probe);
void test_dsg_filter_pm_init(void);
#if defined(__linux__) && defined(__x86_64__)
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/dsg_envelope_birth.h"
#include "storage/dsg_envelope_filter.h"
#include "utils/builtins.h"


static void
Admin(void)
{
	if(!superuser() || !superuser_arg(GetSessionUserId()))
		ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			errmsg("native filter probes require original administrator")));
}

static void *
Thread(void *arg)
{
	*(int *)arg=1;
	return NULL;
}

typedef struct NonLeaderResult
{
 pid_t result;
 int error;
 bool reinit;
 int reinit_error;
} NonLeaderResult;

static void *
NonLeader(void *arg)
{
 NonLeaderResult *r=arg;
 errno=0;r->result=fork_process();r->error=errno;
 if(r->result==0)_exit(74);
 errno=0;r->reinit=DsgBirthFilterReinitializeBegin();r->reinit_error=errno;
 return NULL;
}

#ifdef USE_INJECTION_POINTS
/* 四条真实活child路径：制造错误后仍睡眠，必须由母PM精确kill+wait。
 * 不mock poll/read/wait，不将timeout或EOF冒充child已无mm的证明。 */
static void
ReadyFailures(void)
{
	int mode;
	for (mode = 0; mode < 4; mode++)
	{
		pid_t child;
		pid_t extra;
		int saved_errno;
		errno = 0;
		child = fork_process();
		if (child == 0)
		{
			int fd = DsgBirthFilterTestReadyFD();
			uint32 wrong = 0x11223344U;
			if (fd < 0)
				_exit(81);
			{
				const char *markers[] = {
					"DSGA_READY_CHILD_LIVE mode=short\n",
					"DSGA_READY_CHILD_LIVE mode=wrong\n",
					"DSGA_READY_CHILD_LIVE mode=eof\n",
					"DSGA_READY_CHILD_LIVE mode=timeout\n"
				};
				/* 到达marker前任何fixture错误不得被EIO结果掩盖。 */
				if (write(STDERR_FILENO, markers[mode], strlen(markers[mode])) != strlen(markers[mode]))
					_exit(83);
			}
			/* marker先于消息：母PM一旦读错误消息即可kill，不能依赖其后日志。
			 * 真写入失败先报告独立fixture错误，测试必须拒绝这类假阳。 */
			if (mode < 2 && write(fd, &wrong, mode == 0 ? 2 : 4) != (mode == 0 ? 2 : 4))
			{
				const char *failure = "DSGA_READY_SETUP_FAILED\n";
				(void) write(STDERR_FILENO, failure, strlen(failure));
				_exit(82);
			}
			if (mode < 3)
				close(fd);
			/* mode3不写也不关，必须真正走10秒poll超时。SIGKILL不可阻塞。 */
			for (;;)
				pause();
		}
		saved_errno = errno;
		if (child != -1 || saved_errno != EIO)
			elog(ERROR, "ready negative mode %d did not reject with EIO", mode);
		errno = 0;
		extra = waitpid(-1, NULL, WNOHANG);
		if (extra != -1 || errno != ECHILD)
			elog(ERROR, "ready negative mode %d retained an unregistered child", mode);
	}
	ereport(LOG, (errmsg("DSGA_FILTER_READY_NEGATIVE_OK short wrong eof timeout exact_reap=4")));
}
#endif

/* preloaded PM只在显式fixture执行；没有任何SQL调用此函数。 */
void
test_dsg_filter_pm_init(void)
{
	const char *raw=getenv("NEON_DSGA_FILTER_PM_PROBE");
	DsgBirthFilterSnapshot filter;
	DsgBirthArraysSnapshot arrays;
	int i;
	int rc;
	NonLeaderResult nonleader={0};
	pid_t child,waited;
	pthread_t thread;
	if(raw==NULL)
		return;
	DsgBirthFilterGetSnapshot(&filter);
	if((strcmp(raw,"exercise-v1")!=0 && strcmp(raw,"exercise-negative-v1")!=0) ||
		!process_shared_preload_libraries_in_progress ||
		IsUnderPostmaster || !filter.enabled || !filter.finalized || filter.child_installed)
		elog(ERROR,"PM probe requires exact original test-only filter preload");
	if (strcmp(raw, "exercise-negative-v1") == 0)
	{
#ifdef USE_INJECTION_POINTS
		ReadyFailures();
#else
		elog(ERROR, "ready negative fixture requires injection-points build");
#endif
	}
	DsgBirthArraysGetSnapshot(&arrays);
	for(i=0;i<DSG_BUFFER_ARRAY_COUNT;i++)
	{
		DsgRegion copy=arrays.ranges[i].region;
		/* 真PM策略必须准原几何Rearm/Park；最后恢复无访问占位交buf_init。 */
		DsgRegionRearm(&copy);
		copy.address[0]=0x31;
		DsgRegionPark(&copy);
		errno=0;
		if(madvise(copy.address,copy.mapped_bytes,MADV_DONTFORK)!=-1 || errno!=EPERM)
			elog(ERROR,"PM filter allowed an R DONTFORK escape");
	}
	/* PM与child都可有合法pthread；可信原生fork/reinit只准原leader。
	 * 这不是纯cBPF身份判定，生产还必须绑定无旁路fork的固定代码manifest。 */
	rc=pthread_create(&thread,NULL,NonLeader,&nonleader);
	if(rc==0)rc=pthread_join(thread,NULL);
	if(rc!=0 || nonleader.result!=-1 || nonleader.error!=EPERM ||
		nonleader.reinit || nonleader.reinit_error!=EPERM)
		elog(ERROR,"native nonleader fork/reinit gate failed");
	/* 真fork后child未写ready即退出：母PM须确认reap后才返回EIO清门禁。 */
	errno=0;child=fork_process();
	if(child==0)_exit(71);
	if(child!=-1 || errno!=EIO)
		elog(ERROR,"unready child was not synchronously rejected and reaped");
	/* 正常child在真实策略安装后ready，母PM保持pending到明确wait/reap后结清。 */
	child=fork_process();
	if(child==0)
	{
		DsgBirthFilterInstallChild();DsgBirthFilterChildReady();_exit(0);
	}
	if(child<0)elog(ERROR,"ready child fork unexpectedly failed");
	/* 就绪不等于名单登记，唯一活shadow不能被第二fork或Park/reinit越过。 */
	errno=0;
	if(DsgBirthFilterReinitializeBegin() || errno!=EBUSY)
		elog(ERROR,"ready but unregistered child allowed reinitialization");
	errno=0;
	if(fork_process()!=-1 || errno!=EBUSY)
		elog(ERROR,"ready but unregistered child allowed a second fork");
	do { waited=waitpid(child,NULL,0); } while(waited<0 && errno==EINTR);
	if(waited!=child)elog(ERROR,"ready child was not reaped");
	DsgBirthFilterForkRegistered(child);
	errno=0;
	if(syscall(SYS_clone3,NULL,0)!=-1 || errno!=ENOSYS)
		elog(ERROR,"PM clone3 did not fail with exact ENOSYS");
	ereport(LOG,(errmsg("DSGA_FILTER_PM_TEST_OK exact_rearm_park=4 leader_gate ready_reaped pthread_preserved no_clone3")));
}

Datum
test_dsg_birth_filter(PG_FUNCTION_ARGS)
{
	DsgBirthFilterSnapshot s;
	StringInfoData out;
	Admin();DsgBirthFilterGetSnapshot(&s);initStringInfo(&out);
	appendStringInfo(&out,"{\"protocol_version\":%u,\"enabled\":%s,\"postmaster_installed\":%s,"
		"\"child_installed\":%s,\"finalized\":%s,\"postmaster_instructions\":%u,"
		"\"final_postmaster_instructions\":%u,\"child_instructions\":%u,\"root_enrolled\":false}",
		s.protocol_version,s.enabled?"true":"false",s.postmaster_installed?"true":"false",
		s.child_installed?"true":"false",s.finalized?"true":"false",
		s.postmaster_instructions,s.final_postmaster_instructions,s.child_instructions);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

Datum
test_dsg_filter_probe(PG_FUNCTION_ARGS)
{
	DsgBirthFilterSnapshot filter;
	DsgBirthArraysSnapshot arrays;
	DsgRegion *r;
	Size page;
	char *action;
	long result=-2;
	int error=0;
	StringInfoData out;
	Admin();DsgBirthFilterGetSnapshot(&filter);
	if(!filter.enabled || !filter.child_installed || !filter.finalized)
		elog(ERROR,"filter probes require actual native child policy");
	DsgBirthArraysGetSnapshot(&arrays);r=&arrays.ranges[0].region;page=arrays.page_size_bytes;
	action=text_to_cstring(PG_GETARG_TEXT_PP(0));
	errno=0;
	if(strcmp(action,"mprotect-r")==0)
		result=mprotect(r->address,page,PROT_READ);
	else if(strcmp(action,"mprotect-straddle")==0)
		result=mprotect(r->address-page,2*page,PROT_READ);
	else if(strcmp(action,"pkey-r")==0)
		result=syscall(SYS_pkey_mprotect,r->address,page,PROT_READ,0);
	else if(strcmp(action,"munmap-r")==0)
		result=munmap(r->address,page);
	else if(strcmp(action,"munmap-straddle")==0)
		result=munmap(r->address-page,page+1);
	else if(strcmp(action,"munmap-wrap")==0)
		result=munmap((void *)(UINTPTR_MAX-page+1),2*page);
	else if(strcmp(action,"madv-dontfork-r")==0)
		result=madvise(r->address,page,MADV_DONTFORK);
	else if(strcmp(action,"madv-wipe-r")==0)
		result=madvise(r->address,page,MADV_WIPEONFORK);
	else if(strcmp(action,"madv-cold-r")==0)
		result=madvise(r->address,page,MADV_COLD);
	else if(strcmp(action,"mmap-exact-park")==0)
		result=mmap(r->address,r->mapped_bytes,PROT_NONE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED?-1:0;
	else if(strcmp(action,"mmap-partial-r")==0)
		result=mmap(r->address,page,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED?-1:0;
	else if(strcmp(action,"anonymous-shared-outside")==0)
	{
		void *address=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
		if(address!=MAP_FAILED)
			munmap(address,page);
		result=address==MAP_FAILED?-1:0;
	}
	else if(strcmp(action,"shmat-remap")==0)
		result=syscall(SYS_shmat,-1,r->address,SHM_REMAP);
	else if(strcmp(action,"mremap-r")==0)
		result=syscall(SYS_mremap,r->address,page,page,0);
	else if(strcmp(action,"remap-file")==0)
		result=syscall(SYS_remap_file_pages,r->address,page,0,0,0);
	else if(strcmp(action,"fork")==0 || strcmp(action,"clone-nonthread")==0)
	{
		pid_t pid=strcmp(action,"fork")==0?fork():syscall(SYS_clone,SIGCHLD,NULL,NULL,NULL,0);
		if(pid==0)
			_exit(72);
		if(pid>0)
			waitpid(pid,NULL,0);
		result=pid==-1?-1:0;
	}
	else if(strcmp(action,"clone3")==0)
		result=syscall(SYS_clone3,NULL,0);
	else if(strcmp(action,"pthread")==0)
	{
		pthread_t thread;
		int ran=0,rc=pthread_create(&thread,NULL,Thread,&ran);
		if(rc==0)
			rc=pthread_join(thread,NULL);
		result=rc==0 && ran==1?0:-1;errno=rc;
	}
	else if(strcmp(action,"private-outside")==0)
	{
		char *memory=mmap(NULL,page*3,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
		if(memory==MAP_FAILED)
			result=-1;
		else
		{
			memset(memory,0x55,page*3);
			result=mprotect(memory+page,page,PROT_READ);
			if(result==0)result=mprotect(memory+page,page,PROT_READ|PROT_WRITE);
			if(result==0)result=madvise(memory,page*3,MADV_DONTNEED);
			if(munmap(memory,page*3)!=0)result=-1;
		}
	}
	else if(strcmp(action,"memfd")==0)
		result=syscall(SYS_memfd_create,"dsg-filter-negative",0);
	else if(strcmp(action,"splice")==0)
		result=syscall(SYS_splice,-1,NULL,-1,NULL,page,0);
	else if(strcmp(action,"zero-copy")==0)
		result=send(-1,"x",1,MSG_ZEROCOPY);
	else if(strcmp(action,"set-zero-copy")==0)
	{
		int yes=1;
		result=setsockopt(-1,SOL_SOCKET,SO_ZEROCOPY,&yes,sizeof(yes));
	}
	else if(strcmp(action,"direct-open")==0)
		result=open("/definitely-absent-dsga-filter",O_RDONLY|O_DIRECT);
	else if(strcmp(action,"direct-fcntl")==0)
		result=fcntl(-1,F_SETFL,O_DIRECT);
	else if(strcmp(action,"io-uring")==0)
		result=syscall(SYS_io_uring_setup,0,NULL);
	else if(strcmp(action,"dumpable")==0)
		result=prctl(PR_SET_DUMPABLE,1L,0L,0L,0L);
	else
		elog(ERROR,"unknown fixed native filter probe");
	error=result<0?errno:0;
	initStringInfo(&out);
	appendStringInfo(&out,"{\"action\":\"%s\",\"result\":%ld,\"errno\":%d}",action,result,error);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

#else
void test_dsg_filter_pm_init(void)
{
 if(getenv("NEON_DSGA_FILTER_PM_PROBE")!=NULL)
  ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("native filter tests require Linux AMD64")));
}
Datum test_dsg_birth_filter(PG_FUNCTION_ARGS)
{
 ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("native filter tests require Linux AMD64")));
 PG_RETURN_NULL();
}
Datum test_dsg_filter_probe(PG_FUNCTION_ARGS)
{
 ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("native filter tests require Linux AMD64")));
 PG_RETURN_NULL();
}
#endif
