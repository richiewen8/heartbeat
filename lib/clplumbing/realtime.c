/* $Id: realtime.c,v 1.24 2005/05/18 19:53:01 alan Exp $ */
#include <portability.h>
#include <sys/types.h>
#include <stdlib.h>
/* The BSD's do not use malloc.h directly. */
/* They use stdlib.h instead */
#ifndef BSD
#ifdef HAVE_MALLOC_H
#	include <malloc.h>
#endif
#endif
#include <unistd.h>
#ifdef _POSIX_MEMLOCK
#	include <sys/mman.h>
#endif
#ifdef _POSIX_PRIORITY_SCHEDULING
#	include <sched.h>
#endif
#include <string.h>
#include <clplumbing/cl_log.h>
#include <clplumbing/realtime.h>
#include <clplumbing/uids.h>
#include <time.h>

static gboolean	cl_realtimepermitted = TRUE;
static void cl_rtmalloc_setup(void);

#if defined(SCHED_RR) && defined(_POSIX_PRIORITY_SCHEDULING) && !defined(ON_DARWIN)
#	define DEFAULT_REALTIME	SCHED_RR
#endif

#define HOGRET	0xff
/*
 * Slightly wacko recursive function to touch requested amount
 * of stack so we have it pre-allocated inside our realtime code
 * as per suggestion from mlockall(2)
 */
#ifdef _POSIX_MEMLOCK
static int
cl_stack_hogger(char * inbuf, int kbytes)
{
	char	buf[1024];
	
	if (inbuf == NULL) {
		memset(buf, HOGRET, sizeof(buf));
	}else{
		memcpy(buf, inbuf, sizeof(buf));
	}

	if (kbytes > 0) {
		return cl_stack_hogger(buf, kbytes-1);
	}else{
		return buf[sizeof(buf)-1];
	}
/* #else
	return HOGRET;
*/
}
#endif
/*
 * We do things this way to hopefully defeat "smart" malloc code which
 * handles large mallocs as special cases using mmap().
 */
static void
cl_malloc_hogger(int kbytes)
{
	long	size		= kbytes * 1024;
	int	chunksize	= 1024;
	long	nchunks		= (int)(size / chunksize);
	int	chunkbytes 	= nchunks * sizeof(void *);
	void**	chunks		= malloc(chunkbytes);
	int	j;

	if (chunks == NULL) {
		cl_log(LOG_INFO, "Could not preallocate (%d) bytes" 
		,	chunkbytes);
		return;
	}
	memset(chunks, 0, chunkbytes);

	for (j=0; j < nchunks; ++j) {
		chunks[j] = malloc(chunksize);
		if (chunks[j] == NULL) {
			cl_log(LOG_INFO, "Could not preallocate (%d) bytes" 
		,	chunksize);
		}else{
			memset(chunks[j], 0, chunksize);
		}
	}
	for (j=0; j < nchunks; ++j) {
		if (chunks[j]) {
			free(chunks[j]);
			chunks[j] = NULL;
		}
	}
	free(chunks);
	chunks = NULL;
}

/*
 *	Make us behave like a soft real-time process.
 *	We need scheduling priority and being locked in memory.
 *	If you ask us nicely, we'll even grow the stack and heap
 *	for you before locking you into memory ;-).
 */
void
cl_make_realtime(int spolicy, int priority,  int stackgrowK, int heapgrowK)
{
#ifdef DEFAULT_REALTIME
	struct sched_param	sp;
	int			staticp;
#endif

	if (heapgrowK > 0) {
		cl_malloc_hogger(heapgrowK);
	}

#ifdef _POSIX_MEMLOCK
	if (stackgrowK > 0) {
		int	ret;
		if ((ret=cl_stack_hogger(NULL, stackgrowK)) != HOGRET) {
			cl_log(LOG_INFO, "Stack hogger failed 0x%x"
			,	ret);
		}
	}
#endif
	cl_rtmalloc_setup();

	if (!cl_realtimepermitted) {
		cl_log(LOG_INFO
		,	"Request to set pid %ld to realtime ignored."
		,	(long)getpid());
		return;
	}

#ifdef DEFAULT_REALTIME

	if (spolicy <= 0) {
		spolicy = DEFAULT_REALTIME;
	}

	if (priority <= 0) {
		priority = sched_get_priority_min(spolicy);
	}

	if (priority > sched_get_priority_max(spolicy)) {
		priority = sched_get_priority_max(spolicy);
	}


	if ((staticp=sched_getscheduler(0)) < 0) {
		cl_perror("unable to get scheduler parameters.");
	}else{
		memset(&sp, 0, sizeof(sp));
		sp.sched_priority = priority;

		if (sched_setscheduler(0, spolicy, &sp) < 0) {
			cl_perror("Unable to set scheduler parameters.");
		}
	}
#endif

#ifdef _POSIX_MEMLOCK
	if (mlockall(MCL_CURRENT|MCL_FUTURE) < 0) {
		cl_perror("Unable to lock pid %d in memory", (int) getpid());
	}else{
		cl_log(LOG_INFO, "pid %d locked in memory.", (int) getpid());
	}
#endif
}

void
cl_make_normaltime()
{
#ifdef DEFAULT_REALTIME
	struct sched_param	sp;

	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = sched_get_priority_min(SCHED_OTHER);
	if (sched_setscheduler(0, SCHED_OTHER, &sp) < 0) {
		cl_perror("unable to (re)set scheduler parameters.");
	}
#endif
#ifdef _POSIX_MEMLOCK
	/* Not strictly necessary. */
	munlockall();
#endif
}

void
cl_disable_realtime(void)
{
	cl_realtimepermitted = FALSE;
}

void
cl_enable_realtime(void)
{
	cl_realtimepermitted = TRUE;
}

/* Give up the CPU for a little bit */
/* This is similar to sched_yield() but allows lower prio processes to run */
int
cl_shortsleep(void)
{
	static const struct timespec	req = {0,2000001L};

	return nanosleep(&req, NULL);
}


static int		post_rt_morecore_count = 0;
static unsigned long	init_malloc_arena = 0L;

#ifdef HAVE_MALLINFO
#	define	MALLOC_TOTALSIZE()	(((unsigned long)mallinfo().arena)+((unsigned long)mallinfo().hblkhd))
#else
#	define	MALLOC_TOTALSIZE()	(OL)
#endif



/* Return the number of times we went after more core */
int
cl_nonrealtime_malloc_count(void)
{
	return post_rt_morecore_count;
}
unsigned long
cl_nonrealtime_malloc_size(void)
{
		return (MALLOC_TOTALSIZE() - init_malloc_arena);
}
/* Log the number of times we went after more core */
void
cl_realtime_malloc_check(void)
{
	static	int		lastcount = 0;
	static unsigned long	oldarena = 0UL;

	if (post_rt_morecore_count > lastcount) {
		cl_log(LOG_WARNING
		,	"Performed %d more non-realtime malloc calls."
		,	post_rt_morecore_count - lastcount);
		lastcount = post_rt_morecore_count;
	}
	if (oldarena == 0UL) {
		oldarena = init_malloc_arena;
	}
	if (MALLOC_TOTALSIZE() > oldarena) {
		cl_log(LOG_INFO
		,	"Total non-realtime malloc bytes: %ld"
		,	MALLOC_TOTALSIZE() - init_malloc_arena);
		oldarena = MALLOC_TOTALSIZE();
	}
}

#ifdef HAVE___DEFAULT_MORECORE

static void	(*our_save_morecore_hook)(void) = NULL;
static void	cl_rtmalloc_morecore_fun(void);

static void
cl_rtmalloc_morecore_fun(void)
{
	post_rt_morecore_count++;
	if (our_save_morecore_hook) {
		our_save_morecore_hook();
	}
}
#endif

static void
cl_rtmalloc_setup(void)
{
	static gboolean	inityet = FALSE;
	if (!inityet) {
		init_malloc_arena = MALLOC_TOTALSIZE();
#ifdef HAVE___DEFAULT_MORECORE
		our_save_morecore_hook = __after_morecore_hook;
	 	__after_morecore_hook = cl_rtmalloc_morecore_fun;
		inityet = TRUE;
	}
#endif
}
