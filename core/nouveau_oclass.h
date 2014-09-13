#ifndef __NOUVEAU_OCLASS_H__
#define __NOUVEAU_OCLASS_H__

enum nv_subdev_type {
	NVDEV_ENGINE_DEVICE,
	NVDEV_SUBDEV_VBIOS,

	/* All subdevs from DEVINIT to DEVINIT_LAST will be created before
	 * *any* of them are initialised.  This subdev category is used
	 * for any subdevs that the VBIOS init table parsing may call out
	 * to during POST.
	 */
	NVDEV_SUBDEV_DEVINIT,
	NVDEV_SUBDEV_GPIO,
	NVDEV_SUBDEV_I2C,
	NVDEV_SUBDEV_DEVINIT_LAST = NVDEV_SUBDEV_I2C,

	/* This grouping of subdevs are initialised right after they've
	 * been created, and are allowed to assume any subdevs in the
	 * list above them exist and have been initialised.
	 */
	NVDEV_SUBDEV_MXM,
	NVDEV_SUBDEV_MC,
	NVDEV_SUBDEV_BUS,
	NVDEV_SUBDEV_TIMER,
	NVDEV_SUBDEV_FB,
	NVDEV_SUBDEV_LTCG,
	NVDEV_SUBDEV_IBUS,
	NVDEV_SUBDEV_INSTMEM,
	NVDEV_SUBDEV_VM,
	NVDEV_SUBDEV_BAR,
	NVDEV_SUBDEV_PWR,
	NVDEV_SUBDEV_VOLT,
	NVDEV_SUBDEV_THERM,
	NVDEV_SUBDEV_CLOCK,

	NVDEV_ENGINE_FIRST,
	NVDEV_ENGINE_DMAOBJ = NVDEV_ENGINE_FIRST,
	NVDEV_ENGINE_IFB,
	NVDEV_ENGINE_FIFO,
	NVDEV_ENGINE_SW,
	NVDEV_ENGINE_GR,
	NVDEV_ENGINE_MPEG,
	NVDEV_ENGINE_ME,
	NVDEV_ENGINE_VP,
	NVDEV_ENGINE_CRYPT,
	NVDEV_ENGINE_BSP,
	NVDEV_ENGINE_PPP,
	NVDEV_ENGINE_COPY0,
	NVDEV_ENGINE_COPY1,
	NVDEV_ENGINE_COPY2,
	NVDEV_ENGINE_VIC,
	NVDEV_ENGINE_VENC,
	NVDEV_ENGINE_DISP,
	NVDEV_ENGINE_PERFMON,

	NVDEV_SUBDEV_NR,
};


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

struct nouveau_engine {
    struct nouveau_subdev base;
    struct nouveau_oclass *cclass;
    struct nouveau_oclass *sclass;

    struct list_head contexts;
    spinlock_t lock;
    void (*tile_prog)(struct nouveau_engine *, int region);
    int (*tlb_flush)(struct nouveau_engine *);
};



struct nouveau_device {
    struct nouveau_engine base;
    struct list_head head;

    struct pci_dev *pdev;
    struct platform_device *platformdev;
    u64 handle;

    const char *cfgopt;
    const char *dbgopt;
    const char *name;
    const char *cname;
    u64 disable_mask;

    enum {
	NV_04    = 0x04,
	NV_10    = 0x10,
	NV_11    = 0x11,
	NV_20    = 0x20,
	NV_30    = 0x30,
	NV_40    = 0x40,
	NV_50    = 0x50,
	NV_C0    = 0xc0,
	NV_D0    = 0xd0,
	NV_E0    = 0xe0,
	GM100    = 0x110,
    } card_type;
    u32 chipset;
    u32 crystal;

    struct nouveau_oclass *oclass[NVDEV_SUBDEV_NR];
    struct nouveau_object *subdev[NVDEV_SUBDEV_NR];
};

//--

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

struct nouveau_mc_intr {
    uint32_t stat;
    uint32_t unit;
};


/* --Function--  */

static inline struct nouveau_subdev *nv_subdev(void *obj){
    return obj;
}

static inline struct nouveau_object *nv_object(void *obj){
    return obj;
}

static inline struct nouveau_device *nv_device(void *obj){
	struct nouveau_object *device = obj;

	if (device->engine)
	    device = device->engine;
	if (device->parent)
	    device = device->parent;

	return (void*)device;
}

static inline struct nouveau_subdev *nouveau_subdev(void *obj, int sub){

	if(nv_device(obj)->subdev[sub])
	    return nv_subdev(nv_device(obj)->subdev[sub]);
}

static inline uint32_t nv_rd32(void *obj, uint32_t addr){
	struct nouveau_subdev *subdev = nv_subdev(obj);
	uint32_t data = ioread32(subdev->mmio + addr );

	return data;
}
static inline void nv_wr32(void *obj, uint32_t addr, uint32_t data){
	struct nouveau_subdev *subdev = nv_subdev(obj);
	iowrite32(data, subdev->mmio + addr );
}

#endif

