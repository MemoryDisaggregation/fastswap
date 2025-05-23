#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include "rpage_allocator.h"

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/shmem_fs.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/smp.h>
#include <linux/delay.h>

atomic_t num_alloc_blocks = ATOMIC_INIT(0);
EXPORT_SYMBOL(num_alloc_blocks);

atomic_t num_free_blocks = ATOMIC_INIT(0);
EXPORT_SYMBOL(num_free_blocks);

atomic_t num_free_fail = ATOMIC_INIT(0);
EXPORT_SYMBOL(num_free_fail);

u32 get_rkey(u64 raddr) {
    struct block_info *bi = NULL;
    
    BUG_ON((raddr & ((1 << BLOCK_SHIFT) - 1)) != 0);

    bi = rhashtable_lookup_fast(blocks_map, &raddr, blocks_map_params);
    if(!bi/* || bi->rkey == 0*/) {
        pr_err("cannot get rkey(with remote address:%p)\n", (void*)raddr);
        return 0;
    }
    return bi->rkey;
}
EXPORT_SYMBOL(get_rkey);

void cpu_cache_dump(void) {
    pr_info("cpu_cache_ block_size = %lld\n", cpu_cache_->block_size);
}
EXPORT_SYMBOL(cpu_cache_dump);

void cpu_cache_delete(void) {
    vunmap(cpu_cache_);
}
EXPORT_SYMBOL(cpu_cache_delete);

int cpu_cache_init(void) {
    struct path path_;
    struct address_space *addr_space_;
    struct page *page_;
    struct page **pages_ = NULL;
    void **slot_;
    struct radix_tree_iter iter_;
    int i = 0;
    int ret;

    ret = kern_path("/dev/shm/cpu_cache", LOOKUP_FOLLOW, &path_);
    if (ret != 0) {
        // handle error
        pr_err("debug: cannot find /cpu_cache with error code %d\n", ret);
        return -1;
    }

    addr_space_ = path_.dentry->d_inode->i_mapping;
    if(addr_space_ == NULL) {
        pr_err("cannot get address space\n");
        return -1;
    }
    pr_info("num of pages: %ld\n", addr_space_->nrpages);

    pages_ = (struct page **) kmalloc(sizeof(struct page *) * addr_space_->nrpages, GFP_KERNEL);
    if(pages_ == NULL) {
        pr_err("Bad alloc for pages_(struct page**)\n");
        return -1;
    }
    
    radix_tree_iter_init(&iter_, 0);
    radix_tree_for_each_slot(slot_, &addr_space_->i_pages, &iter_, 0) {
        page_ = radix_tree_deref_slot(slot_);
        // do something with page
        pages_[i] = page_;
        pr_info("%d page ptr: %p\n", i, pages_[i]);
        i++;
    }

    if(i != addr_space_->nrpages) {
        pr_info("i != nrpages\n");
    } else {
        pr_info("i == nrpages\n");
    }
    // return 0;

    cpu_cache_ = (struct cpu_cache_storage *) vmap(pages_, addr_space_->nrpages, VM_MAP, PAGE_KERNEL);
    if(cpu_cache_ == NULL) {
        pr_err("Bad v-mapping for cpu_cache_\n");
        kfree(pages_);
        return -1;
    }

    pr_info("cpu_cache_ address is %p\n", (void*)cpu_cache_);

    kfree(pages_);
    return 0;
}
EXPORT_SYMBOL(cpu_cache_init);

u32 get_length_fetch(u32 nproc) {
    u32 writer = cpu_cache_->writer[nproc];
    u32 reader = cpu_cache_->reader[nproc];
    if (writer == reader) {
        return 0;
    }
    if (writer > reader) {
        return (writer - reader);
    } else {
        return (max_alloc_item - reader + writer);
    }
}

u32 get_length_free(u32 nproc) {
    u32 writer = cpu_cache_->free_writer[nproc];
    u32 reader = cpu_cache_->free_reader[nproc];
    if (writer == reader) {
        return 0;
    }
    if (writer > reader) {
        return (writer - reader);
    } else {
        return (max_free_item - reader + writer);
    }
}

int fetch_cache(u64 *raddr, u32 *rkey) {
    u32 nproc = raw_smp_processor_id();
    u32 reader;
    // struct raddr_rkey fetch_one;

    BUG_ON(nproc > nprocs);

    reader = cpu_cache_->reader[nproc];
    BUG_ON(reader >= max_alloc_item);

    while(get_length_fetch(nproc) == 0) ;
    cpu_cache_->reader[nproc] = (cpu_cache_->reader[nproc] + 1) % max_alloc_item;

    while(cpu_cache_->items[nproc][reader].addr == -1 || cpu_cache_->items[nproc][reader].rkey == -1) ;
    
    *raddr = cpu_cache_->items[nproc][reader].addr;
    *rkey = cpu_cache_->items[nproc][reader].rkey;
    
    BUG_ON(*raddr == 0);
    BUG_ON(*rkey == 0);

    cpu_cache_->items[nproc][reader].addr = -1;
    cpu_cache_->items[nproc][reader].rkey = -1;

    return 0;
}

void add_free_cache(u64 raddr/*, u32 rkey*/) {
    u32 nproc = raw_smp_processor_id();
    u32 writer;
    
    BUG_ON(nproc > nprocs);
    writer = cpu_cache_->free_writer[nproc];

    if(get_length_free(nproc) < max_free_item - 1) {
        cpu_cache_->free_writer[nproc] = (cpu_cache_->free_writer[nproc] + 1) % max_free_item;
    } else {
        atomic_inc(&num_free_fail);
        return;
    }
 
    cpu_cache_->free_items[nproc][writer] = raddr;
}

// must obtain "free_blocks_list_lock" when excute this function
int alloc_remote_block(u32 free_list_idx) {
    struct block_info *bi;
    u64 raddr_ = 0;
    u32 rkey_ = 0;
    int ret;

    ret = fetch_cache(&raddr_, &rkey_);
    if(ret) {
        pr_err("fetch cache error.\n");
        return -1;
    }

    BUG_ON(raddr_ == 0 || rkey_ == 0 || raddr_ == -1 || rkey_ == -1);

    //pr_info("fetch a block with raddr = %p, rkey = %u\n", (void*)raddr_, rkey_);
    
    bi = kmalloc(sizeof(struct block_info), GFP_KERNEL);
    if(!bi) {
        pr_err("init block meta data failed.\n");
        return -1;
    }

    // block_info init
    bi->raddr = raddr_;
    bi->rkey = rkey_;
    bi->cnt = rblock_size >> PAGE_SHIFT;
    bi->free_list_idx = free_list_idx;
    spin_lock_init(&(bi->block_lock));
    bitmap_zero(bi->rpages_bitmap, rblock_size >> PAGE_SHIFT);
    INIT_LIST_HEAD(&bi->block_node_list);

    // insert to rhashtable (blocks_map)
    rhashtable_insert_fast(blocks_map, &bi->block_node_rhash, blocks_map_params);
    
    // insert to free block list
    //spin_lock(&free_blocks_list_lock);
    list_add(&bi->block_node_list, free_blocks_lists + free_list_idx);
    //spin_unlock(&free_blocks_list_lock);

    atomic_inc(&num_alloc_blocks);
    return 0;
}
EXPORT_SYMBOL(alloc_remote_block);



u64 alloc_remote_page(void) {
    struct block_info *bi/*, *entry, *next_entry*/;
    u32 offset;
    u64 raddr;
    int ret;
    //int counter = 0;
    //int flag;
    u32 nproc = raw_smp_processor_id();
    u32 free_list_idx = nproc % num_free_lists;
    u32 raw_free_list_idx = free_list_idx;
    u8 locked = 0;

    do{
        if(spin_trylock(free_blocks_list_locks + free_list_idx)) {
            if(!list_empty(free_blocks_lists + free_list_idx)) {
                locked = 1;
                break;
            } else {
                spin_unlock(free_blocks_list_locks + free_list_idx);
            }
        } 
        /*
        spin_lock(free_blocks_list_locks + free_list_idx);
        if(!list_empty(free_blocks_lists + free_list_idx)) {
            locked = 1;
            break;
        } else {
            spin_unlock(free_blocks_list_locks + free_list_idx);
        }*/
        free_list_idx = (free_list_idx + 1) % num_free_lists;
    }while(free_list_idx != raw_free_list_idx);

    if(locked == 0) {
        spin_lock(free_blocks_list_locks + free_list_idx);
    }

    if(list_empty(free_blocks_lists + free_list_idx)) {
        ret = alloc_remote_block(free_list_idx);
        if(ret) {
            pr_err("cannot fetch a block from cache.\n");
            spin_unlock(free_blocks_list_locks + free_list_idx);
            return 0;
        }
    }

    bi = list_first_entry(free_blocks_lists + free_list_idx, struct block_info, block_node_list);
    if(!bi) {
        pr_err("fail to add new block to free_blocks_list\n");
        spin_unlock(free_blocks_list_locks + free_list_idx);
        return 0;
    }

    //BUG_ON(bi->free_list_idx != free_list_idx);
    if(bi->free_list_idx != free_list_idx) {
        pr_err("block_info's free_list_idx error: 2\n");
    }

    spin_lock(&bi->block_lock);
    offset = find_first_zero_bit(bi->rpages_bitmap, rblock_size >> PAGE_SHIFT);
    BUG_ON(offset == (rblock_size >> PAGE_SHIFT));
    set_bit(offset, bi->rpages_bitmap);
    
    bi->cnt -= 1;
    BUG_ON(bi->cnt > (rblock_size >> PAGE_SHIFT));

    if(bi->cnt == 0) {
        list_del(&bi->block_node_list);
        bi->free_list_idx = num_free_lists;
    }

    spin_unlock(&bi->block_lock);

    /*
    counter = 0;
    list_for_each_entry_safe(entry, next_entry, free_blocks_lists + nproc, block_node_list) {
        spin_lock(&entry->block_lock);
        BUG_ON(entry->free_list_idx != nproc);
        //if(entry->free_list_idx != nproc) {
        //    pr_err("block_info's free_list_idx error: 2\n");
        //}
        flag = 0;
        if(entry->cnt == (rblock_size >> PAGE_SHIFT)) {
            counter++;
            if(counter > 1) {
                free_remote_block(entry);
                flag = 1;
            }
        }
        if(flag == 0) {
            spin_unlock(&entry->block_lock);
        }
    }*/
    spin_unlock(free_blocks_list_locks + free_list_idx);

    raddr = bi->raddr + (offset << PAGE_SHIFT);
    return raddr;
}
EXPORT_SYMBOL(alloc_remote_page);

void free_remote_page(u64 raddr) {
    struct block_info *bi = NULL;
    u64 raddr_block; 
    u32 offset; 
    u32 nproc = raw_smp_processor_id();
    u32 free_list_idx = nproc % num_free_lists;
    
    BUG_ON((raddr & ((1 << PAGE_SHIFT) - 1)) != 0);

    raddr_block = raddr >> BLOCK_SHIFT;
    raddr_block = raddr_block << BLOCK_SHIFT;
    bi = rhashtable_lookup_fast(blocks_map, &raddr_block, blocks_map_params);
    if(!bi) {
        pr_err("the page being free(%p) is not exit: cannot find out block_info.\n", (void*)raddr);
        return;
    }

    BUG_ON(raddr_block != bi->raddr);
    BUG_ON(raddr < bi->raddr);

    //spin_lock(free_blocks_list_locks + nproc);
    spin_lock(&bi->block_lock);

    offset = (raddr - bi->raddr) >> PAGE_SHIFT;
    BUG_ON(offset >= (rblock_size >> PAGE_SHIFT));

    if(test_bit(offset, bi->rpages_bitmap)) {
        clear_bit(offset, bi->rpages_bitmap);

        bi->cnt += 1;
        //if(bi->cnt == (rblock_size >> PAGE_SHIFT)) {
        //    free_remote_block(bi);
        //    spin_unlock(free_blocks_list_locks + nproc);
        //    return; // no need to release block's lock
        //} else if(bi->cnt == 1) {
        if(bi->cnt == 1) {
            //BUG_ON(bi->free_list_idx != num_free_lists);
            if(bi->free_list_idx != num_free_lists) {
                pr_err("block_info's free_list_idx error: 3\n");
            }
            //while (!spin_trylock(free_blocks_list_locks + free_list_idx)) {
            //    msleep(10);
            //}
            spin_lock(free_blocks_list_locks + free_list_idx);
            bi->free_list_idx = free_list_idx;
            list_add(&bi->block_node_list, free_blocks_lists + free_list_idx);
            spin_unlock(free_blocks_list_locks + free_list_idx);
        }
    }
    else {
        // error handler...
        pr_err("the page being free(%p) is not exit: bitmap is incorrect.\n", (void*)raddr);
        // return;
    }

    spin_unlock(&bi->block_lock);
    //spin_unlock(free_blocks_list_locks + nproc);
}
EXPORT_SYMBOL(free_remote_page);

// must obtain free_blocks_list_lock when excute this function
void free_remote_block(struct block_info *bi) {
    list_del(&bi->block_node_list);
    rhashtable_remove_fast(blocks_map, &bi->block_node_rhash, blocks_map_params);

    add_free_cache(bi->raddr/*, bi->rkey*/);

    kfree(bi);

    atomic_inc(&num_free_blocks);
}
EXPORT_SYMBOL(free_remote_block);

void gc_timer_callback(struct timer_list *timer) {
  struct block_info *entry, *next_entry;  
  u32 counter = 0;
  int i;

  for(i = 0;i < num_free_lists; ++i) {
    if(spin_trylock(free_blocks_list_locks + i)) {
        list_for_each_entry_safe(entry, next_entry, free_blocks_lists + i, block_node_list) {
            spin_lock(&entry->block_lock);
            //BUG_ON(entry->free_list_idx != i);
            if(entry->free_list_idx != i) {
                pr_err("entry's free list idx error: 1\n");
            }
            if(entry->cnt == (rblock_size >> PAGE_SHIFT)) {
                counter++;
                free_remote_block(entry);
                continue;
            }
            spin_unlock(&entry->block_lock);
        }
        spin_unlock(free_blocks_list_locks + i);
    }
  }

  mod_timer(timer, jiffies + msecs_to_jiffies(rblock_gc_interval)); 
}

static int __init rpage_allocator_init_module(void) {
    int ret = 0;
    int i = 0;

    ret = cpu_cache_init();
    if (ret) {
        pr_err("cpu cache init failed\n");
        return ret;
    }

    cpu_cache_dump();

    blocks_map = kmalloc(sizeof(struct rhashtable), GFP_KERNEL);
    if (!blocks_map) {
        pr_err("alloc memory for blocks_map failed\n");
        return -1;
    }

    rhashtable_init(blocks_map, &blocks_map_params);

    for(i = 0; i < num_free_lists ; ++i) {
        INIT_LIST_HEAD(free_blocks_lists + i);
        spin_lock_init(free_blocks_list_locks + i);
    }

    timer_setup(&gc_timer, gc_timer_callback, 0);
    mod_timer(&gc_timer, jiffies + msecs_to_jiffies(rblock_gc_interval));

    return 0;
}

static void __exit rpage_allocator_cleanup_module(void) {
    cpu_cache_delete();
    kfree(blocks_map);
}

module_init(rpage_allocator_init_module);
module_exit(rpage_allocator_cleanup_module);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Remote Page Allocator");