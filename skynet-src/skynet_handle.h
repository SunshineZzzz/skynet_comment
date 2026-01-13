// Comment: 服务管理相关

#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// reserve high 8 bits for remote id
#define HANDLE_MASK 0xffffff
#define HANDLE_REMOTE_SHIFT 24

struct skynet_context;

// 服务注册，返回一个唯一的handle(包含harbor id)
uint32_t skynet_handle_register(struct skynet_context *);
int skynet_handle_retire(uint32_t handle);
// 获取服务对象
struct skynet_context * skynet_handle_grab(uint32_t handle);
void skynet_handle_retireall();

// 全局服务信息对象中查找服务名对应的handle
uint32_t skynet_handle_findname(const char * name);
// 注册服务器信息(名字和handle)到全局服务信息对象中
const char * skynet_handle_namehandle(uint32_t handle, const char *name);

// 初始化全局服务信息对象
void skynet_handle_init(int harbor);

#endif
