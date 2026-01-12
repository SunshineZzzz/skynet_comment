// Comment: Skynet 框架中的全局环境变量管理器，它使用一个轻量级的 Lua 虚拟机来存储和管理整个进程的配置信息，实现服务间共享环境变量

#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

// 获取全局环境变量
const char * skynet_getenv(const char *key);
// 设置全局环境变量
void skynet_setenv(const char *key, const char *value);

// 初始化全局环境变量
void skynet_env_init();

#endif
