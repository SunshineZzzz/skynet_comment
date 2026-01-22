// Comment: 消息队列相关

#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

// 消息
struct skynet_message {
	// 消息源服务句柄
	uint32_t source;
	// 会话 ID，用于响应消息
	int session;
	// 消息数据指针
	void * data;
	// 消息数据长度
	size_t sz;
};

// type is encoding in skynet_message.sz high 8bit
// 高 8 位存消息类型，低位存消息长度
#define MESSAGE_TYPE_MASK (SIZE_MAX >> 8)
// 消息类型偏移量
#define MESSAGE_TYPE_SHIFT ((sizeof(size_t)-1) * 8)

struct message_queue;

// 将服务消息队列加入到全局队列中
void skynet_globalmq_push(struct message_queue * queue);
// 从全局队列中取出一条服务消息队列
struct message_queue * skynet_globalmq_pop(void);

// 创建服务消息队列
struct message_queue * skynet_mq_create(uint32_t handle);
void skynet_mq_mark_release(struct message_queue *q);

typedef void (*message_drop)(struct skynet_message *, void *);

// 安全地销毁或回收一个私有消息队列到全局队列中
void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud);
// 返回队列对应的服务句柄
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
// 从当前服务对应消息队列中取出一条消息
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);
// 将一条消息压入到当前服务对应消息队列中
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

// return the length of message queue, for debug
// 当前未读消息个数，用于调试
int skynet_mq_length(struct message_queue *q);
// 未读消息是否超过阈值
int skynet_mq_overload(struct message_queue *q);

// 初始化全局队列
void skynet_mq_init();

#endif
