// Comment: 守护进程相关

#ifndef skynet_daemon_h
#define skynet_daemon_h

// 初始化守护进程
int daemon_init(const char *pidfile);
// 退出守护进程
int daemon_exit(const char *pidfile);

#endif
