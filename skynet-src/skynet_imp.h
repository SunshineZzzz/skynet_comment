// Comment: 定义 Skynet 服务器框架的核心配置参数和入口函数

#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

// skynet 服务器框架的核心配置结构体
struct skynet_config {
	// 工作线程数量。通常设置为服务器 CPU 的核数，用于并发处理业务逻辑
	int thread;
	// 集群节点ID。用于标识 Skynet 集群中的不同节点。如果配置为 0，则表示以单节点模式运行
	int harbor;
	// 性能分析开关。控制是否启用性能分析，用于统计各服务模块的 CPU 时间等指标
	int profile;
	// 守护进程参数。如果配置（非 NULL），Skynet 将以守护进程的方式在后台运行
	const char * daemon;
	// 	C 服务模块的搜索路径。指定 Skynet 从哪个路径加载用 C 语言编写的服务模
	const char * module_path;
	// 引导服务配置。指定 Skynet 启动的第一个服务，默认为 "snlua bootstrap"。这会启动一个名为 snlua的 C 服务，并由它去执行 bootstrap.lua脚本，从而拉起整个 Lua 业务层
	const char * bootstrap;
	// 日志输出路径。指定日志文件路径。如果为 NULL，则所有日志输出到标准输出（stdout）
	const char * logger;
	// 日志服务模块名。指定用于处理日志的 C 服务模块名称，默认为 "logger"
	const char * logservice;
};

// 线程类型
// 工作线程
#define THREAD_WORKER 0
// 主线程
#define THREAD_MAIN 1
// 网络线程
#define THREAD_SOCKET 2
// 定时器线程
#define THREAD_TIMER 3
// 监控线程
#define THREAD_MONITOR 4

// skynet启动函数
void skynet_start(struct skynet_config * config);

#endif
