// Comment: 监控器相关

#ifndef SKYNET_MONITOR_H
#define SKYNET_MONITOR_H

#include <stdint.h>

struct skynet_monitor;

// 创建监控器对象
struct skynet_monitor * skynet_monitor_new(); 
// 销毁监控器对象
void skynet_monitor_delete(struct skynet_monitor *);
// 服务开始处理消息前，标记消息处理开始
void skynet_monitor_trigger(struct skynet_monitor *, uint32_t source, uint32_t destination);
// 检测，工作线程调度间隙定期调用
void skynet_monitor_check(struct skynet_monitor *);

#endif
