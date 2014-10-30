#ifndef __LIB_API_GPU_H__
#define __LIB_API_GPU_H__

#include <stdint.h>

#define GDEV_IOCTL_OPEN 201
#define GDEV_IOCTL_LAUNCH 202
#define GDEV_IOCTL_SYNC 203
#define GDEV_IOCTL_CLOSE 204

struct rtxGhandle{
    uint32_t dev_id;
    uint32_t vdev_id;
    uint32_t cid;
    void *task;
};

/************************************************************
 * APIs for GPU scheduling.
 ************************************************************/

int rtx_gpu_init(void);
int rtx_gpu_exit(void);
int rtx_gpu_open(struct rtxGhandle **arg, unsigned int dev_id);
int rtx_gpu_launch(struct rtxGhandle **arg);
int rtx_gpu_sync(struct rtxGhandle **arg);
int rtx_gpu_close(struct rtxGhandle **arg);

#endif
