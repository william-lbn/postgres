#define _GNU_SOURCE 1
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include "storage/dsg_envelope_filter.h"
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(migration_filter_probe);
Datum migration_filter_probe(PG_FUNCTION_ARGS)
{
 DsgBirthFilterSnapshot s;
 StringInfoData out;
 long result;
 int saved;
 if(!superuser() || !superuser_arg(GetSessionUserId()))
  ereport(ERROR,(errmsg("probe requires original administrator")));
 DsgBirthFilterGetSnapshot(&s);initStringInfo(&out);
 appendStringInfo(&out,"{\"enabled\":%s,\"finalized\":%s,\"child_installed\":%s,\"calls\":{",s.enabled?"true":"false",s.finalized?"true":"false",s.child_installed?"true":"false");
#define PROBE(name, ...) do { errno=0;result=syscall(__VA_ARGS__);saved=errno;appendStringInfo(&out,"\"%s\":{\"result\":%ld,\"errno\":%d},",name,result,saved); } while(0)
 PROBE("mbind",__NR_mbind,0L,0L,~0UL,0L,0L,0L);
 PROBE("set_mempolicy",__NR_set_mempolicy,~0UL,0L,0L);
 PROBE("migrate_pages",__NR_migrate_pages,(long)getpid(),0L,0L,0L);
 PROBE("move_pages",__NR_move_pages,(long)getpid(),0L,0L,0L,0L,0L);
 PROBE("unshare",__NR_unshare,0L);
 PROBE("setns",__NR_setns,-1L,0L);
 PROBE("io_uring_setup",__NR_io_uring_setup,0L,0L);
 PROBE("io_uring_enter",__NR_io_uring_enter,-1L,0L,0L,0L,0L,0L);
 PROBE("io_uring_register",__NR_io_uring_register,-1L,~0UL,0L,0L);
 PROBE("userfaultfd",__NR_userfaultfd,~0UL);
 PROBE("process_madvise",__NR_process_madvise,-1L,0L,0L,0L,0L);
 PROBE("process_mrelease",__NR_process_mrelease,-1L,0L);
 /* amd64 450; harmless len0 shows this unlisted syscall reaches kernel. */
 PROBE("set_mempolicy_home_node",450,0L,0L,0L,0L);
#undef PROBE
 out.len--;out.data[out.len]='\0';appendStringInfoString(&out,"},\"normal_permit\":false}");
 PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
