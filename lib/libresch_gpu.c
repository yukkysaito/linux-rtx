/*
 *  libresch_gpu.c: the library for the RESCH gpu
 * 	
 */

#include "api_gpu.h"
#include "api.h"
#include <resch-api.h>
#include <resch-config.h>
#include <linux/sched.h>

#define discard_arg(arg)	asm("" : : "r"(arg))
#define gettid() syscall(SYS_gettid)

struct rtxGhandle **ghandler;

int fd = 0; /* module's file discripter */

/**
 * internal function for gpu scheduler core, using ioctl() system call.
 */
static inline int __rtx_gpu_ioctl(unsigned long cmd, unsigned long val)
{
    int ret;

    if(!fd){
	fd = open(RESCH_DEVNAME, O_RDWR);
	if (fd < 0) {
	    printf("Error: failed to access the module!\n");
	    return RES_FAULT;
	}
    }
    ret = ioctl(fd, cmd, val);

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

    return rt_exit();
}

static int Ghandle_init(struct rtxGhandle **arg)
{
    if (!(*arg)) {
	*arg =(struct rtxGhandle*)malloc(sizeof(struct rtxGhandle));
	if (!(*arg))
	    return -ENOMEM;
	
	ghandler = arg;
    }

    return 1;
}

int rtx_gpu_open(struct rtxGhandle **arg, unsigned int dev_id)
{
    int ret;
    struct rtxGhandle *handle;
    
    /* module check*/
    fd = open(RESCH_DEVNAME, O_RDWR);
    if (fd < 0) {
	printf("Error: failed to access the module!: %s\n", strerror(fd));
	return -ENODEV;
    }

    if (Ghandle_init(arg)<0){
	return -ENOMEM;
    }

    handle = *arg;
    handle->dev_id = dev_id;
    handle->sched_flag = GPU_SCHED_FLAG_OPEN ;

    if ((ret = __rtx_gpu_ioctl(GDEV_IOCTL_CTX_CREATE, (unsigned long)handle))<0){
    	return -ENODEV;
    }
	
#ifdef USE_NVIDIA_DRIVER
    return rtx_nvrm_init(arg, dev_id);
#else
    return ret;
#endif

}

int rtx_gpu_launch(struct rtxGhandle **arg)
{
    int ret;
    struct rtxGhandle *handle = *arg; 

    if ( !(handle) )
	return -EINVAL;

    if ( !(handle->sched_flag & GPU_SCHED_FLAG_OPEN)){
	fprintf(stderr, "rtx_gpu_open is not called yet\n");
	fprintf(stderr, "rtx_gpu_open was not called\n");
	fprintf(stderr,	"Please call rtx_gpu_open before rtx_gpu_launch!\n");
	return -EINVAL;
    }

    handle->sched_flag &= ~GPU_SCHED_FLAG_SYNCH;
    handle->sched_flag |= GPU_SCHED_FLAG_LAUNCH;

    ret = __rtx_gpu_ioctl(GDEV_IOCTL_LAUNCH, (unsigned long)handle);
    return ret;
}

int rtx_gpu_notify(struct rtxGhandle **arg)
{
    int ret;
    struct rtxGhandle *handle = *arg; 
    
    if ( !(handle) )
	return -EINVAL;

    if(!(ret = __rtx_gpu_ioctl(GDEV_IOCTL_NOTIFY, (unsigned long)handle)))
	return ret;

#ifdef USE_NVIDIA_DRIVER
    return rtx_nvrm_notify(arg);
#else
    return ret;
#endif
}

int rtx_gpu_sync(struct rtxGhandle **arg)
{
    struct rtxGhandle *handle = *arg; 
    
    if ( !(handle) )
	return -EINVAL;
    
    if ( !((handle)->sched_flag & GPU_SCHED_FLAG_LAUNCH)){
	fprintf(stderr, "rtx_gpu_launch is not called yet\n");
	fprintf(stderr,	"Sync must call after launch!\n");
	return -EINVAL;
    }

    if ( ((handle)->sched_flag & GPU_SCHED_FLAG_SYNCH)){
	fprintf(stderr, "rtx_gpu_sync has already called\n");
	fprintf(stderr,	"Sync is only one call per one handle.\n");
	return -EINVAL;
    }


    handle->sched_flag |= GPU_SCHED_FLAG_SYNCH;
    handle->sched_flag &= ~GPU_SCHED_FLAG_LAUNCH;

    return __rtx_gpu_ioctl(GDEV_IOCTL_SYNC, (unsigned long)handle);
}

int rtx_gpu_close(struct rtxGhandle **arg)
{
    struct rtxGhandle *handle = *arg; 
    
    if ( !handle )
	return -EINVAL;

    if ( !(handle->sched_flag & GPU_SCHED_FLAG_OPEN)){
	fprintf(stderr, "rtx_gpu_open is not called yet\n");
	return -EINVAL;
    }

    int ret = __rtx_gpu_ioctl(GDEV_IOCTL_CLOSE, (unsigned long)handle);

    free(handle);
    if(fd)
	close(fd);
 
    return ret;
}

int rtx_gpu_setcid(struct rtxGhandle **arg, int new_cid)
{
    struct rtxGhandle *handle = *arg;

    if(!fd){
	return -ENODEV;
    }

    if( !(*ghandler) ){
	return -ENODEV;
    }

    return (*ghandler)->cid;
#if 0

    if( !(arg) ){
	__arg = *ghandler;
    }
 
    __arg->setcid = new_cid;

    return __rtx_gpu_ioctl(GDEV_IOCTL_SETCID, (unsigned long)__arg);
#endif
}
