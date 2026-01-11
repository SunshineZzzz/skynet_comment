include platform.mk

# Lua C 模块的编译输出目录，默认为 luaclib
LUA_CLIB_PATH ?= luaclib
# C 服务模块的编译输出目录，默认为 cservice。
CSERVICE_PATH ?= cservice

# 定义 Skynet 主程序的构建输出目录，默认为当前目录。
SKYNET_BUILD_PATH ?= .

# 设置 C 编译器的标志
CFLAGS = -g -O0 -Wall -I$(LUA_INC) $(MYCFLAGS)
# CFLAGS += -DUSE_PTHREAD_LOCK

# lua

# Lua 静态库的具体路径
LUA_STATICLIB := 3rd/lua/liblua.a
LUA_LIB ?= $(LUA_STATICLIB)
# Lua 头文件的路径
LUA_INC ?= 3rd/lua

# 定义如何构建 Lua 静态库的规则
$(LUA_STATICLIB) :
	cd 3rd/lua && $(MAKE) CC='$(CC) -std=gnu99' $(PLAT)

# https : turn on TLS_MODULE to add https support

# HTTPS/TLS 支持部分（默认关闭）
# TLS_MODULE=ltls
TLS_LIB=
TLS_INC=

# jemalloc

# 定义 Jemalloc 库的路径和包含路径。
JEMALLOC_STATICLIB := 3rd/jemalloc/lib/libjemalloc_pic.a
JEMALLOC_INC := 3rd/jemalloc/include/jemalloc

# 定义默认目标 all依赖于 jemalloc目标。这意味着构建 all前会先确保 jemalloc 已构建。
all : jemalloc
	
.PHONY : jemalloc update3rd

# 通过命令行传递的变量优先级高于文件内部定义的变量。
# 如果是macosx，这里是空置
MALLOC_STATICLIB := $(JEMALLOC_STATICLIB)

$(JEMALLOC_STATICLIB) : 3rd/jemalloc/Makefile
	cd 3rd/jemalloc && $(MAKE) CC=$(CC) 

3rd/jemalloc/autogen.sh :
	git submodule update --init

# 规则：|表示顺序依赖（order-only prerequisite，只要 autogen.sh 存在即可，不需要检查它的更新时间，就算修改时间新于Makefile，也不会再次执行生成 Makefile）。
# 如果 Makefile不存在，但 autogen.sh存在，则运行 autogen.sh脚本生成 Makefile，并传递配置参数。
3rd/jemalloc/Makefile : | 3rd/jemalloc/autogen.sh
	cd 3rd/jemalloc && ./autogen.sh --with-jemalloc-prefix=je_ --enable-prof

jemalloc : $(MALLOC_STATICLIB)

# 定义 update3rd目标，用于清理并重新初始化第三方库（这里是 jemalloc）
update3rd :
	rm -rf 3rd/jemalloc && git submodule update --init

# skynet

# 定义要构建的 C 服务模块列表
# 递归展开式赋值，如果变量的值引用了其他变量，它会一直向后寻找，直到找到该变量在整个 Makefile 最终定义的值。
CSERVICE = snlua logger gate harbor
# 定义要构建的 Lua C 扩展模块列表
LUA_CLIB = skynet \
  client \
  bson md5 sproto lpeg $(TLS_MODULE)

# 定义构成 skynet.so这个核心 C 模块的所有源文件
LUA_CLIB_SKYNET = \
  lua-skynet.c lua-seri.c \
  lua-socket.c \
  lua-mongo.c \
  lua-netpack.c \
  lua-memory.c \
  lua-multicast.c \
  lua-cluster.c \
  lua-crypt.c lsha1.c \
  lua-sharedata.c \
  lua-stm.c \
  lua-debugchannel.c \
  lua-datasheet.c \
  lua-sharetable.c \
  \

# 定义构建 Skynet 主程序所需的所有核心源文件
SKYNET_SRC = skynet_main.c skynet_handle.c skynet_module.c skynet_mq.c \
  skynet_server.c skynet_start.c skynet_timer.c skynet_error.c \
  skynet_harbor.c skynet_env.c skynet_monitor.c skynet_socket.c socket_server.c \
  malloc_hook.c skynet_daemon.c skynet_log.c

# 定义了完整构建 Skynet 需要依赖的所有目标文件
# $(SKYNET_BUILD_PATH)/skynet - 主可执行文件
# $(foreach v, $(CSERVICE), $(CSERVICE_PATH)/$(v).so - 一个 foreach循环，为 CSERVICE列表中的每个服务名 v，生成一个目标，形如 cservice/snlua.so, cservice/logger.so等。
# $(foreach v, $(LUA_CLIB), $(LUA_CLIB_PATH)/$(v).so) - LUA_CLIB中的每个模块生成对应的 .so文件目标，如 luaclib/skynet.so, luaclib/client.so等。
all : \
  $(SKYNET_BUILD_PATH)/skynet \
  $(foreach v, $(CSERVICE), $(CSERVICE_PATH)/$(v).so) \
  $(foreach v, $(LUA_CLIB), $(LUA_CLIB_PATH)/$(v).so) 

# 规则：如何链接生成 Skynet 主程序
# 依赖项：所有 Skynet 核心源文件、Lua 库、内存分配器库。
# 命令：使用 $(CC)编译器，将所有依赖项($^)链接成目标文件($@)，并指定头文件搜索路径(-Iskynet-src -I$(JEMALLOC_INC))，各种编译参数
$(SKYNET_BUILD_PATH)/skynet : $(foreach v, $(SKYNET_SRC), skynet-src/$(v)) $(LUA_LIB) $(MALLOC_STATICLIB)
	$(CC) $(CFLAGS) -o $@ $^ -Iskynet-src -I$(JEMALLOC_INC) $(LDFLAGS) $(EXPORT) $(SKYNET_LIBS) $(SKYNET_DEFINES)

# 如果输出目录不存在，则创建它们
$(LUA_CLIB_PATH) :
	mkdir $(LUA_CLIB_PATH)

# 如果输出目录不存在，则创建它们
$(CSERVICE_PATH) :
	mkdir $(CSERVICE_PATH)

# 定义一个多行的宏/模板CSERVICE_TEMP
# $(1) 是传入的服务名，比如 snlua
# $$(CSERVICE_PATH)/$(1).so 是目标文件
# service-src/service_$(1).c 是源文件
# | $$(CSERVICE_PATH) 确保目录在编译前存在，且不会因目录更新触发误重编
define CSERVICE_TEMP
  $$(CSERVICE_PATH)/$(1).so : service-src/service_$(1).c | $$(CSERVICE_PATH)
	$$(CC) $$(CFLAGS) $$(SHARED) $$< -o $$@ -Iskynet-src
endef

# 动态展开CSERVICE_TEMP宏，为每个服务名生成规则
$(foreach v, $(CSERVICE), $(eval $(call CSERVICE_TEMP,$(v))))

# 构建 Skynet 的核心 Lua 绑定模块
# addprefix: 这是一个组合技巧。LUA_CLIB_SKYNET 变量里可能存了多个文件名，通过这个函数统一加上路径前缀。
# | $(LUA_CLIB_PATH): 这里的 | 符号表示 Order-only prerequisites。这意味着 Makefile 会检查该目录是否存在，如果不存在就创建它，但目录的更新时间戳改变时，不会触发 .so 的重新编译。
$(LUA_CLIB_PATH)/skynet.so : $(addprefix lualib-src/,$(LUA_CLIB_SKYNET)) | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Iskynet-src -Iservice-src -Ilualib-src

# 构建 BSON（Binary JSON）序列化支持
$(LUA_CLIB_PATH)/bson.so : lualib-src/lua-bson.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -Iskynet-src $^ -o $@

# 构建 MD5 哈希算法支持
$(LUA_CLIB_PATH)/md5.so : 3rd/lua-md5/md5.c 3rd/lua-md5/md5lib.c 3rd/lua-md5/compat-5.2.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -I3rd/lua-md5 $^ -o $@ 

# 构建网络客户端和加密功能
$(LUA_CLIB_PATH)/client.so : lualib-src/lua-clientsocket.c lualib-src/lua-crypt.c lualib-src/lsha1.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -lpthread

# 构建 Sproto 协议序列化库
$(LUA_CLIB_PATH)/sproto.so : lualib-src/sproto/sproto.c lualib-src/sproto/lsproto.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -Ilualib-src/sproto $^ -o $@ 

# 默认不构建，构建 TLS/SSL 加密支持（HTTPS）
$(LUA_CLIB_PATH)/ltls.so : lualib-src/ltls.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -Iskynet-src -L$(TLS_LIB) -I$(TLS_INC) $^ -o $@ -lssl

# 构建 LPeg 解析表达式文法库
$(LUA_CLIB_PATH)/lpeg.so : 3rd/lpeg/lpcap.c 3rd/lpeg/lpcode.c 3rd/lpeg/lpprint.c 3rd/lpeg/lptree.c 3rd/lpeg/lpvm.c 3rd/lpeg/lpcset.c | $(LUA_CLIB_PATH)
	$(CC) $(CFLAGS) $(SHARED) -I3rd/lpeg $^ -o $@ 

# clean目标，删除主程序、所有 C 服务和 Lua C 模块，以及 macOS 上的调试符号目录（dSYM）
clean :
	rm -f $(SKYNET_BUILD_PATH)/skynet $(CSERVICE_PATH)/*.so $(LUA_CLIB_PATH)/*.so && \
  rm -rf $(SKYNET_BUILD_PATH)/*.dSYM $(CSERVICE_PATH)/*.dSYM $(LUA_CLIB_PATH)/*.dSYM

# cleanall目标，先执行 clean，然后深度清理第三方库（jemalloc 和 lua）的构建文件和生成的库文件。
cleanall: clean
ifneq (,$(wildcard 3rd/jemalloc/Makefile))
	cd 3rd/jemalloc && $(MAKE) clean && rm Makefile
endif
	cd 3rd/lua && $(MAKE) clean
	rm -f $(LUA_STATICLIB)

