// Comment: 工作服务(sandbox)模块相关

#include "skynet.h"
#include "atomic.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/task.h>
#include <mach/mach.h>
#endif

#define NANOSEC 1000000000
#define MICROSEC 1000000

// #define DEBUG_LOG

// 内存阈值，当 snlua 占用的内存超过阈值则触发警报
#define MEMORY_WARNING_REPORT (1024 * 1024 * 32)

// lua服务
struct snlua {
	// 专属的 lua 状态机
	lua_State * L;
	// 对应的c服务
	struct skynet_context * ctx;
	// 实时记录该虚拟机当前占用的总内存字节数
	size_t mem;
	// 内存阈值，超过该阈值会触发警报
	size_t mem_report;
	// 内存上限
	size_t mem_limit;
	// 当前活跃协程
	lua_State * activeL;
	ATOM_INT trap;
};

// LUA_CACHELIB may defined in patched lua for shared proto
#ifdef LUA_CACHELIB

#define codecache luaopen_cache

#else

static int
cleardummy(lua_State *L) {
  return 0;
}

static int 
codecache(lua_State *L) {
	luaL_Reg l[] = {
		{ "clear", cleardummy },
		{ "mode", cleardummy },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);
	lua_getglobal(L, "loadfile");
	lua_setfield(L, -2, "loadfile");
	return 1;
}

#endif

static void
signal_hook(lua_State *L, lua_Debug *ar) {
	void *ud = NULL;
	lua_getallocf(L, &ud);
	struct snlua *l = (struct snlua *)ud;

	lua_sethook (L, NULL, 0, 0);
	if (ATOM_LOAD(&l->trap)) {
		ATOM_STORE(&l->trap , 0);
		luaL_error(L, "signal 0");
	}
}

static void
switchL(lua_State *L, struct snlua *l) {
	l->activeL = L;
	if (ATOM_LOAD(&l->trap)) {
		lua_sethook(L, signal_hook, LUA_MASKCOUNT, 1);
	}
}

static int
lua_resumeX(lua_State *L, lua_State *from, int nargs, int *nresults) {
	void *ud = NULL;
	lua_getallocf(L, &ud);
	struct snlua *l = (struct snlua *)ud;
	switchL(L, l);
	int err = lua_resume(L, from, nargs, nresults);
	if (ATOM_LOAD(&l->trap)) {
		// wait for lua_sethook. (l->trap == -1)
		while (ATOM_LOAD(&l->trap) >= 0) ;
	}
	switchL(from, l);
	return err;
}

static double
get_time() {
#if  !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	int sec = ti.tv_sec & 0xffff;
	int nsec = ti.tv_nsec;

	return (double)sec + (double)nsec / NANOSEC;
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	int sec = aTaskInfo.user_time.seconds & 0xffff;
	int msec = aTaskInfo.user_time.microseconds;

	return (double)sec + (double)msec / MICROSEC;
#endif
}

static inline double
diff_time(double start) {
	double now = get_time();
	if (now < start) {
		return now + 0x10000 - start;
	} else {
		return now - start;
	}
}

// coroutine lib, add profile

/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors.
*/
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status, nres;
  if (!lua_checkstack(co, narg)) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  lua_xmove(L, co, narg);
  status = lua_resumeX(co, L, narg, &nres);
  if (status == LUA_OK || status == LUA_YIELD) {
    if (!lua_checkstack(L, nres + 1)) {
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {
    lua_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}

static int
timing_enable(lua_State *L, int co_index, lua_Number *start_time) {
	lua_pushvalue(L, co_index);
	lua_rawget(L, lua_upvalueindex(1));
	if (lua_isnil(L, -1)) {		// check total time
		lua_pop(L, 1);
		return 0;
	}
	*start_time = lua_tonumber(L, -1);
	lua_pop(L,1);
	return 1;
}

static double
timing_total(lua_State *L, int co_index) {
	lua_pushvalue(L, co_index);
	lua_rawget(L, lua_upvalueindex(2));
	double total_time = lua_tonumber(L, -1);
	lua_pop(L,1);
	return total_time;
}

static int
timing_resume(lua_State *L, int co_index, int n) {
	lua_State *co = lua_tothread(L, co_index);
	lua_Number start_time = 0;
	if (timing_enable(L, co_index, &start_time)) {
		start_time = get_time();
#ifdef DEBUG_LOG
		double ti = diff_time(start_time);
		fprintf(stderr, "PROFILE [%p] resume %lf\n", co, ti);
#endif
		lua_pushvalue(L, co_index);
		lua_pushnumber(L, start_time);
		lua_rawset(L, lua_upvalueindex(1));	// set start time
	}

	int r = auxresume(L, co, n);

	if (timing_enable(L, co_index, &start_time)) {
		double total_time = timing_total(L, co_index);
		double diff = diff_time(start_time);
		total_time += diff;
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] yield (%lf/%lf)\n", co, diff, total_time);
#endif
		lua_pushvalue(L, co_index);
		lua_pushnumber(L, total_time);
		lua_rawset(L, lua_upvalueindex(2));
	}

	return r;
}

static int luaB_coresume (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTHREAD);
  int r = timing_resume(L, 1, lua_gettop(L) - 1);
  if (r < 0) {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}

static int luaB_auxwrap (lua_State *L) {
  lua_State *co = lua_tothread(L, lua_upvalueindex(3));
  int r = timing_resume(L, lua_upvalueindex(3), lua_gettop(L));
  if (r < 0) {
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD)
      lua_closethread(co, L);  /* close variables in case of errors */
    if (lua_type(L, -1) == LUA_TSTRING) {  /* error object is a string? */
      luaL_where(L, 1);  /* add extra info, if available */
      lua_insert(L, -2);
      lua_concat(L, 2);
    }
    return lua_error(L);  /* propagate error */
  }
  return r;
}

static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  NL = lua_newthread(L);
  lua_pushvalue(L, 1);  /* move function to top */
  lua_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}

static int luaB_cowrap (lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushvalue(L, lua_upvalueindex(2));
  luaB_cocreate(L);
  lua_pushcclosure(L, luaB_auxwrap, 3);
  return 1;
}

// profile lib

// skynet.profile.start(co)，启动对 co 协程的性能统计
static int
lstart(lua_State *L) {
	// 确定要统计哪个协程
	if (lua_gettop(L) != 0) {
		// 多穿的参数直接丢弃
		lua_settop(L,1);
		// 确保第一个参数是协程
		luaL_checktype(L, 1, LUA_TTHREAD);
	} else {
		// 如果没传参数，就把当前运行的协程压入栈顶
		lua_pushthread(L);
	}
	// 去 Upvalue 1 查，如果已经有 start_time 了，说明还没 stop 就又调了 start
	lua_Number start_time = 0;
	if (timing_enable(L, 1, &start_time)) {
		return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
	}

	// 重置累计时间
	// reset total time
	lua_pushvalue(L, 1);
	lua_pushnumber(L, 0);
	// Upvalue 2 是 TotalTimeTable。执行：TotalTimeTable[co] = 0
	lua_rawset(L, lua_upvalueindex(2));

	// 设置开始时刻
	// set start time
	lua_pushvalue(L, 1);
	start_time = get_time();
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] start\n", L);
#endif
	lua_pushnumber(L, start_time);
	// Upvalue 1 是 StartTimeTable。执行：StartTimeTable[co] = start_time
	lua_rawset(L, lua_upvalueindex(1));

	return 0;
}

// skynet.profile.stop(co)，停止对 co 协程的性能统计
static int
lstop(lua_State *L) {
	// 同上
	if (lua_gettop(L) != 0) {
		lua_settop(L,1);
		luaL_checktype(L, 1, LUA_TTHREAD);
	} else {
		lua_pushthread(L);
	}
	// 看看之前有没有 start 过
	lua_Number start_time = 0;
	if (!timing_enable(L, 1, &start_time)) {
		return luaL_error(L, "Call profile.start() before profile.stop()");
	}
	// 计算当前时间与 start_time 的差值
	double ti = diff_time(start_time);
	// 从 Upvalue 2 获取中途累加的累计时间
	double total_time = timing_total(L,1);

	// 清理 StartTimeTable[co] = nil
	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

	// 清理 TotalTimeTable[co] = nil
	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(2));

	// 最终时间(纯粹的 CPU 耗时) = 中途累计时间 + 本次运行时间
	total_time += ti;
	// 把结果压栈，返回给 Lua
	lua_pushnumber(L, total_time);
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] stop (%lf/%lf)\n", lua_tothread(L,1), ti, total_time);
#endif

	return 1;
}

// 初始化skynet.profile库
static int
init_profile(lua_State *L) {
	luaL_Reg l[] = {
		// 开始对特定Lua协程进行性能分析（Profiling），记录其开始执行的时间点。
		{ "start", lstart },
		// 停止对特定Lua协程的性能分析，计算其从 start到 stop的总执行耗时。
		{ "stop", lstop },
		// 	替换Lua原生的 coroutine.resume。在恢复协程执行的前后注入钩子代码，用于透明地统计协程的执行时间。
		{ "resume", luaB_coresume },
		// 	替换Lua原生的 coroutine.wrap。功能与 resume类似，是对创建协程包装函数的增强，同样用于性能监控。
		{ "wrap", luaB_cowrap },
		{ NULL, NULL },
	};
	// 创建一个lua扩展tale
	luaL_newlibtable(L,l);
	// 创建第一个表，准备用来存每个协程开始跑的时间
	lua_newtable(L);	// table thread->start time
	// 创建第二个表，准备用来存每个协程总共跑了多久
	lua_newtable(L);	// table thread->total time

	// 创建全弱表
	lua_newtable(L);	// weak table
	lua_pushliteral(L, "kv");
	lua_setfield(L, -2, "__mode");

	// StartTimeTable 和 TotalTimeTable 都具有相同元表
	// StartTimeTable 和 TotalTimeTable 都是全弱表了
	lua_pushvalue(L, -1);
	lua_setmetatable(L, -3);
	lua_setmetatable(L, -3);

	// 注册l & 2个全弱表上值
	luaL_setfuncs(L,l,2);

	return 1;
}

/// end of coroutine

static int 
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

static void
report_launcher_error(struct skynet_context *ctx) {
	// sizeof "ERROR" == 5
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);
	if (ret == NULL) {
		return str;
	}
	return ret;
}

// 初始化snlua
static int
init_cb(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {
	lua_State *L = l->L;
	l->ctx = ctx;
	// 停止GC
	lua_gc(L, LUA_GCSTOP, 0);
	// 忽略环境变量
	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
	// 加载标准库
	luaL_openlibs(L);
	// 加载skynet.profile库
	luaL_requiref(L, "skynet.profile", init_profile, 0);

	// 替换了 coroutine.resume和 coroutine.wrap这两个核心函数，目的是统计协程的CPU耗时
	int profile_lib = lua_gettop(L);
	// replace coroutine.resume / coroutine.wrap
	lua_getglobal(L, "coroutine");
	lua_getfield(L, profile_lib, "resume");
	lua_setfield(L, -2, "resume");
	lua_getfield(L, profile_lib, "wrap");
	lua_setfield(L, -2, "wrap");

	// 把栈清了
	lua_settop(L, profile_lib-1);

	// 全局注册表["skynet_context"] = ctx
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	// 加载skynet.codecache库
	luaL_requiref(L, "skynet.codecache", codecache , 0);
	lua_pop(L,1);

	// 开启分代GC
	lua_gc(L, LUA_GCGEN, 0, 0);

	// lua搜索路径
	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");
	// luaC库搜索路径
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");
	// Skynet服务脚本搜索路径
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);
	lua_setglobal(L, "LUA_SERVICE");
	// 可以设置预加载的模块
	const char *preload = skynet_command(ctx, "GETENV", "preload");
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");

	// 错误处理函数，如果 Lua 运行出错了，它会去抓取当前的调用栈信息，格式化成易读的字符串。
	lua_pushcfunction(L, traceback);
	assert(lua_gettop(L) == 1);

	// snlua加载器脚本路径
	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	// 词语发解析，不执行
	int r = luaL_loadfile(L,loader);
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}
	// 压入参数
	lua_pushlstring(L, args, sz);
	//  调用
	r = lua_pcall(L,1,0,1);
	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}
	lua_settop(L,0);
	// 如果设置了内存限制，就设置到服务中
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
		size_t limit = lua_tointeger(L, -1);
		l->mem_limit = limit;
		skynet_error(ctx, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");
	}
	lua_pop(L, 1);

	// 开启GC
	lua_gc(L, LUA_GCRESTART, 0);

	return 0;
}

static int
launch_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);
	struct snlua *l = ud;
	// 后续都会交由lua接管，所以这里先清空回调函数和其用户数据参数
	skynet_callback(context, NULL, NULL);
	// 初始化snlua
	int err = init_cb(l, context, msg, sz);
	if (err) {
		skynet_command(context, "EXIT", NULL);
	}

	return 0;
}

int
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args);
	char * tmp = skynet_malloc(sz);
	memcpy(tmp, args, sz);
	skynet_callback(ctx, l , launch_cb);
	const char * self = skynet_command(ctx, "REG", NULL);
	uint32_t handle_id = strtoul(self+1, NULL, 16);
	// it must be first message
	// 给自己发送消息，内容为 args
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY,0, tmp, sz);
	return 0;
}

static void *
lalloc(void * ud, void *ptr, size_t osize, size_t nsize) {
	struct snlua *l = ud;
	size_t mem = l->mem;
	l->mem += nsize;
	if (ptr)
		l->mem -= osize;
	if (l->mem_limit != 0 && l->mem > l->mem_limit) {
		if (ptr == NULL || nsize > osize) {
			l->mem = mem;
			return NULL;
		}
	}
	if (l->mem > l->mem_report) {
		l->mem_report *= 2;
		skynet_error(l->ctx, "Memory warning %.2f M", (float)l->mem / (1024 * 1024));
	}
	return skynet_lalloc(ptr, osize, nsize);
}

struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));
	memset(l,0,sizeof(*l));
	l->mem_report = MEMORY_WARNING_REPORT;
	l->mem_limit = 0;
	l->L = lua_newstate(lalloc, l);
	l->activeL = NULL;
	ATOM_INIT(&l->trap , 0);
	return l;
}

void
snlua_release(struct snlua *l) {
	lua_close(l->L);
	skynet_free(l);
}

void
snlua_signal(struct snlua *l, int signal) {
	skynet_error(l->ctx, "recv a signal %d", signal);
	if (signal == 0) {
		if (ATOM_LOAD(&l->trap) == 0) {
			// only one thread can set trap ( l->trap 0->1 )
			if (!ATOM_CAS(&l->trap, 0, 1))
				return;
			lua_sethook (l->activeL, signal_hook, LUA_MASKCOUNT, 1);
			// finish set ( l->trap 1 -> -1 )
			ATOM_CAS(&l->trap, 1, -1);
		}
	} else if (signal == 1) {
		skynet_error(l->ctx, "Current Memory %.3fK", (float)l->mem / 1024);
	}
}
