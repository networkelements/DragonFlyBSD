/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <phk@FreeBSD.ORG> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.   Poul-Henning Kamp
 * ----------------------------------------------------------------------------
 *
 * $FreeBSD: src/sys/dev/md/md.c,v 1.8.2.2 2002/08/19 17:43:34 jdp Exp $
 * $DragonFly: src/sys/dev/disk/md/md.c,v 1.18 2007/09/05 05:28:32 dillon Exp $
 *
 */

#include "opt_md.h"		/* We have adopted some tasks from MFS */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/devicestat.h>
#include <sys/disk.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/linker.h>
#include <sys/proc.h>
#include <sys/buf2.h>
#include <sys/thread2.h>

#ifndef MD_NSECT
#define MD_NSECT (10000 * 2)
#endif

MALLOC_DEFINE(M_MD, "MD disk", "Memory Disk");
MALLOC_DEFINE(M_MDSECT, "MD sectors", "Memory Disk Sectors");

static int md_debug;
SYSCTL_INT(_debug, OID_AUTO, mddebug, CTLFLAG_RW, &md_debug, 0, "");

#if defined(MD_ROOT) && defined(MD_ROOT_SIZE)
/* Image gets put here: */
static u_char mfs_root[MD_ROOT_SIZE*1024] = "MFS Filesystem goes here";
static u_char end_mfs_root[] __unused = "MFS Filesystem had better STOP here";
#endif

static int mdrootready;

static void mdcreate_malloc(void);

#define CDEV_MAJOR	95

static d_strategy_t mdstrategy;
static d_strategy_t mdstrategy_preload;
static d_strategy_t mdstrategy_malloc;
static d_open_t mdopen;
static d_ioctl_t mdioctl;

static struct dev_ops md_ops = {
	{ "md", CDEV_MAJOR, D_DISK | D_CANFREE | D_MEMDISK },
        .d_open =	mdopen,
        .d_close =	nullclose,
        .d_read =	physread,
        .d_write =	physwrite,
        .d_ioctl =	mdioctl,
        .d_strategy =	mdstrategy,
};

struct md_s {
	int unit;
	struct devstat stats;
	struct bio_queue_head bio_queue;
	struct disk disk;
	cdev_t dev;
	int busy;
	enum {MD_MALLOC, MD_PRELOAD} type;
	unsigned nsect;

	/* MD_MALLOC related fields */
	unsigned nsecp;
	u_char **secp;

	/* MD_PRELOAD related fields */
	u_char *pl_ptr;
	unsigned pl_len;
};

static int mdunits;

static int
mdopen(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct md_s *sc;
	struct disk_info info;

	if (md_debug)
		kprintf("mdopen(%s %x %x)\n",
			devtoname(dev), ap->a_oflags, ap->a_devtype);

	sc = dev->si_drv1;
	if (sc->unit + 1 == mdunits)
		mdcreate_malloc();

	bzero(&info, sizeof(info));
	info.d_media_blksize = DEV_BSIZE;	/* mandatory */
	info.d_media_blocks = sc->nsect;

	info.d_secpertrack = 1024;		/* optional */
	info.d_nheads = 1;
	info.d_secpercyl = info.d_secpertrack * info.d_nheads;
	info.d_ncylinders = (u_int)(info.d_media_blocks / info.d_secpercyl);
	disk_setdiskinfo(&sc->disk, &info);

	return (0);
}

static int
mdioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;

	if (md_debug)
		kprintf("mdioctl(%s %lx %p %x)\n",
			devtoname(dev), ap->a_cmd, ap->a_data, ap->a_fflag);

	return (ENOIOCTL);
}

static int
mdstrategy(struct dev_strategy_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	struct md_s *sc;

	if (md_debug > 1) {
		kprintf("mdstrategy(%p) %s %08x, %lld, %d, %p)\n",
		    bp, devtoname(dev), bp->b_flags, bio->bio_offset, 
		    bp->b_bcount, bp->b_data);
	}
	bio->bio_driver_info = dev;
	sc = dev->si_drv1;
	if (sc->type == MD_MALLOC) {
		mdstrategy_malloc(ap);
	} else {
		mdstrategy_preload(ap);
	}
	return(0);
}


static int
mdstrategy_malloc(struct dev_strategy_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	unsigned secno, nsec, secval, uc;
	u_char *secp, **secpp, *dst;
	devstat_trans_flags dop;
	struct md_s *sc;
	int i;

	if (md_debug > 1)
		kprintf("mdstrategy_malloc(%p) %s %08xx, %lld, %d, %p)\n",
		    bp, devtoname(dev), bp->b_flags, bio->bio_offset, 
		    bp->b_bcount, bp->b_data);

	sc = dev->si_drv1;

	crit_enter();

	bioqdisksort(&sc->bio_queue, bio);

	if (sc->busy) {
		crit_exit();
		return(0);
	}

	sc->busy++;
	
	while (1) {
		bio = bioq_first(&sc->bio_queue);
		if (bio == NULL) {
			crit_exit();
			break;
		}
		crit_exit();
		bioq_remove(&sc->bio_queue, bio);
		bp = bio->bio_buf;

		devstat_start_transaction(&sc->stats);

		switch(bp->b_cmd) {
		case BUF_CMD_FREEBLKS:
			dop = DEVSTAT_NO_DATA;
			break;
		case BUF_CMD_READ:
			dop = DEVSTAT_READ;
			break;
		case BUF_CMD_WRITE:
			dop = DEVSTAT_WRITE;
			break;
		default:
			panic("md: bad b_cmd %d", bp->b_cmd);
		}

		nsec = bp->b_bcount >> DEV_BSHIFT;
		secno = (unsigned)(bio->bio_offset >> DEV_BSHIFT);
		dst = bp->b_data;
		while (nsec--) {
			if (secno < sc->nsecp) {
				secpp = &sc->secp[secno];
				if ((u_int)*secpp > 255) {
					secp = *secpp;
					secval = 0;
				} else {
					secp = 0;
					secval = (u_int) *secpp;
				}
			} else {
				secpp = 0;
				secp = 0;
				secval = 0;
			}
			if (md_debug > 2)
				kprintf("%08x %p %p %d\n", bp->b_flags, secpp, secp, secval);

			switch(bp->b_cmd) {
			case BUF_CMD_FREEBLKS:
				if (secpp) {
					if (secp)
						FREE(secp, M_MDSECT);
					*secpp = 0;
				}
				break;
			case BUF_CMD_READ:
				if (secp) {
					bcopy(secp, dst, DEV_BSIZE);
				} else if (secval) {
					for (i = 0; i < DEV_BSIZE; i++)
						dst[i] = secval;
				} else {
					bzero(dst, DEV_BSIZE);
				}
				break;
			case BUF_CMD_WRITE:
				uc = dst[0];
				for (i = 1; i < DEV_BSIZE; i++) 
					if (dst[i] != uc)
						break;
				if (i == DEV_BSIZE && !uc) {
					if (secp)
						FREE(secp, M_MDSECT);
					if (secpp)
						*secpp = (u_char *)uc;
				} else {
					if (!secpp) {
						MALLOC(secpp, u_char **, (secno + nsec + 1) * sizeof(u_char *), M_MD, M_WAITOK);
						bzero(secpp, (secno + nsec + 1) * sizeof(u_char *));
						bcopy(sc->secp, secpp, sc->nsecp * sizeof(u_char *));
						FREE(sc->secp, M_MD);
						sc->secp = secpp;
						sc->nsecp = secno + nsec + 1;
						secpp = &sc->secp[secno];
					}
					if (i == DEV_BSIZE) {
						if (secp)
							FREE(secp, M_MDSECT);
						*secpp = (u_char *)uc;
					} else {
						if (!secp) 
							MALLOC(secp, u_char *, DEV_BSIZE, M_MDSECT, M_WAITOK);
						bcopy(dst, secp, DEV_BSIZE);

						*secpp = secp;
					}
				}
				break;
			default:
				panic("md: bad b_cmd %d", bp->b_cmd);

			}
			secno++;
			dst += DEV_BSIZE;
		}
		bp->b_resid = 0;
		devstat_end_transaction_buf(&sc->stats, bp);
		biodone(bio);
		crit_enter();
	}
	sc->busy = 0;
	return(0);
}


static int
mdstrategy_preload(struct dev_strategy_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	devstat_trans_flags dop;
	struct md_s *sc;

	if (md_debug > 1)
		kprintf("mdstrategy_preload(%p) %s %08x, %lld, %d, %p)\n",
		    bp, devtoname(dev), bp->b_flags, bio->bio_offset, 
		    bp->b_bcount, bp->b_data);

	sc = dev->si_drv1;

	crit_enter();

	bioqdisksort(&sc->bio_queue, bio);

	if (sc->busy) {
		crit_exit();
		return(0);
	}

	sc->busy++;
	
	while (1) {
		bio = bioq_first(&sc->bio_queue);
		if (bio)
			bioq_remove(&sc->bio_queue, bio);
		crit_exit();
		if (bio == NULL)
			break;

		devstat_start_transaction(&sc->stats);

		switch(bp->b_cmd) {
		case BUF_CMD_FREEBLKS:
			dop = DEVSTAT_NO_DATA;
			break;
		case BUF_CMD_READ:
			dop = DEVSTAT_READ;
			bcopy(sc->pl_ptr + bio->bio_offset, 
			       bp->b_data, bp->b_bcount);
			break;
		case BUF_CMD_WRITE:
			dop = DEVSTAT_WRITE;
			bcopy(bp->b_data, sc->pl_ptr + bio->bio_offset,
			      bp->b_bcount);
			break;
		default:
			panic("md: bad cmd %d\n", bp->b_cmd);
		}
		bp->b_resid = 0;
		devstat_end_transaction_buf(&sc->stats, bp);
		biodone(bio);
		crit_enter();
	}
	sc->busy = 0;
	return(0);
}

static struct md_s *
mdcreate(void)
{
	struct md_s *sc;

	MALLOC(sc, struct md_s *,sizeof(*sc), M_MD, M_WAITOK);
	bzero(sc, sizeof(*sc));
	sc->unit = mdunits++;
	bioq_init(&sc->bio_queue);
	devstat_add_entry(&sc->stats, "md", sc->unit, DEV_BSIZE,
		DEVSTAT_NO_ORDERED_TAGS, 
		DEVSTAT_TYPE_DIRECT | DEVSTAT_TYPE_IF_OTHER,
		DEVSTAT_PRIORITY_OTHER);
	sc->dev = disk_create(sc->unit, &sc->disk, &md_ops);
	sc->dev->si_drv1 = sc;
	sc->dev->si_iosize_max = DFLTPHYS;

	return (sc);
}

static void
mdcreate_preload(u_char *image, unsigned length)
{
	struct md_s *sc;

	sc = mdcreate();
	sc->type = MD_PRELOAD;
	sc->nsect = length / DEV_BSIZE;
	sc->pl_ptr = image;
	sc->pl_len = length;

	if (sc->unit == 0) 
		mdrootready = 1;
}

static void
mdcreate_malloc(void)
{
	struct md_s *sc;

	sc = mdcreate();
	sc->type = MD_MALLOC;

	sc->nsect = MD_NSECT;	/* for now */
	MALLOC(sc->secp, u_char **, sizeof(u_char *), M_MD, M_WAITOK);
	bzero(sc->secp, sizeof(u_char *));
	sc->nsecp = 1;
	kprintf("md%d: Malloc disk\n", sc->unit);
}

static void
md_drvinit(void *unused)
{

	caddr_t mod;
	caddr_t c;
	u_char *ptr, *name, *type;
	unsigned len;

#ifdef MD_ROOT_SIZE
	mdcreate_preload(mfs_root, MD_ROOT_SIZE*1024);
#endif
	mod = NULL;
	while ((mod = preload_search_next_name(mod)) != NULL) {
		name = (char *)preload_search_info(mod, MODINFO_NAME);
		type = (char *)preload_search_info(mod, MODINFO_TYPE);
		if (name == NULL)
			continue;
		if (type == NULL)
			continue;
		if (strcmp(type, "md_image") && strcmp(type, "mfs_root"))
			continue;
		c = preload_search_info(mod, MODINFO_ADDR);
		ptr = *(u_char **)c;
		c = preload_search_info(mod, MODINFO_SIZE);
		len = *(unsigned *)c;
		kprintf("md%d: Preloaded image <%s> %d bytes at %p\n",
		   mdunits, name, len, ptr);
		mdcreate_preload(ptr, len);
	} 
	mdcreate_malloc();
}

SYSINIT(mddev,SI_SUB_DRIVERS,SI_ORDER_MIDDLE+CDEV_MAJOR, md_drvinit,NULL)

#ifdef MD_ROOT
static void
md_takeroot(void *junk)
{
	if (mdrootready)
		rootdevnames[0] = "ufs:/dev/md0c";
}

SYSINIT(md_root, SI_SUB_MOUNT_ROOT, SI_ORDER_FIRST, md_takeroot, NULL);
#endif
