 /*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/queue.h"

#include "bdev_raid.h"
#include "service.h"

#define DEBUG__

/* ============================ TESTS ==================================== */

// TODO: Сейчас iovcnt = 1, доделать, чтоб можно было создавать любого количества
#define _MUX_BUF_LENGTH 1
// ----- //

/* ======================= Poller functionality =========================== */

/*
 * The function shold be run after rebuilding of concrete area from iteration
 */
static inline void
partly_submit_iteration(bool result ,uint64_t iter_idx, uint16_t area_idx, struct raid_rebuild *rebuild)
{
    struct rebuild_cycle_iteration *iter = &(rebuild->cycle_progress->cycle_iteration);

    if(result)
    {
        SPDK_REMOVE_BIT(&(rebuild->rebuild_matrix[iter_idx]), area_idx);
    }

    ATOMIC_INCREMENT(&(iter->pr_area_cnt));
}

static inline void
_free_sg_buffer_part(struct iovec *vec_array, uint64_t len)
{ //TODO: проверить, что оно работает (в for совсем неуверен)
    struct iovec *base_vec;
    //TODO: мб вообще не надо.

    for(base_vec = vec_array; base_vec < vec_array + len; base_vec++)
    {
        spdk_dma_free(base_vec->iov_base);
    }
}

static inline void
free_sg_buffer(struct iovec *vec_array, uint64_t len)
{
    /* usage: struct iovec *a; free_sg_buffer(&a, b); */
    if(len != 0)
    {
        _free_sg_buffer_part(vec_array, len);
    }    
    free(vec_array);
}

uint64_t
get_area_offset(size_t area_idx, size_t area_size, size_t strip_size)
{
    return area_idx*area_size*strip_size;
} 

uint64_t
get_area_size(size_t area_size, size_t strip_size)
{
    return area_size*strip_size;
}

static inline struct iovec *
allocate_sg_buffer(size_t elem_size, size_t elemcnt, size_t align)
{
    struct iovec *vec_array = calloc(elemcnt, sizeof(struct iovec));
    if(vec_array == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < elemcnt; i++)
    { //TODO: люблю лажать с индексами, проверить, что все ок
        vec_array[i].iov_len = elem_size;
        vec_array[i].iov_base = (void*)spdk_dma_zmalloc(sizeof(uint8_t)*vec_array[i].iov_len, align, NULL);
        if(vec_array[i].iov_base == NULL)
        {
            _free_sg_buffer_part(vec_array, i);
            free(vec_array);
            return NULL;
        }
    }
    return vec_array;
}

void
reset_buffer(struct iovec *vec_array, uint32_t len)
{
    struct iovec *base_vec;
    if(len == 0) return;

    for(base_vec = vec_array; base_vec < vec_array + len; base_vec++)
    {
        memset(base_vec->iov_base, 0, base_vec->iov_len);
    }
}

static inline void
_free_base_bdevs_buff(struct raid_bdev *raid_bdev, struct rebuild_progress *cycle_progress, uint8_t arr_num)
{
    for (uint8_t i = 0; i < arr_num; i++)
    {
        free_sg_buffer(cycle_progress->base_bdevs_sg_buf[i], raid_bdev->rebuild->strips_per_area);
        cycle_progress->base_bdevs_sg_buf[i] = NULL;
    }
}

static inline void
free_base_bdevs_buff(struct raid_bdev *raid_bdev, struct rebuild_progress *cycle_progress)
{
    _free_base_bdevs_buff(raid_bdev, cycle_progress, raid_bdev->num_base_bdevs);
}

static inline int
alloc_base_bdevs_buff(struct raid_bdev *raid_bdev, struct rebuild_progress *cycle_progress)
{
    uint64_t elem_size = spdk_bdev_get_block_size(&(raid_bdev->bdev))*raid_bdev->strip_size;
    uint8_t i = 0;
    struct raid_base_bdev_info *base_info;
    
    RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info)
    {
        struct spdk_bdev *bbdev = spdk_bdev_desc_get_bdev(base_info->desc);
        if(spdk_bdev_get_write_unit_size(bbdev) != 1)
        {
            SPDK_WARNLOG("Unsupported write_unit_size in base bdev of raid");
        }
        
        cycle_progress->base_bdevs_sg_buf[i] = allocate_sg_buffer(elem_size, raid_bdev->rebuild->strips_per_area, bbdev->required_alignment);
        if (cycle_progress->base_bdevs_sg_buf[i] == NULL)
        {
            _free_base_bdevs_buff(raid_bdev, cycle_progress, i);
            return -ENOMEM;
        }
    }
    return 0;
}

static inline uint16_t
count_broken_areas(ATOMIC_SNAPSHOT_TYPE area_str)
{
    uint16_t cnt = 0;

    for (uint16_t i = 0; i < LEN_AREA_STR_IN_BIT; i++)
    {
        if(SPDK_TEST_BIT(&area_str, i)) cnt += 1;
    }

    return cnt;
}

static inline int64_t
init_rebuild_cycle(struct rebuild_progress *cycle_progress, struct raid_bdev *raid_bdev)
{
    int64_t start_idx = NOT_NEED_REBUILD;
    struct raid_rebuild *rebuild = raid_bdev->rebuild;

    cycle_progress->clear_area_str_cnt = 0;
    cycle_progress->area_str_cnt = 0;

    for (uint64_t i = 0; i < rebuild->num_memory_areas ; i++)
    {
        if (ATOMIC_IS_AREA_STR_CLEAR(rebuild->rebuild_matrix[i])) continue;

        if(start_idx == NOT_NEED_REBUILD)
        {
            start_idx = i;
        }

        SPDK_SET_BIT(&(cycle_progress->area_proection[b_GET_IDX_BP(i)]), b_GET_SHFT_BP(i));

        cycle_progress->area_str_cnt += 1;
    }

    if (start_idx != NOT_NEED_REBUILD)
    {
        raid_bdev->rebuild->cycle_progress = cycle_progress;
    } else {
        raid_bdev->rebuild->cycle_progress = NULL;
    }

    return start_idx;
}

static inline int64_t
get_iter_idx(int64_t prev_idx, struct raid_bdev *raid_bdev)
{
    struct rebuild_progress *cycle_progress = raid_bdev->rebuild->cycle_progress;

    for (int64_t i = prev_idx + 1; i < (int64_t)raid_bdev->rebuild->num_memory_areas; i++)
    {
        if (!SPDK_TEST_BIT(&(cycle_progress->area_proection[b_GET_IDX_BP(i)]), b_GET_SHFT_BP(i))) continue;
        return i;
    }
    return NOT_NEED_REBUILD;
}

static inline void
finish_rebuild_cycle(struct raid_bdev *raid_bdev)
{   
    struct raid_rebuild *rebuild = raid_bdev->rebuild;

    if (rebuild == NULL)
    {
        return;
    }
    free_base_bdevs_buff(raid_bdev, rebuild->cycle_progress); 
    free(rebuild->cycle_progress);
    rebuild->cycle_progress = NULL;
    SPDK_REMOVE_BIT(fl(rebuild), REBUILD_FLAG_IN_PROGRESS);
}

static inline void
init_cycle_iteration(struct raid_rebuild *rebuild, int64_t curr_idx)
{
    struct rebuild_cycle_iteration *cycle_iter = &(rebuild->cycle_progress->cycle_iteration);

    cycle_iter->iter_idx = curr_idx;
    cycle_iter->complete = false;
    cycle_iter->snapshot = CREATE_AREA_STR_SNAPSHOT(&(rebuild->rebuild_matrix[curr_idx]));
    cycle_iter->br_area_cnt = count_broken_areas(cycle_iter->snapshot);
    cycle_iter->pr_area_cnt = 0;
    cycle_iter->iter_progress = cycle_iter->snapshot;
}

void init_cb_arg(struct iteration_step *iter_info, int64_t iter_idx, int16_t area_idx, struct rebuild_cycle_iteration *iteration, struct raid_bdev *raid_bdev) 
{
    iter_info->area_idx = area_idx;
    iter_info->iter_idx = iter_idx;
    iter_info->iteration = iteration;
    iter_info->raid_bdev = raid_bdev;
}

struct iteration_step *
alloc_cb_arg(int64_t iter_idx, int16_t area_idx, struct rebuild_cycle_iteration *iteration, struct raid_bdev *raid_bdev)
{
    struct iteration_step *iter_info = calloc(1, sizeof(struct iteration_step));
    if (iter_info == NULL)
    {
        return NULL;
    }
    init_cb_arg(iter_info, iter_idx, area_idx, iteration, raid_bdev);
    return iter_info;
}

void
free_cd_arg(struct iteration_step *cb)
{
    if(cb == NULL)
    {
       return; 
    }
    free(cb);
}

/*
 * Callback function.
 */
void
continue_rebuild(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    int64_t iter_idx = ((struct iteration_step *)cb_arg)->iter_idx;
    int16_t area_idx = ((struct iteration_step *)cb_arg)->area_idx;
    struct rebuild_cycle_iteration *iteration = ((struct iteration_step *)cb_arg)->iteration;
    struct raid_bdev *raid_bdev = ((struct iteration_step *)cb_arg)->raid_bdev;
    struct rebuild_progress *cycle_progress = raid_bdev->rebuild->cycle_progress;
    int64_t next_iter_idx;
    int ret = 0;

    free_cd_arg(cb_arg);
    spdk_bdev_free_io(bdev_io);
    partly_submit_iteration(success, iter_idx, area_idx, raid_bdev->rebuild);

    /* Test whether the end of the iteration or not */
    if(!ATOMIC_EXCHANGE(&(iteration->pr_area_cnt), iteration->br_area_cnt, 0))
    {
        return;
    }

    ++cycle_progress->clear_area_str_cnt;

    /* Wether it is the last iteration or not */
    if(cycle_progress->clear_area_str_cnt == cycle_progress->area_str_cnt)
    { //TODO: if clear_area_str_cnt - atomic => problem
        finish_rebuild_cycle(raid_bdev);
        return;
    }

    next_iter_idx = get_iter_idx(iter_idx, raid_bdev);
    init_cycle_iteration(raid_bdev->rebuild, next_iter_idx);

    ret = raid_bdev->module->rebuild_request(raid_bdev, cycle_progress, continue_rebuild);

    if(spdk_unlikely(ret != 0))
    {
        SPDK_SET_BIT(&(raid_bdev->rebuild->rebuild_flag), REBUILD_FLAG_FATAL_ERROR);
    }
}

int
run_rebuild_poller(void* arg)
{ //TODO: в целом реализация наивная (без учета многопоточности) - не очень пока понимаю, как ее прикрутить.
    struct raid_bdev *raid_bdev = arg;
    struct raid_rebuild *rebuild = raid_bdev->rebuild;
    struct rebuild_progress *cycle_progress = NULL;
    int ret = 0;

#ifdef DEBUG__
    SPDK_WARNLOG("poller is working now with: %s!\n", raid_bdev->bdev.name);
#endif

    if (rebuild == NULL)
    {
       SPDK_WARNLOG("%s doesn't have rebuild struct!\n", raid_bdev->bdev.name);
       return -ENODEV;
    }
    if (!SPDK_TEST_BIT(fl(rebuild), REBUILD_FLAG_INITIALIZED))
    {
        /*
         * the rebuild structure has not yet been initialized
         */
        return 0;
    }
    if (SPDK_TEST_BIT(&(rebuild->rebuild_flag), REBUILD_FLAG_FATAL_ERROR))
    {
        SPDK_WARNLOG("%s catch fatal error during rebuild process!\n", raid_bdev->bdev.name);
        return -ENOEXEC;
    }
    if (SPDK_TEST_BIT(fl(rebuild), REBUILD_FLAG_IN_PROGRESS))
    {
        /*
         * Previous recovery process is not complete
         */
        return 0;
    }

    cycle_progress = calloc(1, sizeof(struct rebuild_progress));

    if (cycle_progress == NULL)
    {
        SPDK_ERRLOG("the struct rebuild_progress wasn't allocated \n");
        return -ENOMEM;
    }

    /*
     * Representation of area-stripe index in the area_proection
     * (from which the rebuild cycle will begin)
     */
    int64_t start_idx = NOT_NEED_REBUILD;

    start_idx = init_rebuild_cycle(cycle_progress, raid_bdev);

    if(start_idx == NOT_NEED_REBUILD)
    {
        /*
         * no need rebuild
         */
        free(cycle_progress);
        return ret;
    }

    if (raid_bdev->module->rebuild_request != NULL)
    {   
        SPDK_SET_BIT(fl(rebuild), REBUILD_FLAG_IN_PROGRESS);

        init_cycle_iteration(rebuild, start_idx);
        
        if(alloc_base_bdevs_buff(raid_bdev, cycle_progress) != 0)
        {
            return -ENOMEM;
        }
        
        ret = raid_bdev->module->rebuild_request(raid_bdev, cycle_progress, continue_rebuild);
        switch (ret)
        {
        case -ENOMEM:
        case -EIO:
            finish_rebuild_cycle(raid_bdev);
            break;
        }
    } else {
        SPDK_ERRLOG("rebuild_request inside raid%d doesn't implemented\n", raid_bdev->level);
        return -ENODEV;
    }
    return ret;
}

// ===============================TESTs=========================================== //
// TODO: мб не надо, переделать.
// struct container {
//     struct raid_bdev *raid_bdev;
//     int idx;
//     struct iovec * buff;
// } typedef container;


// static inline struct iovec *
// alloc_continuous_buffer_part(size_t iovlen, size_t align)
// {
//     struct iovec *buf;
//     buf = spdk_dma_zmalloc(sizeof(struct iovec), 0, NULL);
//     if(buf == NULL)
//     {
//         return NULL;
//     }

//     buf->iov_len = iovlen;
//     buf->iov_base = spdk_dma_zmalloc(sizeof(uint8_t)*(buf->iov_len), 0, NULL);
//     if(buf->iov_base == NULL)
//     {
//         spdk_dma_free(buf);
//         return NULL;
//     }

//     return buf;
// }

// static inline void
// free_continuous_buffer_part(struct iovec * buf_elem)
// {
//     spdk_dma_free(buf_elem->iov_base);
//     spdk_dma_free(buf_elem);
// }


// static inline int
// fill_ones_write_request(struct iovec * buf_array, int buf_len)
// {
//     // не учитывает align внутри iov_base
//     SPDK_WARNLOG("i'm here 2 \n");

//     if(buf_len > _MUX_BUF_LENGTH)
//     {
//         return -1;
//     }
//     for(size_t i = 0; i<buf_len; i++)
//     {
//         SPDK_WARNLOG("i'm here 3 \n");
//         *((uint8_t *)(buf_array[i].iov_base)) = UINT8_MAX;
//         SPDK_WARNLOG("i'm here 4 \n");
//         SPDK_WARNLOG("%d", UINT8_MAX);
//         SPDK_WARNLOG("i'm here 5 \n");

//     }

//     return 0;
// }

// void
// cd_read_func(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
// {
//     container *cont = cb_arg;
//     if(success)
//     {
//         SPDK_WARNLOG("test (read) success\n");
//         SPDK_WARNLOG("\n\n test = %d \n\n", *((uint8_t*)cont->buff->iov_base));
//     } else {
//         SPDK_ERRLOG("test (read) fail\n");
//     }

//     free_continuous_buffer_part(cont->buff);
//     spdk_bdev_free_io(bdev_io);
//     free(cont);
//     SPDK_WARNLOG("test (read) success\n");
// }

// void
// cd_write_func(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
// {
//     container *cont = cb_arg;
//     if(success)
//     {
//         SPDK_WARNLOG("test (write) success\n");
//     } else {
//         SPDK_ERRLOG("test (write) fail\n");
//     }

//     free_continuous_buffer_part(cont->buff);
//     spdk_bdev_free_io(bdev_io);
//     submit_read_request_base_bdev(cont->raid_bdev, cont->idx);
//     free(cont);
// }

// void
// submit_write_request_base_bdev(struct raid_bdev *raid_bdev, uint8_t idx)
// {
//     struct spdk_bdev_desc *desc = __base_desc_from_raid_bdev(raid_bdev, idx);
//     struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(desc);
//     struct spdk_io_channel *ch = spdk_bdev_get_io_channel(desc);
//     struct iovec *buffer;
//     container *cont;
//     int ret;


//     buffer = alloc_continuous_buffer_part(base_bdev->blocklen, base_bdev->required_alignment);
//     if (fill_ones_write_request(buffer, 1) != 0)
//     {
//         SPDK_ERRLOG("fill_ones_write_request was fail\n");
//     }
//     cont = calloc(1, sizeof(container));
//     if (cont == NULL){
//         SPDK_ERRLOG("cont alloc failed\n");
//     }

//     cont->buff = buffer;
//     cont->idx = 0;
//     cont->raid_bdev = raid_bdev;
//     ret = spdk_bdev_writev_blocks (desc, ch, buffer, 1, 0, 1, cd_write_func, cont);

//     if(spdk_likely(ret == 0))
//     {
//         SPDK_WARNLOG("submit test (write) success\n");
//     } else {
//         SPDK_ERRLOG("submit test (write) fail\n");
//     }
// }

// void
// submit_read_request_base_bdev(struct raid_bdev *raid_bdev, uint8_t idx)
// {
//     struct spdk_bdev_desc *desc = __base_desc_from_raid_bdev(raid_bdev, idx);
//     struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(desc);
//     struct spdk_io_channel *ch = spdk_bdev_get_io_channel(desc);
//     struct iovec *buffer;
//     container *cont;
//     int ret;

//     buffer = alloc_continuous_buffer_part(base_bdev->blocklen, base_bdev->required_alignment);

//     cont = calloc(1, sizeof(container));
//     if (cont == NULL){
//         SPDK_ERRLOG("cont alloc failed\n");
//     }

//     cont->buff = buffer;
//     cont->idx = 0;
//     cont->raid_bdev = raid_bdev;
//     ret = spdk_bdev_readv_blocks(desc, ch, buffer, 1, 0, 1, cd_read_func, cont);

//     if(spdk_likely(ret == 0))
//     {
//         SPDK_WARNLOG("submit test (read) success\n");
//     } else {
//         SPDK_ERRLOG("submit test (read) fail\n");
//     }
// }
/* ======================================================================== */