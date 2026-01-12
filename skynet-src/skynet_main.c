#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

// 获取配置项的整数值，如果不存在则设置并返回默认值
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

// 获取配置项的布尔值，如果不存在则设置并返回默认值
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

// 获取配置项的字符串值，如果不存在则设置并返回默认值
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

// 将配置表中的所有键值对设置到全局环境变量中
static void
_init_env(lua_State *L) {
	// 目的是从头开始遍历table中的所有键值对
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

// 忽略SIGPIPE信号，避免连接已断开(管道、套接字)导致的程序意外终止
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

// 用于加载配置文件的Lua代码字符串
// result - 这是一个空表，所有配置文件中定义的变量最终都会存放在这里。
// getenv - 一个辅助函数，用于强制获取系统环境变量，如果变量不存在则报错。
// local sep = package.config:sub(1,1) - 路径分隔符，根据不同操作系统可能不同（如Windows使用反斜杠\，Unix/Linux使用正斜杠/）
// local current_path = [[.]]..sep - 当前工作目录，初始化为当前脚本所在目录
// include - 实现include函数，用于加载其他配置文件，使用正则匹配 $VAR 或 $NAME，并调用上面定义的 getenv 函数替换为真实值
// setmetatable(result, { __index = { include = include } }) - 设置result表的元表，使include函数可以通过 result.include 调用，因为load环境需要这个env
// local config_name = ...; include(config_name) 加载配置！
// 删除元表，返回结果
static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

int
main(int argc, char *argv[]) {
	// 获取配置文件
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	// 初始化全局环境
	skynet_globalinit();
	// 初始化全局环境变量
	skynet_env_init();

	// 忽略SIGPIPE信号
	sigign();

	// 配置
	struct skynet_config config;

	// 如果开启了代码缓存，初始化代码缓存锁
#ifdef LUA_CACHELIB
	// init the lock of code cache
	luaL_initcodecache();
#endif

	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);	// link lua lib

	// 加载解析配置辅助代码字符串，词语法分析生成LClosure
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	// 压入配置文件名
	lua_pushstring(L, config_file);

	// 执行解析配置，1个参数配置文件路径，1个返回值{k = v}
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	// 将配置表中的所有键值对设置到全局环境变量中
	_init_env(L);
	// 关闭Lua状态机
	lua_close(L);

	// 获取一下配置项的值，如果没有设置，则使用默认值，并且将默认值设置到全局环境变量中
	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);
	config.logservice = optstring("logservice", "logger");
	config.profile = optboolean("profile", 1);

	// 启动skynet服务器
	skynet_start(&config);
	// 全局退出skynet服务器
	skynet_globalexit();

	return 0;
}
