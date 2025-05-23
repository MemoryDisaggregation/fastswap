#include <linux/types.h>
#include <linux/bitmap.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/rhashtable.h>
#include <linux/module.h>

#define addr_space (1024 * 1024 * 1024 * 200l)
#define rblock_size (4 * 1024 * 1024)
#define max_block_num (addr_space / rblock_size)
#define BLOCK_SHIFT 22
#define MB_SHIFT 20

// u32 num_cpus = num_online_cpus();
#define nprocs 128
#define max_alloc_item 256
#define max_free_item 20480
#define max_class_free_item 512
#define class_num 16
#define rblock_gc_interval 500
#define num_free_lists 8

extern atomic_t num_alloc_blocks;
extern atomic_t num_free_blocks;
extern atomic_t num_free_fail;


struct raddr_rkey{
    u64 addr;
    u32 rkey;
};

struct item {
    u64 addr;
    u32 rkey;
};

struct cpu_cache_storage {
    u64 block_size;
    struct raddr_rkey items[nprocs][max_alloc_item];
    u64 free_items[nprocs][max_free_item];
    struct raddr_rkey class_items[class_num][max_alloc_item];
    u64 class_free_items[class_num][max_class_free_item];

    u32 reader[nprocs];
    u32 class_reader[class_num];
    u32 free_reader[nprocs];
    u32 class_free_reader[nprocs];

    u32 writer[nprocs];
    u32 class_writer[class_num];
    u32 free_writer[nprocs];
    u32 class_free_writer[nprocs];
};

struct block_info{
    u64 raddr;
    u32 rkey;
    spinlock_t block_lock;
    u16 cnt;
    u32 free_list_idx;
    DECLARE_BITMAP(rpages_bitmap, (rblock_size >> PAGE_SHIFT));

    struct rhash_head block_node_rhash;
    struct list_head block_node_list;
};

struct rhashtable_params blocks_map_params = {
    .head_offset = offsetof(struct block_info, block_node_rhash),
    .key_offset = offsetof(struct block_info, raddr),
    .key_len = sizeof(((struct block_info *)0)->raddr),
    .hashfn = jhash,
    // .nulls_base = (1U << RHT_BASE_SHIFT), 
    // not support in kernel 5.15
};

struct rhashtable *blocks_map = NULL;
struct list_head free_blocks_lists[num_free_lists];
spinlock_t free_blocks_list_locks[num_free_lists];

struct cpu_cache_storage *cpu_cache_ = NULL;
struct timer_list gc_timer;

int cpu_cache_init(void);
void cpu_cache_dump(void);
void cpu_cache_delete(void);

int alloc_remote_block(u32 free_list_idx);
void free_remote_block(struct block_info *bi);
u64 alloc_remote_page(void);
void free_remote_page(u64 raddr);
int fetch_cache(u64 *raddr, u32 *rkey);
void add_free_cache(u64 raddr/*, u32 rkey*/);
u32 get_rkey(u64 raddr);
