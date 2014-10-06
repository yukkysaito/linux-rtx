/* resch */
#include <resch-api.h>
#include <resch-config.h>
#include <resch-core.h>

#include <linux/pci.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>

/* interrupt  */
#include <linux/signal.h>
#include <linux/interrupt.h>
#include <linux/irqdesc.h>
#include <linux/irq.h>

/* gpu  */
#include <resch-gpu-core.h>
#include "gpu_proc.h"
#include "nouveau_oclass.h"

struct gdev_device gdev_vds[GDEV_DEVICE_MAX_COUNT];
struct gdev_device phys_ds[GDEV_DEVICE_MAX_COUNT];
int gdev_count = 0;
int gdev_vcount = GDEV_DEVICE_MAX_COUNT;
struct pci_dev *pdev;
struct resch_irq_desc *resch_desc;

struct gdev_sched_entity *sched_entity_ptr[GDEV_CONTEXT_MAX_COUNT];

struct resch_irq_desc {
    struct pci_dev *dev;
    int resch_irq;
    struct irq_desc *ldesc;
    struct nouveau_mc *pmc;
    char *nouveau_name;
    struct nouveau_mc_intr *map_graph;
    irq_handler_t nouveau_handler;
    irq_handler_t resch_handler;
    int sched_flag;
    spinlock_t release_lock;
    struct tasklet_struct *wake_tasklet;
};


void cpu_wq_sleep(struct gdev_sched_entity *se)
{
    struct task_struct *task = se->task;
    struct gdev_device *gdev = se->gdev;
    struct resch_irq_desc *desc = resch_desc;

    spin_lock_irq(&desc->release_lock);
    if(se->wait_cond != 0xDEADBEEF){
	se->wqueue = (wait_queue_head_t *)kmalloc(sizeof(wait_queue_head_t),GFP_KERNEL);
	init_waitqueue_head(se->wqueue);

	se->wait_cond = 0xCAFE;
	se->dl_runtime = current->dl.runtime;
	se->wait_time = sched_clock();
	se->dl_deadline = current->dl.deadline;
	RESCH_G_PRINT("Process GOTO SLEEP Ctx#0x%lx\n",se->ctx);
	wait_event(*se->wqueue, se->wait_cond);
    }else{
	RESCH_G_PRINT("Already fisnihed Ctx#%d\n",se->ctx->cid);
	se->wait_cond = 0x0;
    }
    spin_unlock_irq(&desc->release_lock);
}

void cpu_wq_wakeup(struct gdev_sched_entity *se)
{
    struct task_struct *task = se->task;
    struct gdev_device *gdev = se->gdev;
    struct resch_irq_desc *desc = resch_desc;
    long long delta;
 
    spin_lock_irq(&desc->release_lock);
    if(se->wait_cond == 0xCAFE){
	delta = sched_clock() - se->wait_time;
	if( delta > task->dl.runtime - 1000000)
	    delta = task->dl.runtime - 1000000;

	task->dl.runtime -= delta;
	wake_up(se->wqueue);
	RESCH_G_PRINT("Process Finish! Wakeup Ctx#0x%lx\n",se->ctx);
	kfree(se->wqueue);
	se->wait_cond = 0x0;
    }else{
	se->wait_cond = 0xDEADBEEF;
	RESCH_G_PRINT("Not have sleep it!%d\n",se->ctx->cid);
    }
    spin_unlock_irq(&desc->release_lock);
}

void cpu_wq_wakeup_tasklet(unsigned long arg)
{
    struct gdev_sched_entity *se  = (struct gdev_sched_entity*)arg;
    cpu_wq_wakeup(se);
}

irqreturn_t nouveau_master_intr(int irq, void *arg) 
{
    struct resch_irq_desc *desc = (struct resch_irq_desc*)arg;
    struct nouveau_mc *pmc = desc->pmc;
    struct nouveau_mc_intr *map = desc->map_graph;
    void *priv;
    uint32_t stat, addr, cid, op;
    uint32_t intr;
    struct gdev_sched_entity *se;

    nv_wr32(pmc, 0x00140,0);
    nv_rd32(pmc, 0x00140);

    intr = nv_rd32(pmc, 0x00100);
    if( intr & map->stat){
	priv = nouveau_subdev(pmc, NVDEV_ENGINE_GR);
	if(priv)
	    stat = nv_rd32(priv, 0x400100);
	if(stat & 0x1){
	    RESCH_G_DPRINT("GDEV_INTERRUPT. stat:0x%lx,addr:0x%lx,cid:0x%lx\n");
	    addr = nv_rd32(priv, 0x400704);
	    op =  (addr & 0x00070000) >> 16; /* for operation dscrimination  */
	    cid =  nv_rd32(priv, 0x400708);
	    se = sched_entity_ptr[cid];
	    //tasklet_hi_schedule(desc->wake_tasklet);
	    wake_up_process(se->gdev->sched_com_thread);
	}
    }

    return desc->nouveau_handler(desc->resch_irq, desc->pmc);
}

struct pci_dev* nouveau_intr_init(struct resch_irq_desc *desc)
{
    int ret;
    struct irqaction *irq_act = irq_to_desc(desc->dev->irq)->action;

    desc->resch_irq = desc->dev->irq;
    desc->resch_handler = nouveau_master_intr;

    if(!irq_act)
    	return -ENODEV;

    desc->nouveau_name = irq_act->name;
#if 0
    if(strcmp(desc->nouveau_name, "nouveau")!=0){
	desc->sched_flag = 0xDEAD;
	printk("Not found nouveau interrupt handler!\n");
	return -ENODEV;
    }
#endif
    desc->pmc = irq_act->dev_id;
    desc->nouveau_handler = irq_act->handler;
    desc->map_graph = ((struct nouveau_mc_oclass *)nv_object(desc->pmc)->oclass)->intr;
    
    while(desc->map_graph->unit && desc->map_graph->unit != NVDEV_ENGINE_GR)
	desc->map_graph++; 

    /* release interrupt handler of the nouveau */
    free_irq(desc->resch_irq, desc->pmc);
    
    /* registration of hool interrupt handler */
    request_irq(desc->resch_irq, nouveau_master_intr, IRQF_SHARED, "resch", desc);

    /* initialized tasklet  */
    desc->wake_tasklet = (struct tasklet_struct*)kmalloc(sizeof(struct tasklet_struct), GFP_KERNEL);
    tasklet_init(desc->wake_tasklet, cpu_wq_wakeup_tasklet, desc);

    return desc->dev;
}

void nouveau_intr_exit(struct resch_irq_desc *desc)
{
    request_irq(desc->resch_irq, desc->nouveau_handler, IRQF_SHARED, desc->nouveau_name, desc->pmc);
    free_irq(desc->resch_irq, desc);

}


static int gdev_sched_com_thread(void *data)
{
    struct gdev_device *gdev = (struct gdev_device*)data;

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
static int gdev_sched_mem_thread(void *data)
{
    struct gdev_device *gdev = (struct gdev_device*)data;

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

static void gdev_credit_handler(unsigned long data)
{
    struct task_struct *p = (struct task_struct *)data;
    wake_up_process(p);
}

static int gdev_credit_com_thread(void *data)
{
    struct gdev_device *gdev = (struct gdev_device*)data;
    struct gdev_time now, last, elapse, interval;
    struct timer_list timer;
    unsigned long effective_jiffies;

    RESCH_G_PRINT("Gdev#%d compute reserve running\n", gdev->id);

    setup_timer_on_stack(&timer, gdev_credit_handler, (unsigned long)current);

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

static int gdev_credit_mem_thread(void *data)
{
    struct gdev_device *gdev = (struct gdev_device*)data;
    struct gdev_time now, last, elapse, interval;
    struct timer_list timer;
    unsigned long effective_jiffies;

    RESCH_G_PRINT("Gdev#%d memory reserve running\n", gdev->id);

    setup_timer_on_stack(&timer, gdev_credit_handler, (unsigned long)current);

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

#define ENABLE_CREDIT_THREAD
void gsched_create_scheduler(struct gdev_device *gdev)
{
    struct task_struct *sched_com, *sched_mem;
    struct task_struct *credit_com, *credit_mem;
    char name[64];
    struct gdev_device *phys = gdev->parent;
    struct sched_param sp = {.sched_priority = MAX_RT_PRIO -1 };

    /* create compute and memory scheduler threads. */
    sprintf(name, "gschedc%d", gdev->id);
    sched_com = kthread_create(gdev_sched_com_thread, (void*)gdev, name);
    if (sched_com) {
	sched_setscheduler(sched_com, SCHED_FIFO, &sp);
	wake_up_process(sched_com);
	gdev->sched_com_thread = sched_com;
    }
#ifdef ENABLE_MEM_SCHED
    sprintf(name, "gschedm%d", gdev->id);
    sched_mem = kthread_create(gdev_sched_mem_thread, (void*)gdev, name);
    if (sched_mem) {
	sched_setscheduler(sched_mem, SCHED_FIFO, &sp);
	wake_up_process(sched_mem);
	gdev->sched_mem_thread = sched_mem;
    }
#endif

#ifdef ENABLE_CREDIT_THREAD
    /* create compute and memory credit replenishment threads. */
    sprintf(name, "gcreditc%d", gdev->id);
    credit_com = kthread_create(gdev_credit_com_thread, (void*)gdev, name);
    if (credit_com) {
	sched_setscheduler(credit_com, SCHED_FIFO, &sp);
	wake_up_process(credit_com);
	gdev->credit_com_thread = credit_com;
    }
#endif
#ifdef ENABLE_MEM_SCHED
    sprintf(name, "gcreditm%d", gdev->id);
    credit_mem = kthread_create(gdev_credit_mem_thread, (void*)gdev, name);
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
#ifdef ENABLE_MEM_SCHED
    if (gdev->credit_mem_thread) {
	kthread_stop(gdev->credit_mem_thread);
	gdev->credit_mem_thread = NULL;
    }
#endif

#ifdef ENABLE_CREDIT_THREAD
    printk("credit_thread\n");
    if (gdev->credit_com_thread) {
	kthread_stop(gdev->credit_com_thread);
	gdev->credit_com_thread = NULL;
    }
#endif
#ifdef ENABLE_MEM_SCHED
    if (gdev->sched_mem_thread) {
	kthread_stop(gdev->sched_mem_thread);
	gdev->sched_mem_thread = NULL;
    }
#endif

#if 1
    printk("sched_thread =0x%lx\n",gdev->sched_com_thread);
    if (gdev->sched_com_thread) {
	kthread_stop(gdev->sched_com_thread);
	gdev->sched_com_thread = NULL;
    }
#endif
    schedule_timeout_uninterruptible(usecs_to_jiffies(gdev->period));
}


static void gpu_device_init(struct gdev_device *dev, int id){
    dev->id = id;
    dev->users = 0;
    dev->accessed = 0;
    dev->blocked = 0;
    dev->com_bw = 0;
    dev->mem_bw = 0;
    dev->period = 0;
    dev->com_time = 0;
    dev->mem_time = 0;
    dev->sched_com_thread = NULL;
    dev->sched_mem_thread = NULL;
    dev->credit_com_thread = NULL;
    dev->credit_mem_thread = NULL;
    dev->current_com = NULL;
    dev->current_mem = NULL;
    dev->parent = NULL;
    dev->priv = NULL;
    gdev_time_us(&dev->credit_com, 0);
    gdev_time_us(&dev->credit_mem, 0);
    gdev_list_init(&dev->sched_com_list, NULL);
    gdev_list_init(&dev->sched_mem_list, NULL);
    gdev_list_init(&dev->vas_list, NULL);
    gdev_list_init(&dev->shm_list, NULL);
    gdev_lock_init(&dev->sched_com_lock);
    gdev_lock_init(&dev->sched_mem_lock);
    gdev_lock_init(&dev->vas_lock);
    gdev_lock_init(&dev->global_lock);
    gdev_mutex_init(&dev->shm_mutex);
}


static int gpu_virtual_device_init(struct gdev_device *dev, int id, uint32_t weight, struct gdev_device *phys){

    gpu_device_init(dev, id);
    dev->period = GDEV_PERIOD_DEFAULT;
    dev->parent = phys;
    dev->com_bw = weight;
    dev->mem_bw = weight;
    return 0;
}


void gsched_init(void){

    int i;

    /* create physical device */
    gpu_device_init(&phys_ds[0], 0);

    RESCH_G_PRINT("Found %d physical device(s).\n-Initialize device structure.....", gdev_count);

    /* look at pci devices */
    resch_desc = (struct resch_irq_desc*)kmalloc(sizeof(struct resch_irq_desc), GFP_KERNEL);
    resch_desc->dev = pci_get_class( PCI_CLASS_DISPLAY_VGA << 8, pdev);
    resch_desc->sched_flag = 0;
    gdev_lock_init(&resch_desc->release_lock);

    /* create virtual device  */
    /* create scheduler thread */
    for (i = 0; i< gdev_vcount; i++){
	gpu_virtual_device_init(&gdev_vds[i], i, 100/gdev_vcount, &phys_ds[0]);
	gsched_create_scheduler(&gdev_vds[i]);
    }
    for(i=0;i<GDEV_CONTEXT_MAX_COUNT;i++)
	sched_entity_ptr[i]=NULL;

    RESCH_G_PRINT("Configured %d virtual device(s).\n", gdev_vcount);

    /* create /proc entries */
       gdev_proc_create();
      for (i = 0; i< gdev_vcount; i++){
      	gdev_proc_minor_create(i);
     }



    /* set interrupt handler  */
    if (!nouveau_intr_init(resch_desc))
	gsched_exit();

    /* exit  */
}


void gsched_exit(void){
    int i;
    /*minmor*/
    for (i = 0; i< gdev_vcount; i++){
	printk("goto destroy scheduler #%d\n",i);
	gsched_destroy_scheduler(&gdev_vds[i]);
	printk("end destroy scheduler #%d\n",i);
    }
    if (resch_desc->sched_flag != 0xDEAD)
	nouveau_intr_exit(resch_desc);
    /*unsetnotify*/
       gdev_proc_delete();

}
