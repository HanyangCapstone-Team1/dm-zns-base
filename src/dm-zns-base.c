// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M1/M2 임의 쓰기 변환 테스트용 코드
 *
 * 상위에서는 일반 블록 장치로 보이게 했음.
 * 쓰기는 active zone의 wp에 순서대로 저장하고 논리 주소와 실제 저장
 * 위치는 메모리 LSM 구조에 기록함. M2에서는 ext4가 bio를 다르게
 * 나눠 보내도 읽을 수 있도록 sector 단위로 매핑했음.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/blkzoned.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define DM_MSG_PREFIX "zns-m1"
#define MAP_GRANULARITY_SECTORS 1
#define MEMTABLE_MAX_ENTRIES 4096
#define RUNS_PER_COMPACTION 4
#define MAX_LSM_LEVELS 16

struct zone_state {
	sector_t wp;
	sector_t capacity;
	bool is_active;
	bool is_full;
};

struct map_entry {
	sector_t logical_sector;
	u32 zone_idx;
	sector_t zone_offset;
	u64 sequence;
	struct rb_node node;
};

struct sorted_run {
	struct rb_root root;
	u32 nr_entries;
	u32 level;
	u64 generation;
	bool compacting;
	struct list_head list;
};

struct zns_m1_c {
	struct dm_dev *dev;
	u32 nr_zones;
	sector_t zone_size;
	struct zone_state *zones;
	u32 active_zone;
	struct rb_root active_memtable;
	u32 active_entries;
	u64 next_sequence;
	u64 next_generation;
	struct list_head runs;
	u32 nr_runs;
	struct workqueue_struct *compaction_wq;
	struct work_struct compaction_work;
	bool compaction_running;
	bool stopping;
	spinlock_t lock;
};

static struct map_entry *tree_lookup(struct rb_root *root,
				     sector_t logical_sector)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct map_entry *e = rb_entry(node, struct map_entry, node);

		if (logical_sector < e->logical_sector)
			node = node->rb_left;
		else if (logical_sector > e->logical_sector)
			node = node->rb_right;
		else
			return e;
	}

	return NULL;
}

static int tree_insert(struct rb_root *root, sector_t logical, u32 zone_idx,
		       sector_t zone_offset, u64 sequence, gfp_t gfp,
		       bool replace_newer)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	struct map_entry *e;

	while (*link) {
		parent = *link;
		e = rb_entry(parent, struct map_entry, node);

		if (logical < e->logical_sector) {
			link = &parent->rb_left;
		} else if (logical > e->logical_sector) {
			link = &parent->rb_right;
		} else {
			if (!replace_newer || sequence >= e->sequence) {
				e->zone_idx = zone_idx;
				e->zone_offset = zone_offset;
				e->sequence = sequence;
			}
			return 0;
		}
	}

	e = kmalloc(sizeof(*e), gfp);
	if (!e)
		return -ENOMEM;

	e->logical_sector = logical;
	e->zone_idx = zone_idx;
	e->zone_offset = zone_offset;
	e->sequence = sequence;
	rb_link_node(&e->node, parent, link);
	rb_insert_color(&e->node, root);
	return 1;
}

static void free_tree(struct rb_root *root)
{
	struct rb_node *node;

	while ((node = rb_first(root))) {
		struct map_entry *e = rb_entry(node, struct map_entry, node);

		rb_erase(node, root);
		kfree(e);
	}
}

static void free_run(struct sorted_run *run)
{
	free_tree(&run->root);
	kfree(run);
}

static struct map_entry *map_lookup(struct zns_m1_c *c,
				    sector_t logical_sector)
{
	struct map_entry *best;
	struct sorted_run *run;

	best = tree_lookup(&c->active_memtable, logical_sector);
	list_for_each_entry(run, &c->runs, list) {
		struct map_entry *e = tree_lookup(&run->root, logical_sector);

		if (e && (!best || e->sequence > best->sequence))
			best = e;
	}

	return best;
}

static unsigned int count_level_runs(struct zns_m1_c *c, u32 level)
{
	struct sorted_run *run;
	unsigned int count = 0;

	list_for_each_entry(run, &c->runs, list) {
		if (run->level == level && !run->compacting)
			count++;
	}

	return count;
}

static bool compaction_needed(struct zns_m1_c *c)
{
	u32 level;

	for (level = 0; level < MAX_LSM_LEVELS - 1; level++) {
		if (count_level_runs(c, level) >= RUNS_PER_COMPACTION)
			return true;
	}

	return false;
}

static void maybe_queue_compaction(struct zns_m1_c *c)
{
	if (!c->stopping && !c->compaction_running &&
	    compaction_needed(c)) {
		c->compaction_running = true;
		queue_work(c->compaction_wq, &c->compaction_work);
	}
}

static void rotate_memtable(struct zns_m1_c *c)
{
	struct sorted_run *run;

	if (c->active_entries < MEMTABLE_MAX_ENTRIES)
		return;

	run = kmalloc(sizeof(*run), GFP_ATOMIC);
	if (!run)
		return;

	run->root = c->active_memtable;
	run->nr_entries = c->active_entries;
	run->level = 0;
	run->generation = ++c->next_generation;
	run->compacting = false;
	INIT_LIST_HEAD(&run->list);
	list_add(&run->list, &c->runs);
	c->nr_runs++;

	c->active_memtable = RB_ROOT;
	c->active_entries = 0;
	DMINFO("memtable rotated: run=%llu entries=%u",
	       (unsigned long long)run->generation, run->nr_entries);
	maybe_queue_compaction(c);
}

static int map_insert(struct zns_m1_c *c, sector_t logical, u32 zone_idx,
		      sector_t zone_offset)
{
	int ret;

	ret = tree_insert(&c->active_memtable, logical, zone_idx, zone_offset,
			  ++c->next_sequence, GFP_ATOMIC, false);
	if (ret < 0)
		return ret;
	if (ret > 0)
		c->active_entries++;

	rotate_memtable(c);
	return 0;
}

static int advance_active_zone(struct zns_m1_c *c)
{
	u32 i;

	for (i = 0; i < c->nr_zones; i++) {
		if (!c->zones[i].is_full && !c->zones[i].is_active &&
		    c->zones[i].wp == 0) {
			c->zones[c->active_zone].is_active = false;
			c->active_zone = i;
			c->zones[i].is_active = true;
			DMINFO("active zone -> %u", i);
			return 0;
		}
	}

	return -ENOSPC;
}

static int fill_zone_cb(struct blk_zone *zone, unsigned int idx, void *data)
{
	struct zns_m1_c *c = data;

	if (idx >= c->nr_zones)
		return -ERANGE;

	c->zones[idx].capacity = zone->capacity;
	c->zones[idx].wp = zone->wp - zone->start;
	c->zones[idx].is_full = zone->cond == BLK_ZONE_COND_FULL;
	c->zones[idx].is_active = false;
	return 0;
}

static int find_initial_active_zone(struct zns_m1_c *c)
{
	u32 i;

	for (i = 0; i < c->nr_zones; i++) {
		if (!c->zones[i].is_full &&
		    c->zones[i].wp < c->zones[i].capacity) {
			c->active_zone = i;
			c->zones[i].is_active = true;
			return 0;
		}
	}

	return -ENOSPC;
}

static int select_compaction(struct zns_m1_c *c,
			     struct sorted_run **selected, u32 *level)
{
	struct sorted_run *run;
	u32 candidate;
	unsigned int nr;

	for (candidate = 0; candidate < MAX_LSM_LEVELS - 1; candidate++) {
		if (count_level_runs(c, candidate) < RUNS_PER_COMPACTION)
			continue;

		nr = 0;
		list_for_each_entry_reverse(run, &c->runs, list) {
			if (run->level != candidate || run->compacting)
				continue;

			run->compacting = true;
			selected[nr++] = run;
			if (nr == RUNS_PER_COMPACTION)
				break;
		}

		if (nr == RUNS_PER_COMPACTION) {
			*level = candidate;
			return nr;
		}
	}

	return 0;
}

static void unmark_compaction(struct sorted_run **selected, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++)
		selected[i]->compacting = false;
}

static void compact_runs(struct work_struct *work)
{
	struct zns_m1_c *c = container_of(work, struct zns_m1_c,
					  compaction_work);
	struct sorted_run *selected[RUNS_PER_COMPACTION];
	unsigned long flags;

	while (1) {
		struct sorted_run *output;
		u64 generation = 0;
		u32 level = 0;
		unsigned int nr;
		unsigned int i;
		int ret = 0;

		spin_lock_irqsave(&c->lock, flags);
		if (c->stopping) {
			c->compaction_running = false;
			spin_unlock_irqrestore(&c->lock, flags);
			return;
		}

		nr = select_compaction(c, selected, &level);
		if (!nr) {
			c->compaction_running = false;
			spin_unlock_irqrestore(&c->lock, flags);
			return;
		}
		spin_unlock_irqrestore(&c->lock, flags);

		output = kzalloc(sizeof(*output), GFP_KERNEL);
		if (!output) {
			ret = -ENOMEM;
			goto failed;
		}

		output->root = RB_ROOT;
		output->level = level + 1;
		INIT_LIST_HEAD(&output->list);

		for (i = 0; i < nr && !ret; i++) {
			struct rb_node *node;

			generation = max(generation, selected[i]->generation);
			for (node = rb_first(&selected[i]->root); node;
			     node = rb_next(node)) {
				struct map_entry *e;
				int inserted;

				e = rb_entry(node, struct map_entry, node);
				inserted = tree_insert(&output->root,
						e->logical_sector, e->zone_idx,
						e->zone_offset, e->sequence,
						GFP_KERNEL, true);
				if (inserted < 0) {
					ret = inserted;
					break;
				}
				if (inserted > 0)
					output->nr_entries++;
			}
		}

		if (ret) {
			free_run(output);
			goto failed;
		}

		output->generation = generation;
		spin_lock_irqsave(&c->lock, flags);
		for (i = 0; i < nr; i++) {
			list_del(&selected[i]->list);
			c->nr_runs--;
		}
		list_add_tail(&output->list, &c->runs);
		c->nr_runs++;
		spin_unlock_irqrestore(&c->lock, flags);

		for (i = 0; i < nr; i++)
			free_run(selected[i]);

		DMINFO("compaction: L%u %u runs -> L%u (%u entries)",
		       level, nr, output->level, output->nr_entries);
		continue;

failed:
		spin_lock_irqsave(&c->lock, flags);
		unmark_compaction(selected, nr);
		c->compaction_running = false;
		spin_unlock_irqrestore(&c->lock, flags);
		DMERR("compaction failed: %d", ret);
		return;
	}
}

static int zns_m1_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_m1_c *c;
	struct block_device *bdev;
	unsigned int nr_rep;
	int ret;

	if (argc != 1) {
		ti->error = "expected one argument: underlying device";
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ti->error = "out of memory";
		return -ENOMEM;
	}

	spin_lock_init(&c->lock);
	c->active_memtable = RB_ROOT;
	INIT_LIST_HEAD(&c->runs);
	INIT_WORK(&c->compaction_work, compact_runs);
	c->compaction_wq = alloc_ordered_workqueue("zns_lsm_compact",
						  WQ_MEM_RECLAIM);
	if (!c->compaction_wq) {
		ti->error = "failed to create compaction workqueue";
		ret = -ENOMEM;
		goto err_free;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		goto err_wq;
	}

	bdev = c->dev->bdev;
	c->zone_size = bdev_zone_sectors(bdev);
	if (!c->zone_size) {
		ti->error = "underlying device is not zoned";
		ret = -EINVAL;
		goto err_put;
	}

	c->nr_zones = (u32)(ti->len / c->zone_size);
	if (!c->nr_zones) {
		ti->error = "target length smaller than one zone";
		ret = -EINVAL;
		goto err_put;
	}

	c->zones = kcalloc(c->nr_zones, sizeof(*c->zones), GFP_KERNEL);
	if (!c->zones) {
		ti->error = "out of memory for zone state";
		ret = -ENOMEM;
		goto err_put;
	}

	nr_rep = c->nr_zones;
	ret = blkdev_report_zones(bdev, ti->begin, nr_rep, fill_zone_cb, c);
	if (ret < 0) {
		ti->error = "blkdev_report_zones failed";
		goto err_zones;
	}

	ret = find_initial_active_zone(c);
	if (ret) {
		ti->error = "no writable zone";
		goto err_zones;
	}

	/*
	 * 매핑 하나가 512B sector 하나를 나타내도록 했음.
	 * ext4의 읽기/쓰기 bio 경계가 달라도 찾을 수 있게 큰 bio도 같은
	 * 단위로 나눔.
	 */
	ret = dm_set_target_max_io_len(ti, MAP_GRANULARITY_SECTORS);
	if (ret) {
		ti->error = "failed to set mapping granularity";
		goto err_zones;
	}

	ti->private = c;
	ti->num_flush_bios = 1;

	DMINFO("ctr: target attached on top of '%s' (%u zones, %llu sectors/zone)",
	       argv[0], c->nr_zones, (unsigned long long)c->zone_size);
	return 0;

err_zones:
	kfree(c->zones);
err_put:
	dm_put_device(ti, c->dev);
err_wq:
	destroy_workqueue(c->compaction_wq);
err_free:
	kfree(c);
	return ret;
}

static void zns_m1_dtr(struct dm_target *ti)
{
	struct zns_m1_c *c = ti->private;
	struct sorted_run *run;
	struct sorted_run *tmp;
	unsigned long flags;

	spin_lock_irqsave(&c->lock, flags);
	c->stopping = true;
	spin_unlock_irqrestore(&c->lock, flags);
	cancel_work_sync(&c->compaction_work);
	destroy_workqueue(c->compaction_wq);

	free_tree(&c->active_memtable);
	list_for_each_entry_safe(run, tmp, &c->runs, list) {
		list_del(&run->list);
		free_run(run);
	}

	kfree(c->zones);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int complete_bio_error(struct bio *bio, blk_status_t status)
{
	bio->bi_status = status;
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

static int zns_m1_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_m1_c *c = ti->private;
	sector_t logical_sector = bio->bi_iter.bi_sector;
	sector_t nr_sectors = bio_sectors(bio);
	unsigned long flags;

	if (bio_op(bio) == REQ_OP_FLUSH) {
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}

	/*
	 * 논리 주소와 실제 저장 위치가 다르므로 discard를 그대로 내리면
	 * 엉뚱한 위치가 지워질 수 있음. M2에서는 discard를 넘기지 않고
	 * 바로 완료 처리했음. 매핑 무효화와 zone reset은 M3에서 처리함.
	 */
	if (bio_op(bio) == REQ_OP_DISCARD) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	if (nr_sectors != MAP_GRANULARITY_SECTORS) {
		DMERR("unexpected bio size: %llu sectors",
		      (unsigned long long)nr_sectors);
		return complete_bio_error(bio, BLK_STS_IOERR);
	}

	spin_lock_irqsave(&c->lock, flags);

	if (bio_data_dir(bio) == READ) {
		struct map_entry *e = map_lookup(c, logical_sector);

		if (!e) {
			spin_unlock_irqrestore(&c->lock, flags);
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = ti->begin +
			(sector_t)e->zone_idx * c->zone_size + e->zone_offset;
	} else {
		struct zone_state *az = &c->zones[c->active_zone];

		if (az->wp + nr_sectors > az->capacity) {
			az->is_full = true;
			if (advance_active_zone(c)) {
				spin_unlock_irqrestore(&c->lock, flags);
				DMERR("device full, no free zones");
				return complete_bio_error(bio, BLK_STS_NOSPC);
			}
			az = &c->zones[c->active_zone];
		}

		if (map_insert(c, logical_sector, c->active_zone, az->wp)) {
			spin_unlock_irqrestore(&c->lock, flags);
			DMERR("map_insert OOM");
			return complete_bio_error(bio, BLK_STS_RESOURCE);
		}

		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = ti->begin +
			(sector_t)c->active_zone * c->zone_size + az->wp;
		az->wp += nr_sectors;
	}

	spin_unlock_irqrestore(&c->lock, flags);
	return DM_MAPIO_REMAPPED;
}

static struct target_type zns_m1_target = {
	.name = "zns-m1",
	.version = { 0, 3, 0 },
	.features = 0,
	.module = THIS_MODULE,
	.ctr = zns_m1_ctr,
	.dtr = zns_m1_dtr,
	.map = zns_m1_map,
};

static int __init zns_m1_init(void)
{
	int ret = dm_register_target(&zns_m1_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("target registered");

	return ret;
}

static void __exit zns_m1_exit(void)
{
	dm_unregister_target(&zns_m1_target);
	DMINFO("target unregistered");
}

module_init(zns_m1_init);
module_exit(zns_m1_exit);

MODULE_DESCRIPTION("ZNS M1/M2: in-memory LSM mapping DM target");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
