/*-
 * 四数组专用Root收费传输；不扩泛Provider/DSM，不授normal或VM许可。
 * 512B请求/160B回复为独立版本化私有ABI，PG17/18按同布局适配。
 */
#ifndef DSG_ARRAY_ROOT_H
#define DSG_ARRAY_ROOT_H
#include "storage/dynamic_shared_buffers.h"
#define NEON_DSG_ARRAY_ROOT_API_VERSION 1
#define DSG_ARRAY_ROOT_PROFILE_ENV "NEON_DSGA_ARRAY_ROOT_PROFILE"
#define DSG_ARRAY_ROOT_SOCKET "/run/neon-dsga/array-populate.sock"
#define DSG_ARRAY_ROOT_REQUEST_BYTES 512
#define DSG_ARRAY_ROOT_REPLY_BYTES 160
extern bool DsgArrayRootEnabled(void);
extern void DsgArrayRootRegisterAll(void);
extern void DsgArrayRootRetire(void);
extern DsgMemoryResult DsgBufferPopulateArray(DsgBufferArrayId id, DsgRegion *region,
 Size offset, Size bytes, const DsgBufferResizeRequest *request, int remaining_ms);
#endif
