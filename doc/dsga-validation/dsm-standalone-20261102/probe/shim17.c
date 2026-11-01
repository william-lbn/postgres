/* 仅真实PG17定向测试；不签Root/normal，不改原生DSM对象。 */
#include "postgres.h"
#include "fmgr.h"
#include "storage/dsm.h"
#include "storage/dsg_dsm.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg17_dsm_cycle);
Datum pg17_dsm_cycle(PG_FUNCTION_ARGS) { dsm_segment *s=dsm_create(4096,0); volatile unsigned char *p=dsm_segment_address(s); *p=42; if(*p!=42)elog(ERROR,"actual DSM payload differs"); dsm_detach(s); PG_RETURN_BOOL(true); }
PG_FUNCTION_INFO_V1(pg17_dsm_snapshot);
Datum pg17_dsm_snapshot(PG_FUNCTION_ARGS) { DsgDsmSnapshot s; DsgDsmGetSnapshot(&s); PG_RETURN_TEXT_P(cstring_to_text(psprintf("{\"reserved_upper_bytes\":" UINT64_FORMAT ",\"external_segments\":%u,\"pending_creations\":%u,\"unknown_segments\":%u,\"external_budget_bytes\":" UINT64_FORMAT ",\"accounting_valid\":%s,\"limit_enforced\":%s,\"Source\":false}",s.dsm.reserved_upper_bytes,s.dsm.external_segments,s.dsm.pending_creations,s.dsm.unknown_segments,s.dsm.external_budget_bytes,s.dsm.accounting_valid?"true":"false",s.dsm.limit_enforced?"true":"false"))); }
PG_FUNCTION_INFO_V1(pg17_dsm_hold);
Datum pg17_dsm_hold(PG_FUNCTION_ARGS) { int64 n=PG_GETARG_INT64(0); dsm_segment *s; dsm_handle h; if(n<=0||n>1048576)elog(ERROR,"test DSM size outside bounded range"); s=dsm_create((Size)n,0); *((volatile unsigned char*)dsm_segment_address(s))=42; h=dsm_segment_handle(s); dsm_pin_segment(s); dsm_detach(s); PG_RETURN_INT64((int64)h); }
PG_FUNCTION_INFO_V1(pg17_dsm_release);
Datum pg17_dsm_release(PG_FUNCTION_ARGS) { dsm_unpin_segment((dsm_handle)PG_GETARG_INT64(0)); PG_RETURN_BOOL(true); }
PG_FUNCTION_INFO_V1(pg17_dsm_pgstat_area);
Datum pg17_dsm_pgstat_area(PG_FUNCTION_ARGS) { if(pgStatLocal.dsa==NULL)elog(ERROR,"actual pgstat DSA unavailable"); fprintf(stderr,"PG17_DSM_NATIVE_BASELINE_BEGIN pid=%d\n",MyProcPid); dsa_dump(pgStatLocal.dsa); fprintf(stderr,"PG17_DSM_NATIVE_BASELINE_END pid=%d\n",MyProcPid); PG_RETURN_VOID(); }
