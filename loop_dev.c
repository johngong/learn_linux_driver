#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/falloc.h>
#include <linux/kobj_map.h>
#include <linux/kthread.h>
#include "lo_jg.h"

static DEFINE_MUTEX(loop_index_mutex);
static struct lo_dev *g_lo = NULL;


static int loop_set_fd(struct lo_dev *lo, struct block_device *bd,
		fmode_t mode, unsigned long arg)
{
	

}


static int lo_ioctl(struct block_device *bd, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct lo_dev *lo = bd->bd_disk->private_data;
	int err;

	switch (cmd) {
	case LOOP_SET_FD:
		err = loop_set_fd(lo, bd, mode, arg);
		break;
	case LOOP_SET_CAPACITY:
		err = -EPERM;
		break;
	default:
		break;
	}

	return 0;
}

static void lo_release(struct gendisk *gd, fmode_t mode)
{
	struct lo_dev *lo = gd->private_data;
	if (atomic_dec_return(lo->lo_refcnt))
		return;
}

static int lo_open(struct block_device *bd, fmode_t mode)
{
	struct lo_dev *lo;
	int err = 0;

	mutex_lock(&loop_index_mutex);
	lo = bd->bd_disk->private_data;
	if (!lo) {
		err = -ENXIO;
		goto out;
	}

	atomic_inc(&lo->lo_refcnt);
out:
	mutex_unlock(&loop_index_mutex);
	return err;
}

static const struct block_device_operations lo_fops = {
	.owner	=	THIS_MODULE,
	.open	=	lo_open,
	.release	=	lo_release,
	.ioctl	=	lo_ioctl,
};

static int lo_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct lo_cmd *cmd = blk_mq_rq_to_pdu(bd->rq);
	struct lo_dev *lo = cmd->rq->q->queuedata;
	blk_mq_start_request(bd->rq);

	kthread_queue_work(&lo->worker, &cmd->work);
	return BLK_MQ_RQ_QUEUE_OK;
}

static int lo_read_simple(struct lo_dev *lo, struct request *rq,
                loff_t pos)
{
        struct bio_vec bvec;
        struct req_iterator iter;
        struct iov_iter i;
        ssize_t len;

        rq_for_each_segment(bvec, rq, iter) {
                iov_iter_bvec(&i, ITER_BVEC, &bvec, 1, bvec.bv_len);
                len = vfs_iter_read(lo->lo_backing_file, &i, &pos);
                if (len < 0)
                        return len;

                flush_dcache_page(bvec.bv_page);

                if (len != bvec.bv_len) {
                        struct bio *bio;

                        __rq_for_each_bio(bio, rq)
                                zero_fill_bio(bio);
                        break;
                }
                cond_resched();
        }

        return 0;
}

static int lo_write_bvec(struct file *file, struct bio_vec *bvec, loff_t *ppos)
{
        struct iov_iter i;
        ssize_t bw;

        iov_iter_bvec(&i, ITER_BVEC, bvec, 1, bvec->bv_len);

        file_start_write(file);
        bw = vfs_iter_write(file, &i, ppos);
        file_end_write(file);

        if (likely(bw ==  bvec->bv_len))
                return 0;

        printk_ratelimited(KERN_ERR
                "loop: Write error at byte offset %llu, length %i.\n",
                (unsigned long long)*ppos, bvec->bv_len);
        if (bw >= 0)
                bw = -EIO;
        return bw;
}

static int lo_write_simple(struct lo_dev *lo, struct request *rq,
                loff_t pos)
{
        struct bio_vec bvec;
        struct req_iterator iter;
        int ret = 0;

        rq_for_each_segment(bvec, rq, iter) {
                ret = lo_write_bvec(lo->lo_backing_file, &bvec, &pos);
                if (ret < 0)
                        break;
                cond_resched();
        }

        return ret;
}

static int lo_discard(struct lo_dev *lo, struct request *rq, loff_t pos)
{
        /*
         * We use punch hole to reclaim the free space used by the
         * image a.k.a. discard. However we do not support discard if
         * encryption is enabled, because it may give an attacker
         * useful information.
         */
        struct file *file = lo->lo_backing_file;
        int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
        int ret;

        ret = file->f_op->fallocate(file, mode, pos, blk_rq_bytes(rq));
        if (unlikely(ret && ret != -EINVAL && ret != -EOPNOTSUPP))
                ret = -EIO;

        return ret;
}

static int lo_req_flush(struct lo_dev *lo, struct request *rq)
{
        struct file *file = lo->lo_backing_file;
        int ret = vfs_fsync(file, 0);
        if (unlikely(ret && ret != -EINVAL))
                ret = -EIO;

        return ret;
}

static int do_req_filebacked(struct lo_dev *lo, struct request *rq)
{
        loff_t pos = ((loff_t) blk_rq_pos(rq) << 9) + lo->lo_offset;

        switch (req_op(rq)) {
        case REQ_OP_FLUSH:
                return lo_req_flush(lo, rq);
        case REQ_OP_DISCARD:
                return lo_discard(lo, rq, pos);
        case REQ_OP_WRITE:
                return lo_write_simple(lo, rq, pos);
        case REQ_OP_READ:
                return lo_read_simple(lo, rq, pos);
        default:
                WARN_ON_ONCE(1);
                return -EIO;
                break;
        }
}

static void loop_handle_cmd(struct lo_cmd *cmd)
{
	struct lo_dev *lo = cmd->rq->q->queuedata;
	int ret = 0;

	ret = do_req_filebacked(lo, cmd->rq);

	if (ret)
		blk_mq_complete_request(cmd->rq, ret ? -EIO : 0);
}

static void loop_queue_work(struct kthread_work *work)
{
	struct lo_cmd *cmd =
		container_of(work, struct lo_cmd, work);
	loop_handle_cmd(cmd);
}

static int lo_init_request(void *data, struct request *rq,
		unsigned int hctx_idx, unsigned int request_idx, 
		unsigned int numa_node)
{
	struct lo_cmd *cmd = blk_mq_rq_to_pdu(rq);

	cmd->rq = rq;
	kthread_init_work(&cmd->work, loop_queue_work);
	return 0;
}

static struct blk_mq_ops lo_mq_ops = {
	.queue_rq	=	lo_queue_rq,
	.init_request	=	lo_init_request,
};

static int major;

static int add_loop_dev(struct lo_dev **ld)
{
	int err;
	struct lo_dev	*l;
	struct gendisk	*gd;

	l = kzalloc(sizeof(*l), GFP_KERNEL);

	l->tag_set.ops = &lo_mq_ops;
	l->tag_set.nr_hw_queues = 1;
	l->tag_set.queue_depth = 128;
	l->tag_set.numa_node = NUMA_NO_NODE;
	l->tag_set.cmd_size = sizeof(struct lo_cmd);
	l->tag_set.flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_SG_MERGE;
	l->tag_set.driver_data = l;

	err = blk_mq_alloc_tag_set(&l->tag_set);
	l->lo_q = blk_mq_init_queue(&l->tag_set);

	l->lo_q->queuedata = l;

	__set_bit(QUEUE_FLAG_NOMERGES, &l->lo_q->queue_flags);

	gd = l->gd = alloc_disk(0);
	gd->major = major;
	gd->first_minor = 0;
	gd->fops = &lo_fops;
	gd->private_data = l;
	gd->queue = l->lo_q;
	sprintf(gd->disk_name, "loop0");

	add_disk(gd);

	*ld = l;
	return 0;
}

static int loop_lookup(struct lo_dev **l)
{
	if (g_lo) {
		*l = g_lo;
		return 0;
	} else {
		*l = NULL;
		return -ENOMEM;
	}
}

static struct kobject *loop_probe(dev_t dev, int *part, void *data)
{
        struct lo_dev *lo;
        struct kobject *kobj;
        int err;

        mutex_lock(&loop_index_mutex);
        err = loop_lookup(&lo);
        if (err < 0)
                err = add_loop_dev(&lo);
        if (err < 0)
                kobj = NULL;
        else
                kobj = get_disk(lo->gd);
        mutex_unlock(&loop_index_mutex);

        *part = 0;
        return kobj;
}

static int __init loop_init(void)
{
	struct lo_dev *ld;

	register_blkdev(LOOP_MAJOR, "jgloop");

	blk_register_region(MKDEV(LOOP_MAJOR, 0), (1UL),
                                  THIS_MODULE, loop_probe, NULL, NULL);

	add_loop_dev(&ld);
	return 0;
}

static void del_loop_dev(struct lo_dev *lo)
{
	if (lo) {
		blk_cleanup_queue(lo->lo_q);
		del_gendisk(lo->gd);
		blk_mq_free_tag_set(&lo->tag_set);
		put_disk(lo->gd);
		lo = NULL;
	}
}

static void __exit loop_exit(void)
{
	del_loop_dev(g_lo);
	blk_unregister_region(MKDEV(LOOP_MAJOR, 0), 1UL);
	unregister_blkdev(LOOP_MAJOR, "jgloop");
}

module_init(loop_init);
module_exit(loop_exit);
MODULE_LICENSE("GPL");
