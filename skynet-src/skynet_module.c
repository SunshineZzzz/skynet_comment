#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

// 最大模块类型数量
#define MAX_MODULE_TYPE 32

// 模块管理器
struct modules {
	// 记录当前已加载的模块数量
	int count;
	// 自旋锁，多个 Worker 线程同时尝试加载不同的服务，防止竞态
	struct spinlock lock;
	// 模块搜索路径
	const char * path;
	// 模块数组
	struct skynet_module m[MAX_MODULE_TYPE];
};

// 全局模块管理器对象
static struct modules * M = NULL;

// 尝试加载模块
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		while (*path == ';') path++;
		if (*path == '\0') break;
		l = strchr(path, ';');
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size);
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

// 查询模块
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

// 获取模块的 API 函数指针
static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	return dlsym(mod->module, ptr);
}

// 从动态库中映射模块的 API 函数指针
static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");
	mod->init = get_api(mod, "_init");
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

struct skynet_module * 
skynet_module_query(const char * name) {
	// 不加锁先瞟一眼
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	SPIN_LOCK(M)

	// 加锁后再次查询
	result = _query(name); // double check

	// 确实没有，并且模块类型数量还没有达到上限
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	SPIN_UNLOCK(M)

	return result;
}

void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path);

	SPIN_INIT(m)

	M = m;
}
