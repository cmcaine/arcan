#include "arcan_shmif.h"
#include "arcan_shmif_debugif.h"
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <poll.h>

/*
 * ideally all this would be fork/asynch-signal safe
 */

#define ARCAN_TUI_DYNAMIC
#include "arcan_tui.h"
#include "arcan_tuisym.h"
#include "arcan_tui_listwnd.h"
#include "arcan_tui_bufferwnd.h"

#ifndef COUNT_OF
#define COUNT_OF(x) \
	((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
#endif

/*
 * menu code here is really generic enough that it should be move elsewhere,
 * frameserver/util or a single-header TUI suppl file possibly
 */
struct debug_ctx {
	struct arcan_shmif_cont cont;
	struct tui_context* tui;
	int infd;
	int outfd;
	bool dead;
};

static const char* stat_to_str(struct stat* s)
{
	const char* ret = "unknown";
	switch(s->st_mode & S_IFMT){
	case S_IFIFO:
		ret = "fifo";
	break;
	case S_IFCHR:
		ret = "char";
	break;
	case S_IFDIR:
		ret = "dir";
	break;
	case S_IFREG:
		ret = "file";
	break;
	case S_IFBLK:
		ret = "block";
	break;
	case S_IFSOCK:
		ret = "socket";
	break;
	default:
	break;
	}
	return ret;
}

enum intercept_type {
	INTERCEPT_MITM_PIPE,
	INTERCEPT_MITM_SOCKET,
	INTERCEPT_MAP
};

static int can_intercept(struct stat* s)
{
	int mode = s->st_mode & S_IFMT;
	if (mode & S_IFIFO)
		return INTERCEPT_MITM_PIPE;
	else if (mode & S_IFREG)
		return INTERCEPT_MAP;
	else if (mode & S_IFSOCK)
		return INTERCEPT_MITM_SOCKET;
	return -1;
}

static void fd_to_flags(char buf[static 8], int fd)
{
	buf[7] = '\0';

/* first, cloexec */
	buf[0] = ' ';
	int rv = fcntl(fd, F_GETFD);
	if (-1 == rv){
		buf[0] = '?';
	}
	else if (rv & O_CLOEXEC){
		buf[0] = 'x';
	}

/* now flags */
	buf[1] = ' ';
	rv = fcntl(fd, F_GETFL);
	if (-1 == rv){
		buf[1] = '?';
	}
	else if ((rv & O_NONBLOCK))
	;
	else
		buf[1] = 'b';

	if (rv & O_RDWR){
		buf[2] = 'r';
		buf[3] = 'w';
	}
	else if (rv & O_WRONLY){
		buf[2] = ' ';
		buf[3] = 'w';
	}
	else {
		buf[2] = 'r';
	buf[3] = ' ';
	}
}

static void run_descriptor(struct debug_ctx* dctx, int fdin, int fdout, int type)
{
	uint8_t buf[4096];

	for(;;){
/* first populate */
		arcan_tui_erase_screen(dctx->tui, false);
		arcan_tui_move_to(dctx->tui, 0, 0);
		arcan_tui_printf(dctx->tui, NULL, "Waiting for data...\n");
		arcan_tui_refresh(dctx->tui);

		struct tui_process_res res = arcan_tui_process(&dctx->tui, 1, &fdin, 1, -1);
		if (res.errc == TUI_ERRC_OK){
			if (-1 == arcan_tui_refresh(dctx->tui) && errno == EINVAL)
				break;
		}
	}

/* then swap to bufferwnd: */

/* on swap-out */

	for(;;){
		struct tui_process_res res = arcan_tui_process(&dctx->tui, 1, NULL, 0, -1);
		arcan_tui_refresh(dctx->tui);
	}

	snprintf((char*)buf, 256, "intercept(%d), waiting for data", fdin);

	read(fdin, buf, 4096);


/* alloc the pair */
/* dup source to temporary */

/* switch context to bufferwnd */

/* on 'finish' calls, if it is commit, commit and flush */

/* Not really safe to dup out again, so for now, spawn a thread that takes over
 * the proxying until death. Other interesting options would be intercepting
 * syscalls (again ptrace) and simulate the descriptor operations failing. */

}

static void gen_descriptor_menu(struct debug_ctx* dctx)
{
/* build a large pollset, fire it once and see which ones were valid */
	struct rlimit rlim;
	int lim = 512;
	if (0 == getrlimit(RLIMIT_NOFILE, &rlim))
		lim = rlim.rlim_cur;

/* this is anything but atomic, we run inside an unwitting process, so either
 * we do something nasty like fork+stop-resume parent on collect or just accept
 * the race for now - there seem to be no portable solution for this so go with
 * what we can */
	struct pollfd* set = malloc(sizeof(struct pollfd) * lim);
	for (size_t i = 0; i < lim; i++)
		set[i].fd = i;

	poll(set, lim, 0);

	size_t count = 0;
	for (size_t i = 0; i < lim; i++){
		if (!(set[i].revents & POLLNVAL))
			count++;
	}

	if (count == 0){
		free(set);
		return;
	}

/* convert / stat-test the valid descriptors, and build the final menu list */
	struct tui_list_entry* lents = malloc(sizeof(struct tui_list_entry) * count);
	if (!lents){
		free(set);
		return;
	}

	struct dent {
		struct stat stat;
		int fd;
	}* dents = malloc(sizeof(struct dent) * count);
	if (!dents){
		free(lents);
		free(set);
		return;
	}

	count = 0;
	for (size_t i = 0; i < lim; i++){
		if (set[i].revents & POLLNVAL)
			continue;
		struct tui_list_entry* lent = &lents[count];
		lents[count] = (struct tui_list_entry){
			.tag = count,
/*		.label = "set later" */
/*		.attributes =
 *        CHECKED : already used?
 *        PASSIVE : couldn't be stat:ed
 */
		};

		size_t lbl_len = 256;
		char* lbl_prefix = malloc(lbl_len);
		if (!lbl_prefix)
			continue;

		dents[count].fd = set[i].fd;
		if (-1 == fstat(dents[count].fd, &dents[count].stat)){
			lents[count].attributes |= LIST_PASSIVE;
			snprintf(lbl_prefix, lbl_len, "%d(stat-fail) : %s",
				set[i].fd, strerror(errno));
		}
		else {
			char scratch[8];
			fd_to_flags(scratch, set[i].fd);
			if (-1 == can_intercept(&dents[count].stat))
				lents[count].attributes |= LIST_PASSIVE;

#ifdef __LINUX
			char buf[256];
			snprintf(buf, 256, "/proc/self/fd/%d", set[i].fd);
/* using buf on both arguments should be safe here due to the whole 'need the
 * full path before able to generate output' criteria, but explicitly terminate
 * on truncation */
			int rv = readlink(buf, buf, 255);
			if (-1 == rv){
				snprintf(buf, 256, "error: %s", strerror(errno));
			}
			else
				buf[rv] = '\0';

			snprintf(lbl_prefix, lbl_len, "%4d[%s](%s)\t: %s",
				set[i].fd, scratch, stat_to_str(&dents[count].stat), buf);
#else
			snprintf(lbl_prefix, lbl_len,	"%4d[%s](%s)\t: can't resolve",
				set[i].fd, scratch, stat_to_str(&dents[count].stat));
#endif
		}
		lents[count].label = lbl_prefix;

/* resolve to pathname if possible */
#ifdef F_GETPATH
#endif
		count++;
	}

/* switch to new menu */
	arcan_tui_update_handlers(dctx->tui,
		&(struct tui_cbcfg){}, NULL, sizeof(struct tui_cbcfg));
	arcan_tui_listwnd_setup(dctx->tui, lents, count);
	for(;;){
		struct tui_process_res res = arcan_tui_process(&dctx->tui, 1, NULL, 0, -1);
		if (res.errc == TUI_ERRC_OK){
			if (-1 == arcan_tui_refresh(dctx->tui) && errno == EINVAL)
				break;
		}

/* special treatment for STDIN, STDOUT, STDERR as well as those can go to a
 * tty/pty, meaning that our normal 'check if pipe' won't just work by default */

/* Pipes are 'easy', we can check if the end is read or write and setup the
 * interception accordingly. Sockets have types and are bidirectional, so
 * either we request a new window and use one for the read and one for the
 * write end - as well as getsockopt on type etc. to figure out if the socket
 * can actually be intercepted or not. */
		struct tui_list_entry* ent;
		if (arcan_tui_listwnd_status(dctx->tui, &ent)){
			if (ent){
/* run descriptor, with ent tag as index */
				int icept = can_intercept(&dents[ent->tag].stat);
				run_descriptor(dctx, dents[ent->tag].fd, -1, icept);
			}
			break;
		}
	}

	for (size_t i = 0; i < count; i++){
		free(lents[i].label);
	}

	free(set);
	free(lents);
}

/*
 * debugger and the others are more difficult as we currently need to handover
 * exec into afsrv_terminal in a two phase method where we need to stall the
 * child a little bit while we set prctl(PR_SET_PTRACE, pid, ...).
 */
static void gen_debugger_menu(struct debug_ctx* dctx)
{

}

static void build_process_str(char* dst, size_t dst_sz)
{
/* bufferwnd currently 'misses' a way of taking some in-line formatted string
 * and resolving, the intention was to add that as a tack-on layer and use the
 * offset- lookup coloring to perform that resolution, simple strings for now */
	pid_t cpid = getpid();
	pid_t ppid = getppid();
	snprintf(dst, dst_sz, "PID: %zd Parent: %zd", (ssize_t) cpid, (ssize_t) ppid);
}

static void set_process_window(struct debug_ctx* dctx)
{
/* build a process description string that we periodically update */
	char outbuf[2048];
	build_process_str(outbuf, sizeof(outbuf));
	struct tui_bufferwnd_opts opts = {
		.read_only = true,
		.view_mode = BUFFERWND_VIEW_ASCII,
		.allow_exit = true
	};

	arcan_tui_bufferwnd_setup(dctx->tui,
		(uint8_t*) outbuf, strlen(outbuf)+1, &opts, sizeof(struct tui_bufferwnd_opts));

/* normal event-loop, but with ESCAPE as a 'return' behavior */
	while(arcan_tui_bufferwnd_status(dctx->tui) == 1){
		struct tui_process_res res = arcan_tui_process(&dctx->tui, 1, NULL, 0, -1);
		if (res.errc == TUI_ERRC_OK){
			if (-1 == arcan_tui_refresh(dctx->tui) && errno == EINVAL){
				dctx->dead = true;
				break;
			}
		}
		else{
			dctx->dead = true;
			break;
		}
/* check last- refresh and build process str and call bufferwnd_synch */
	}

/* return the context to normal, dead-flag will propagate and free if set */
	arcan_tui_bufferwnd_release(dctx->tui);
	arcan_tui_update_handlers(dctx->tui,
		&(struct tui_cbcfg){}, NULL, sizeof(struct tui_cbcfg));
}

static void root_menu(struct debug_ctx* dctx)
{
	struct tui_list_entry menu_root[] = {
		{
			.label = "File Descriptors",
			.attributes = LIST_HAS_SUB,
			.tag = 0
		},
		{
			.label = "Debugger",
			.attributes = LIST_HAS_SUB,
			.tag = 1
		},
		{
			.label = "Process Information",
			.attributes = LIST_HAS_SUB,
			.tag = 2,
		}
	};

	while(!dctx->dead){
/* update the handlers so there's no dangling handlertbl+cfg */
		arcan_tui_update_handlers(dctx->tui,
			&(struct tui_cbcfg){}, NULL, sizeof(struct tui_cbcfg));
		arcan_tui_listwnd_setup(dctx->tui, menu_root, COUNT_OF(menu_root));

		for(;;){
			struct tui_process_res res =
				arcan_tui_process(&dctx->tui, 1, NULL, 0, -1);

			if (-1 == arcan_tui_refresh(dctx->tui) && errno == EINVAL)
				return;

			struct tui_list_entry* ent;
			if (arcan_tui_listwnd_status(dctx->tui, &ent)){
				arcan_tui_listwnd_release(dctx->tui);
				arcan_tui_update_handlers(dctx->tui,
					&(struct tui_cbcfg){}, NULL, sizeof(struct tui_cbcfg));

/* this will just chain into a new listwnd setup, and if they cancel
 * we can just repeat the setup - until the dead state has been set */
				if (ent){
					if (ent->tag == 0)
						gen_descriptor_menu(dctx);
					else if (ent->tag == 1)
						gen_debugger_menu(dctx);
					else if (ent->tag == 2)
						set_process_window(dctx);
				}
				break;
			}

		}
	}
}

static void* debug_thread(void* thr)
{
	struct debug_ctx* dctx = thr;

	if (!dctx->tui){
		arcan_shmif_drop(&dctx->cont);
		free(thr);
		return NULL;
	}

	root_menu(dctx);

	arcan_tui_destroy(dctx->tui, NULL);
	free(thr);
	return NULL;
}

bool arcan_shmif_debugint_spawn(struct arcan_shmif_cont* c, void* tuitag)
{
/* make sure we have the TUI functions for the debug thread along with
 * the respective widgets, dynamically load the symbols */
	if (!arcan_tui_setup ||
			!arcan_tui_listwnd_setup ||
			!arcan_tui_bufferwnd_setup
	){
		void* openh = dlopen(
"libarcan_tui."
#ifndef __APPLE__
	"so"
#else
	"dylib"
#endif
		, RTLD_LAZY);
		if (!arcan_tui_dynload(dlsym, openh))
			return false;

		if (!arcan_tui_listwnd_dynload(dlsym, openh))
			return false;

		if (!arcan_tui_bufferwnd_dynload(dlsym, openh))
			return false;
	}

	pthread_t pth;
	pthread_attr_t pthattr;
	pthread_attr_init(&pthattr);
	pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);
	struct tui_settings cfg = arcan_tui_defaults(c, tuitag);
	struct debug_ctx* hgs = malloc(sizeof(struct debug_ctx));
	if (!hgs)
		return false;

	*hgs = (struct debug_ctx){
		.infd = -1,
		.outfd = -1,
		.cont = *c,
		.tui = arcan_tui_setup(c,
			&cfg, &(struct tui_cbcfg){}, sizeof(struct tui_cbcfg))
	};

	if (!hgs->tui){
		free(hgs);
		return false;
	}

	arcan_tui_set_flags(hgs->tui, TUI_HIDE_CURSOR);

	if (-1 == pthread_create(&pth, &pthattr, debug_thread, hgs)){
		free(hgs);
		return false;
	}

	return true;
}
