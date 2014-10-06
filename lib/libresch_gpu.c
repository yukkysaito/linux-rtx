/*
 *  libresch_gpu.c: the library for the RESCH gpu
 * 	
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <resch-api.h>
#include "api.h"
#include "api_gpu.h"

#define discard_arg(arg)	asm("" : : "r"(arg))
void *ghandler= NULL;

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

int rtx_gpu_open(void *arg)
{
    ghandler = arg;
    return __rtx_gpu_ioctl(GDEV_IOCTL_OPEN, arg);
}
int rtx_gpu_launch(void *arg)
{
    return __rtx_gpu_ioctl(GDEV_IOCTL_LAUNCH, arg);
}
int rtx_gpu_sync(void *arg)
{
    return __rtx_gpu_ioctl(GDEV_IOCTL_SYNC, arg);
}
int rtx_gpu_close(void *arg)
{
    return __rtx_gpu_ioctl(GDEV_IOCTL_CLOSE, arg);
}

