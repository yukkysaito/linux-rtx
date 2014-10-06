#ifndef __LIB_API_GPU_H__
#define __LIB_API_GPU_H__

#define GDEV_IOCTL_OPEN 201
#define GDEV_IOCTL_LAUNCH 202
#define GDEV_IOCTL_SYNC 203
#define GDEV_IOCTL_CLOSE 204

/************************************************************
 * APIs for GPU scheduling.
 ************************************************************/

int rtx_gpu_init(void);
int rtx_gpu_exit(void);
int rtx_gpu_open(void *arg);
int rtx_gpu_launch(void *arg);
int rtx_gpu_sync(void *arg);
int rtx_gpu_close(void *arg);

#endif
