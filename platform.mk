# 定义平台变量 PLAT，默认值为 none
# ?=表示如果 PLAT未定义则赋值为 none，已定义则保持不变
PLAT ?= none
# 定义支持的平台列表：linux、freebsd、macosx
PLATS = linux freebsd macosx

# 定义编译器变量 CC，默认使用 gcc
CC ?= gcc

# 声明伪目标
.PHONY : none $(PLATS) clean all cleanall

#ifneq ($(PLAT), none)

# 在 Makefile 中，make 命令在不带参数时，默认执行的是 文件中的第一个目标，也就是这里的 default
.PHONY : default

# make PLAT=linux
default :
# 递归调用 make 来构建指定的平台，如果没有指定平台，则默认构建 none
	$(MAKE) $(PLAT)

#endif

# 显示使用说明，提示用户选择平台
none :
	@echo "Please do 'make PLATFORM' where PLATFORM is one of these:"
	@echo "   $(PLATS)"

# 定义 skynet 基础链接库：pthread（线程）和 m（数学库）
SKYNET_LIBS := -lpthread -lm
# 定义共享库编译标志：位置无关代码 + 生成共享库。是为了让库能被别人用
SHARED := -fPIC --shared
# 定义导出符号标志（Linux 下的动态符号导出）。是为了让主程序能被库反向使用
EXPORT := -Wl,-E

linux : PLAT = linux
macosx : PLAT = macosx
freebsd : PLAT = freebsd

macosx : SHARED := -fPIC -dynamiclib -Wl,-undefined,dynamic_lookup
macosx : EXPORT :=
# 定义在 macosx 和 linux 下需要链接的库：dl（动态链接库）
macosx linux : SKYNET_LIBS += -ldl
# 定义在 linux 和 freebsd 下需要链接的库：rt（实时库）
linux freebsd : SKYNET_LIBS += -lrt

# Turn off jemalloc and malloc hook on macosx
macosx : MALLOC_STATICLIB :=
macosx : SKYNET_DEFINES :=-DNOUSE_JEMALLOC

# 递归调用 make 构建 all 目标
# $@ - 是自动化变量，代表当前的目标名。
# SKYNET_LIBS - 平台依赖库
# SHARED - 共享库编译标志
# EXPORT - 导出符号标志
# MALLOC_STATICLIB - jemalloc静态库路径
# SKYNET_DEFINES - 传递宏定义
linux macosx freebsd :
	$(MAKE) all PLAT=$@ SKYNET_LIBS="$(SKYNET_LIBS)" SHARED="$(SHARED)" EXPORT="$(EXPORT)" MALLOC_STATICLIB="$(MALLOC_STATICLIB)" SKYNET_DEFINES="$(SKYNET_DEFINES)"
