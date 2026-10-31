/*-
 * dsg_envelope_filter.h -- 原生出生范围保护，独立ABI1，默认关闭。
 * test-v1只验证实际seccomp边界；没有Root出生/FS/物理包络资格。
 */
#ifndef DSG_ENVELOPE_FILTER_H
#define DSG_ENVELOPE_FILTER_H
#define NEON_DSG_BIRTH_FILTER_API_VERSION 1
#define DSG_BIRTH_FILTER_ENV "NEON_DSGA_EARLY_ARRAYS_FILTER"
#define DSG_BIRTH_WIRE_FD 198
typedef struct DsgBirthFilterSnapshot
{
	uint32 protocol_version;
	bool enabled;
	bool postmaster_installed;
	bool child_installed;
	bool finalized;
	uint32 postmaster_instructions;
	uint32 final_postmaster_instructions;
	uint32 child_instructions;
} DsgBirthFilterSnapshot;
extern void DsgBirthFilterEarlyInit(void);
extern void DsgBirthFilterSealPostmaster(void);
extern void DsgBirthFilterInstallChild(void);
extern void DsgBirthFilterGetSnapshot(DsgBirthFilterSnapshot *snapshot);
extern bool DsgBirthFilterEnabled(void);
/* 原生可信fork入口；失败绝不产生未登记的活child。active到真实PID登记才降。 */
extern bool DsgBirthFilterForkBegin(void);
extern pid_t DsgBirthFilterForkFinish(pid_t result, int original_errno);
extern void DsgBirthFilterForkRegistered(pid_t pid);
extern void DsgBirthFilterChildReady(void);
#ifdef USE_INJECTION_POINTS
/* 仅测试构建的只读子进程FD观察；无生产故障开关/状态修改/Root ACK权限。 */
extern int DsgBirthFilterTestReadyFD(void);
#endif
extern bool DsgBirthFilterReinitializeBegin(void);
extern void DsgBirthFilterReinitializeEnd(void);
#endif
