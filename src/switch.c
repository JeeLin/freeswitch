/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Michael Jerris <mike@jerris.com>
 * Pawel Pierscionek <pawel@voiceworks.pl>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 *
 *
 * switch.c -- Main
 *
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#ifndef WIN32
#include <poll.h>
#ifdef HAVE_SETRLIMIT
#include <sys/resource.h>
#endif
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <switch.h>
#include "private/switch_apr_pvt.h"
#include "private/switch_core_pvt.h"

// pidfile
/* pid filename: Stores the process id of the freeswitch process */
#define PIDFILE "freeswitch.pid"
static char *pfile = PIDFILE;
static int system_ready = 0;

/* Picky compiler */
#ifdef __ICC
// 告知编译器，停止167类型警告
#pragma warning (disable:167)
#endif

#ifdef WIN32
/* If we are a windows service, what should we be called */
#define SERVICENAME_DEFAULT "FreeSWITCH"
#define SERVICENAME_MAXLEN 256
static char service_name[SERVICENAME_MAXLEN];
static switch_core_flag_t service_flags = SCF_NONE;
#include <winsock2.h>
#include <windows.h>

/* event to signal shutdown (for you unix people, this is like a pthread_cond) */
static HANDLE shutdown_event;

#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#endif

/* signal handler for when freeswitch is running in background mode.
 * signal triggers the shutdown of freeswitch
# */
static void handle_SIGILL(int sig)
{
	int32_t arg = 0;
	if (sig) {}
	/* send shutdown signal to the freeswitch core */
	switch_core_session_ctl(SCSC_SHUTDOWN, &arg);
	return;
}

static void handle_SIGTERM(int sig)
{
	int32_t arg = 0;
	if (sig) {}
	/* send shutdown signal to the freeswitch core */
	switch_core_session_ctl(SCSC_SHUTDOWN_ELEGANT, &arg);
	return;
}

// 杀死后台进程
/* kill a freeswitch process running in background mode */
static int freeswitch_kill_background()
{
	FILE *f;					/* FILE handle to open the pid file */
	char path[PATH_MAX] = "";		/* full path of the PID file */
	pid_t pid = 0;				/* pid from the pid file */
	// 设置所有配置，避免为空
	/* set the globals so we can use the global paths. */
	switch_core_set_globals();

	// switch_snprintfv(char *zBuf, int n, const char *zFormat, ...)
	// 将可变参数根据zFormat格式化后，赋值给zBuf
	/* get the full path of the pid file. */
	switch_snprintf(path, sizeof(path), "%s%s%s", SWITCH_GLOBAL_dirs.run_dir, SWITCH_PATH_SEPARATOR, pfile);

	/* open the pid file */
	if ((f = fopen(path, "r")) == 0) {
		/* pid file does not exist */
		fprintf(stderr, "Cannot open pid file %s.\n", path);
		return 255;
	}
	// (int *) (intptr_t) & pid, 经过变换返回pid的指针位置
	// intptr_t 和 uintptr_t 类型用来存放指针地址。
	// 它们提供了一种可移植且安全的方法声明指针，而且和系统中使用的指针长度相同，对于把指针转化成整数形式来说很有用
	/* pull the pid from the file */
	if (fscanf(f, "%d", (int *) (intptr_t) & pid) != 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to get the pid!\n");
	}

	/* if we have a valid pid */
	if (pid > 0) {

		/* kill the freeswitch running at the pid we found */
		fprintf(stderr, "Killing: %d\n", (int) pid);
#ifdef WIN32
		/* for windows we need the event to signal for shutting down a background FreeSWITCH */
		snprintf(path, sizeof(path), "Global\\Freeswitch.%d", pid);

		/* open the event so we can signal it */
		shutdown_event = OpenEvent(EVENT_MODIFY_STATE, FALSE, path);

		/* did we successfully open the event */
		if (!shutdown_event) {
			/* we can't get the event, so we can't signal the process to shutdown */
			fprintf(stderr, "ERROR: Can't Shutdown: %d\n", (int) pid);
		} else {
			/* signal the event to shutdown */
			SetEvent(shutdown_event);
			/* cleanup */
			CloseHandle(shutdown_event);
		}
#else
		/* for unix, send the signal to kill. */
		kill(pid, SIGTERM);
#endif
	}

	/* be nice and close the file handle to the pid file */
	fclose(f);

	return 0;
}

#ifdef WIN32

/* we need these vars to handle the service */
SERVICE_STATUS_HANDLE hStatus;
SERVICE_STATUS status;

/* Handler function for service start/stop from the service */
void WINAPI ServiceCtrlHandler(DWORD control)
{
	switch (control) {
	case SERVICE_CONTROL_SHUTDOWN:
	case SERVICE_CONTROL_STOP:
		/* Shutdown freeswitch */
		switch_core_destroy();
		/* set service status values */
		status.dwCurrentState = SERVICE_STOPPED;
		status.dwWin32ExitCode = 0;
		status.dwCheckPoint = 0;
		status.dwWaitHint = 0;
		break;
	case SERVICE_CONTROL_INTERROGATE:
		/* we already set the service status every time it changes. */
		/* if there are other times we change it and don't update, we should do so here */
		break;
	}

	SetServiceStatus(hStatus, &status);
}

/* the main service entry point */
void WINAPI service_main(DWORD numArgs, char **args)
{
	switch_core_flag_t flags = SCF_USE_SQL | SCF_USE_AUTO_NAT | SCF_USE_NAT_MAPPING | SCF_CALIBRATE_CLOCK | SCF_USE_CLOCK_RT;
	const char *err = NULL;		/* error value for return from freeswitch initialization */

	/* Override flags if they have been set earlier */
	if (service_flags != SCF_NONE)
		flags = service_flags;

	/*  we have to initialize the service-specific stuff */
	memset(&status, 0, sizeof(SERVICE_STATUS));
	status.dwServiceType = SERVICE_WIN32;
	status.dwCurrentState = SERVICE_START_PENDING;
	status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

	/* register our handler for service control messages */
	hStatus = RegisterServiceCtrlHandler(service_name, &ServiceCtrlHandler);

	/* update the service status */
	SetServiceStatus(hStatus, &status);

	switch_core_set_globals();

	/* attempt to initialize freeswitch and load modules */
	if (switch_core_init_and_modload(flags, SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS) {
		/* freeswitch did not start successfully */
		status.dwCurrentState = SERVICE_STOPPED;
	} else {
		/* freeswitch started */
		status.dwCurrentState = SERVICE_RUNNING;
	}

	/* update the service status */
	SetServiceStatus(hStatus, &status);
}

#else

static int check_fd(int fd, int ms)
{
	struct pollfd pfds[2] = { { 0 } };
	int s, r = 0, i = 0;

	pfds[0].fd = fd;
	pfds[0].events = POLLIN | POLLERR;
	s = poll(pfds, 1, ms);

	if (s == 0 || s == -1) {
		r = s;
	} else {
		r = -1;

		if ((pfds[0].revents & POLLIN)) {
			if ((i = read(fd, &r, sizeof(r))) > -1) {
				(void)write(fd, &r, sizeof(r));
			}
		}
	}

	return r;
}
// 进程处理
// --ncwait时 fds 否则 NULL
static void daemonize(int *fds)
{
	int fd;
	pid_t pid;
	unsigned int sanity = 60;

	if (!fds) {
		// fork 复制当前进程创建子进程，主子两个进程都从此处开始执行
		// 主进程返回值为子进程的pid，子进程返回为0，失败<0
		switch (fork()) {
		case 0:		/* child process */
			break;
		case -1:
			fprintf(stderr, "Error Backgrounding (fork)! %d - %s\n", errno, strerror(errno));
			exit(EXIT_SUCCESS);
			break;
		default:	/* parent process */
			// 主进程退出
			exit(EXIT_SUCCESS);
		}
		// 子进程不再受主进程停止影响
		// https://blog.csdn.net/smcnjyddx0623/article/details/52336294
		if (setsid() < 0) {
			fprintf(stderr, "Error Backgrounding (setsid)! %d - %s\n", errno, strerror(errno));
			exit(EXIT_SUCCESS);
		}
	}
	// 以上到此只剩一个进程，再次进程分裂
	pid = switch_fork();

	switch (pid) {
	case 0:		/* child process */
		// 子进程关闭读端fds[0]
		if (fds) {
			close(fds[0]);
		}
		break;
	case -1:
		fprintf(stderr, "Error Backgrounding (fork2)! %d - %s\n", errno, strerror(errno));
		exit(EXIT_SUCCESS);
		break;
	default:	/* parent process */
		fprintf(stderr, "%d Backgrounding.\n", (int) pid);

		if (fds) {
			char *o;
			// 父进程关闭写端fds[1]
			close(fds[1]);
			// 重试次数 backgrouding timeout
			if ((o = getenv("FREESWITCH_BG_TIMEOUT"))) {
				int tmp = atoi(o);
				if (tmp > 0) {
					sanity = tmp;
				}
			}

			do {
				system_ready = check_fd(fds[0], 2000);

				if (system_ready == 0) {
					printf("FreeSWITCH[%d] Waiting for background process pid:%d to be ready.....\n", (int)getpid(), (int) pid);
				}

			} while (--sanity && system_ready == 0);
			// 关闭读写与传输
			shutdown(fds[0], 2);
			// 关闭文件描述符
			close(fds[0]);
			fds[0] = -1;


			if (system_ready < 0) {
				printf("FreeSWITCH[%d] Error starting system! pid:%d\n", (int)getpid(), (int) pid);
				kill(pid, 9);
				exit(EXIT_FAILURE);
			}

			printf("FreeSWITCH[%d] System Ready pid:%d\n", (int) getpid(), (int) pid);
		}

		exit(EXIT_SUCCESS);
	}

	if (fds) {
		setsid();
	}
	// 文件描述符 0是标准输入，1是标准输出，2是标准错误
	/* redirect std* to null */
	fd = open("/dev/null", O_RDONLY);
	switch_assert( fd >= 0 );
	if (fd != 0) {
		dup2(fd, 0);
		close(fd);
	}

	fd = open("/dev/null", O_WRONLY);
	switch_assert( fd >= 0 );
	if (fd != 1) {
		dup2(fd, 1);
		close(fd);
	}

	fd = open("/dev/null", O_WRONLY);
	switch_assert( fd >= 0 );
	if (fd != 2) {
		dup2(fd, 2);
		close(fd);
	}
	return;
}

static pid_t reincarnate_child = 0;
static void reincarnate_handle_sigterm (int sig) {
	if (!sig) return;
	if (reincarnate_child) kill(reincarnate_child, sig);
	return;
}

static void reincarnate_protect(char **argv) {
	int i; struct sigaction sa, sa_dfl, sa4_prev, sa15_prev, sa17_prev;
	memset(&sa, 0, sizeof(sa)); memset(&sa_dfl, 0, sizeof(sa_dfl));
	// 指定信号量处理函数
	sa.sa_handler = reincarnate_handle_sigterm;
	// SIG_DFL 默认信号处理方法
	sa_dfl.sa_handler = SIG_DFL;
 refork:
	if ((i=fork())) { /* parent */
		int s; pid_t r;
		reincarnate_child = i;
		sigaction(SIGILL, &sa, &sa4_prev);
		sigaction(SIGTERM, &sa, &sa15_prev);
		sigaction(SIGCHLD, &sa_dfl, &sa17_prev);
	rewait:
		// 主进程是master守护进程，在子进程死亡时重启
		// 等待i进程 https://blog.csdn.net/Roland_Sun/article/details/32084825
		// 如果执行成功则返回子进程识别码(PID) ,如果有错误发生则返回返回值-1。失败原因存于 errno 中。
		r = waitpid(i, &s, 0);
		if (r == (pid_t)-1) {
			if (errno == EINTR) goto rewait;
			exit(EXIT_FAILURE);
		}
		if (r != i) goto rewait;
		// 判断子进程结束原因
		if (WIFEXITED(s)
			&& (WEXITSTATUS(s) == EXIT_SUCCESS
				|| WEXITSTATUS(s) == EXIT_FAILURE)) {
			exit(WEXITSTATUS(s));
		}
		// 若意外终止则重新拉起
		if (WIFEXITED(s) || WIFSIGNALED(s)) {
			sigaction(SIGILL, &sa4_prev, NULL);
			sigaction(SIGTERM, &sa15_prev, NULL);
			sigaction(SIGCHLD, &sa17_prev, NULL);
			if (argv) {
				// execv会停止执行当前的进程，并且以path应用进程替换被停止执行的进程，进程ID没有改变
				if (argv[0] && execv(argv[0], argv) == -1) {
					char buf[256];
					fprintf(stderr, "Reincarnate execv() failed: %d %s\n", errno,
							switch_strerror_r(errno, buf, sizeof(buf)));
				}
				fprintf(stderr, "Trying reincarnate-reexec plan B...\n");
				if (argv[0] && execvp(argv[0], argv) == -1) {
					char buf[256];
					fprintf(stderr, "Reincarnate execvp() failed: %d %s\n", errno,
							switch_strerror_r(errno, buf, sizeof(buf)));
				}
				fprintf(stderr, "Falling back to normal reincarnate behavior...\n");
				goto refork;
			} else goto refork;
		}
		goto rewait;
	} else { /* child */
#ifdef __linux__
		// 子进程是worker进程
		// 以下用于父进程SIGTERM信号时避免子进程未退出
		// https://blog.fluyy.net/post/20181208/54v7hgr0z1wu4zebhc7nr
		prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
	}
}

#endif
/*
（1）-nf 不允许Fork新进程

（2）-u [user] 启动后以非root用户user身份运行

（3）-g [group] 启动后以非root组group身份运行

（4）-version 显示版本信息

（5）-waste 允许浪费内存地址空间，FreeSWITCH仅需240KB的栈空间，你可以使用ulimit -s
240限制栈空间使用，或使用该选项忽略警告信息

（6）-core 出错时进行内核转储

（7）-rp 开启高优先级（实时）设置

（8）-lp 开启低优先级设置

（9）-np 普通优先级

（10）-vg 在valgrind下运行，调试内存泄漏时使用

（11）-nosql 不使用SQL，show channels类的命令将不能显示结果

（12）-heavy-timer 更精确的时钟，可能会更精确，但对系统要求更高

（13）-nonat
如果路由器支持uPnP或NAT-PMP，则FreeSWITCH可以自动解决NAT穿越问题。如果路由器不支持，则该选项可以使启动更快。

（14）-nocal
关闭时钟核准。FreeSWITCH理想的运行环境是1000Hz的内核时钟。如果你在内核时钟小于1000Hz或在虚拟机上，可以尝试关闭该选项

（15）-nort 关闭实时时钟

（16）-stop 关闭FreeSWITCH，它会在run目录中查找PID文件

（17）-nc 启动到后台模式，没有控制台

（18）-ncwait 后台模式，等待系统完全初始化完毕之后再退出父进程，隐含“-nc”选项

（19）-c 启动到控制台，默认Options to control location of files

（20）-base [confdir] 指定其他的基准目录，在配置文件中使用$${base}

（21）-conf [confdir] 指定其他的配置文件所在目录，需与-log、-db合用

（22）-log [logdir] 指定其他的日志目录

（23）-run [rundir] 指定其他存放PID文件的运行目录

（24）-db [dbdir] 指定其他数据库的目录

（25）-mod [moddir] 指定其他模块目录

（26）-htdocs [htdocsdir] 指定其他HTTP根目录

（27）-scripts [scriptsdir] 指定其他脚本目录

（28）-temp [directory] 指定其他临时文件目录

（29）-grammar [directory] 指定其他语法目录

（30）-certs [directory] 指定其他SSL证书路径

（31）-recordings [directory] 指定其他录音目录

（32）-storage [directory] 指定其他存储目录（语音信箱等）

（33）-sounds [directory] 指定其他声音文件目录
*/
static const char usage[] =
	"Usage: freeswitch [OPTIONS]\n\n"
	"These are the optional arguments you can pass to freeswitch:\n"
#ifdef WIN32
	"\t-service [name]        -- start freeswitch as a service, cannot be used if loaded as a console app\n"
	"\t-install [name]        -- install freeswitch as a service, with optional service name\n"
	"\t-uninstall             -- remove freeswitch as a service\n"
	"\t-monotonic-clock       -- use monotonic clock as timer source\n"
#else
	"\t-nf                    -- no forking\n"
	"\t-reincarnate           -- restart the switch on an uncontrolled exit\n"
	"\t-reincarnate-reexec    -- run execv on a restart (helpful for upgrades)\n"
	"\t-u [user]              -- specify user to switch to\n"
	"\t-g [group]             -- specify group to switch to\n"
#endif
#ifdef HAVE_SETRLIMIT
#ifndef FS_64BIT
	"\t-waste                 -- allow memory waste\n"
#endif
	"\t-core                  -- dump cores\n"
#endif
	"\t-help                  -- this message\n"
	"\t-version               -- print the version and exit\n"
	"\t-rp                    -- enable high(realtime) priority settings\n"
	"\t-lp                    -- enable low priority settings\n"
	"\t-np                    -- enable normal priority settings\n"
	"\t-vg                    -- run under valgrind\n"
	"\t-nosql                 -- disable internal sql scoreboard\n"
	"\t-heavy-timer           -- Heavy Timer, possibly more accurate but at a cost\n"
	"\t-nonat                 -- disable auto nat detection\n"
	"\t-nonatmap              -- disable auto nat port mapping\n"
	"\t-nocal                 -- disable clock calibration\n"
	"\t-nort                  -- disable clock clock_realtime\n"
	"\t-stop                  -- stop freeswitch\n"
	"\t-nc                    -- do not output to a console and background\n"
#ifndef WIN32
	"\t-ncwait                -- do not output to a console and background but wait until the system is ready before exiting (implies -nc)\n"
#endif
	"\t-c                     -- output to a console and stay in the foreground\n"
	"\n\tOptions to control locations of files:\n"
	"\t-base [basedir]         -- alternate prefix directory\n"
	"\t-cfgname [filename]     -- alternate filename for FreeSWITCH main configuration file\n"
	"\t-conf [confdir]         -- alternate directory for FreeSWITCH configuration files\n"
	"\t-log [logdir]           -- alternate directory for logfiles\n"
	"\t-run [rundir]           -- alternate directory for runtime files\n"
	"\t-db [dbdir]             -- alternate directory for the internal database\n"
	"\t-mod [moddir]           -- alternate directory for modules\n"
	"\t-htdocs [htdocsdir]     -- alternate directory for htdocs\n"
	"\t-scripts [scriptsdir]   -- alternate directory for scripts\n"
	"\t-temp [directory]       -- alternate directory for temporary files\n"
	"\t-grammar [directory]    -- alternate directory for grammar files\n"
	"\t-certs [directory]      -- alternate directory for certificates\n"
	"\t-recordings [directory] -- alternate directory for recordings\n"
	"\t-storage [directory]    -- alternate directory for voicemail storage\n"
	"\t-cache [directory]      -- alternate directory for cache files\n"
	"\t-sounds [directory]     -- alternate directory for sound files\n";


/**
 * Check if value string starts with "-"
 */
static switch_bool_t is_option(const char *p)
{
	/* skip whitespaces */
	// Char("9") tab(水平制表符)
	// Char("10") 换行
	// Char("11") tab(垂直制表符)
	// Char("13") 回车 chr(13)&chr(10) 回车和换行的组合
	// Char("32") 空格 SPACE
	while ((*p == 13) || (*p == 10) || (*p == 9) || (*p == 32) || (*p == 11)) p++;
	return (p[0] == '-');
}

// 程序入口
/* the main application entry point */
int main(int argc, char *argv[])
{
	char pid_path[PATH_MAX] = "";	/* full path to the pid file */
	char pid_buffer[32] = "";	/* pid string */
	char old_pid_buffer[32] = { 0 };	/* pid string */
	// typedef 原类型 别名
	switch_size_t pid_len, old_pid_len;
	const char *err = NULL;		/* error value for return from freeswitch initialization */
#ifndef WIN32
	// 是否不允许Fork新进程
	switch_bool_t nf = SWITCH_FALSE;				/* TRUE if we are running in nofork mode */
	// 系统完全初始化完毕之后再退出父进程
	// 保证nc后的程序初始化完毕
	switch_bool_t do_wait = SWITCH_FALSE;
	// 启动后以非root用户user身份运行
	char *runas_user = NULL;
	// 启动后以非root组group身份运行
	char *runas_group = NULL;
	// -reincarnate  restart the switch on an uncontrolled exit worker意外退出，master重新拉起
	// -reincarnate-reexec run execv on a restart 重新拉起时重新执行参数配置
	switch_bool_t reincarnate = SWITCH_FALSE, reincarnate_reexec = SWITCH_FALSE;
	// TODO 主子进程
	int fds[2] = { 0, 0 };
#else
	const switch_bool_t nf = SWITCH_TRUE;		     /* On Windows, force nf to true*/
	switch_bool_t win32_service = SWITCH_FALSE;
#endif
	// 后台运行
	switch_bool_t nc = SWITCH_FALSE;				/* TRUE if we are running in noconsole mode */
	// -elegant-term TODO
	switch_bool_t elegant_term = SWITCH_FALSE;
	pid_t pid = 0;
	int i, x;
	char *opts;
	char opts_str[1024] = "";
	char *local_argv[1024] = { 0 };
	int local_argc = argc;
	char *arg_argv[128] = { 0 };
	// alt_dirs 自定义配置文件夹标识，以下配置使其+1 -conf -log -db
	// alt_base 配置了prefix directory文件夹标识 --base， log_set 配置了log文件夹标识，run_set 配置了runtime文件夹标识，
	// do_kill 关闭标识
	int alt_dirs = 0, alt_base = 0, log_set = 0, run_set = 0, do_kill = 0;
	// 优先级设置
	// hp rp 2
	// lp -1
	// np 1
	int priority = 0;
#if (defined(__SVR4) && defined(__sun))
	switch_core_flag_t flags = SCF_USE_SQL | SCF_CALIBRATE_CLOCK | SCF_USE_CLOCK_RT;
#else
	// SCF_USE_SQL = 1<<0 , SCF_USE_AUTO_NAT = 1<<7 , SCF_USE_NAT_MAPPING = 1<<16 , SCF_CALIBRATE_CLOCK = 1<<9 ,
	// SCF_USE_CLOCK_RT = 1<<17
	switch_core_flag_t flags = SCF_USE_SQL | SCF_USE_AUTO_NAT | SCF_USE_NAT_MAPPING | SCF_CALIBRATE_CLOCK | SCF_USE_CLOCK_RT;
#endif
	int ret = 0;
	switch_status_t destroy_status;
	switch_file_t *fd;
	switch_memory_pool_t *pool = NULL;
#ifdef HAVE_SETRLIMIT
#ifndef FS_64BIT
	// 允许浪费内存地址空间，FreeSWITCH仅需240KB的栈空间，你可以使用ulimit -s
	// 240限制栈空间使用，或使用该选项忽略警告信息
	switch_bool_t waste = SWITCH_FALSE;
#endif
#endif

	for (x = 0; x < argc; x++) {
		local_argv[x] = argv[x];
	}

	// 获取环境变量中的配置
	if ((opts = getenv("FREESWITCH_OPTS"))) {
		strncpy(opts_str, opts, sizeof(opts_str) - 1);
		i = switch_separate_string(opts_str, ' ', arg_argv, (sizeof(arg_argv) / sizeof(arg_argv[0])));
		for (x = 0; x < i; x++) {
			local_argv[local_argc++] = arg_argv[x];
		}
	}

	if (local_argv[0] && strstr(local_argv[0], "freeswitchd")) {
		nc = SWITCH_TRUE;
	}

	for (x = 1; x < local_argc; x++) {

		if (switch_strlen_zero(local_argv[x]))
			continue;

		if (!strcmp(local_argv[x], "-help") || !strcmp(local_argv[x], "-h") || !strcmp(local_argv[x], "-?")) {
			printf("%s\n", usage);
			exit(EXIT_SUCCESS);
		}
#ifdef WIN32
		if (x == 1 && !strcmp(local_argv[x], "-service")) {
			/* New installs will always have the service name specified, but keep a default for compat */
			x++;
			if (!switch_strlen_zero(local_argv[x])) {
				switch_copy_string(service_name, local_argv[x], SERVICENAME_MAXLEN);
			} else {
				switch_copy_string(service_name, SERVICENAME_DEFAULT, SERVICENAME_MAXLEN);
			}

			win32_service = SWITCH_TRUE;
			continue;
		}

		else if (x == 1 && !strcmp(local_argv[x], "-install")) {
			char servicePath[PATH_MAX];
			char exePath[PATH_MAX];
			SC_HANDLE hService;
			SC_HANDLE hSCManager;
			SERVICE_DESCRIPTION desc;
			desc.lpDescription = "The FreeSWITCH service.";

			x++;
			if (!switch_strlen_zero(local_argv[x])) {
				switch_copy_string(service_name, local_argv[x], SERVICENAME_MAXLEN);
			} else {
				switch_copy_string(service_name, SERVICENAME_DEFAULT, SERVICENAME_MAXLEN);
			}

			GetModuleFileName(NULL, exePath, sizeof(exePath));
			snprintf(servicePath, sizeof(servicePath), "%s -service %s", exePath, service_name);

			/* Perform service installation */

			hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
			if (!hSCManager) {
				fprintf(stderr, "Could not open service manager (%u).\n", GetLastError());
				exit(EXIT_FAILURE);
			}

			hService = CreateService(hSCManager, service_name, service_name, GENERIC_READ | GENERIC_EXECUTE | SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_IGNORE,
						 servicePath, NULL, NULL, NULL, NULL, /* Service start name */ NULL);
			if (!hService) {
				fprintf(stderr, "Error creating freeswitch service (%u).\n", GetLastError());
				CloseServiceHandle(hSCManager);
				exit(EXIT_FAILURE);
			}

			/* Set desc, and don't care if it succeeds */
			if (!ChangeServiceConfig2(hService, SERVICE_CONFIG_DESCRIPTION, &desc)) {
				fprintf(stderr, "FreeSWITCH installed, but could not set the service description (%u).\n", GetLastError());
			}

			CloseServiceHandle(hService);
			CloseServiceHandle(hSCManager);
			exit(EXIT_SUCCESS);
		}

		else if (x == 1 && !strcmp(local_argv[x], "-uninstall")) {
			SC_HANDLE hService;
			SC_HANDLE hSCManager;
			BOOL deleted;

			x++;
			if (!switch_strlen_zero(local_argv[x])) {
				switch_copy_string(service_name, local_argv[x], SERVICENAME_MAXLEN);
			} else {
				switch_copy_string(service_name, SERVICENAME_DEFAULT, SERVICENAME_MAXLEN);
			}

			/* Do the uninstallation */
			hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
			if (!hSCManager) {
				fprintf(stderr, "Could not open service manager (%u).\n", GetLastError());
				exit(EXIT_FAILURE);
			}

			hService = OpenService(hSCManager, service_name, DELETE);
			if (!hService) {
				fprintf(stderr, "Error opening service (%u).\n", GetLastError());
				CloseServiceHandle(hSCManager);
				exit(EXIT_FAILURE);
			}

			/* remove the service! */
			deleted = DeleteService(hService);
			if (!deleted) {
				fprintf(stderr, "Error deleting service (%u).\n", GetLastError());
			}

			CloseServiceHandle(hService);
			CloseServiceHandle(hSCManager);
			exit(deleted ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		else if (!strcmp(local_argv[x], "-monotonic-clock")) {
			flags |= SCF_USE_WIN32_MONOTONIC;
		}
#else
		else if (!strcmp(local_argv[x], "-u")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "Option '%s' requires an argument!\n", local_argv[x - 1]);
				exit(EXIT_FAILURE);
			}
			runas_user = local_argv[x];
		}
		// 用户组
		else if (!strcmp(local_argv[x], "-g")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "Option '%s' requires an argument!\n", local_argv[x - 1]);
				exit(EXIT_FAILURE);
			}
			runas_group = local_argv[x];
		}
		// no forking 不允许Fork新进程
		else if (!strcmp(local_argv[x], "-nf")) {
			nf = SWITCH_TRUE;
		}
		// TODO
		else if (!strcmp(local_argv[x], "-elegant-term")) {
			elegant_term = SWITCH_TRUE;
		} else if (!strcmp(local_argv[x], "-reincarnate")) {
			reincarnate = SWITCH_TRUE;
		} else if (!strcmp(local_argv[x], "-reincarnate-reexec")) {
			reincarnate = SWITCH_TRUE;
			reincarnate_reexec = SWITCH_TRUE;
		}
#endif
#ifdef HAVE_SETRLIMIT
		// 出错时进行内核转储
		else if (!strcmp(local_argv[x], "-core")) {
			struct rlimit rlp;
			memset(&rlp, 0, sizeof(rlp));
			rlp.rlim_cur = RLIM_INFINITY;
			rlp.rlim_max = RLIM_INFINITY;
			setrlimit(RLIMIT_CORE, &rlp);
		}
		// 允许浪费内存地址空间
		else if (!strcmp(local_argv[x], "-waste")) {
#ifndef FS_64BIT
			fprintf(stderr, "WARNING: Wasting up to 8 megs of memory per thread.\n");
			sleep(2);
			waste = SWITCH_TRUE;
#endif
		}
		// TODO
		else if (!strcmp(local_argv[x], "-no-auto-stack")) {
#ifndef FS_64BIT
			waste = SWITCH_TRUE;
#endif
		}
#endif
		else if (!strcmp(local_argv[x], "-version")) {
			fprintf(stdout, "FreeSWITCH version: %s (%s)\n", switch_version_full(), switch_version_revision_human());
			exit(EXIT_SUCCESS);
		}
		// 开启高优先级（实时）设置
		else if (!strcmp(local_argv[x], "-hp") || !strcmp(local_argv[x], "-rp")) {
			priority = 2;
		}
		// 开启低优先级设置
		else if (!strcmp(local_argv[x], "-lp")) {
			priority = -1;
		}
		// 普通优先级
		else if (!strcmp(local_argv[x], "-np")) {
			priority = 1;
		}
		// donot use SQL 1<<0
		// 不使用SQL，show channels类的命令将不能显示结果
		else if (!strcmp(local_argv[x], "-nosql")) {
			flags &= ~SCF_USE_SQL;
		}
		// disable auto nat detection  1<<7
		// 如果路由器支持uPnP或NAT-PMP，则FreeSWITCH可以自动解决NAT穿越问题。如果路由器不支持，则该选项可以使启动更快。
		else if (!strcmp(local_argv[x], "-nonat")) {
			flags &= ~SCF_USE_AUTO_NAT;
		}
		// disable auto nat port mapping TODO 1<<16
		else if (!strcmp(local_argv[x], "-nonatmap")) {
			flags &= ~SCF_USE_NAT_MAPPING;
		}
		// 更精确的时钟，可能会更精确，但对系统要求更高 1<<10
		else if (!strcmp(local_argv[x], "-heavy-timer")) {
			flags |= SCF_USE_HEAVY_TIMING;
		}
		// 关闭实时时钟 1<<11
		else if (!strcmp(local_argv[x], "-nort")) {
			flags &= ~SCF_USE_CLOCK_RT;
		}
		// 关闭时钟核准 1<<9
		// FreeSWITCH理想的运行环境是1000Hz的内核时钟。如果你在内核时钟小于1000Hz或在虚拟机上，可以尝试关闭该选项
		else if (!strcmp(local_argv[x], "-nocal")) {
			flags &= ~SCF_CALIBRATE_CLOCK;
		}
		// 在valgrind下运行，调试内存泄漏时使用 1<<4
		// valgrind @link https://www.jianshu.com/p/5a31d9aa1be2
		else if (!strcmp(local_argv[x], "-vg")) {
			flags |= SCF_VG;
		}
		// 关闭FreeSWITCH，它会在run目录中查找PID文件
		else if (!strcmp(local_argv[x], "-stop")) {
			do_kill = SWITCH_TRUE;
		}
		// no console 启动到后台模式，没有控制台
		else if (!strcmp(local_argv[x], "-nc")) {
			nc = SWITCH_TRUE;
		}
#ifndef WIN32
		// 后台模式，等待系统完全初始化完毕之后再退出父进程，隐含“-nc”选项
		else if (!strcmp(local_argv[x], "-ncwait")) {
			nc = SWITCH_TRUE;
			do_wait = SWITCH_TRUE;
		}
#endif
		// 启动到控制台，默认Options to control location of files
		else if (!strcmp(local_argv[x], "-c")) {
			nc = SWITCH_FALSE;
		}
		// 指定其他的配置文件所在目录，需与-log、-db合用
		else if (!strcmp(local_argv[x], "-conf")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -conf you must specify a config directory\n");
				return 255;
			}
			// 终止符\/0 所以+1
			SWITCH_GLOBAL_dirs.conf_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.conf_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.conf_dir, local_argv[x]);
			alt_dirs++;
		}
		// 指定其他模块目录
		else if (!strcmp(local_argv[x], "-mod")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -mod you must specify a module directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.mod_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.mod_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.mod_dir, local_argv[x]);
		}
		// 指定其他的日志目录
		else if (!strcmp(local_argv[x], "-log")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -log you must specify a log directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.log_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.log_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.log_dir, local_argv[x]);
			alt_dirs++;
			log_set = SWITCH_TRUE;
		}
		// 指定其他存放PID文件的运行目录
		else if (!strcmp(local_argv[x], "-run")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -run you must specify a pid directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.run_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.run_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.run_dir, local_argv[x]);
			run_set = SWITCH_TRUE;
		}
		// 指定其他数据库的目录
		else if (!strcmp(local_argv[x], "-db")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -db you must specify a db directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.db_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.db_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.db_dir, local_argv[x]);
			alt_dirs++;
		}
		// 脚本文件夹
		else if (!strcmp(local_argv[x], "-scripts")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -scripts you must specify a scripts directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.script_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.script_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.script_dir, local_argv[x]);
		}
		// 指定其他HTTP根目录 TODO
		else if (!strcmp(local_argv[x], "-htdocs")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -htdocs you must specify a htdocs directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.htdocs_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.htdocs_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.htdocs_dir, local_argv[x]);
		}
		// 指定其他的基准目录，在配置文件中使用$${base}
		else if (!strcmp(local_argv[x], "-base")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -base you must specify a base directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.base_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.base_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.base_dir, local_argv[x]);
			alt_base = 1;
		}
		// 指定其他临时文件目录
		else if (!strcmp(local_argv[x], "-temp")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -temp you must specify a temp directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.temp_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.temp_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.temp_dir, local_argv[x]);
		}
		// 指定其他存储目录（语音信箱等）
		else if (!strcmp(local_argv[x], "-storage")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -storage you must specify a storage directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.storage_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.storage_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.storage_dir, local_argv[x]);
		}
		// 缓存文件夹
		else if (!strcmp(local_argv[x], "-cache")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -cache you must specify a cache directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.cache_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.cache_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.cache_dir, local_argv[x]);
		}
		// 指定其他录音目录
		else if (!strcmp(local_argv[x], "-recordings")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -recordings you must specify a recording directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.recordings_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.recordings_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.recordings_dir, local_argv[x]);
		}
		// 指定其他语法目录
		else if (!strcmp(local_argv[x], "-grammar")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -grammar you must specify a grammar directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.grammar_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.grammar_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.grammar_dir, local_argv[x]);
		}
		// 指定其他SSL证书路径
		else if (!strcmp(local_argv[x], "-certs")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -certs you must specify a certificates directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.certs_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.certs_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.certs_dir, local_argv[x]);
		}
		// 指定其他声音文件目录
		else if (!strcmp(local_argv[x], "-sounds")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -sounds you must specify a sounds directory\n");
				return 255;
			}

			SWITCH_GLOBAL_dirs.sounds_dir = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_dirs.sounds_dir) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_dirs.sounds_dir, local_argv[x]);
		}
		// 主配置文件名
		else if (!strcmp(local_argv[x], "-cfgname")) {
			x++;
			if (switch_strlen_zero(local_argv[x]) || is_option(local_argv[x])) {
				fprintf(stderr, "When using -cfgname you must specify a filename\n");
				return 255;
			}

			SWITCH_GLOBAL_filenames.conf_name = (char *) malloc(strlen(local_argv[x]) + 1);
			if (!SWITCH_GLOBAL_filenames.conf_name) {
				fprintf(stderr, "Allocation error\n");
				return 255;
			}
			strcpy(SWITCH_GLOBAL_filenames.conf_name, local_argv[x]);
		}

		/* Unknown option (always last!) */
		else {
			fprintf(stderr, "Unknown option '%s', see '%s -help' for a list of valid options\n",
				local_argv[x], local_argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	// 配置了-log，未配置-run  run使用log文件夹
	if (log_set && !run_set) {
		SWITCH_GLOBAL_dirs.run_dir = (char *) malloc(strlen(SWITCH_GLOBAL_dirs.log_dir) + 1);
		if (!SWITCH_GLOBAL_dirs.run_dir) {
			fprintf(stderr, "Allocation error\n");
			return 255;
		}
		strcpy(SWITCH_GLOBAL_dirs.run_dir, SWITCH_GLOBAL_dirs.log_dir);
	}
	// -stop 关闭标识
	if (do_kill) {
		return freeswitch_kill_background();
	}
	// 初始化apr模块
	// {@link /freeswitch/libs/apr/misc/unix/start.c}
	if (apr_initialize() != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "FATAL ERROR! Could not initialize APR\n");
		return 255;
	}

	if (alt_dirs && alt_dirs != 3 && !alt_base) {
		fprintf(stderr, "You must specify all or none of -conf, -log, and -db\n");
		return 255;
	}

#ifndef FS_64BIT
#if defined(HAVE_SETRLIMIT) && !defined(__sun)
	// SCF_VG: run under valgrind
	if (!waste && !(flags & SCF_VG)) {
		struct rlimit rlp;

		memset(&rlp, 0, sizeof(rlp));
		getrlimit(RLIMIT_STACK, &rlp);

		if (rlp.rlim_cur != SWITCH_THREAD_STACKSIZE) {
			char buf[1024] = "";
			int i = 0;

			memset(&rlp, 0, sizeof(rlp));
			rlp.rlim_cur = SWITCH_THREAD_STACKSIZE;
			rlp.rlim_max = SWITCH_SYSTEM_THREAD_STACKSIZE;
			setrlimit(RLIMIT_STACK, &rlp);

			apr_terminate();
			if (argv) ret = (int) execv(argv[0], argv);

			for (i = 0; i < argc; i++) {
				switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s ", argv[i]);
			}

			return system(buf);
		}
	}
#endif
#endif
	// 使用handle_SIGILL函数处理非法指令
	signal(SIGILL, handle_SIGILL);
	// 指定函数处理终止请求
	if (elegant_term) {
		signal(SIGTERM, handle_SIGTERM);
	} else {
		signal(SIGTERM, handle_SIGILL);
	}

#ifndef WIN32
	if (do_wait) {
		// pipe 建立fds[1]到fds[0]的单向管道
		// 成功0，失败-1
		// fds会赋值为管道两端的文件描述符
		// pipe fork实现进程通信：https://www.cnblogs.com/MrListening/p/5858358.html
		if (pipe(fds)) {
			fprintf(stderr, "System Error!\n");
			exit(-1);
		}
	}
#endif

	if (nc) {
#ifdef WIN32
		FreeConsole();
#else
		if (!nf) {
			// 切换到子进程及通信建立
			daemonize(do_wait ? fds : NULL);
			// 到此已切换到后台程序执行
		}
#endif
	}
#ifndef WIN32
	// 是否拉起新worker进程
	if (reincarnate)
		// 执行完以下函数，切换到worker进程继续执行
		reincarnate_protect(reincarnate_reexec ? argv : NULL);
#endif

	if (switch_core_set_process_privileges() < 0) {
		return 255;
	}

	switch (priority) {
	case 2:
		// 设置程序优先度-10
		set_realtime_priority();
		break;
	case 1:
		// 设置程序优先度0
		set_normal_priority();
		break;
	case -1:
		// 设置程序优先度19
		set_low_priority();
		break;
	default:
		set_auto_priority();
		break;
	}

	switch_core_setrlimits();


#ifndef WIN32
	// 切换运行账户
	if (runas_user || runas_group) {
		if (change_user_group(runas_user, runas_group) < 0) {
			fprintf(stderr, "Failed to switch user [%s] / group [%s]\n",
				switch_strlen_zero(runas_user)  ? "-" : runas_user,
				switch_strlen_zero(runas_group) ? "-" : runas_group);
			return 255;
		}
	}
#else
	if (win32_service) {
		/* Attempt to start service */
		SERVICE_TABLE_ENTRY dispatchTable[] = {
			{service_name, &service_main}
			,
			{NULL, NULL}
		};
		service_flags = flags; /* copy parsed flags for service startup */

		if (StartServiceCtrlDispatcher(dispatchTable) == 0) {
			/* Not loaded as a service */
			fprintf(stderr, "Error Freeswitch loaded as a console app with -service option\n");
			fprintf(stderr, "To install the service load freeswitch with -install\n");
		}
		exit(EXIT_SUCCESS);
	}
#endif
	// 统一处理文件夹配置
	switch_core_set_globals();

	pid = getpid();

	memset(pid_buffer, 0, sizeof(pid_buffer));
	switch_snprintf(pid_path, sizeof(pid_path), "%s%s%s", SWITCH_GLOBAL_dirs.run_dir, SWITCH_PATH_SEPARATOR, pfile);
	switch_snprintf(pid_buffer, sizeof(pid_buffer), "%d", pid);
	pid_len = strlen(pid_buffer);
	// 创建内存池
	// APR的意思是Apache可移植运行库，是Apache portable Run-time Libraries的缩写
	apr_pool_create(&pool, NULL);
	// SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE | SWITCH_FPROT_UEXECUTE | SWITCH_FPROT_GREAD | SWITCH_FPROT_GEXECUTE
	// 0x0400 | 0x0200| 0x0100 | 0x0040 | 0x0010 = 0x0750
	// 创建新文件夹 权限750
	switch_dir_make_recursive(SWITCH_GLOBAL_dirs.run_dir, SWITCH_DEFAULT_DIR_PERMS, pool);
	// 检查pid文件
	if (switch_file_open(&fd, pid_path, SWITCH_FOPEN_READ, SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE, pool) == SWITCH_STATUS_SUCCESS) {

		old_pid_len = sizeof(old_pid_buffer) -1;
		switch_file_read(fd, old_pid_buffer, &old_pid_len);
		switch_file_close(fd);
	}
	// 打开pid文件
	if (switch_file_open(&fd,
						 pid_path,
						 SWITCH_FOPEN_WRITE | SWITCH_FOPEN_CREATE | SWITCH_FOPEN_TRUNCATE,
						 SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE, pool) != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "Cannot open pid file %s.\n", pid_path);
		return 255;
	}
	// pid文件读锁,允许其他读者进线程对它读，但不能有写者对它操作
	if (switch_file_lock(fd, SWITCH_FLOCK_EXCLUSIVE | SWITCH_FLOCK_NONBLOCK) != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "Cannot lock pid file %s.\n", pid_path);
		old_pid_len = strlen(old_pid_buffer);
		if (strlen(old_pid_buffer)) {
			switch_file_write(fd, old_pid_buffer, &old_pid_len);
		}
		return 255;
	}

	switch_file_write(fd, pid_buffer, &pid_len);

	if (switch_core_init_and_modload(flags, nc ? SWITCH_FALSE : SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "Cannot Initialize [%s]\n", err);
		return 255;
	}

#ifndef WIN32
	if (do_wait) {
		if (fds[1] > -1) {
			int i, v = 1;
			// 子进程通知主进程就绪 {@link daemonize(int *fds)}
			if ((i = write(fds[1], &v, sizeof(v))) < 0) {
				fprintf(stderr, "System Error [%s]\n", strerror(errno));
			} else {
				(void)read(fds[1], &v, sizeof(v));
			}

			shutdown(fds[1], 2);
			close(fds[1]);
			fds[1] = -1;
		}
	}
#endif

	if (nc && nf) {
		signal(SIGINT, handle_SIGILL);
	}

	switch_core_runtime_loop(nc);

	destroy_status = switch_core_destroy();

	switch_file_close(fd);
	apr_pool_destroy(pool);

	if (unlink(pid_path) != 0) {
		fprintf(stderr, "Failed to delete pid file [%s]\n", pid_path);
	}

	if (destroy_status == SWITCH_STATUS_RESTART) {
		char buf[1024] = { 0 };
		int j = 0;

		switch_sleep(1000000);
		if (!argv || !argv[0] || execv(argv[0], argv) == -1) {
			fprintf(stderr, "Restart Failed [%s] resorting to plan b\n", strerror(errno));
			for (j = 0; j < argc; j++) {
				switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s ", argv[j]);
			}
			ret = system(buf);
		}
	}

	return ret;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
