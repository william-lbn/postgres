#define _GNU_SOURCE 1
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "storage/dsg_envelope_filter.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_pg17_pipe);
PGDLLEXPORT void _PG_init(void);
/* 仅当前独占测试PG的固定pipe/FD2原语，无路径、地址或任意fcntl输入。 */
static char *
PipeCapacityProbe(void)
{
	int fds[2], duplicate;
	int fd2_before, fd2_set, fd2_error;
	int before, after, requests[3], results[3], errors[3];
	int flags, setfl, setfd, dup_error, bad_set, bad_get;
	bool nonblock, cloexec, roundtrip, closed;
	char sent = 'p', received = 0;
	long page = sysconf(_SC_PAGESIZE);
	StringInfoData out;
	int i;

	fd2_before = fcntl(STDERR_FILENO, F_GETPIPE_SZ);
	errno = 0;
	fd2_set = fcntl(STDERR_FILENO, F_SETPIPE_SZ, fd2_before > 0 ? fd2_before : 4096);
	fd2_error = fd2_set < 0 ? errno : 0;
	if (pipe2(fds, O_CLOEXEC) != 0)
		elog(ERROR, "dedicated pipe capacity probe could not create own pipe");
	before = fcntl(fds[0], F_GETPIPE_SZ);
	if (before <= 0 || before > PG_INT32_MAX / 2 || page <= 0 || page > PG_INT32_MAX)
	{
		close(fds[0]); close(fds[1]);
		elog(ERROR, "dedicated pipe capacity probe has invalid original geometry");
	}
	requests[0] = before; requests[1] = (int) page; requests[2] = before * 2;
	for (i = 0; i < 3; i++)
	{
		errno = 0;
		results[i] = fcntl(fds[0], F_SETPIPE_SZ, requests[i]);
		errors[i] = results[i] < 0 ? errno : 0;
	}
	after = fcntl(fds[0], F_GETPIPE_SZ);
	flags = fcntl(fds[0], F_GETFL);
	setfl = fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	nonblock = (fcntl(fds[0], F_GETFL) & O_NONBLOCK) != 0;
	setfd = fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	cloexec = (fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0;
	errno = 0; duplicate = fcntl(fds[0], F_DUPFD_CLOEXEC, 3);
	dup_error = duplicate < 0 ? errno : 0;
	if (duplicate >= 0) close(duplicate);
	roundtrip = write(fds[1], &sent, 1) == 1 && read(fds[0], &received, 1) == 1 && received == sent;
	close(fds[0]); close(fds[1]);
	errno = 0; closed = fcntl(fds[0], F_GETFD) == -1 && errno == EBADF;
	errno = 0; (void) fcntl(-1, F_SETPIPE_SZ, 4096); bad_set = errno;
	errno = 0; (void) fcntl(-1, F_GETPIPE_SZ); bad_get = errno;
	initStringInfo(&out);
	appendStringInfo(&out, "{\"pid\":%d,\"stderr_capacity_before\":%d,\"stderr_same_result\":%d,\"stderr_same_errno\":%d,"
		"\"pipe_capacity_before\":%d,\"pipe_capacity_after\":%d,\"requests\":[%d,%d,%d],"
		"\"results\":[%d,%d,%d],\"errors\":[%d,%d,%d],\"setfl\":%d,\"setfd\":%d,\"dup_errno\":%d,"
		"\"nonblock\":%s,\"cloexec\":%s,\"roundtrip\":%s,\"closed\":%s,\"bad_set_errno\":%d,\"bad_get_errno\":%d}",
		(int)getpid(),fd2_before,fd2_set,fd2_error,before,after,requests[0],requests[1],requests[2],
		results[0],results[1],results[2],errors[0],errors[1],errors[2],setfl,setfd,dup_error,
		nonblock?"true":"false",cloexec?"true":"false",roundtrip?"true":"false",closed?"true":"false",bad_set,bad_get);
	return out.data;
}


static char *Envelope(void){
 DsgBirthFilterSnapshot s;StringInfoData out;DsgBirthFilterGetSnapshot(&s);initStringInfo(&out);
 appendStringInfo(&out,"{\"pg_version_num\":%d,\"enabled\":%s,\"postmaster_installed\":%s,\"finalized\":%s,\"child_installed\":%s,\"pipe\":%s}",PG_VERSION_NUM,s.enabled?"true":"false",s.postmaster_installed?"true":"false",s.finalized?"true":"false",s.child_installed?"true":"false",PipeCapacityProbe());return out.data;
}
Datum test_pg17_pipe(PG_FUNCTION_ARGS){
 if(!superuser()||!superuser_arg(GetSessionUserId()))ereport(ERROR,(errmsg("pipe probe requires original administrator")));
 PG_RETURN_TEXT_P(cstring_to_text(Envelope()));
}
void _PG_init(void){
 const char *mode=getenv("NEON_TEST_PIPE_PM");
 if(mode!=NULL&&strcmp(mode,"1")==0){
  DsgBirthFilterSnapshot s;DsgBirthFilterGetSnapshot(&s);
  if(!process_shared_preload_libraries_in_progress||IsUnderPostmaster||!s.enabled||!s.finalized||s.child_installed)elog(ERROR,"PG17 pipe PM probe requires original finalized preload");
  ereport(LOG,(errmsg("PG17_PIPE_PM %s",Envelope())));
 }
}
