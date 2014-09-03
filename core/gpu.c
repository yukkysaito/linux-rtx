#include <resch-gpu-core.h>
#include <resch-api.h>
#include <resch-config.h>
#include <resch-core.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

#include "gdev_vsched_band.c"
struct gdev_vsched_policy *gdev_vsched = &gdev_vsched_band;

struct gdev_sched_entity* gdev_sched_entity_create(struct gdev_device *gdev, gdev_ctx_t *ctx);

  
struct nouveau_cli *cli;
struct nouveau_drm *drm;

struct nouveau_channel {
    struct nouveau_cli *cli;
    struct nouveau_drm *drm;

    uint32_t handle;
    /* syouryaku */
};

int gsched_ctxcreate(unsigned long __arg)
{

    struct gdev_handle *h = (struct gdev_handle*)__arg;
    struct gdev_ctx *__ctx = h->ctx;
    struct gdev_device *gdev = h->gdev;
    struct gdev_device *__dev = &gdev_vds[gdev->id];
    struct gdev_device *__phys = __dev->parent;
    uint32_t cid;

    struct gdev_vas *vas = __ctx->vas;
    struct nouveau_channel *chan = (struct nouveau_channel *)vas->pvas;

    for(cid = 0; cid < GDEV_CONTEXT_MAX_COUNT; cid++){
	if(!sched_entity_ptr[cid])
	    break;
    }
    //cid = chan->handle & 0xffff;
    printk("[%s]cid =%d \n",__func__, cid);

    if(__phys){
retry:
	gdev_lock(&__phys->global_lock);
	if(__phys->users > 30 ){/*GDEV_CONTEXT_LIMIT Check*/
		gdev_unlock(&__phys->global_lock);
		schedule_timeout(5);
		goto retry;
	}
	__phys->users++;
	gdev_unlock(&__phys->global_lock);
    }
    __dev->users++;

    __ctx->cid = cid;
    RESCH_G_PRINT("Opened RESCH_G, CTX#%d, GDEV=0x%lx\n",__ctx->cid,__dev);
    struct gdev_sched_entity *__se = gdev_sched_entity_create(__dev, __ctx);
    
}

int gsched_launch(unsigned long __arg)
{
    struct gdev_handle *h = (struct gdev_handle*)__arg;
    struct gdev_ctx *__ctx = h->ctx;
    struct gdev_sched_entity *__se = sched_entity_ptr[__ctx->cid];
    RESCH_G_PRINT("Launch RESCH_G, CTX#%d\n",__ctx->cid);
    RESCH_G_PRINT("--goto schedule\n");

    gdev_schedule_compute(__se);

}

int gsched_sync(unsigned long __arg)
{
    struct gdev_handle *h = (struct gdev_handle*)__arg;
    struct gdev_ctx *__ctx = h->ctx;
    struct gdev_sched_entity *__se = sched_entity_ptr[__ctx->cid];
    struct gdev_device *gdev = &gdev_vds[h->gdev->id];

    wake_up_process(gdev->sched_com_thread);
}

/**
 * create a new scheduling entity.
 */
struct gdev_sched_entity* gdev_sched_entity_create(struct gdev_device *gdev, gdev_ctx_t *ctx)
{
	struct gdev_sched_entity *se;
/*
	if (!(se= gdev_sched_entity_alloc(sizeof(*se))))
		return NULL;
*/
	se = (struct gdev_sched_entity*)kmalloc(sizeof(*se),GFP_KERNEL);

	/* set up the scheduling entity. */
	se->gdev = gdev;
	se->task = current;
	se->ctx = ctx;
	se->prio = 0;
	se->rt_prio = 0;
	se->launch_instances = 0;
	se->memcpy_instances = 0;
	gdev_list_init(&se->list_entry_com, (void*)se);
	gdev_list_init(&se->list_entry_mem, (void*)se);
	gdev_time_us(&se->last_tick_com, 0);
	gdev_time_us(&se->last_tick_mem, 0);
/*XXX*/
	sched_entity_ptr[ctx->cid] = se;
	return se;
}

/**
 * destroy the scheduling entity.
 */
void gdev_sched_entity_destroy(struct gdev_sched_entity *se)
{
//	free(se);
}

/**
 * insert the scheduling entity to the priority-ordered compute list.
 * gdev->sched_com_lock must be locked.
 */
 void __gdev_enqueue_compute(struct gdev_device *gdev, struct gdev_sched_entity *se)
{
	struct gdev_sched_entity *p;

	gdev_list_for_each (p, &gdev->sched_com_list, list_entry_com) {
		if (se->prio > p->prio) {
			gdev_list_add_prev(&se->list_entry_com, &p->list_entry_com);
			break;
		}
	}
	if (gdev_list_empty(&se->list_entry_com))
		gdev_list_add_tail(&se->list_entry_com, &gdev->sched_com_list);
}

/**
 * delete the scheduling entity from the priority-ordered compute list.
 * gdev->sched_com_lock must be locked.
 */
 void __gdev_dequeue_compute(struct gdev_sched_entity *se)
{
	gdev_list_del(&se->list_entry_com);
}

/**
 * insert the scheduling entity to the priority-ordered memory list.
 * gdev->sched_mem_lock must be locked.
 */
 void __gdev_enqueue_memory(struct gdev_device *gdev, struct gdev_sched_entity *se)
{
	struct gdev_sched_entity *p;

	gdev_list_for_each (p, &gdev->sched_mem_list, list_entry_mem) {
		if (se->prio > p->prio) {
			gdev_list_add_prev(&se->list_entry_mem, &p->list_entry_mem);
			break;
		}
	}
	if (gdev_list_empty(&se->list_entry_mem))
		gdev_list_add_tail(&se->list_entry_mem, &gdev->sched_mem_list);
}

/**
 * delete the scheduling entity from the priority-ordered memory list.
 * gdev->sched_mem_lock must be locked.
 */
 void __gdev_dequeue_memory(struct gdev_sched_entity *se)
{
	gdev_list_del(&se->list_entry_mem);
}


void gdev_sched_sleep(void){


}

void gdev_sched_wakeup(void){

}

void* gdev_current_com_get(struct gdev_device *gdev)
{
    return !gdev->current_com? NULL:(void*)gdev->current_com;
}

void gdev_current_com_set(struct gdev_device *gdev, void *com){
    gdev->current_com = com;
}

void gdev_lock_init(gdev_lock_t *__p)
{
	spin_lock_init(&__p->lock);
}

void gdev_lock(gdev_lock_t *__p)
{
    spin_lock_irq(&__p->lock);
}

void gdev_unlock(gdev_lock_t *__p)
{
    spin_unlock_irq(&__p->lock);
}

void gdev_lock_save(gdev_lock_t *__p, unsigned long *__pflags)
{
    spin_lock_irqsave(&__p->lock, *__pflags);
}

void gdev_unlock_restore(gdev_lock_t *__p, unsigned long *__pflags)
{
    spin_unlock_irqrestore(&__p->lock, *__pflags);
}

void gdev_lock_nested(gdev_lock_t *__p)
{
    spin_lock(&__p->lock);
}

void gdev_unlock_nested(gdev_lock_t *__p)
{
    spin_unlock(&__p->lock);
}

void gdev_mutex_init(struct gdev_mutex *__p)
{
    mutex_init(&__p->mutex);
}

void gdev_mutex_lock(struct gdev_mutex *__p)
{
    mutex_lock(&__p->mutex);
}

void gdev_mutex_unlock(struct gdev_mutex *__p)
{
    mutex_unlock(&__p->mutex);
}

struct gdev_device* gdev_phys_get(struct gdev_device *gdev)
{
	return gdev->parent? gdev->parent:NULL;
}
