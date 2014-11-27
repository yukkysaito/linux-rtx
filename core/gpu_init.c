/* resch */
#include <resch-api.h>
#include <resch-config.h>
#include <resch-core.h>
#include <linux/pci.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <asm/io.h>

/* interrupt  */
#include <linux/signal.h>

/* gpu  */
#include <resch-gpu-core.h>
#include "gpu_proc.h"

#define NVDEV_ENGINE_GR_INTR    0x1000
#define NVDEV_ENGINE_FIFO_INTR   0x100
#define NVDEV_ENGINE_COPY0_INTR   0x20
#define NVDEV_ENGINE_COPY1_INTR   0x40
#define NVDEV_ENGINE_COPY2_INTR   0x80

#define nv_rd32(addr,offset) ioread32(addr+offset)
#define nv_rd32_native(addr,offset) ioread32be(addr+offset)

#ifdef RESCH_GPU_DEBUG_PRINT
#define nv_wr32(addr,offset, value) do{ \
    printk("[%s]addr:0x%lx,offset:0x%lx\n",__func__,addr,offset);   \
    iowrite32(addr+offset, value); \
}while(0)
#else
#define nv_wr32(addr,offset, value) iowrite32(addr+offset, value);
#endif

struct gdev_device gdev_vds[GDEV_DEVICE_MAX_COUNT];
struct gdev_device phys_ds[GDEV_DEVICE_MAX_COUNT];

int gpu_count=0;
int gpu_vcount = GDEV_DEVICE_MAX_COUNT;

struct resch_irq_desc *resch_desc[GDEV_DEVICE_MAX_COUNT];
struct gdev_sched_entity *sched_entity_ptr[GDEV_CONTEXT_MAX_COUNT];

struct irq_num {
    int num;
    char dev_name[30];
};

struct gdev_list list_entry_irq_head;

extern unsigned long cid_bitmap[100];

const struct irq_num nvc0_irq_num[] ={
    {0x04000000, "ENGINE_DISP" },  /* DISP first, so pageflip timestamps wo    rk. */
    { 0x00000001,"ENGINE_PPP" },
    { 0x00000020,"ENGINE_COPY0" },
    { 0x00000040,"ENGINE_COPY1" },
    { 0x00000080,"ENGINE_COPY2" },
    { 0x00000100,"ENGINE_FIFO" },
    { 0x00001000,"ENGINE_GR" },
    { 0x00002000,"SUBDEV_FB" },
    { 0x00008000,"ENGINE_BSP" },
    { 0x00040000,"SUBDEV_THERM" },
    { 0x00020000,"ENGINE_VP" },
    { 0x00100000,"SUBDEV_TIMER" },
    { 0x00200000,"SUBDEV_GPIO" },      /* PMGR->GPIO */
    { 0x00200000,"SUBDEV_I2C" },       /* PMGR->I2C/AUX */
    { 0x01000000,"SUBDEV_PWR" },
    { 0x02000000,"SUBDEV_LTCG" },
    { 0x08000000,"SUBDEV_FB" },
    { 0x10000000,"SUBDEV_BUS" },
    { 0x40000000,"SUBDEV_IBUS" },
    { 0x80000000,"ENGINE_SW" },
    {0,""},
    {}
};


void sched_thread_wakeup_tasklet(unsigned long arg)
{
    struct gdev_device *gdev = (struct gdev_device *)arg;

    if(gdev->sched_com_thread)
	if(!wake_up_process(gdev->sched_com_thread)) {
	    schedule_timeout_interruptible(1);
	    wake_up_process(gdev->sched_com_thread);
	}
}

irqreturn_t gsched_intr(int irq, void *arg)
{
    struct resch_irq_desc *__desc = (struct resch_irq_desc*)arg;
#if 1 /* nofunction */
    void *priv = __desc->mappings;
    uint32_t intr, stat, addr, cid, op;
    struct gdev_sched_entity *se;
    int i=0;
    static int count=0;
int print_stat = 0;

    atomic_inc(&__desc->intr_flag);

    intr = nv_rd32(priv, 0x00100);
    print_stat = nv_rd32(priv, 0x00100);

#if 0 /*for detailed debugging*/
    if(intr  != 0x100000)
    {   printk("[%s]:0x%lx(",__func__,print_stat);
    while(print_stat){
	if(print_stat & nvc0_irq_num[i].num){
	    print_stat &= ~nvc0_irq_num[i].num;
	    printk("%s ||",nvc0_irq_num[i].dev_name);
	}
	i++;
    }
    printk(")\n");
}
#endif
retry:
    if (intr & NVDEV_ENGINE_GR_INTR){
	stat = nv_rd32(priv, 0x400100);
	if(stat & 0x3){
	    addr = nv_rd32(priv, 0x400704);
	    op = (addr & 0x00007000) >> 16;
	    cid = nv_rd32(priv, 0x400708);
	    se = sched_entity_ptr[cid];
	    if(stat & 0x1){
		if( atomic_read(&se->launch_count) ){
		    atomic_dec(&se->launch_count);
		    RESCH_G_DPRINT("GDEV_COMPUTE_INTERRUPT! addr:0x%08lx,stat:0x%08lx,cid:0x%08lx\n", addr,stat,cid);
		    gdev_list_add_tail(&se->list_entry_irq, &list_entry_irq_head);

		    if(!wake_up_process(se->gdev->sched_com_thread)){
			tasklet_schedule(se->gdev->wakeup_tasklet_t);
			RESCH_G_PRINT("Failed wakeup sched_com_thread\ncall wakeup_tasklet\n");
		    }
		
		}else{
		    RESCH_G_DPRINT("INTERRUPT_DUPLICATE!!!!!, ctx#%d\n",cid);
		    if( cid != nv_rd32(priv, 0x400708))
			goto retry;
		}
	    }
	    if(stat & 0x2){
		RESCH_G_DPRINT("NVRM_COMPUTE_INTERRUPT! addr:0x%08lx,stat:0x%08lx,cid:0x%08lx\n", addr,stat,cid);
	    }

	}
    }

#endif
	
    print_stat = nv_rd32(priv,0x100);
    if(intr != print_stat){
	RESCH_G_DPRINT("[%s]: not equal intr stat! 0x%lx\n",__func__,print_stat);
	intr = nv_rd32(priv,0x100);
    	goto retry;
    }
    return __desc->gpu_driver_handler(__desc->irq_num, __desc->dev_id_orig);
  

}

extern spinlock_t reschg_global_spinlock;

static int gdev_sched_com_thread(void *data)
{
    struct gdev_device *gdev = (struct gdev_device*)data;
    struct gdev_sched_entity *se = NULL;
    struct gdev_device *phys = gdev->parent;
	static int count = 0;

    RESCH_G_PRINT("RESCH_G#%d-%d compute scheduler runnning\n", gdev->parent?gdev->parent->id:0,gdev->id);
    gdev->sched_com_thread = current;

    while (!kthread_should_stop()) {
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();

/* wakeup sleeping task */
	se = gdev_list_container(gdev_list_head(&list_entry_irq_head));
retry_get_list:
	if(se){
	    if( se->gdev != gdev){
		se = gdev_list_container(se->list_entry_irq.next);
	    	goto retry_get_list;
	    }

	    gdev_list_del(&se->list_entry_irq);
	    spin_lock( &reschg_global_spinlock );
	    if(se->wait_cond==2){
		RESCH_G_DPRINT("[%s:%d]: compute ctx#%d wakeup \n",__func__,current->pid,se->ctx);
		spin_unlock( &reschg_global_spinlock );
		if(se->task){
		    gdev_sched_wakeup(se);
		}
	    }else{
		spin_unlock( &reschg_global_spinlock );
	    }
	    se->wait_cond = 1;
	}
/**/

	if (gdev->users)
	{
	    gdev_select_next_compute(gdev);
	}
    }
    return 0;
}
static int gdev_sched_mem_thread(void *data)
{
    struct gdev_device *gdev = (struct gdev_device*)data;

    RESCH_G_PRINT("RESCH_G#%d-%d compute scheduler runnning\n", gdev->parent?gdev->parent->id:0,gdev->id);
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
    struct task_struct *p;

    static long stay_count = 0;
    static unsigned long long stay_com;

    RESCH_G_PRINT("RESCH_G#%d compute reserve running\n", gdev->id);

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

    RESCH_G_PRINT("RESCH_G#%d memory reserve running\n", gdev->id);

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

void gsched_intr_print(void *data){

    int old_cid=0, cid;
    int intr;
    struct resch_irq_desc *__desc = (struct resch_irq_desc*)data;
    void *priv = __desc->mappings;

    while (!kthread_should_stop()) {
	intr = nv_rd32(priv, 0x00100);
	if(intr & NVDEV_ENGINE_GR_INTR){
	    cid = nv_rd32(priv, 0x400708);
	    if(old_cid != cid){
		old_cid = cid;
		printk("[%s]: 0x400708: 0x%lx \n",cid);
	    }

	}
	yield();
    }
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

    /* create compute and memory credit replenishment threads. */
    sprintf(name, "gcreditc%d", gdev->id);
    credit_com = kthread_create(gdev_credit_com_thread, (void*)gdev, name);
    if (credit_com) {
	sched_setscheduler(credit_com, SCHED_FIFO, &sp);
	wake_up_process(credit_com);
	gdev->credit_com_thread = credit_com;
    }

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
    if (gdev->sched_com_thread) {
	kthread_stop(gdev->sched_com_thread);
	gdev->sched_com_thread = NULL;
    }
#endif
    schedule_timeout_uninterruptible(usecs_to_jiffies(gdev->period));
}


void gpu_device_init(struct gdev_device *dev, int id)
{
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
	
    /* ialized tasklet  */
    dev->wakeup_tasklet_t = (struct tasklet_struct*)kmalloc(sizeof(struct tasklet_struct), GFP_KERNEL);
    tasklet_init(dev->wakeup_tasklet_t, sched_thread_wakeup_tasklet, (unsigned long)dev);
}


int gpu_virtual_device_init(struct gdev_device *dev, int id, uint32_t weight, struct gdev_device *phys)
{
    gpu_device_init(dev, id);
    dev->period = GDEV_PERIOD_DEFAULT;
    dev->parent = phys;
    dev->com_bw = weight;
    dev->mem_bw = weight;
    return 0;
}

int gsched_irq_intercept_init(struct resch_irq_desc *desc)
{
    struct pci_dev *__dev = NULL;
    struct resch_irq_desc *__desc = NULL;

    /* registration of hook interrupt handler */
    request_irq(desc->irq_num, gsched_intr/*change*/, IRQF_SHARED, "resch", desc);
    /* release interrupt handler of the nouveau */
    free_irq(desc->irq_num, desc->dev_id_orig);

}

void nouveau_intr_exit(struct resch_irq_desc **desc)
{
    int i;
    struct resch_irq_desc *__desc;
    for (i = 0; i< gpu_count; i++){
	__desc = desc[i];
	if(__desc){
	    request_irq(__desc->irq_num, __desc->gpu_driver_handler, IRQF_SHARED, __desc->gpu_driver_name, __desc->dev_id_orig);
	    free_irq(__desc->irq_num, __desc);
	}
    }
}

int gsched_pci_init(struct resch_irq_desc **desc_top)
{
#define MMIO_BAR0 0
    struct pci_dev *__dev = NULL;
    struct resch_irq_desc *__rdesc = NULL;
    struct irqaction *__act = NULL;
    int __gpu_count = 0;

    while(__dev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, __dev))
    {
	__act = irq_to_desc(__dev->irq)->action;

	__rdesc = (struct resch_irq_desc*)kmalloc(sizeof(struct resch_irq_desc), GFP_KERNEL);
	__rdesc->dev = __dev;
	__rdesc->irq_num = __dev->irq;
	__rdesc->gpu_driver_name = __act->name;
	__rdesc->dev_id_orig = __act->dev_id;
	__rdesc->gpu_driver_handler = __act->handler;
	__rdesc->sched_flag = 0;
	__rdesc->mappings = ioremap(pci_resource_start(__dev, MMIO_BAR0), pci_resource_len(__dev, MMIO_BAR0)); 
	printk("%d devices remapped [0x%lx-0x%lx].sizeof:0x%lx\n",__dev->irq, pci_resource_start(__dev, MMIO_BAR0),pci_resource_end(__dev,MMIO_BAR0), pci_resource_len(__dev,MMIO_BAR0));
	

	gsched_irq_intercept_init(__rdesc);
	gdev_lock_init(&__rdesc->release_lock);
	desc_top[__gpu_count] = __rdesc;
	__gpu_count++;
    }
    atomic_set(&resch_desc[0]->intr_flag,0);

    return __gpu_count;
}

void gsched_init(void)
{
    unsigned long rstart,rend,rflags;
    int vendor_id, device_id, class, sub_vendor_id, sub_device_id, irq;
    int gpu_device_num;
    int i;

    /* look at pci devices */

    gpu_count = gsched_pci_init(resch_desc);

    if(!gpu_count)
	return -ENODEV;

    /* create physical device */
    for (i = 0; i < gpu_count; i++)
	gpu_device_init(&phys_ds[i], i);

    RESCH_G_PRINT("Found %d physical device(s).\n-Initialize device structure.....", gpu_count);


#ifdef ALLOC_VGPU_PER_ONETASK
    gpu_vcount = 100;
#endif

    /* create virtual device  */
    /* create scheduler thread */
    for (i = 0; i< gpu_vcount; i++){
#ifndef ALLOC_VGPU_PER_ONETASK
	gpu_virtual_device_init(&gdev_vds[i], i, 100/gpu_vcount, &phys_ds[i/ (gpu_vcount/gpu_count) ]);
	gsched_create_scheduler(&gdev_vds[i]);
#else 
	gpu_virtual_device_init(&gdev_vds[i], i, 100, NULL);
#endif
    }

    for(i=0;i<GDEV_CONTEXT_MAX_COUNT;i++)
	sched_entity_ptr[i]=NULL;

    RESCH_G_PRINT("Configured %d virtual device(s). \n", gpu_vcount);
    RESCH_G_PRINT("Each physical device(s) have %d virtual device(s).\n", gpu_vcount/gpu_count);

    /* create /proc entries */
    gdev_proc_create();
    for (i = 0; i< gpu_vcount; i++){
	gdev_proc_minor_create(i);
    }
    for(i=0;i<100;i++)
	cid_bitmap[i]=0;

    /* for irq list */
    gdev_list_init(&list_entry_irq_head, NULL);

}

void gsched_exit(void)
{
    int i;
    /*minmor*/
#ifndef ALLOC_VGPU_PER_ONETASK
    for (i = 0; i< gpu_vcount; i++){
	gsched_destroy_scheduler(&gdev_vds[i]);
    }
#endif
    i = 0;
    nouveau_intr_exit(resch_desc);
    /*unsetnotify*/
    gdev_proc_delete();
}
