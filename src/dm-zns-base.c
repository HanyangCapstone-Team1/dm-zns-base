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
#include <linux/mutex.h>
#include <linux/blkzoned.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mm.h>

#define DM_MSG_PREFIX "zns-m1"
#define MAP_BLOCK_SIZE 4096
#define MAP_GRANULARITY_SECTORS (MAP_BLOCK_SIZE >> SECTOR_SHIFT)
#define MEMTABLE_MAX_ENTRIES 1024
#define RUNS_PER_COMPACTION 4
#define MAX_LSM_LEVELS 16
#define CLONE_BIO_POOL_SIZE 128

struct zone_state {
   sector_t wp;
   sector_t capacity;
   u64 invalid_sectors;
   bool is_active;
   bool is_full;
};

struct map_entry {
   sector_t logical_sector;
   u32 zone_idx;
   sector_t zone_offset;
   u64 sequence;
   bool tombstone;
   struct rb_node node;
};

struct zns_m1_c;

struct zns_io_work {
   struct work_struct work;
   struct zns_m1_c *ctx;
   struct dm_target *ti;
   struct bio *bio;
   bool is_flush;
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
   struct workqueue_struct *io_wq;
   struct bio_set clone_bioset;
   struct mutex lock;
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
             sector_t zone_offset, u64 sequence, bool tombstone,
             gfp_t gfp, bool replace_newer)
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
            e->tombstone = tombstone;
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
   e->tombstone = tombstone;
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

   run = kmalloc(sizeof(*run), GFP_KERNEL);
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
            sector_t zone_offset, bool tombstone)
{
   struct map_entry *old;
   u32 old_zone_idx = 0;
   bool old_mapping_valid = false;
   int ret;

   old = map_lookup(c, logical);
   if (old && !old->tombstone && old->zone_idx < c->nr_zones) {
      old_zone_idx = old->zone_idx;
      old_mapping_valid = true;
   }

   ret = tree_insert(&c->active_memtable, logical, zone_idx, zone_offset,
           ++c->next_sequence, tombstone, GFP_KERNEL, false);
   if (ret < 0)
      return ret;

   if (old_mapping_valid)
      c->zones[old_zone_idx].invalid_sectors +=
         MAP_GRANULARITY_SECTORS;

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
   c->zones[idx].invalid_sectors = 0;
   c->zones[idx].is_full = zone->cond == BLK_ZONE_COND_FULL;
   c->zones[idx].is_active = false;
   return 0;
}

static int find_initial_active_zone(struct zns_m1_c *c)
{
   u32 i;

   for (i = 0; i < c->nr_zones; i++) {
      if (!c->zones[i].is_full &&
          c->zones[i].wp < c->zones[i].capacity &&
          IS_ALIGNED(c->zones[i].wp, MAP_GRANULARITY_SECTORS)) {
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

   while (1) {
      struct sorted_run *output;
      u64 generation = 0;
      u32 level = 0;
      unsigned int nr;
      unsigned int i;
      int ret = 0;

      mutex_lock(&c->lock);
      if (c->stopping) {
         c->compaction_running = false;
         mutex_unlock(&c->lock);
         return;
      }

      nr = select_compaction(c, selected, &level);
      if (!nr) {
         c->compaction_running = false;
         mutex_unlock(&c->lock);
         return;
      }
      mutex_unlock(&c->lock);

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
                  e->tombstone, GFP_KERNEL, true);
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
      mutex_lock(&c->lock);
      for (i = 0; i < nr; i++) {
         list_del(&selected[i]->list);
         c->nr_runs--;
      }
      list_add_tail(&output->list, &c->runs);
      c->nr_runs++;
      mutex_unlock(&c->lock);

      for (i = 0; i < nr; i++)
         free_run(selected[i]);

      DMINFO("compaction: L%u %u runs -> L%u (%u entries)",
             level, nr, output->level, output->nr_entries);
      continue;

failed:
      mutex_lock(&c->lock);
      unmark_compaction(selected, nr);
      c->compaction_running = false;
      mutex_unlock(&c->lock);
      DMERR("compaction failed: %d", ret);
      return;
   }
}

static int submit_clone_wait(struct zns_m1_c *c, struct bio *bio,
              sector_t physical_sector, bool remap_sector)
{
   struct bio *clone;
   int ret;

   clone = bio_alloc_clone(c->dev->bdev, bio, GFP_NOIO,
            &c->clone_bioset);
   if (!clone)
      return -ENOMEM;

   if (remap_sector)
      clone->bi_iter.bi_sector = physical_sector;

   ret = submit_bio_wait(clone);
   bio_put(clone);
   return ret;
}

static int submit_zero_write_wait(struct zns_m1_c *c, struct bio *orig,
              sector_t physical_sector)
{
   struct bio *write_bio;
   unsigned int bytes = MAP_GRANULARITY_SECTORS << SECTOR_SHIFT;
   int ret;

   write_bio = bio_alloc(c->dev->bdev, 1, REQ_OP_WRITE, GFP_NOIO);
   if (!write_bio)
      return -ENOMEM;

   write_bio->bi_iter.bi_sector = physical_sector;
   write_bio->bi_opf |= orig->bi_opf &
      (REQ_SYNC | REQ_META | REQ_PRIO | REQ_FUA);
   if (bio_add_page(write_bio, ZERO_PAGE(0), bytes, 0) != bytes) {
      bio_put(write_bio);
      return -EIO;
   }

   ret = submit_bio_wait(write_bio);
   bio_put(write_bio);
   return ret;
}

static void complete_original_bio(struct bio *bio, int ret)
{
   if (ret)
      bio->bi_status = errno_to_blk_status(ret);
   bio_endio(bio);
}

static int process_read(struct zns_io_work *io)
{
   struct zns_m1_c *c = io->ctx;
   struct bio *bio = io->bio;
   struct map_entry *e;
   sector_t physical = 0;
   bool zero = false;

   mutex_lock(&c->lock);
   e = map_lookup(c, bio->bi_iter.bi_sector);
   if (!e || e->tombstone)
      zero = true;
   else
      physical = io->ti->begin +
         (sector_t)e->zone_idx * c->zone_size + e->zone_offset;
   mutex_unlock(&c->lock);

   if (zero) {
      zero_fill_bio(bio);
      return 0;
   }

   return submit_clone_wait(c, bio, physical, true);
}

static int process_write(struct zns_io_work *io)
{
   struct zns_m1_c *c = io->ctx;
   struct bio *bio = io->bio;
   struct zone_state *az;
   sector_t physical;
   u32 zone_idx;
   int ret;

   mutex_lock(&c->lock);
   az = &c->zones[c->active_zone];
   if (az->wp + MAP_GRANULARITY_SECTORS > az->capacity) {
      az->is_full = true;
      ret = advance_active_zone(c);
      if (ret) {
         mutex_unlock(&c->lock);
         DMERR("device full, no free zones");
         return ret;
      }
      az = &c->zones[c->active_zone];
   }

   zone_idx = c->active_zone;
   physical = io->ti->begin + (sector_t)zone_idx * c->zone_size + az->wp;
   mutex_unlock(&c->lock);

   /* 하위 장치의 WRITE_ZEROES 지원 여부와 관계없이 순차 WRITE로 기록함. */
   if (bio_op(bio) == REQ_OP_WRITE_ZEROES)
      ret = submit_zero_write_wait(c, bio, physical);
   else
      ret = submit_clone_wait(c, bio, physical, true);
   if (ret)
      return ret;

   mutex_lock(&c->lock);
   az = &c->zones[zone_idx];
   az->wp += MAP_GRANULARITY_SECTORS;
   ret = map_insert(c, bio->bi_iter.bi_sector, zone_idx,
          az->wp - MAP_GRANULARITY_SECTORS, false);
   mutex_unlock(&c->lock);
   return ret;
}

static int process_discard(struct zns_io_work *io)
{
   struct zns_m1_c *c = io->ctx;
   sector_t logical = io->bio->bi_iter.bi_sector;
   sector_t end = logical + bio_sectors(io->bio);
   int ret = 0;

   mutex_lock(&c->lock);
   while (logical < end) {
      ret = map_insert(c, logical, 0, 0, true);
      if (ret)
         break;
      logical += MAP_GRANULARITY_SECTORS;
   }
   mutex_unlock(&c->lock);
   return ret;
}

static void process_io(struct work_struct *work)
{
   struct zns_io_work *io = container_of(work, struct zns_io_work, work);
   struct bio *bio = io->bio;
   int ret;

   if (io->is_flush) {
      ret = submit_clone_wait(io->ctx, bio, 0, false);
      complete_original_bio(bio, ret);
      return;
   }

   switch (bio_op(bio)) {
   case REQ_OP_READ:
      ret = process_read(io);
      break;
   case REQ_OP_WRITE:
   case REQ_OP_WRITE_ZEROES:
      ret = process_write(io);
      break;
   case REQ_OP_DISCARD:
      ret = process_discard(io);
      break;
   default:
      DMERR("unsupported bio operation: %u", bio_op(bio));
      ret = -EOPNOTSUPP;
      break;
   }

   complete_original_bio(bio, ret);
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

   mutex_init(&c->lock);
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

   ret = bioset_init(&c->clone_bioset, CLONE_BIO_POOL_SIZE, 0,
           BIOSET_NEED_RESCUER);
   if (ret) {
      ti->error = "failed to create clone bioset";
      goto err_compaction_wq;
   }

   c->io_wq = alloc_ordered_workqueue("zns_io", WQ_MEM_RECLAIM);
   if (!c->io_wq) {
      ti->error = "failed to create I/O workqueue";
      ret = -ENOMEM;
      goto err_bioset;
   }

   ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
             &c->dev);
   if (ret) {
      ti->error = "failed to open underlying device";
      goto err_io_wq;
   }

   bdev = c->dev->bdev;
   if (MAP_BLOCK_SIZE % bdev_logical_block_size(bdev)) {
      ti->error = "4 KiB mapping is not aligned to underlying device";
      ret = -EINVAL;
      goto err_put;
   }

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
    * 매핑 하나가 4 KiB block 하나를 나타내도록 했음.
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
   ti->num_discard_bios = 1;
   ti->num_write_zeroes_bios = 1;
   ti->per_io_data_size = sizeof(struct zns_io_work);
   ti->flush_supported = true;
   ti->discards_supported = true;
   ti->max_write_zeroes_granularity = true;

   DMINFO("ctr: target attached on top of '%s' (%u zones, %llu sectors/zone)",
          argv[0], c->nr_zones, (unsigned long long)c->zone_size);
   return 0;

err_zones:
   kfree(c->zones);
err_put:
   dm_put_device(ti, c->dev);
err_io_wq:
   destroy_workqueue(c->io_wq);
err_bioset:
   bioset_exit(&c->clone_bioset);
err_compaction_wq:
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

   destroy_workqueue(c->io_wq);

   mutex_lock(&c->lock);
   c->stopping = true;
   mutex_unlock(&c->lock);
   cancel_work_sync(&c->compaction_work);
   destroy_workqueue(c->compaction_wq);
   bioset_exit(&c->clone_bioset);

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

static int zns_m1_map(struct dm_target *ti, struct bio *bio)
{
   struct zns_m1_c *c = ti->private;
   struct zns_io_work *io;
   sector_t sector = bio->bi_iter.bi_sector;
   sector_t nr_sectors = bio_sectors(bio);
   bool is_flush = bio_op(bio) == REQ_OP_FLUSH ||
      (!nr_sectors && (bio->bi_opf & REQ_PREFLUSH));

   if (!is_flush) {
      if (!IS_ALIGNED(sector, MAP_GRANULARITY_SECTORS) ||
          !IS_ALIGNED(nr_sectors, MAP_GRANULARITY_SECTORS) ||
          !nr_sectors) {
         DMERR("unaligned bio: op=%u sector=%llu sectors=%llu",
               bio_op(bio), (unsigned long long)sector,
               (unsigned long long)nr_sectors);
         return DM_MAPIO_KILL;
      }

      if (bio_op(bio) != REQ_OP_DISCARD &&
          nr_sectors != MAP_GRANULARITY_SECTORS)
         return DM_MAPIO_KILL;
   }

   io = dm_per_bio_data(bio, sizeof(*io));
   INIT_WORK(&io->work, process_io);
   io->ctx = c;
   io->ti = ti;
   io->bio = bio;
   io->is_flush = is_flush;
   queue_work(c->io_wq, &io->work);
   return DM_MAPIO_SUBMITTED;
}

static void zns_m1_io_hints(struct dm_target *ti,
             struct queue_limits *limits)
{
   unsigned int max_discard = min_t(sector_t, ti->len, UINT_MAX);

   limits->logical_block_size = MAP_BLOCK_SIZE;
   limits->physical_block_size = MAP_BLOCK_SIZE;
   limits->io_min = MAP_BLOCK_SIZE;
   limits->io_opt = MAP_BLOCK_SIZE;
   limits->discard_granularity = MAP_BLOCK_SIZE;
   limits->discard_alignment = 0;
   limits->max_discard_sectors = max_discard;
   limits->max_hw_discard_sectors = max_discard;
   limits->max_discard_segments = 1;
   limits->max_write_zeroes_sectors = MAP_GRANULARITY_SECTORS;
}

static int zns_m1_iterate_devices(struct dm_target *ti,
              iterate_devices_callout_fn fn, void *data)
{
   // struct zns_m1_c *c = ti->private;
   // return fn(ti, c->dev, 0, ti->len, data);
   return 0;
}

static struct target_type zns_m1_target = {
   .name = "zns-m1",
   .version = { 0, 4, 0 },
   .features = 0,
   .module = THIS_MODULE,
   .ctr = zns_m1_ctr,
   .dtr = zns_m1_dtr,
   .map = zns_m1_map,
   .io_hints = zns_m1_io_hints,
   .iterate_devices = zns_m1_iterate_devices,
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


