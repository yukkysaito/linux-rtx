#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/signal.h>
#include <linux/interrupt.h>
#include <linux/irqdesc.h>
#include <linux/irq.h>
#include <resch-api.h>
#include <resch-config.h>
#include <resch-core.h>
#include <resch-gpu-core.h>
#include "gpu_proc.h"
#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/kthread.h>

struct gdev_device gdev_vds[GDEV_DEVICE_MAX_COUNT];
struct gdev_device phys[GDEV_DEVICE_MAX_COUNT];
int gdev_count = 0;
int gdev_vcount = GDEV_DEVICE_MAX_COUNT;

struct gdev_sched_entity *sched_entity_ptr[GDEV_CONTEXT_MAX_COUNT];



void gpu_release_deadline(struct gdev_ctx *ctx, int flag){

    struct gdev_sched_entity *se = sched_entity_ptr[ctx->cid];
    long long delta;

    switch(flag){
	case 1:/* WAKEUP  */
	    if(!se->wait_cond){
		delta = sched_clock() - se->wait_time;
		if( delta > se->current_task->dl.runtime - 1000000)
		    delta = se->current_task->dl.runtime - 1000000;

		se->current_task->dl.runtime -= delta;
		RESCH_G_PRINT("Process Finish! Wakeup Ctx#%d\n",se->ctx->cid);
		se->wait_cond = 1;
		kfree(se->wqueue);
	    }
	    break;
	case 2:/* SLEEP */
		se->current_task = current;
		se->wqueue = (wait_queue_head_t *)kmalloc(sizeof(wait_queue_head_t),GFP_KERNEL);
		init_waitqueue_head(se->wqueue);

		se->wait_cond = 0;
		se->dl_runtime = current->dl.runtime;
		se->wait_time = sched_clock();
		se->dl_deadline = current->dl.deadline;
		wait_event(*se->wqueue, se->wait_cond);
		break;
	default:
		break;
    }
}
//#include "gdev_vsched_band.c"
//struct gdev_vsched_policy *gdev_vsched = &gdev_vsched_band;
struct nouveau_ofuncs;
struct nouveau_omthds;

struct nouveau_oclass{
    uint32_t handle;
    struct nouveau_ofuncs * const ofuncs;
    struct novueau_omthds * const omthds;
    struct lock_class_key lock_class_key;
};

struct nouveau_object {
    struct nouveau_oclass *oclass;
    struct nouveau_object *parent;
    struct nouveau_object *engine;
    atomic_t refcount;
    atomic_t usecount;
};


struct nouveau_subdev {
    struct nouveau_object base;
    struct mutex mutex;
    const char *name;
    void __iomem *mmio;
    uint32_t debug;
    uint32_t unit;

    void (*intr)(struct nouveau_subdev *);
};


struct nouveau_mc_intr {
    uint32_t stat;
    uint32_t unit;
};

struct nouveau_mc {
    struct nouveau_subdev base;
    bool use_msi;
    unsigned int irq;
};


struct nouveau_mc_oclass {
    struct nouveau_oclass base;
    const struct nouveau_mc_intr *intr;
    void (*msi_rearm)(struct nouveau_mc *);
};




static inline struct nouveau_subdev *nv_subdev(void *obj){
    return obj;
}

static inline struct nouveau_object *nv_object(void *obj){
    return obj;
}

static inline uint32_t nv_rd32(void *obj, uint32_t addr){
	struct nouveau_subdev *subdev = nv_subdev(obj);
	uint32_t data = ioread32be(subdev->mmio + addr );

	return data;
}
static inline void nv_wr32(void *obj, uint32_t addr, uint32_t data){
	struct nouveau_subdev *subdev = nv_subdev(obj);
	iowrite32be(data, subdev->mmio + addr );
}


irqreturn_t nouveau_master_intr(int irq, void *arg) {

    struct pci_dev *__pdev = (struct pci_dev*)arg;
    uint32_t intr;
    struct irq_desc *desc = irq_to_desc(__pdev->irq);
    struct irqaction *irq_act = desc->action;
    struct nouveau_mc *pmc = irq_act->dev_id;
    struct nouveau_mc_oclass *oclass = (void *)nv_object(pmc)->oclass;
    struct nouveau_mc_intr *map = oclass->intr;

    printk("[RESCH_INTERRUPT]:irq=%d, arg:0x%lx\n", irq, arg);
    printk("--pmc=0x%lx\n", pmc);
    printk("--oclass=0x%lx\n", oclass);
    printk("--map=0x%lx\n", map);
 
    intr = nv_rd32(pmc, 0x100);
    printk("@@intr=0x%lx\n", intr);

    return IRQ_HANDLED;
}

struct pci_dev* nouveau_intr_init(void){

    int ret;
    struct pci_dev *__pdev = pci_get_class( PCI_CLASS_DISPLAY_VGA << 8, __pdev);
    
    printk("[RESCH_%s]:__pdev=0x%lx, irq=%lx\n",__func__, __pdev, __pdev->irq);
    request_irq(__pdev->irq, nouveau_master_intr, IRQF_SHARED, "resch", __pdev);

/*
    struct irq_desc *desc = irq_to_desc(__pdev->irq);
    struct irqaction *irq_act = desc->action;
    struct irqaction *irq_act_next = desc->action->next;
    desc->action->next = NULL;
    desc->action = irq_act;
    desc->action->next = irq_act_next;
*/  
    return __pdev;
}

void nouveau_intr_exit(struct pci_dev* __pdev){
    free_irq(__pdev->irq, __pdev);
}


static int __gdev_sched_com_thread(void *__data)
{
    struct gdev_device *gdev = (struct gdev_device*)__data;

    RESCH_G_PRINT("RESCH_G#%d compute scheduler runnning\n", gdev->id);
    gdev->sched_com_thread = current;

    while (!kthread_should_stop()) {
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
	if (gdev->users)
	    gdev_select_next_compute(gdev);
    }

    return 0;

}

static int __gdev_sched_mem_thread(void *__data)
{
    struct gdev_device *gdev = (struct gdev_device*)__data;

    RESCH_G_PRINT("RESCH_G#%d compute scheduler runnning\n", gdev->id);
    gdev->sched_mem_thread = current;

    while (!kthread_should_stop()) {
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
	if (gdev->users)
	    gdev_select_next_memory(gdev);
    }

    return 0;
}

static void __gdev_credit_handler(unsigned long __data)
{
    struct task_struct *p = (struct task_struct *)__data;
    wake_up_process(p);
}

static int __gdev_credit_com_thread(void *__data)
{
    struct gdev_device *gdev = (struct gdev_device*)__data;
    struct gdev_time now, last, elapse, interval;
    struct timer_list timer;
    unsigned long effective_jiffies;

    RESCH_G_PRINT("Gdev#%d compute reserve running\n", gdev->id);

    setup_timer_on_stack(&timer, __gdev_credit_handler, (unsigned long)current);

    gdev_time_us(&interval, GDEV_UPDATE_INTERVAL);
    gdev_time_stamp(&last);
    effective_jiffies = jiffies;

    while (!kthread_should_stop()) {
	gdev_replenish_credit_compute(gdev);
	mod_timer(&timer, effective_jiffies + usecs_to_jiffies(gdev->period));
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
	effective_jiffies = jiffies;

	gdev_lock(&gdev->sched_com_lock);
	gdev_time_stamp(&now);
	gdev_time_sub(&elapse, &now, &last);
	gdev->com_bw_used = gdev->com_time * 100 / gdev_time_to_us(&elapse);
	if (gdev->com_bw_used > 100)
	    gdev->com_bw_used = 100;
	if (gdev_time_ge(&elapse, &interval)) {
	    gdev->com_time = 0;
	    gdev_time_stamp(&last);
	}
	gdev_unlock(&gdev->sched_com_lock);
    }

    local_irq_enable();
    if (timer_pending(&timer)) {
	del_timer_sync(&timer);
    }
    local_irq_disable();
    destroy_timer_on_stack(&timer);

    return 0;
}

static int __gdev_credit_mem_thread(void *__data)
{
    struct gdev_device *gdev = (struct gdev_device*)__data;
    struct gdev_time now, last, elapse, interval;
    struct timer_list timer;
    unsigned long effective_jiffies;

    RESCH_G_PRINT("Gdev#%d memory reserve running\n", gdev->id);

    setup_timer_on_stack(&timer, __gdev_credit_handler, (unsigned long)current);

    gdev_time_us(&interval, GDEV_UPDATE_INTERVAL);
    gdev_time_stamp(&last);
    effective_jiffies = jiffies;

    while (!kthread_should_stop()) {
	gdev_replenish_credit_memory(gdev);
	mod_timer(&timer, effective_jiffies + usecs_to_jiffies(gdev->period));
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
	effective_jiffies = jiffies;

	gdev_lock(&gdev->sched_mem_lock);
	gdev_time_stamp(&now);
	gdev_time_sub(&elapse, &now, &last);
	gdev->mem_bw_used = gdev->mem_time * 100 / gdev_time_to_us(&elapse);
	if (gdev->mem_bw_used > 100)
	    gdev->mem_bw_used = 100;
	if (gdev_time_ge(&elapse, &interval)) {
	    gdev->mem_time = 0;
	    gdev_time_stamp(&last);
	}
	gdev_unlock(&gdev->sched_mem_lock);
    }

    local_irq_enable();
    if (timer_pending(&timer)) {
	del_timer_sync(&timer);
    }
    local_irq_disable();
    destroy_timer_on_stack(&timer);

    return 0;
}


void gsched_create_scheduler(struct gdev_device *gdev)
{
    struct task_struct *sched_com, *sched_mem;
    struct task_struct *credit_com, *credit_mem;
    char name[64];
    struct gdev_device *phys = gdev->parent;
    struct sched_param sp = {.sched_priority = MAX_RT_PRIO -1 };

    RESCH_G_PRINT("[%s] gdev = 0x%lx\n",__func__, gdev);
    /* create compute and memory scheduler threads. */
    sprintf(name, "gschedc%d", gdev->id);
    sched_com = kthread_create(__gdev_sched_com_thread, (void*)gdev, name);
    if (sched_com) {
	sched_setscheduler(sched_com, SCHED_FIFO, &sp);
	wake_up_process(sched_com);
	gdev->sched_com_thread = sched_com;
    }
#ifdef ENABLE_MEM_SCHED
    sprintf(name, "gschedm%d", gdev->id);
    sched_mem = kthread_create(__gdev_sched_mem_thread, (void*)gdev, name);
    if (sched_mem) {
	sched_setscheduler(sched_mem, SCHED_FIFO, &sp);
	wake_up_process(sched_mem);
	gdev->sched_mem_thread = sched_mem;
    }
#endif
    /* create compute and memory credit replenishment threads. */
    sprintf(name, "gcreditc%d", gdev->id);
    credit_com = kthread_create(__gdev_credit_com_thread, (void*)gdev, name);
    if (credit_com) {
	sched_setscheduler(credit_com, SCHED_FIFO, &sp);
	wake_up_process(credit_com);
	gdev->credit_com_thread = credit_com;
    }
#ifdef ENABLE_MEM_SCHED
    sprintf(name, "gcreditm%d", gdev->id);
    credit_mem = kthread_create(__gdev_credit_mem_thread, (void*)gdev, name);
    if (credit_mem) {
	sched_setscheduler(credit_mem, SCHED_FIFO, &sp);
	wake_up_process(credit_mem);
	gdev->credit_mem_thread = credit_mem;
    }
#endif
    /* set up virtual GPU schedulers, if required. */
    if (phys) {
	gdev_lock(&phys->sched_com_lock);
	gdev_list_init(&gdev->list_entry_com, (void*)gdev);
	gdev_list_add(&gdev->list_entry_com, &phys->sched_com_list);
	gdev_unlock(&phys->sched_com_lock);
	gdev_replenish_credit_compute(gdev);

	gdev_lock(&phys->sched_mem_lock);
	gdev_list_init(&gdev->list_entry_mem, (void*)gdev);
	gdev_list_add(&gdev->list_entry_mem, &phys->sched_mem_list);
	gdev_unlock(&phys->sched_mem_lock);
	gdev_replenish_credit_memory(gdev);
    }
    return 0;
}

void gsched_destroy_scheduler(struct gdev_device *gdev)
{
    RESCH_G_PRINT("[%s] gdev = 0x%lx\n",__func__,gdev);
#ifdef ENABLE_MEM_SCHED
    if (gdev->credit_mem_thread) {
	kthread_stop(gdev->credit_mem_thread);
	gdev->credit_mem_thread = NULL;
    }
#endif
    printk("credit_thread\n");
    if (gdev->credit_com_thread) {
	kthread_stop(gdev->credit_com_thread);
	gdev->credit_com_thread = NULL;
    }
#ifdef ENABLE_MEM_SCHED
    if (gdev->sched_mem_thread) {
	kthread_stop(gdev->sched_mem_thread);
	gdev->sched_mem_thread = NULL;
    }
#endif
#if 1
    printk("sched_thread\n");
    if (gdev->sched_com_thread) {
	kthread_stop(gdev->sched_com_thread);
	gdev->sched_com_thread = NULL;
    }
#endif
    schedule_timeout_uninterruptible(usecs_to_jiffies(gdev->period));
}


static void __gpu_device_init(struct gdev_device *__dev, int id){
    __dev->id = id;
    __dev->users = 0;
    __dev->accessed = 0;
    __dev->blocked = 0;
    __dev->com_bw = 0;
    __dev->mem_bw = 0;
    __dev->period = 0;
    __dev->com_time = 0;
    __dev->mem_time = 0;
    __dev->sched_com_thread = NULL;
    __dev->sched_mem_thread = NULL;
    __dev->credit_com_thread = NULL;
    __dev->credit_mem_thread = NULL;
    __dev->current_com = NULL;
    __dev->current_mem = NULL;
    __dev->parent = NULL;
    __dev->priv = NULL;
    gdev_time_us(&__dev->credit_com, 0);
    gdev_time_us(&__dev->credit_mem, 0);
    gdev_list_init(&__dev->sched_com_list, NULL);
    gdev_list_init(&__dev->sched_mem_list, NULL);
    gdev_list_init(&__dev->vas_list, NULL);
    gdev_list_init(&__dev->shm_list, NULL);
    gdev_lock_init(&__dev->sched_com_lock);
    gdev_lock_init(&__dev->sched_mem_lock);
    gdev_lock_init(&__dev->vas_lock);
    gdev_lock_init(&__dev->global_lock);
    gdev_mutex_init(&__dev->shm_mutex);
}


static int __gpu_virtual_device_init(struct gdev_device *__dev, int id, uint32_t weight, struct gdev_device *__phys){

    __gpu_device_init(__dev, id);
    __dev->period = 10;
    //GDEV_PERIOD_DEFAULT;
    __dev->parent = __phys;
    __dev->com_bw = weight;
    __dev->mem_bw = weight;
    return 0;
}


struct pci_dev *pdev;
void gsched_init(void){

    int i;
    /* create physical device */
    __gpu_device_init(&phys[0], 0);

    RESCH_G_PRINT("Found %d physical device(s).\n-Initialize device structure.....", gdev_count);

    /* create virtual device  */
    for (i = 0; i< gdev_vcount; i++){
	__gpu_virtual_device_init(&gdev_vds[i], i, 100/MAX, &phys[0]);
	gsched_create_scheduler(&gdev_vds[i]);
    }
    for(i=0;i<GDEV_CONTEXT_MAX_COUNT;i++)
	sched_entity_ptr[i]=NULL;

    RESCH_G_PRINT("Configured %d virtual device(s).\n", gdev_vcount);

    /* create /proc entries */
    //    gdev_proc_create();
    //  for (i = 0; i< gdev_vcount; i++){
    //  	gdev_proc_minor_create(i);
    // }

    /* create scheduler thread */
    //gdev_init_scheduler(&gdev_vds[i]);
    /* set interrupt handler  */
    pdev = nouveau_intr_init();

    /* exit  */
}


void gsched_exit(void){

    int i;
    /*minmor*/
    for (i = 0; i< gdev_vcount; i++){
	gsched_destroy_scheduler(&gdev_vds[i]);
    }
    nouveau_intr_exit(pdev);
    /*unsetnotify*/
    //   gdev_proc_delete();


}
