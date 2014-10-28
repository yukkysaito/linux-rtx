/*
 * Copyright (C) Yusuke FUJII
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define SUBCH_NV_COMPUTE 1
#define DEV_NVIDIA_CTL "/dev/nvidiactl"
#define DEV_NVIDIA_NUM "/dev/nvidia"
#include "nvrm_priv.h"
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>


#define GDEV_FENCE_BUF_SIZE 0x10000

struct nvrm_desc{
    struct nvrm_device *dev;
    struct nvrm_context *ctx;
    struct nvrm_channel *chan;
    struct nvrm_vspace *nvas;

    /*context object struct*/
    uint32_t chipset;
    int cid;
    struct nvrm_fifo{
	volatile uint32_t *regs; /* channel control registers. */
	void *ib_bo; /* driver private object. */
	uint32_t *ib_map;
	uint32_t ib_order;
	uint64_t ib_base;
	uint32_t ib_mask;
	uint32_t ib_put;
	uint32_t ib_get;
	void *pb_bo; /* driver private object. */
	uint32_t *pb_map;
	uint32_t pb_order;
	uint64_t pb_base;
	uint32_t pb_mask;
	uint32_t pb_size;
	uint32_t pb_pos;
	uint32_t pb_put;
	uint32_t pb_get;
	void (*space)(struct nvrm_desc *, uint32_t);
	void (*push)(struct nvrm_desc *, uint64_t, uint32_t, int);
	void (*kick)(struct nvrm_desc *);
	void (*update_get)(struct nvrm_desc *);
    } fifo; /* command FIFO queue struct. */
    struct nvrm_fence { /* fence objects (for compute and dma). */
	void *bo;
	uint32_t *map;
	uint64_t addr;
	uint32_t seq;
    }fence;
    struct nvrm_intr {
	void *bo;
	uint64_t addr;
    } notify;

void *pctx;

    void (*begin_ring)(struct nvrm_desc *, int, int, int);
    void (*out_ring)(struct nvrm_desc *,uint32_t );
    void (*fire_ring)(struct nvrm_desc *);

    uint32_t dummy;
};

#include "nvrm_fifo.h"
struct nvrm_desc *nvdesc;

static void nvrm_ctx_new(struct nvrm_desc *desc)
{
    struct nvrm_channel *chan;
    struct nvrm_vspace *nvas = desc->nvas;
    uint32_t chipset = 0xe0;
    uint32_t cls;
    uint32_t ccls;
    uint32_t accls = 0;

    if (chipset < 0x80)
	cls = 0x506f, ccls = 0x50c0;
    else if (chipset < 0xc0)
	cls = 0x826f, ccls = 0x50c0;
    else if (chipset < 0xe0)
	cls = 0x906f, ccls = 0x90c0;
    else if (chipset < 0xf0)
	cls = 0xa06f, ccls = 0xa0c0, accls = 0xa040;
    else
	cls = 0xa16f, ccls = 0xa1c0, accls = 0xa140;


    /* FIFO indirect buffer setup. */
    desc->fifo.ib_order = 10;
    desc->fifo.ib_bo = nvrm_bo_create(nvas, 8 << desc->fifo.ib_order, 1);
    if (!desc->fifo.ib_bo)
	goto fail_ib;
    desc->fifo.ib_map = nvrm_bo_host_map(desc->fifo.ib_bo);
    desc->fifo.ib_base = nvrm_bo_gpu_addr(desc->fifo.ib_bo);
    desc->fifo.ib_mask = (1 << desc->fifo.ib_order) - 1;
    desc->fifo.ib_put = desc->fifo.ib_get = 0;

    /* FIFO push buffer setup. */
    desc->fifo.pb_order = 18;
    desc->fifo.pb_mask = (1 << desc->fifo.pb_order) - 1;
    desc->fifo.pb_size = (1 << desc->fifo.pb_order);
    desc->fifo.pb_bo = nvrm_bo_create(nvas, desc->fifo.pb_size, 1);
    if (!desc->fifo.pb_bo)
	goto fail_pb;
    desc->fifo.pb_map = nvrm_bo_host_map(desc->fifo.pb_bo);
    desc->fifo.pb_base = nvrm_bo_gpu_addr(desc->fifo.pb_bo);
    desc->fifo.pb_pos = desc->fifo.pb_put = desc->fifo.pb_get = 0;
    desc->fifo.push = resch_fifo_push;
    desc->fifo.update_get = resch_fifo_update_get;

    /* FIFO init */
    chan = nvrm_channel_create_ib(nvas, cls, desc->fifo.ib_bo);
    if (!chan)
	goto fail_chan;
    desc->pctx = chan;
#if 1
    /* gr init */
    if (!nvrm_eng_create(chan, NVRM_FIFO_ENG_GRAPH, ccls))
	goto fail_eng;
#endif
#if 0 /* fix this  */
    /* copy init */
    if (accls && !nvrm_eng_create(chan, NVRM_FIFO_ENG_COPY2, accls))
	goto fail_eng;
#endif
#if 1
    /* bring it up */
    if (nvrm_channel_activate(chan))
	goto fail_activate;
#endif
    /* FIFO command queue registers. */
    desc->fifo.regs = nvrm_channel_host_map_regs(chan);
#if 0
    /* fence buffer. */
    desc->fence.bo = nvrm_bo_create(nvas, GDEV_FENCE_BUF_SIZE, 1);
    if (!desc->fence.bo)
	goto fail_fence_alloc;
    desc->fence.map = nvrm_bo_host_map(desc->fence.bo);
    desc->fence.addr = nvrm_bo_gpu_addr(desc->fence.bo);
    desc->fence.seq = 0;
#endif
    /* interrupt buffer. */
    desc->notify.bo = nvrm_bo_create(nvas, 64, 0);
    if (!desc->notify.bo)
	goto fail_notify_alloc;
    desc->notify.addr = nvrm_bo_gpu_addr(desc->notify.bo);

    /* private data */
    desc->pctx = (void *) chan;

    return;

fail_notify_alloc:
fail_desc_alloc:
    nvrm_bo_destroy(desc->fence.bo);
fail_fence_alloc:
fail_activate:
fail_eng:
    nvrm_channel_destroy(chan);
fail_chan:
    nvrm_bo_destroy(desc->fifo.pb_bo);
fail_pb:
    nvrm_bo_destroy(desc->fifo.ib_bo);
fail_ib:
    free(desc);
fail_ctx:
    return NULL;
}

static int nvrm_open_file(const char *fname) {
    int res = open(fname, O_RDWR);
    if (res < 0)
	return res;
    if (fcntl(res, F_SETFD, FD_CLOEXEC) < 0) {
	close(res);
	return -1;
    }
    return res;
}

static struct nvrm_context* nvrm_open(int minor)
{
    int major, max = 0;
int i;
    /*nvrm open*/
    int fd;
    uint32_t gpu_id[NVRM_MAX_DEV];
    struct nvrm_context *ctx = calloc(sizeof *ctx, 1);
    if (!ctx)
	return ctx;

    ctx->fd_ctl = nvrm_open_file(DEV_NVIDIA_CTL);
    if (ctx->fd_ctl < 0) {
	free(ctx);
    }

    if(nvrm_create_cid(ctx)){
	goto fail_context_open; 
    }
    
    printf("cid :0x%x\n",ctx->cid);
   // ctx->cid -=0x10;

    printf("cid :0x%x\n",ctx->cid);
    if (nvrm_mthd_context_list_devices(ctx, ctx->cid, gpu_id)) {
	goto fail_context_open;
    }

    printf("cid :0x%x\n",ctx->cid);
    for ( i = 0; i < NVRM_MAX_DEV; i++) {
	ctx->devs[i].idx = i;
	ctx->devs[i].ctx = ctx;
	ctx->devs[i].gpu_id = gpu_id[i];
    }
    return ctx;
fail_context_open:
    printf("cid :0x%x\n",ctx->cid);
    close(ctx->fd_ctl);
fail_dev_open:
    free(ctx);
    return 0;
}

static struct nvrm_device *nvrm_device_open(struct nvrm_context *ctx){

    int i;


    /* nvrm_device_open  */
    int idx = nvrm_xlat_device(ctx, idx);
    struct nvrm_device *dev = nvdesc->dev = &ctx->devs[idx];

    if(nvrm_mthd_context_enable_device(ctx, ctx->cid, dev->gpu_id)){
	goto out_enable;
    }
    char buf[20];
   
    snprintf(buf, 20, "/dev/nvidia%d", idx);
   printf("buf :%s\n",buf);
    dev->fd = nvrm_open_file(buf);
    if(dev->fd < 0)
	goto out_open;

    struct nvrm_create_device arg = {
	.idx = idx,
	.cid = ctx->cid,
    };
#if 1
    dev->odev = nvrm_handle_alloc(ctx);
    if (nvrm_ioctl_create(ctx, ctx->cid, dev->odev, NVRM_CLASS_DEVICE_0, &arg))
	goto out_dev;

    dev->osubdev =  nvrm_handle_alloc(ctx);
    if (nvrm_ioctl_create(ctx, dev->odev, dev->osubdev, NVRM_CLASS_SUBDEVICE_0, 0))
	goto out_subdev;
#endif
    dev->ctx = ctx;
    return dev;

out_subdev:
    printf("outsubdev\n");
    nvrm_handle_free(ctx, dev->osubdev);
    nvrm_ioctl_destroy(ctx, ctx->cid, dev->odev);
out_dev:
    printf("outdev\n");
    nvrm_handle_free(ctx, dev->odev);
    close(dev->fd);
out_open:
    printf("outopen\n");
    nvrm_mthd_context_disable_device(ctx, ctx->cid, dev->gpu_id);
out_enable:
    return 0;

}

static struct nvrm_vspace *nvrm_vas_new(struct nvrm_device *dev, uint64_t size)
{
    struct nvrm_vspace *nvas = calloc(sizeof *nvas, 1);
    uint64_t limit = 0;

    if(!nvas)
	return -ENOMEM;

    nvas->ctx = dev->ctx;
    nvas->dev = dev;
    nvas->ovas = nvrm_handle_alloc(nvas->ctx);
    nvas->odma = nvrm_handle_alloc(nvas->ctx);

    if (nvrm_ioctl_create_vspace(nvas->dev, nvas->dev->odev, nvas->ovas, NVRM_CLASS_MEMORY_VM, 0x00010000, &limit, 0))
	goto out_vspace;
    if (nvrm_ioctl_create_dma(nvas->ctx, nvas->ovas,nvas->odma,NVRM_CLASS_DMA_READ, 0x20000000, 0, limit))
	goto out_dma;

    return nvas;

out_dma:
    nvrm_ioctl_destroy(nvas->ctx, nvas->dev->odev, nvas->ovas);
out_vspace:
    nvrm_handle_free(nvas, nvas->odma);
    nvrm_handle_free(nvas, nvas->ovas);
    return 0;
}

/*
 * API 
 * */

int rtx_nvrm_notify(int cid)
{
    if(!nvdesc){
	fprintf(stderr,"Don't initialized nvrm.\n Please call initizalize function\n");
	return -1;
    }
    uint64_t addr = nvdesc->notify.addr;
 printf("[%s]:",__func__);   
 __nvrm_begin_ring_nve4(nvdesc, SUBCH_NV_COMPUTE, 0x110, 1);
 __nvrm_out_ring(nvdesc, 0); /* SERIALIZE */
 __nvrm_begin_ring_nve4(nvdesc, SUBCH_NV_COMPUTE, 0x104, 3);
 __nvrm_out_ring(nvdesc, addr >> 32); /* NOTIFY_HIGH_ADDRESS */
 __nvrm_out_ring(nvdesc, addr); /* NOTIFY_LOW_ADDRESS */
 __nvrm_out_ring(nvdesc, 1); /* WRITTEN_AND_AWAKEN */
 __nvrm_begin_ring_nve4(nvdesc, SUBCH_NV_COMPUTE, 0x100, 1);
 __nvrm_out_ring(nvdesc, 2); /* NOP */

    __nvrm_fire_ring(nvdesc);
#if 0
    nvdesc->begin_ring(nvdesc, SUBCH_NV_COMPUTE, 0x110, 1);
    nvdesc->out_ring(nvdesc, 0);
    nvdesc->begin_ring(nvdesc, SUBCH_NV_COMPUTE, 0x104, 3);
    nvdesc->out_ring(nvdesc, addr >> 32);
    nvdesc->out_ring(nvdesc, 0xffffffff & addr);
    nvdesc->out_ring(nvdesc, 1);
    nvdesc->begin_ring(nvdesc, SUBCH_NV_COMPUTE, 0x100, 1);
    nvdesc->out_ring(nvdesc, cid);

    nvdesc->fire_ring(nvdesc);
#endif
    return 1;
}

int rtx_nvrm_init(void)
{
    nvdesc = (struct nvrm_desc *)malloc(sizeof(struct nvrm_desc));
    memset(nvdesc, 0, sizeof(*nvdesc));

    /* gopen  */
    /** gdev_dev_open  */
    nvdesc->ctx = nvrm_open(0);
    nvdesc->dev = nvrm_device_open(nvdesc->ctx);

    /* gdev_vas_new  */
    nvdesc->nvas = nvrm_vas_new(nvdesc->dev, 0x1000000);
    nvrm_ctx_new(nvdesc);
    nvdesc->chipset = 0xe0;
    switch (nvdesc->chipset & 0x1f0){
	case 0xc0:
	    nvdesc->begin_ring = __nvrm_begin_ring_nvc0;
	    break;
	case 0xe0:
	case 0xf0:
	    nvdesc->begin_ring = __nvrm_begin_ring_nve4;
	    break;
	default:
	    fprintf(stderr,"Don't support your card\n");
	    goto chipset_err;
    }
    nvdesc->out_ring = __nvrm_out_ring;
    nvdesc->fire_ring = __nvrm_fire_ring;

    int i;
    for(i=0;i<128/4; i++)
	__nvrm_out_ring(nvdesc, 0);
    __nvrm_fire_ring(nvdesc);
    
    __nvrm_begin_ring_nve4(nvdesc, SUBCH_NV_COMPUTE, 0, 1);
    __nvrm_out_ring(nvdesc, 0xa0c0);

    __nvrm_begin_ring_nve4(nvdesc, SUBCH_NV_COMPUTE, 0x110, 1);
    __nvrm_out_ring(nvdesc, 0);

    __nvrm_begin_ring_nve4(nvdesc, SUBCH_NV_COMPUTE, 0x310, 1);
    __nvrm_out_ring(nvdesc, 0x300);

    __nvrm_fire_ring(nvdesc);

    return 1;

chipset_err:
    return 0;

}

