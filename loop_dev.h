#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>

struct lo_cmd {
	struct kthread_work	work;
	struct list_head	list;
	struct request	*rq;
	struct kiocb	iocb;
};

struct lo_dev {
	struct file	*lo_backing_file;
	struct block_device	*bd;
	spinlock_t	lo_lock;
	struct blk_mq_tag_set	tag_set;
	struct request_queue	*lo_q;
	struct kthread_worker	worker;
	struct gendisk	*gd;
	atomic_t	lo_refcnt;
	loff_t		lo_offset;
};
