#include <resch-gpu-core.h>
#include <resch-api.h>
#include <resch-config.h>
#include <resch-core.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <asm/uaccess.h>


#include <linux/irq.h>
#include <linux/signal.h>
#include <linux/kthread.h>

#if 1
#include "gdev_vsched_band.c"
struct gdev_vsched_policy *gdev_vsched = &gdev_vsched_band;
#else
#include "gdev_vsched_null.c"
struct gdev_vsched_policy *gdev_vsched = &gdev_vsched_null;
#endif
struct gdev_sched_entity* gdev_sched_entity_create(struct gdev_device *gdev, gdev_ctx_t *ctx);

extern struct resch_irq_desc *resch_desc; 

static struct nouveau_cli *cli;
static struct nouveau_drm *drm;

static struct nouveau_channel {
    struct nouveau_cli *cli;
    struct nouveau_drm *drm;

    uint32_t handle;
};

int gsched_ctxcreate(unsigned long arg)
{
    struct gdev_handle *h = (struct gdev_handle*)arg;
    struct gdev_ctx *ctx = h->ctx;
    struct gdev_device *gdev = h->gdev;
    struct gdev_device *dev;
    struct gdev_device *phys;
    uint32_t cid;
    static uint32_t vgid = 0;
    struct gdev_vas *vas = ctx->vas;
    struct nouveau_channel *chan = (struct nouveau_channel *)vas->pvas;

    /* context is sequencial assigned to vGPU  */
    if( vgid >= GDEV_DEVICE_MAX_COUNT){
    	vgid = 0;
    }

    dev = &gdev_vds[vgid];
    phys = dev->parent;
    /* find empty entity  */
    for(cid = 0; cid < GDEV_CONTEXT_MAX_COUNT; cid++){
	if(!sched_entity_ptr[cid])
	    break;
    }

    if(phys){
retry:
	gdev_lock(&phys->global_lock);
	if(phys->users > GDEV_CONTEXT_MAX_COUNT ){/*GDEV_CONTEXT_LIMIT Check*/
		gdev_unlock(&phys->global_lock);
		schedule_timeout(5);
		goto retry;
	}
	phys->users++;
	gdev_unlock(&phys->global_lock);
    }
    dev->users++;

    ctx->cid = cid;
    RESCH_G_PRINT("Opened RESCH_G, CTX#%d, GDEV=0x%lx\n",ctx->cid,dev);
    struct gdev_sched_entity *se = gdev_sched_entity_create(dev, ctx);
    
}

int gsched_launch(unsigned long arg)
{
    struct gdev_handle *h = (struct gdev_handle*)arg;
    struct gdev_ctx *ctx = h->ctx;
    struct gdev_sched_entity *se = sched_entity_ptr[ctx->cid];

    RESCH_G_PRINT("Launch RESCH_G, CTX#%d\n",ctx->cid);
    gdev_schedule_compute(se);

}
//#define DISABLE_RESCH_INTERRUPT
int gsched_sync(unsigned long arg)
{
    struct gdev_handle *h = (struct gdev_handle*)arg;
    struct gdev_ctx *ctx = h->ctx;
    struct gdev_sched_entity *se = sched_entity_ptr[ctx->cid];

#ifndef DISABLE_RESÇH_INTERRUPT
   // cpu_wq_sleep(se);
#else
    struct gdev_device *gdev = &gdev_vds[h->gdev->id];
    wake_up_process(gdev->sched_com_thread);
#endif
}

int gsched_close(unsigned long arg)
{
    struct gdev_handle *h = (struct gdev_handle*)arg;
    struct gdev_ctx *ctx = h->ctx;
    struct gdev_sched_entity *se = sched_entity_ptr[ctx->cid];
    struct gdev_device *dev = se->gdev;
    struct gdev_device *phys = dev->parent;

    gdev_sched_entity_destroy(se);
    sched_entity_ptr[ctx->cid] = NULL;

    if(phys){
retry:
	gdev_lock(&phys->global_lock);
	phys->users--;
	gdev_unlock(&phys->global_lock);
    }
    dev->users--;
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
	se->wait_cond =0;
/*XXX*/
	sched_entity_ptr[ctx->cid] = se;
	return se;
}

/**
 * destroy the scheduling entity.
 */
void gdev_sched_entity_destroy(struct gdev_sched_entity *se)
{
	kfree(se);
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


void gdev_sched_sleep(struct gdev_sched_entity *se)
{
    struct task_struct *task = se->task;
    
    if(task->policy == SCHED_DEADLINE){
	cpu_wq_sleep(se);
    }else
    {	
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
    }
}


int gdev_sched_wakeup(struct gdev_sched_entity *se)
{
    struct task_struct *task = se->task;
    if(task->policy == SCHED_DEADLINE){
	cpu_wq_wakeup(se);
    }
    else{
	if(!wake_up_process(task)) {
	    schedule_timeout_interruptible(1);
	    if(!wake_up_process(task))
		return -EINVAL;
	}
    }
    return 0;
}

void* gdev_current_com_get(struct gdev_device *gdev)
{
    return !gdev->current_com? NULL:(void*)gdev->current_com;
}

void gdev_current_com_set(struct gdev_device *gdev, void *com){
    gdev->current_com = com;
}

void gdev_lock_init(gdev_lock_t *p)
{
    spin_lock_init(&p->lock);
}

void gdev_lock(gdev_lock_t *p)
{
    spin_lock_irq(&p->lock);
}

void gdev_unlock(gdev_lock_t *p)
{
    spin_unlock_irq(&p->lock);
}

void gdev_lock_save(gdev_lock_t *p, unsigned long *pflags)
{
    spin_lock_irqsave(&p->lock, *pflags);
}

void gdev_unlock_restore(gdev_lock_t *p, unsigned long *pflags)
{
    spin_unlock_irqrestore(&p->lock, *pflags);
}

void gdev_lock_nested(gdev_lock_t *p)
{
    spin_lock(&p->lock);
}

void gdev_unlock_nested(gdev_lock_t *p)
{
    spin_unlock(&p->lock);
}

void gdev_mutex_init(struct gdev_mutex *p)
{
    mutex_init(&p->mutex);
}

void gdev_mutex_lock(struct gdev_mutex *p)
{
    mutex_lock(&p->mutex);
}

void gdev_mutex_unlock(struct gdev_mutex *p)
{
    mutex_unlock(&p->mutex);
}

struct gdev_device* gdev_phys_get(struct gdev_device *gdev)
{
    return gdev->parent? gdev->parent:NULL;
}
