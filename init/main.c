// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/acpi.h>
#include <linux/bootconfig.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/kprobes.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/kfence.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/buildid.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/utsname.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/page_ext.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/padata.h>
#include <linux/pid_namespace.h>
#include <linux/device/driver.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>
#include <linux/jump_label.h>
#include <linux/mem_encrypt.h>
#include <linux/kcsan.h>
#include <linux/init_syscalls.h>
#include <linux/stackdepot.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/initcall.h>

#include <kunit/test.h>

static int kernel_init(void *);

extern void init_IRQ(void);
extern void radix_tree_init(void);

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
/* IAMROOT, 2021.09.16:
 * 이 플래그를 통해 IRQ가 비활성화된 상태에서 부팅 프로세서만 실행되는
 * 'early bootup code'에 있음을 알 수 있습니다. 이것은 두 가지를 의미합니다.
 * 플래그가 지워지기 전에 IRQ가 활성화되어서는 안 되며 IRQ 비활성화로 허용되지
 * 않는 일부 작업은 플래그가 설정되는 동안 허용됩니다.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/*
 * IAMROOT, 2021.10.16:
 * - command line 관련 정리
 *
 * command line관련    | 내용
 * ====================+=============================================
 * boot_command_line   | dt에서 가져옴. [bootcmds...][--][bootcmd init args]
 * --------------------+---------------------------------------------
 * command_line (start | arm64는 boot_command_line를 그대로 사용한다.
 * _kernel의 지역변수  | 
 * --------------------+---------------------------------------------
 * extra_command_line  | initrd에서 bootconfig의 key kernel 와 value들
 * --------------------+---------------------------------------------
 * extra_init_args     | initrd에서 bootconfig의 key init 과 value들
 * --------------------+---------------------------------------------
 * bootcmd init param  | boot_command_line에서 " -- "이후의 command들
 * --------------------+---------------------------------------------
 * saved_command_line  | [extra_command_line][bootcmds....--][extra_init_args]
 *                     | [bootcmd init param]
 * --------------------+---------------------------------------------
 * static_command_line | [extra_command_line][command_line]
 * --------------------+---------------------------------------------
 */
/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
/*
 * IAMROOT, 2022.01.05:
 * - setup_command_line에서 초기화된다.(memblock 할당)
 *
 * - boot_command_line에서 init args가 없는 경우
 *  [extra_command_line][bootcmds...][ -- ][extra_init_args]
 * - boot_command_line에서 init args가 있는 경우
 *  [extra_command_line][bootcmds....--][extra_init_args][bootcmd init param]
 */
char *saved_command_line;
/* Command line for parameter parsing */
/*
 * IAMROOT, 2022.01.05:
 * - setup_command_line에서 초기화된다.(memblock할당)
 *
 * - extra_command_line, command_line를 보관한다.
 *   [extra_command_line][command_line]
 */
static char *static_command_line;

/* Untouched extra command line */
/*
 * IAMROOT, 2022.01.05:
 * - setup_boot_config 에서 초기화된다.
 * - initrd에서 가져온 bootconfig의 kernel key와 value string이 있는 memblock
 *   extra_command_line -> bootconfig kernel.*
 */
static char *extra_command_line;
/* Extra init arguments */
/*
 * IAMROOT, 2022.01.05:
 * - setup_boot_config 에서 초기화된다.
 * - initrd에서 가져온 bootconfig의 init key와 value string이 있는 memblock
 *   extra_init_args    -> bootconfig init.* 
 */
static char *extra_init_args;

#ifdef CONFIG_BOOT_CONFIG
/* Is bootconfig on command line? */
/*
 * IAMROOT, 2022.01.04:
 * - setup_boot_config 에서 bootconfg param이 찾아진경우
 *   bootconfig_found가 true가 된다. 또한 "--" param으로 종료된경우
 *   commandline ~ "--" 까지의 길이가 저장된다.
 */
static bool bootconfig_found;
static size_t initargs_offs;
#else
# define bootconfig_found false
# define initargs_offs 0
#endif

static char *execute_command;
static char *ramdisk_execute_command = "/init";

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

/*
 * IAMROOT, 2022.02.19:
 * - argv_init : 아에 등록이 안된 value가 없는 param.
 *               cmdline의 "--" 뒤에 있는 param들.
 *               bootconfig에 init.로 시작했던 param들.
 * - envp_init : 아에 등록이 안된 value가 있는 param
 * - kernel이 아닌 module등에서 사용될수있는 param들을 저장해 두는것.
 */
static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

/*
 * IAMROOT, 2021.10.16:
 * - INIT_SETUP 을 따라가보면 vmlinux.lds에 .init.setup setction에 위치하고 있음이
 *   보인다.
 */
extern const struct obs_kernel_param __setup_start[], __setup_end[];

/*
 * IAMROOT, 2022.02.19:
 * - old param(__setup_start)에서 @line을 찾는다.
 *   ealry가 아닌 param이고 setup_func가 있는경우 seteup_func를 실행시킨다.
 */
static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

#ifdef CONFIG_BLK_DEV_INITRD
/*
 * IAMROOT, 2022.01.01: 
 * initrd 헤더에서 size와 csum 값을 읽어 반환한다. 
 */
static void * __init get_boot_config_from_initrd(u32 *_size, u32 *_csum)
{
	u32 size, csum;
	char *data;
	u32 *hdr;
	int i;

	if (!initrd_end)
		return NULL;

/*
 * IAMROOT, 2022.01.01: 
 * initrd 영역의 가장 마지막에서 "#BOOTCONFIG\n" 문자열을 찾는다.
 * align 관계로 1바이트씩 최대 4번 이동하여 비교 검색한다.
 */

	data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;
	/*
	 * Since Grub may align the size of initrd to 4, we must
	 * check the preceding 3 bytes as well.
	 */
	for (i = 0; i < 4; i++) {
		if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
			goto found;
		data--;
	}
	return NULL;

/*
 * IAMROOT, 2022.01.01: 
 * <----------------initrd-------------------------------------->
 *                           | header            | #BOOTCONFIG\n
 *                           | size + csum + ... |
 * - Documentation/admin-guide/bootconfig.rst 참고
 *   Since the boot configuration file is loaded with initrd,
 *   it will be added to the end of the initrd (initramfs)
 *   image file with padding, size, checksum and 12-byte magic word
 *   as below.
 *
 *   [initrd][bootconfig][padding][size(le32)][checksum(le32)][#BOOTCONFIGn]
 */

found:
	hdr = (u32 *)(data - 8);
	size = le32_to_cpu(hdr[0]);
	csum = le32_to_cpu(hdr[1]);

	data = ((void *)hdr) - size;
	if ((unsigned long)data < initrd_start) {
		pr_err("bootconfig size %d is greater than initrd size %ld\n",
			size, initrd_end - initrd_start);
		return NULL;
	}

	/* Remove bootconfig from initramfs/initrd */
	initrd_end = (unsigned long)data;
	if (_size)
		*_size = size;
	if (_csum)
		*_csum = csum;

	return data;
}
#else
static void * __init get_boot_config_from_initrd(u32 *_size, u32 *_csum)
{
	return NULL;
}
#endif

#ifdef CONFIG_BOOT_CONFIG

/*
 * IAMROOT, 2022.01.05:
 * - key string을 만들때 임시로 저장할 버퍼
 */
static char xbc_namebuf[XBC_KEYLEN_MAX] __initdata;

#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

/*
 * IAMROOT, 2022.01.05:
 * - root의 value를 buf에 string으로 담는다.
 * - buf, size가 NULL, 0값이면 필요한 buf size가 계산된다.
 * - a.b, c, d=3,4 라는 데이터가 있다면 다음과 같이 string이
 *   만들어질 것이다.
 *   a.b c d=3,4
 */
static int __init xbc_snprint_cmdline(char *buf, size_t size,
				      struct xbc_node *root)
{
	struct xbc_node *knode, *vnode;
	char *end = buf + size;
	const char *val;
	int ret;

	xbc_node_for_each_key_value(root, knode, val) {
		ret = xbc_node_compose_key_after(root, knode,
					xbc_namebuf, XBC_KEYLEN_MAX);
		if (ret < 0)
			return ret;

/*
 * IAMROOT, 2022.01.05:
 * - child가 있다는건 value라는것.
 */
		vnode = xbc_node_get_child(knode);
		if (!vnode) {
/*
 * IAMROOT, 2022.01.05:
 * - value가 없다면 완성된 key string을 buf에 옮긴다.
 */
			ret = snprintf(buf, rest(buf, end), "%s ", xbc_namebuf);
			if (ret < 0)
				return ret;
			buf += ret;
			continue;
		}
/*
 * IAMROOT, 2022.01.05:
 * - value를 string까지 완성시키고 key=value 포맷으로 string을 만든다.
 */
		xbc_array_for_each_value(vnode, val) {
			ret = snprintf(buf, rest(buf, end), "%s=\"%s\" ",
				       xbc_namebuf, val);
			if (ret < 0)
				return ret;
			buf += ret;
		}
	}

	return buf - (end - size);
}
#undef rest

/* Make an extra command line under given key word */
/*
 * IAMROOT, 2022.01.05:
 * - 해당 key string을 xbc_nodes에서 검색한다.
 * - 검색이 되면 memblock_alloc에 해당 key node와 value node로
 *   cmdline string을 만들어 return한다.
 */
static char * __init xbc_make_cmdline(const char *key)
{
	struct xbc_node *root;
	char *new_cmdline;
	int ret, len = 0;

	root = xbc_find_node(key);
	if (!root)
		return NULL;

	/* Count required buffer size */
	len = xbc_snprint_cmdline(NULL, 0, root);
	if (len <= 0)
		return NULL;

/*
 * IAMROOT, 2022.01.05:
 * - 계산된 buf length + 1(null 고려)로 memblock에서 할당받고
 *   할당 된 buffer로 cmdline string을 만든다.
 */
	new_cmdline = memblock_alloc(len + 1, SMP_CACHE_BYTES);
	if (!new_cmdline) {
		pr_err("Failed to allocate memory for extra kernel cmdline.\n");
		return NULL;
	}

	ret = xbc_snprint_cmdline(new_cmdline, len + 1, root);
	if (ret < 0 || ret > len) {
		pr_err("Failed to print extra kernel cmdline.\n");
		memblock_free_ptr(new_cmdline, len + 1);
		return NULL;
	}

	return new_cmdline;
}

static int __init bootconfig_params(char *param, char *val,
				    const char *unused, void *arg)
{
	if (strcmp(param, "bootconfig") == 0) {
		bootconfig_found = true;
	}
	return 0;
}

static int __init warn_bootconfig(char *str)
{
	/* The 'bootconfig' has been handled by bootconfig_params(). */
	return 0;
}

/*
 * IAMROOT, 2022.01.05:
 * - initrd_end 에서 boot_config size와 data를 찾는다.
 * - boot_command_line 에서 bootconfig arg를 찾는다.
 * - bootconfig checksum 검사를 수행한다.
 * - string으로 이루어진 bootconfig를 xbc_nodes로 변환한다.
 * - 변환된 xbc_nodes에서 kernel, init key를 찾아서 저장한다.
 */
static void __init setup_boot_config(void)
{
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
	const char *msg;
	int pos;
	u32 size, csum;
	char *data, *copy, *err;
	int ret;

	/* Cut out the bootconfig data even if we have no bootconfig option */
	data = get_boot_config_from_initrd(&size, &csum);

/*
 * IAMROOT, 2022.01.01: 
 * cmdline 문자열에서 "bootconfig"를 찾는다. 
 * 못찾은 경우 곧바로 빠져나간다.
 */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
			 bootconfig_params);

	if (IS_ERR(err) || !bootconfig_found)
		return;

/*
 * IAMROOT, 2022.01.01: 
 * TODO: bootconfig 용법에 대해, 또한 XBC file에 대해 추후 더 알아보자.
 * -bootconfig
 *  Documentation/admin-guide/bootconfig.rst
 * -XBC(extra boot config)
 */

	/* parse_args() stops at the next param of '--' and returns an address */
/*
 * IAMROOT, 2022.01.04:
 * - boot_command_line에서 "--"기준으로 뒷부분은 init args가 된다.
 *   boot_command_line = [일반 bootcmds][--][bootcmd init args]
 *
 * - 그래서 "--" 이없다면 init args가 없다는 개념이 된다.
 *
 * - param이 "--"로 종료되면 err이 해당 지점의 "--"이후의 postion이다.
 *   initargs_offs 는 cmdline ~ bootconfg ~ "--" string 종료지점까지의
 *   길이가 될것이다.
 * - 즉 init args전까지의 길이를 구해옿고 후에 saved_command_line
 *   설정할때 init args를 맨뒤에 복사해 놓을때 사용한다.
 */
	if (err)
		initargs_offs = err - tmp_cmdline;

	if (!data) {
		pr_err("'bootconfig' found on command line, but no bootconfig found\n");
		return;
	}

	if (size >= XBC_DATA_MAX) {
		pr_err("bootconfig size %d greater than max size %d\n",
			size, XBC_DATA_MAX);
		return;
	}

	if (xbc_calc_checksum(data, size) != csum) {
		pr_err("bootconfig checksum failed\n");
		return;
	}

	copy = memblock_alloc(size + 1, SMP_CACHE_BYTES);
	if (!copy) {
		pr_err("Failed to allocate memory for bootconfig\n");
		return;
	}

	memcpy(copy, data, size);
	copy[size] = '\0';

	ret = xbc_init(copy, &msg, &pos);
	if (ret < 0) {
		if (pos < 0)
			pr_err("Failed to init bootconfig: %s.\n", msg);
		else
			pr_err("Failed to parse bootconfig: %s at %d.\n",
				msg, pos);
	} else {
		pr_info("Load bootconfig: %d bytes %d nodes\n", size, ret);
		/* keys starting with "kernel." are passed via cmdline */
		extra_command_line = xbc_make_cmdline("kernel");
		/* Also, "init." keys are init arguments */
		extra_init_args = xbc_make_cmdline("init");
	}
	return;
}

static void __init exit_boot_config(void)
{
	xbc_destroy_all();
}

#else	/* !CONFIG_BOOT_CONFIG */

static void __init setup_boot_config(void)
{
	/* Remove bootconfig data from initrd */
	get_boot_config_from_initrd(NULL, NULL);
}

static int __init warn_bootconfig(char *str)
{
	pr_warn("WARNING: 'bootconfig' found on the kernel command line but CONFIG_BOOT_CONFIG is not set.\n");
	return 0;
}

#define exit_boot_config()	do {} while (0)

#endif	/* CONFIG_BOOT_CONFIG */

early_param("bootconfig", warn_bootconfig);

/* Change NUL term back to "=", to make "param" the whole string. */
/*
 * IAMROOT, 2022.02.19:
 * - param, value string의 관계가 무조건 param=value 형태로 오게 고쳐준다.
 */
static void __init repair_env_string(char *param, char *val)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
		} else
			BUG();
	}
}

/* Anything after -- gets handed straight to init. */
/*
 * IAMROOT, 2022.02.19:
 * - cmdline을 parse하면서 "--"이후의 params를 처리한다. argv_init에 넣는다.
 * - bootconfig의 init.으로 시작했던 param들도 argv_init에 넣는다.
 */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
/*
 * IAMROOT, 2022.02.19:
 * - @param obs param에 존재하면서 ealry인 경우 무시, ealry가 아니면서 setup_func
 *   가 존재하면 setup_func를 실행한다.
 *   obs_param에 없으면 value 존재 여부에 따라 envp_init이나 argv_init에 등록한다.
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	size_t len = strlen(param);

	repair_env_string(param, val);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strnchr(param, len, '.'))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;

/*
 * IAMROOT, 2022.02.19:
 * - value가 있는 param은 envp_init에 넣는다. 만약 envp_init에 똑같은 param이
 *   존재하면 나중껄로 대체하고, envp_init이 가득 찬경우 panic_later를 설정한다.
 */
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], len+1))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;

/*
 * IAMROOT, 2022.02.19:
 * - value가 없는 param은 argv_init에 넣는다. arg_init이 가득 찬 경우엔
 *   panic_later를 설정한다.
 */
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
/*
 * IAMROOT, 2022.01.05:
 * - saved_command_line, static_command_line을 초기화한다.
 */
static void __init setup_command_line(char *command_line)
{
	size_t len, xlen = 0, ilen = 0;

	if (extra_command_line)
		xlen = strlen(extra_command_line);
	if (extra_init_args)
		ilen = strlen(extra_init_args) + 4; /* for " -- " */

	len = xlen + strlen(boot_command_line) + 1;

	saved_command_line = memblock_alloc(len + ilen, SMP_CACHE_BYTES);
	if (!saved_command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len + ilen);

	static_command_line = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!static_command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

/*
 * IAMROOT, 2022.01.01: 
 * - saved_command_line:
 *      extra_command_line + boot_command_line
 *
 & - static_command_line:
 *      extra_command_line + command_line
 */

	if (xlen) {
		/*
		 * We have to put extra_command_line before boot command
		 * lines because there could be dashes (separator of init
		 * command line) in the command lines.
		 */
		strcpy(saved_command_line, extra_command_line);
		strcpy(static_command_line, extra_command_line);
	}
	strcpy(saved_command_line + xlen, boot_command_line);
	strcpy(static_command_line + xlen, command_line);

	if (ilen) {
		/*
		 * Append supplemental init boot args to saved_command_line
		 * so that user can check what command line options passed
		 * to init.
		 * The order should always be
		 * " -- "[bootconfig init-param][cmdline init-param]
		 */
		if (initargs_offs) {
/*
 * IAMROOT, 2022.01.05:
 *  - initargs_offs가 존재.(boot_command_line에 init param이 존재하는 상황
 *  boot_command_line = [cmds...][ -- ][cmdline init param]
 *                      |.............||..................|
 *                        initargs_off        
 *
 *  saved_command_line은 다음과 같은 상황일 것이다.
 *  saved_command_line = [extra_command_line][boot_command_line               ]
 *                       |                   [cmds...}[ -- ][cmdline initparam]
 *                       |   xlen           + initargs_off |
 */

			len = xlen + initargs_offs;

/*
 * IAMROOT, 2022.01.05:
 *
 * saved_command_line + len의 boot_command_line의 [--]이후의 위치를 뜻하며
 * 그자리에 extra_init_args를 복사한다.
 *
 * saved_command_line = [extra_command_line][bootcmds....--][extra_init_args]
 *                      [ len = xlen        + initargs_offs]
 */
			strcpy(saved_command_line + len, extra_init_args);

/*
 * IAMROOT, 2022.01.05:
 * ilen에는 위에서 " -- "를 고려해서 4byte를 더해놨었다. 하지만
 * initargs_offs에 " -- " string에 대해서 이미 더해져있으므로 다시 ilen에서
 * 4byte를 빼는것이다.
 */
			len += ilen - 4;	/* strlen(extra_init_args) */
/*
 * IAMROOT, 2022.01.05:
 * 제일 뒷부분에 원본 boot_command_line에서 initparam을 가져와 붙인다.
 * - saved_command_line
 * [extra_command_line][bootcmds....--][extra_init_args][bootcmd init param]
 * [ len(    xlen      + initargs_offs +   ilen        |
 */
			strcpy(saved_command_line + len,
				boot_command_line + initargs_offs - 1);
		} else {
/*
 * IAMROOT, 2022.01.05:
 *  - initargs_offs가 안하는 상황
 *  (boot_command_line에 init param이 없음
 *  boot_command_line = [cmds...]
 *
 *  saved_command_line은 다음과 같은 상황일 것이다.
 *  saved_command_line = [extra_command_line][boot_command_line]
 *                                           [cmds.............]
 *
 * " -- "와 extra_init_args를 더해준다.
 *
 *  - saved_command_line
 *  [extra_command_line][bootcmds...][ -- ][extra_init_args]
 */
			len = strlen(saved_command_line);
			strcpy(saved_command_line + len, " -- ");
			len += 4;
			strcpy(saved_command_line + len, extra_init_args);
		}
	}
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

noinline void __ref rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	pid = kernel_thread(kernel_init, NULL, CLONE_FS);
	/*
	 * Pin init on the boot CPU. Task migration is not properly working
	 * until sched_init_smp() has been run. It will set the allowed
	 * CPUs for init to the non isolated CPUs.
	 */
	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	tsk->flags |= PF_NO_SETAFFINITY;
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));
	rcu_read_unlock();

	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	/*
	 * Enable might_sleep() and smp_processor_id() checks.
	 * They cannot be enabled earlier because with CONFIG_PREEMPTION=y
	 * kernel_thread() would trigger might_sleep() splats. With
	 * CONFIG_PREEMPT_VOLUNTARY=y the init task might have scheduled
	 * already, but it's stuck on the kthreadd_done completion.
	 */
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}

/*
 * IAMROOT, 2021.10.16:
 * - memory 할당기등이 초기화되기전에 먼저 초기화될 earlycon같은
 *   early driver들을 찾아서 초기화한다.
 * - __setup등으로 등록된 old parameter관련(obs_kernel_param)을 처리하기위해
 *   진입한다.
 */
/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

/*
 * IAMROOT, 2021.10.16:
 * - .init.setup 을 탐색하는 개념
 *   early_param 으로 source에 정의되어 compile time에 .init.setup에 만들어지는
 *   정의들이 존재하며 그 값들과 비교하는것들이다.
 *
 *   cmd line에 예를들어 다음과 같은 param들이 존재한다고 한다.
 *   
 *   initrd=0x41000000,8M console=ttyS0,115200n8
 *
 *   이 경우 initrd는 early_param("initrd", early_initrd) 로 지정된 값들이
 *   호출된것이고, console은 예외적으로 earlycon을 찾는데 해당 정의는
 *   drivers/tty/serial/earlycon.c 에서 보인다.
 *
 *   (early_param("earlycon", param_setup_earlycon))
 *
 *   원래는 cmd line string과 early의 str 정의가 같아야하지만 console은 옛날부터
 *   cmd line에서 그냥 썻던거라 console과 earlycon을 매칭시켜서 찾는다.
 *
 *   - 대표 setup_func : early_initrd, param_setup_earlycon
 */
	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	/* IAMROOT, 2021.10.16:
	 * - setup_arch(..) OR start_kernel(..) 등의 함수에서 여러번
	 *   호출될 수 있으므로 한번만 실행되도록 flag를 사용한다.
	 */
	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak arch_post_acpi_subsys_init(void) { }

void __init __weak smp_setup_processor_id(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak mem_encrypt_init(void) { }

void __init __weak poking_init(void) { }

void __init __weak pgtable_cache_init(void) { }

void __init __weak trap_init(void) { }

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void);
#else
static inline void initcall_debug_enable(void)
{
}
#endif

/* Report memory auto-initialization states for this boot. */
/*
 * IAMROOT, 2022.02.19:
 * - config에 따라서 현재 memory debug 상태를 출력한다.
 * ex) mem auto-init: stack:off, heap alloc:on, heap free:off
 */
static void __init report_meminit(void)
{
	const char *stack;

	if (IS_ENABLED(CONFIG_INIT_STACK_ALL_PATTERN))
		stack = "all(pattern)";
	else if (IS_ENABLED(CONFIG_INIT_STACK_ALL_ZERO))
		stack = "all(zero)";
	else if (IS_ENABLED(CONFIG_GCC_PLUGIN_STRUCTLEAK_BYREF_ALL))
		stack = "byref_all(zero)";
	else if (IS_ENABLED(CONFIG_GCC_PLUGIN_STRUCTLEAK_BYREF))
		stack = "byref(zero)";
	else if (IS_ENABLED(CONFIG_GCC_PLUGIN_STRUCTLEAK_USER))
		stack = "__user(zero)";
	else
		stack = "off";

	pr_info("mem auto-init: stack:%s, heap alloc:%s, heap free:%s\n",
		stack, want_init_on_alloc(GFP_KERNEL) ? "on" : "off",
		want_init_on_free() ? "on" : "off");
	if (want_init_on_free())
		pr_info("mem auto-init: clearing system memory may take some time...\n");
}

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void)
{
	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */
	page_ext_init_flatmem();
	init_mem_debugging_and_hardening();
	kfence_alloc_pool();
	report_meminit();
	stack_depot_init();
	mem_init();
	mem_init_print_info();
	/* page_owner must be initialized after buddy is ready */
	page_ext_init_flatmem_late();
	kmem_cache_init();
	kmemleak_init();
	pgtable_init();
	debug_objects_mem_init();
	vmalloc_init();
	/* Should be run before the first non-init thread is created */
	init_espfix_bsp();
	/* Should be run after espfix64 is set up. */
	pti_init();
}

#ifdef CONFIG_HAVE_ARCH_RANDOMIZE_KSTACK_OFFSET
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,
			   randomize_kstack_offset);
DEFINE_PER_CPU(u32, kstack_offset);

static int __init early_randomize_kstack_offset(char *buf)
{
	int ret;
	bool bool_result;

	ret = kstrtobool(buf, &bool_result);
	if (ret)
		return ret;

	if (bool_result)
		static_branch_enable(&randomize_kstack_offset);
	else
		static_branch_disable(&randomize_kstack_offset);
	return 0;
}
early_param("randomize_kstack_offset", early_randomize_kstack_offset);
#endif

void __init __weak arch_call_rest_init(void)
{
	rest_init();
}

/*
 * IAMROOT, 2022.02.19:
 * - argv_init, envp_init이 고쳐졌을경우(unknown bootoption이 존재) 한번에 묶어서
 *   print한다.
 */
static void __init print_unknown_bootoptions(void)
{
	char *unknown_options;
	char *end;
	const char *const *p;
	size_t len;

	if (panic_later || (!argv_init[1] && !envp_init[2]))
		return;

	/*
	 * Determine how many options we have to print out, plus a space
	 * before each
	 */
	len = 1; /* null terminator */
	for (p = &argv_init[1]; *p; p++) {
		len++;
		len += strlen(*p);
	}
	for (p = &envp_init[2]; *p; p++) {
		len++;
		len += strlen(*p);
	}

	unknown_options = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!unknown_options) {
		pr_err("%s: Failed to allocate %zu bytes\n",
			__func__, len);
		return;
	}
	end = unknown_options;

	for (p = &argv_init[1]; *p; p++)
		end += sprintf(end, " %s", *p);
	for (p = &envp_init[2]; *p; p++)
		end += sprintf(end, " %s", *p);

	pr_notice("Unknown command line parameters:%s\n", unknown_options);
	memblock_free_ptr(unknown_options, len);
}

/* IAMROOT, 2021.09.11:
 * - __visible:
 *   compile optimization으로 인해 안쓰이는 함수는 symbol export가
 *   되지 않을 수 있는데 이를 방지하기 위해 linker에게 알려주는 역할.
 *
 * - __no_sanitize_address:
 *   sanitize address 관련 compiler option이 enable 되어도 start_kernel은
 *   영향을 받지 않게 한다.
 */
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	set_task_stack_end_magic(&init_task);
	smp_setup_processor_id();
	debug_objects_early_init();
	init_vmlinux_build_id();

	cgroup_init_early();

	local_irq_disable();
	early_boot_irqs_disabled = true;

	/*
	 * Interrupts are still disabled. Do necessary setups, then
	 * enable them.
	 */
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner);
	early_security_init();
	setup_arch(&command_line);
	setup_boot_config();
	setup_command_line(command_line);
	setup_nr_cpu_ids();
	setup_per_cpu_areas();
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */
	boot_cpu_hotplug_init();

	build_all_zonelists(NULL);
	page_alloc_init();

/*
 * IAMROOT, 2022.02.19:
 * - ex) ubuntu 20.04 64bit
 *   Kernel command line: BOOT_IMAGE=/vmlinuz-5.13.0-28-generic
 *   root=UUID=1dc50bb4-e6ef-410a-be8e-d71e554def1d ro quiet splash
 */
	pr_notice("Kernel command line: %s\n", saved_command_line);
	/* parameters may set static keys */
	jump_label_init();
	parse_early_param();

/*
 * IAMROOT, 2022.02.19:
 * - static_command_line에서 -1 level(core_param, module_param_cb등)으로 등록된
 *   param을 처리한다. 아에 검색을 못한것은 unknown_bootoption로 실행된다.
 */
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	print_unknown_bootoptions();

/*
 * IAMROOT, 2022.02.19:
 * - static_command_line에서 "--"이 존재하는경우 이후 string을 parse해
 *   argv_init에 넣는다.
 */
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);

/*
 * IAMROOT, 2022.02.19:
 * - bootconfig의 init.으로 시작하는 param을 argv_init에 넣는다.
 */
	if (extra_init_args)
		parse_args("Setting extra init args", extra_init_args,
			   NULL, 0, -1, -1, NULL, set_init_arg);

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	setup_log_buf(0);
	vfs_caches_init_early();
	sort_main_extable();
	trap_init();
	mm_init();

	ftrace_init();

	/* trace_printk can be enabled here */
	early_trace_init();

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */

/*
 * IAMROOT, 2022.07.09:
 * -papago
 *  인터럽트(예: 타이머 인터럽트)를 시작하기 전에 스케줄러를 설정하십시오.
 *  전체 토폴로지 설정은 smp_init() 시간에 발생하지만 그 동안에는 여전히 작동하는
 *  스케줄러가 있습니다.
 */
	sched_init();

	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	radix_tree_init();

	/*
	 * Set up housekeeping before setting up workqueues to allow the unbound
	 * workqueue to take non-housekeeping into account.
	 */
	housekeeping_init();

	/*
	 * Allow workqueue creation and work item queueing/cancelling
	 * early.  Work item execution depends on kthreads and starts after
	 * workqueue_init().
	 */
	workqueue_init_early();

	rcu_init();

	/* Trace events are available after this */
	trace_init();

	if (initcall_debug)
		initcall_debug_enable();

	context_tracking_init();
	/* init some links before init_ISA_irqs() */
	early_irq_init();
	init_IRQ();
	tick_init();
	rcu_init_nohz();
	init_timers();
	srcu_init();
	hrtimers_init();
	softirq_init();
	timekeeping_init();
	kfence_init();

	/*
	 * For best initial stack canary entropy, prepare it after:
	 * - setup_arch() for any UEFI RNG entropy and boot cmdline access
	 * - timekeeping_init() for ktime entropy used in rand_initialize()
	 * - rand_initialize() to get any arch-specific entropy like RDRAND
	 * - add_latent_entropy() to get any latent entropy
	 * - adding command line entropy
	 */
	rand_initialize();
	add_latent_entropy();
	add_device_randomness(command_line, strlen(command_line));
	boot_init_stack_canary();

	time_init();
	perf_event_init();
	profile_init();
	call_function_init();
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");

	early_boot_irqs_disabled = false;
	local_irq_enable();

	kmem_cache_init_late();

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	console_init();
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	lockdep_init();

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	locking_selftest();

	/*
	 * This needs to be called before any devices perform DMA
	 * operations that might use the SWIOTLB bounce buffers. It will
	 * mark the bounce buffers as decrypted so that their usage will
	 * not cause "plain-text" data to be decrypted when accessed.
	 */
	mem_encrypt_init();

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	setup_per_cpu_pageset();
	numa_policy_init();
	acpi_early_init();
	if (late_time_init)
		late_time_init();
	sched_clock_init();
	calibrate_delay();
	pid_idr_init();
	anon_vma_init();
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
	thread_stack_cache_init();
	cred_init();
	fork_init();
	proc_caches_init();
	uts_ns_init();
	key_init();
	security_init();
	dbg_late_init();
	vfs_caches_init();
	pagecache_init();
	signals_init();
	seq_file_init();
	proc_root_init();
	nsfs_init();
	cpuset_init();
	cgroup_init();
	taskstats_init_early();
	delayacct_init();

	poking_init();
	check_bugs();

	acpi_subsystem_init();
	arch_post_acpi_subsys_init();
	kcsan_init();

	/* Do the rest non-__init'ed, we're now alive */
	arch_call_rest_init();

	prevent_tail_call_optimization();
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
/*
 * For UML, the constructors have already been called by the
 * normal setup code as it's just a normal ELF binary, so we
 * cannot do it again - but we do need CONFIG_CONSTRUCTORS
 * even on UML for modules.
 */
#if defined(CONFIG_CONSTRUCTORS) && !defined(CONFIG_UML)
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = memblock_alloc(sizeof(*entry),
					       SMP_CACHE_BYTES);
			if (!entry)
				panic("%s: Failed to allocate %zu bytes\n",
				      __func__, sizeof(*entry));
			entry->buf = memblock_alloc(strlen(str_entry) + 1,
						    SMP_CACHE_BYTES);
			if (!entry->buf)
				panic("%s: Failed to allocate %zu bytes\n",
				      __func__, strlen(str_entry) + 1);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static __init_or_module void
trace_initcall_start_cb(void *data, initcall_t fn)
{
	ktime_t *calltime = (ktime_t *)data;

	printk(KERN_DEBUG "calling  %pS @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static __init_or_module void
trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	ktime_t *calltime = (ktime_t *)data;
	ktime_t delta, rettime;
	unsigned long long duration;

	rettime = ktime_get();
	delta = ktime_sub(rettime, *calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	printk(KERN_DEBUG "initcall %pS returned %d after %lld usecs\n",
		 fn, ret, duration);
}

static ktime_t initcall_calltime;

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void)
{
	int ret;

	ret = register_trace_initcall_start(trace_initcall_start_cb,
					    &initcall_calltime);
	ret |= register_trace_initcall_finish(trace_initcall_finish_cb,
					      &initcall_calltime);
	WARN(ret, "Failed to register initcall tracepoints\n");
}
# define do_trace_initcall_start	trace_initcall_start
# define do_trace_initcall_finish	trace_initcall_finish
#else
static inline void do_trace_initcall_start(initcall_t fn)
{
	if (!initcall_debug)
		return;
	trace_initcall_start_cb(&initcall_calltime, fn);
}
static inline void do_trace_initcall_finish(initcall_t fn, int ret)
{
	if (!initcall_debug)
		return;
	trace_initcall_finish_cb(&initcall_calltime, fn, ret);
}
#endif /* !TRACEPOINTS_ENABLED */

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


extern initcall_entry_t __initcall_start[];
extern initcall_entry_t __initcall0_start[];
extern initcall_entry_t __initcall1_start[];
extern initcall_entry_t __initcall2_start[];
extern initcall_entry_t __initcall3_start[];
extern initcall_entry_t __initcall4_start[];
extern initcall_entry_t __initcall5_start[];
extern initcall_entry_t __initcall6_start[];
extern initcall_entry_t __initcall7_start[];
extern initcall_entry_t __initcall_end[];

static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static const char *initcall_level_names[] __initdata = {
	"pure",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static int __init ignore_unknown_bootoption(char *param, char *val,
			       const char *unused, void *arg)
{
	return 0;
}

static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, ignore_unknown_bootoption);

	trace_initcall_level(initcall_level_names[level]);
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static void __init do_initcalls(void)
{
	int level;
	size_t len = strlen(saved_command_line) + 1;
	char *command_line;

	command_line = kzalloc(len, GFP_KERNEL);
	if (!command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		/* Parser modifies command_line, restore it each time */
		strcpy(command_line, saved_command_line);
		do_initcall_level(level, command_line);
	}

	kfree(command_line);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();
	driver_init();
	init_irq_proc();
	do_ctors();
	do_initcalls();
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static int run_init_process(const char *init_filename)
{
	const char *const *p;

	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	pr_debug("  with arguments:\n");
	for (p = argv_init; *p; p++)
		pr_debug("    %s\n", *p);
	pr_debug("  with environment:\n");
	for (p = envp_init; *p; p++)
		pr_debug("    %s\n", *p);
	return kernel_execve(init_filename, argv_init, envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

/*
 * IAMROOT, 2021.10.30: 
 * "rodata=off"와 같이 설정한 경우 커널 코드 및 rodata 섹션 영역의 데이터가
 * Read Write가 가능하게 매핑한다.
 * 
 * 주의: early param의 경우 parse_rodata() 함수에 존재한다.
 */
#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;
static int __init set_debug_rodata(char *str)
{
	return strtobool(str, &rodata_enabled);
}
__setup("rodata=", set_debug_rodata);
#endif

#ifdef CONFIG_STRICT_KERNEL_RWX
static void mark_readonly(void)
{
	if (rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned
		 * up with call_rcu().  Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		rcu_barrier();
		mark_rodata_ro();
		rodata_test();
	} else
		pr_info("Kernel memory protection disabled.\n");
}
#elif defined(CONFIG_ARCH_HAS_STRICT_KERNEL_RWX)
static inline void mark_readonly(void)
{
	pr_warn("Kernel memory protection not selected by kernel config.\n");
}
#else
static inline void mark_readonly(void)
{
	pr_warn("This architecture does not have kernel memory protection.\n");
}
#endif

void __weak free_initmem(void)
{
	free_initmem_default(POISON_FREE_INITMEM);
}

static int __ref kernel_init(void *unused)
{
	int ret;

	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done);

	kernel_init_freeable();
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();
	kprobe_free_init_mem();
	ftrace_free_init_mem();
	kgdb_free_init_mem();
	exit_boot_config();
	free_initmem();
	mark_readonly();

	/*
	 * Kernel mappings are now finalized - update the userspace page-table
	 * to finalize PTI.
	 */
	pti_finalize();

	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	rcu_end_inkernel_boot();

	do_sysctl_args();

	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

	if (CONFIG_DEFAULT_INIT[0] != '\0') {
		ret = run_init_process(CONFIG_DEFAULT_INIT);
		if (ret)
			pr_err("Default init %s failed (error %d)\n",
			       CONFIG_DEFAULT_INIT, ret);
		else
			return 0;
	}

	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");
}

/* Open /dev/console, for stdin/stdout/stderr, this should never fail */
void __init console_on_rootfs(void)
{
	struct file *file = filp_open("/dev/console", O_RDWR, 0);

	if (IS_ERR(file)) {
		pr_err("Warning: unable to open an initial console.\n");
		return;
	}
	init_dup(file);
	init_dup(file);
	init_dup(file);
	fput(file);
}

static noinline void __init kernel_init_freeable(void)
{
	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);

	cad_pid = get_pid(task_pid(current));

	smp_prepare_cpus(setup_max_cpus);

	workqueue_init();

	init_mm_internals();

	rcu_init_tasks_generic();
	do_pre_smp_initcalls();
	lockup_detector_init();

	smp_init();
	sched_init_smp();

	padata_init();
	page_alloc_init_late();
	/* Initialize page ext after all struct pages are initialized. */
	page_ext_init();

	do_basic_setup();

	kunit_run_all_tests();

	wait_for_initramfs();
	console_on_rootfs();

	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */
	if (init_eaccess(ramdisk_execute_command) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 *
	 * rootfs is available now, try loading the public keys
	 * and default modules
	 */

	integrity_load_keys();
}
