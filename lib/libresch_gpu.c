/*
 *  libresch_gpu.c: the library for the RESCH gpu
 * 	
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <resch-api.h>
#include "api.h"
#include "api_gpu.h"

#define discard_arg(arg)	asm("" : : "r"(arg))

struct rtxGhandle **ghandler= NULL;

/**
 * internal function for gpu scheduler core, using ioctl() system call.
 */
static inline int __rtx_gpu_ioctl(unsigned long cmd, unsigned long val)
{
    int fd, ret;

    fd = open(RESCH_DEVNAME, O_RDWR);
    if (fd < 0) {
	printf("Error: failed to access the module!\n");
	return RES_FAULT;
    }
    ret = ioctl(fd, cmd, val);
    close(fd);

    return ret;
}

/* consideration of the interference with the implementation of the CPU cide */
static void gpu_kill_handler(int signum)
{
    discard_arg(signum);
    rtx_gpu_exit();
    kill(0, SIGINT);
}

int rtx_gpu_init(void)
{
    struct sigaction sa_kill;
    int ret;

    ret = rt_init();

    /* reregister the KILL signal. */
    memset(&sa_kill, 0, sizeof(sa_kill));
    sigemptyset(&sa_kill.sa_mask);
    sa_kill.sa_handler = gpu_kill_handler;
    sa_kill.sa_flags = 0;
    sigaction(SIGINT, &sa_kill, NULL); /* */

    return ret;
}

int rtx_gpu_exit(void)
{
    if(ghandler)
	rtx_gpu_close(ghandler);

    return rt_exit();
}

static int Ghandle_init(struct rtxGhandle **arg){

    if (!(*arg)){
	*arg =(struct rtxGhandle*)malloc(sizeof(struct rtxGhandle));

	if (!(*arg))
	    return -1;
	
	ghandler = arg;
	(*arg)->sched_flag = GPU_SCHED_FLAG_INIT;
    }

    return 1;
}

int rtx_gpu_open(struct rtxGhandle **arg, unsigned int dev_id)
{
    if (!Ghandle_init(arg))
	return -ENOMEM;

    (*arg)->dev_id = dev_id;
    (*arg)->sched_flag |= GPU_SCHED_FLAG_OPEN;

    return __rtx_gpu_ioctl(GDEV_IOCTL_OPEN, (unsigned long)*arg);
}

int rtx_gpu_launch(struct rtxGhandle **arg)
{
    if ( !(*arg) )
	return -EINVAL;

    if ( !((*arg)->sched_flag & GPU_SCHED_FLAG_OPEN)){
	fprintf(stderr, "You did not call rtx_gpu_open.\n");
	fprintf(stderr,	"Please call rtx_gpu_open before rtx_gpu_launch!\n");
	return -EINVAL;
    }

    (*arg)->sched_flag &= ~GPU_SCHED_FLAG_SYNCH;
    (*arg)->sched_flag |= GPU_SCHED_FLAG_LAUNCH;

    return __rtx_gpu_ioctl(GDEV_IOCTL_LAUNCH, (unsigned long)*arg);
}

int rtx_gpu_sync(struct rtxGhandle **arg)
{
    if ( !(*arg) )
	return -EINVAL;

    if ( !((*arg)->sched_flag & GPU_SCHED_FLAG_LAUNCH)){
	fprintf(stderr, "You did not call rtx_gpu_launch.\n");
	fprintf(stderr,	"Sync must call after launch!\n");
	return -EINVAL;
    }

    if ( ((*arg)->sched_flag & GPU_SCHED_FLAG_SYNCH)){
	fprintf(stderr, "You already called rtx_gpu_sync.\n");
	fprintf(stderr,	"Sync is only one call per one handle.\n");
	return -EINVAL;
    }

    (*arg)->sched_flag |= GPU_SCHED_FLAG_SYNCH;
    (*arg)->sched_flag &= ~GPU_SCHED_FLAG_LAUNCH;

    return __rtx_gpu_ioctl(GDEV_IOCTL_SYNC, (unsigned long)*arg);
}

int rtx_gpu_close(struct rtxGhandle **arg)
{
    if ( !(*arg) )
	return -EINVAL;

    if ( !((*arg)->sched_flag & GPU_SCHED_FLAG_OPEN)){
	fprintf(stderr, "You did not call rtx_gpu_open\n");
	return -EINVAL;
    }

    int ret = __rtx_gpu_ioctl(GDEV_IOCTL_CLOSE, (unsigned long)*arg);

    free(*arg);
    ghandler = NULL;
    return ret;
}

