// Comment: 服务相关

#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>
#include <stdlib.h>

struct skynet_context;
struct skynet_message;
struct skynet_monitor;

// 新建服务
struct skynet_context * skynet_context_new(const char * name, const char * parm);
// 增加服务引用计数
void skynet_context_grab(struct skynet_context *);
void skynet_context_reserve(struct skynet_context *ctx);
// 减少服务引用计数，如果引用计数为 0，则释放服务
struct skynet_context * skynet_context_release(struct skynet_context *);
// 获取服务的 handle
uint32_t skynet_context_handle(struct skynet_context *);
// 将一个消息直接、立即地压入到指定服务的私有消息队列中
int skynet_context_push(uint32_t handle, struct skynet_message *message);
void skynet_context_send(struct skynet_context * context, void * msg, size_t sz, uint32_t source, int type, int session);
// 生成消息会话ID
int skynet_context_newsession(struct skynet_context *);
// 处理某个服务消息队列中的消息，返回下一个服务消息队列
struct message_queue * skynet_context_message_dispatch(struct skynet_monitor *, struct message_queue *, int weight);	// return next queue
int skynet_context_total();
void skynet_context_dispatchall(struct skynet_context * context);	// for skynet_error output before exit

void skynet_context_endless(uint32_t handle);	// for monitor

// 全局初始化
void skynet_globalinit(void);
// 全局退出
void skynet_globalexit(void);
// 初始化当前线程类型
void skynet_initthread(int m);

void skynet_profile_enable(int enable);

#endif
