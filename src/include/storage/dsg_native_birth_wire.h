/*-
 * root-v1原生出生传输与最终Root停点；不提供任何normal许可。
 * ACK只证明按原nonce/摘要完成了该传输，不能替代Root持久来源或FS/I包络。
 */
#ifndef DSG_NATIVE_BIRTH_WIRE_H
#define DSG_NATIVE_BIRTH_WIRE_H

#define DSG_NATIVE_BIRTH_WIRE_VERSION 1
#define DSG_NATIVE_BIRTH_SOCKET "/run/neon-dsga/birth.sock"
#define DSG_NATIVE_BIRTH_NONCE_ENV "NEON_DSGA_NATIVE_BIRTH_NONCE"
#define DSG_NATIVE_BIRTH_SOURCE_ENV "NEON_DSGA_NATIVE_BIRTH_SOURCE_SHA256"
#define DSG_NATIVE_BIRTH_PROFILE_ENV "NEON_DSGA_NATIVE_BIRTH_PROFILE_SHA256"
#define DSG_NATIVE_BIRTH_STARTUP_ENV "NEON_DSGA_NATIVE_BIRTH_STARTUP_BUFFERS"

typedef struct DsgNativeBirthWireSnapshot
{
	uint32 protocol_version;
	bool enabled;
	bool acknowledged;
	bool source_bound;
	bool final_audit_released_non_authorizing;
	bool root_enrolled_non_authorizing;
	const char *phase;
	int startup_buffers;
	pid_t original_postmaster_pid;
	uint8 nonce[32];
	uint8 source_sha256[32];
	uint8 profile_sha256[32];
} DsgNativeBirthWireSnapshot;

/* A/R已真实建立、PMbase尚未安装：connect固定socket并将唯一FD置198。 */
extern void DsgNativeBirthWirePrepare(void);
/* PMbase已安装：严格232B请求/112B ACK；先关闭198，尚不发布enrolled。 */
extern void DsgNativeBirthWireExchange(void);
/* PMfinal/close198后必须经真实Root ptrace停点并detach，才发布非授权事实。 */
extern void DsgNativeBirthWireFinish(void);
/* 供filter本模块阻止顺序错误；不是Root/normal授权检查。 */
extern bool DsgNativeBirthWireRootPrepared(void);
extern bool DsgNativeBirthWireRootACKClosed(void);
extern void DsgNativeBirthWireGetSnapshot(DsgNativeBirthWireSnapshot *snapshot);
/* InitBufferPool每次首次/内部恢复都核M/N0；调用者对false必须FATAL。 */
extern bool DsgNativeBirthWireCheckConfiguration(int maximum, int startup);

#endif
