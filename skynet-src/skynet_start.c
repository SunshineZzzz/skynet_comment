#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// 全局监控器，负责监控所有工作线程
struct monitor {
	// 记录总共有多少个工作线程
	int count;
	// 工作线程对应的监控器数组
	struct skynet_monitor ** m;
	// 条件变量，用于唤醒睡眠的工作线程
	pthread_cond_t cond;
	// 互斥锁，保护条件变量和监控器数组的访问
	pthread_mutex_t mutex;
	// 记录当前有多少个线程正在睡觉。如果这个值等于 count，说明全系统处于空闲状态。
	int sleep;
	// 一个标记位。当系统准备关停时，设置为 1，通知所有线程结束循环并退出。
	int quit;
};

// 工作线程参数
struct worker_parm {
	// 全局监控器对象
	struct monitor *m;
	// 工作线程ID
	int id;
	// 处理消息的权重
	int weight;
};

static volatile int SIG = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

// 创建线程
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

// 套接字线程
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

// 监控器线程
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}

// 定时器线程
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		skynet_socket_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

// 工作线程
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

// 开始工作
static void
start(int thread) {
	pthread_t pid[thread+3];

	// 创建全局监控器对象
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 额外的三个线程：监控线程、定时器线程、套接字线程
	create_thread(&pid[0], thread_monitor, m);
	create_thread(&pid[1], thread_timer, m);
	create_thread(&pid[2], thread_socket, m);

	// 处理消息的权重
	// 在 Skynet 的工作流程中，一个线程会从全局队列里取出一个“服务队列”。这个 weight 决定了该线程这次抓到这个服务后，要处理多少条消息才肯放手。
	// 权重：
	// 		-1：公平竞争模式。这个线程每次只处理该服务队列里的 1 条 消息。做完立即放手，去换下一个服务。这保证了所有服务都能被快速轮询到。 
	// 		 0：极致吞吐量模式。这个线程一旦抓到一个服务队列，就会把该队列里所有的消息全部执行完，直到队列清空。这非常适合处理那种高频、大量的消息流。
	//     > 0：阶梯模式。处理消息的数量是按位移计算的(通常是 队列长度/2^n 条)。
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			// 超过蓝图，就使用极致吞吐量模式
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	free_monitor(m);
}

// 启动引导程序，创建cmdline参数中name服务，并且传递cmdline参数中args参数给该服务
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	int arg_pos;
	sscanf(cmdline, "%s", name);  
	arg_pos = strlen(name);
	if (arg_pos < sz) {
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		strncpy(args, cmdline + arg_pos, sz);
	} else {
		args[0] = '\0';
	}
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	struct sigaction sa;
	// 设置信号处理函数。当进程收到 SIGHUP信号时，将调用用户自定义的函数 handle_hup。
	sa.sa_handler = &handle_hup;
	// 如果某个慢速系统调用（如 read, write, accept等）在执行过程中被信号中断，那么系统调用应该自动重启，而不是返回错误。这提高了程序的健壮性
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	// SIGHUP：
	// 1. 当你在 SSH 连接中运行一个程序，然后直接关闭终端窗口或断开 SSH 连接时，会发送 SIGHUP 信号给该程序。
	// 2. 服务器程序通常是守护进程 (Daemon)，它们没有关联的终端，因此永远不会因为“挂断”而收到这个信号。
	// 于是，开发者们“借用”了这个信号，将其定义为：“重新加载配置/日志”的指令。
	sigaction(SIGHUP, &sa, NULL);

	// 如果配置为守护进程模式，初始化守护进程
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	// 初始化节点高8位，用于判断消息是否为集群远程消息
	skynet_harbor_init(config->harbor);
	// 初始化全局服务信息对象
	skynet_handle_init(config->harbor);
	// 初始化全局队列对象
	skynet_mq_init();
	// 初始化全局模块管理器对象
	skynet_module_init(config->module_path);
	// 初始化全局定时器对象
	skynet_timer_init();
	// 初始化全局套接字对象
	skynet_socket_init();
	// 开启性能分析
	skynet_profile_enable(config->profile);

	// 创建日志服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	// 向全局服务信息对象注册日志服务信息
	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

	// 启动引导程序，一般是snlua bootstrap，前者是服务模块名，后者是传递给服务模块的参数，一般是要加载要的 Lua 脚本文件名
	// 创建snlua服务
	bootstrap(ctx, config->bootstrap);

	// 启动工作线程和辅助线程，开始工作了哦
	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
