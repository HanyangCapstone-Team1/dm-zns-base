// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M0 scaffold for the capstone project.
 *
 * Registers a zoned-aware pass-through DM target on top of a host-managed
 * zoned device. The random-to-sequential translation that the project is
 * actually about is left out on purpose — that's the student's work.
 * See docs/07-milestones.md.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/blkzoned.h>

#define DM_MSG_PREFIX "zns-m1"
#define MAP_HASH_BITS   15      // 해시 테이블 크기

// Zone 하나의 상태
struct zone_state {
    sector_t    wp;         // 현재 write pointer
    sector_t    capacity;   // zone 용량 (sectors)
    bool        is_active;  // 현재 쓰기 대상 zone인지
    bool        is_full;    // wp == capacity
};

// Mapping Table 엔트리 하나
struct map_entry {
    sector_t        logical_sector;   // 키: ext4가 요청한 LBA
    u32             zone_idx;         // 값: 물리 zone 번호
    sector_t        zone_offset;      // 값: zone 내 sector 오프셋
    struct hlist_node node;
};

struct zns_m1_c {
    struct dm_dev *dev;

    // M1
    u32                 nr_zones;
    sector_t            zone_size;
    struct zone_state   *zones;         // nr_zones개 배열
    u32                 active_zone;    // 현재 쓰기 중인 zone 인덱스

    DECLARE_HASHTABLE(map, MAP_HASH_BITS);  // 매핑 테이블

    spinlock_t          lock;
};

static int zns_m1_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    struct zns_m1_c *c;
    int ret;

    struct block_device *bdev;
    unsigned int nr_rep;

    if (argc != 1) {
        ti->error = "expected one argument: underlying device";
        return -EINVAL;
    }

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        ti->error = "out of memory";
        return -ENOMEM;
    }

    ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
                &c->dev);
    if (ret) {
        ti->error = "failed to open underlying device";
        kfree(c);
        return ret;
    }

    bdev = c->dev->bdev;

    // zone 크기와 개수 파악
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

    // 실제 Zone 상태 읽기
    nr_rep = c->nr_zones;
    ret = blkdev_report_zones(bdev, ti->begin, nr_rep, fill_zone_cb, c);
    if (ret < 0) {
        ti->error = "blkdev_report_zones failed";
        goto err_zones;
    }

    // Zone 0을 actice zone으로 시작
    c->active_zone = 0;
    c->zones[0].is_active = true;

    ti->private = c;
    ti->num_flush_bios = 1;
    ti->num_discard_bios = 1;

    DMINFO("ctr: target attached on top of '%s'", argv[0]);
    return 0;

err_zones:
    kfree(c->zones);
err_put:
    dm_put_device(ti, c->dev);
    kfree(c);
    return ret;
}

static void zns_m1_dtr(struct dm_target *ti)
{
    struct zns_m1_c *c = ti->private;

    struct map_entry *e;
    struct hlist_node *tmp;
    unsigned int bkt;

    // 매핑 테이블 해제
    hash_for_each_safe(c->map, bkt, tmp, e, node) {
        hash_del(&e->node);
        kfree(e);
    }

    kfree(c->zones);

    dm_put_device(ti, c->dev);
    kfree(c);
    DMINFO("dtr: target detached");
}

static int zns_m1_map(struct dm_target *ti, struct bio *bio)
{
    struct zns_m1_c *c = ti->private;

    /* Student work goes here: translate random writes into sequential ones. */


    bio_set_dev(bio, c->dev->bdev);
    return DM_MAPIO_REMAPPED;
}

/* 1:1 mapping, so ti->begin is passed straight through. A non-identity
 * mapping would need to translate args->next_sector. */
static int zns_m1_report_zones(struct dm_target *ti,
                 struct dm_report_zones_args *args,
                 unsigned int nr_zones)
{
    struct zns_m1_c *c = ti->private;

    return dm_report_zones(c->dev->bdev, ti->begin,
                   args->next_sector, args, nr_zones);
}

/* DM_TARGET_ZONED_HM is just a capability flag. Without this callback the
 * underlying device's chunk_sectors and zoned attributes never propagate up
 * to the DM queue, and blkzone fails with "unable to determine zone size". */
static int zns_m1_iterate_devices(struct dm_target *ti,
                    iterate_devices_callout_fn fn, void *data)
{
    struct zns_m1_c *c = ti->private;

    return fn(ti, c->dev, 0, ti->len, data);
}

static struct target_type zns_m1_target = {
    .name            = "zns-m1",
    .version         = {0, 1, 0},
    .features        = 0,               // DM_TARGET_ZONED_HM 제거
    .module          = THIS_MODULE,
    .ctr             = zns_m1_ctr,
    .dtr             = zns_m1_dtr,
    .map             = zns_m1_map,
    // .report_zones    = zns_base_report_zones,
    // .iterate_devices = zns_base_iterate_devices,
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

MODULE_DESCRIPTION("ZNS M1: random-to-sequential DM target (in-memory mapping)");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
