#ifndef __LIB_API_GPU_H__
#define __LIB_API_GPU_H__

#include <stdint.h>

#define GDEV_IOCTL_OPEN 201
#define GDEV_IOCTL_LAUNCH 202
#define GDEV_IOCTL_SYNC 203
#define GDEV_IOCTL_CLOSE 204

#define GPU_SCHED_FLAG_INIT   0x01
#define GPU_SCHED_FLAG_OPEN   0x02
#define GPU_SCHED_FLAG_LAUNCH 0x04
#define GPU_SCHED_FLAG_SYNCH  0x08
#define GPU_SCHED_FLAG_CLOSE  0x10

struct rtxGhandle{
    uint32_t dev_id;
    uint32_t vdev_id;
    uint32_t cid;
    void *task;
    uint8_t sched_flag;
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
