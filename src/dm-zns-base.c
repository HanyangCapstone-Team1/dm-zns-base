// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M1/M2 임의 쓰기 변환 테스트용 코드
 *
 * 상위에서는 일반 블록 장치로 보이게 했음.
 * 쓰기는 active zone의 wp에 순서대로 저장하고 논리 주소와 실제 저장
 * 위치는 메모리 해시 테이블에 기록함. M2에서는 ext4가 bio를 다르게
 * 나눠 보내도 읽을 수 있도록 sector 단위로 매핑했음.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/blkzoned.h>
#include <linux/slab.h>

#define DM_MSG_PREFIX "zns-m1"
#define MAP_HASH_BITS 15
#define MAP_GRANULARITY_SECTORS 1

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
	struct hlist_node node;
};

struct zns_m1_c {
	struct dm_dev *dev;
	u32 nr_zones;
	sector_t zone_size;
	struct zone_state *zones;
	u32 active_zone;
	DECLARE_HASHTABLE(map, MAP_HASH_BITS);
	spinlock_t lock;
};

static struct map_entry *map_lookup(struct zns_m1_c *c,
				    sector_t logical_sector)
{
	struct map_entry *e;

	hash_for_each_possible(c->map, e, node, (u32)logical_sector) {
		if (e->logical_sector == logical_sector)
			return e;
	}

	return NULL;
}

static int map_insert(struct zns_m1_c *c, sector_t logical, u32 zone_idx,
		      sector_t zone_offset)
{
	struct map_entry *e = map_lookup(c, logical);

	if (!e) {
		e = kmalloc(sizeof(*e), GFP_ATOMIC);
		if (!e)
			return -ENOMEM;

		e->logical_sector = logical;
		hash_add(c->map, &e->node, (u32)logical);
	}

	e->zone_idx = zone_idx;
	e->zone_offset = zone_offset;
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
	hash_init(c->map);

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		goto err_free;
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
err_free:
	kfree(c);
	return ret;
}

static void zns_m1_dtr(struct dm_target *ti)
{
	struct zns_m1_c *c = ti->private;
	struct map_entry *e;
	struct hlist_node *tmp;
	unsigned int bkt;

	hash_for_each_safe(c->map, bkt, tmp, e, node) {
		hash_del(&e->node);
		kfree(e);
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
	.version = { 0, 2, 0 },
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

MODULE_DESCRIPTION("ZNS M1/M2: random-to-sequential DM target");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
