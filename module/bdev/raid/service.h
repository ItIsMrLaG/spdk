#ifndef SPDK_RAID_SERVICE_INTERNAL_H
#define SPDK_RAID_SERVICE_INTERNAL_H

#include "spdk/queue.h"

//->
#define __base_desc_from_raid_bdev(raid_bdev, idx) (raid_bdev->base_bdev_info[idx].desc)
#define fl(rebuild) &(rebuild->rebuild_flag)
#define NOT_NEED_REBUILD -1
//->
//TODO: Надо проверить, что побитовые макросы с указателями нормально работают, а то я вообще хз, мб тут ошибка
#define ATOMIC_IS_AREA_STR_CLEAR(area_srt) (area_srt)
#define CREATE_AREA_STR_SNAPSHOT(area_srt_ptr) (*area_srt_ptr)
#define ATOMIC_INCREMENT(ptr) ((*ptr)++)
#define ATOMIC_EXCHANGE(dest_ptr, exc, src) (TEST_CAS(dest_ptr, exc, src))
// ->
// TODO: индексы у битов идут справа на лево!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! (все норм?)
#define b_BASE_TYPE uint64_t
#define b_BIT_PROECTION(name) b_BASE_TYPE name[SPDK_CEIL_DIV(MATRIX_REBUILD_SIZE, (sizeof(b_BASE_TYPE)*8))]
#define b_GET_IDX_BP(x) (x / (sizeof(b_BASE_TYPE)*8))
#define b_GET_SHFT_BP(x) (x % (sizeof(b_BASE_TYPE)*8))
//

static inline bool
TEST_CAS(ATOMIC_TYPE *ptr, ATOMIC_SNAPSHOT_TYPE exc, ATOMIC_SNAPSHOT_TYPE src)
{
    if (*ptr == exc){
        *ptr = src;
        return true;
    }
    return false;
}

struct iteration_step
{
    int64_t iter_idx;
    int16_t area_idx;
    struct rebuild_cycle_iteration *iteration;
    struct raid_bdev *raid_bdev;
};


struct rebuild_cycle_iteration
{
   /* true if one part of the rebuild cycle is completed */
    bool complete;

    /* index of the area stripe for rebuld */
    int64_t iter_idx;

    /* number of broken areas in current area stripe */
    int16_t br_area_cnt;

    /* processed areas counter, it increments after completion rebuild a concrete area */
    ATOMIC_DATA(pr_area_cnt);

    /* snapshot of area stripe from rebuild matrix (non atomic) */
    ATOMIC_SNAPSHOT(snapshot);

    /*
     * metadata for current iteration,
     * describing which areas should still be started for rebuild
     * (equals snapshot at initialization stage)
     * (10..010 |-[start rebuild area with index 1]-> 10..000)
     */
    ATOMIC_SNAPSHOT(iter_progress);
};


struct rebuild_progress {
    /*
     * bit proection of rebuild matrix,
     * where each bit corresponds one line(area stripe) in rebuild matrix
     * (if the line contains broken areas, corresponding bit equels 1 othewise 0)
     */
    b_BIT_PROECTION(area_proection);

    /* number of areas stripes with broken areas */
    uint64_t area_str_cnt;

    /* number of area stripes with processed areas (tried to rebuild all the broken areas) */
    uint64_t clear_area_str_cnt; //TODO: думаю, что не должно быть атомиком, потому что измеяется последовательно

    /*
     * To avoid memory overloading, only one area stripe (in need of rebuild)
     * can be processed at a time.
     * The fild describes the rebuild of this area stripe.
     */
    struct rebuild_cycle_iteration cycle_iteration;
};


int
run_rebuild_poller(void* arg);

#endif /* SPDK_RAID_SERVICE_INTERNAL_H */