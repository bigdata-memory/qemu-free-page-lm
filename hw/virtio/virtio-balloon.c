/*
 * Virtio Balloon Device
 *
 * Copyright IBM, Corp. 2008
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2011 Amit Shah <amit.shah@redhat.com>
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/timer.h"
#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "hw/mem/pc-dimm.h"
#include "sysemu/balloon.h"
#include "hw/virtio/virtio-balloon.h"
#include "sysemu/kvm.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi-event.h"
#include "trace.h"
#include "qemu/error-report.h"

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "migration/misc.h"

#define BALLOON_PAGE_SIZE  (1 << VIRTIO_BALLOON_PFN_SHIFT)

static void balloon_page(void *addr, int deflate)
{
    if (!qemu_balloon_is_inhibited() && (!kvm_enabled() ||
                                         kvm_has_sync_mmu())) {
        qemu_madvise(addr, BALLOON_PAGE_SIZE,
                deflate ? QEMU_MADV_WILLNEED : QEMU_MADV_DONTNEED);
    }
}

static const char *balloon_stat_names[] = {
   [VIRTIO_BALLOON_S_SWAP_IN] = "stat-swap-in",
   [VIRTIO_BALLOON_S_SWAP_OUT] = "stat-swap-out",
   [VIRTIO_BALLOON_S_MAJFLT] = "stat-major-faults",
   [VIRTIO_BALLOON_S_MINFLT] = "stat-minor-faults",
   [VIRTIO_BALLOON_S_MEMFREE] = "stat-free-memory",
   [VIRTIO_BALLOON_S_MEMTOT] = "stat-total-memory",
   [VIRTIO_BALLOON_S_AVAIL] = "stat-available-memory",
   [VIRTIO_BALLOON_S_CACHES] = "stat-disk-caches",
   [VIRTIO_BALLOON_S_NR] = NULL
};

/*
 * reset_stats - Mark all items in the stats array as unset
 *
 * This function needs to be called at device initialization and before
 * updating to a set of newly-generated stats.  This will ensure that no
 * stale values stick around in case the guest reports a subset of the supported
 * statistics.
 */
static inline void reset_stats(VirtIOBalloon *dev)
{
    int i;
    for (i = 0; i < VIRTIO_BALLOON_S_NR; dev->stats[i++] = -1);
}

static bool balloon_stats_supported(const VirtIOBalloon *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    return virtio_vdev_has_feature(vdev, VIRTIO_BALLOON_F_STATS_VQ);
}

static bool balloon_stats_enabled(const VirtIOBalloon *s)
{
    return s->stats_poll_interval > 0;
}

static void balloon_stats_destroy_timer(VirtIOBalloon *s)
{
    if (balloon_stats_enabled(s)) {
        timer_del(s->stats_timer);
        timer_free(s->stats_timer);
        s->stats_timer = NULL;
        s->stats_poll_interval = 0;
    }
}

static void balloon_stats_change_timer(VirtIOBalloon *s, int64_t secs)
{
    timer_mod(s->stats_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + secs * 1000);
}

static void balloon_stats_poll_cb(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (s->stats_vq_elem == NULL || !balloon_stats_supported(s)) {
        /* re-schedule */
        balloon_stats_change_timer(s, s->stats_poll_interval);
        return;
    }

    virtqueue_push(s->svq, s->stats_vq_elem, s->stats_vq_offset);
    virtio_notify(vdev, s->svq);
    g_free(s->stats_vq_elem);
    s->stats_vq_elem = NULL;
}

static void balloon_stats_get_all(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    Error *err = NULL;
    VirtIOBalloon *s = opaque;
    int i;

    visit_start_struct(v, name, NULL, 0, &err);
    if (err) {
        goto out;
    }
    visit_type_int(v, "last-update", &s->stats_last_update, &err);
    if (err) {
        goto out_end;
    }

    visit_start_struct(v, "stats", NULL, 0, &err);
    if (err) {
        goto out_end;
    }
    for (i = 0; i < VIRTIO_BALLOON_S_NR; i++) {
        visit_type_uint64(v, balloon_stat_names[i], &s->stats[i], &err);
        if (err) {
            goto out_nested;
        }
    }
    visit_check_struct(v, &err);
out_nested:
    visit_end_struct(v, NULL);

    if (!err) {
        visit_check_struct(v, &err);
    }
out_end:
    visit_end_struct(v, NULL);
out:
    error_propagate(errp, err);
}

static void balloon_stats_get_poll_interval(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    VirtIOBalloon *s = opaque;
    visit_type_int(v, name, &s->stats_poll_interval, errp);
}

static void balloon_stats_set_poll_interval(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    VirtIOBalloon *s = opaque;
    Error *local_err = NULL;
    int64_t value;

    visit_type_int(v, name, &value, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (value < 0) {
        error_setg(errp, "timer value must be greater than zero");
        return;
    }

    if (value > UINT32_MAX) {
        error_setg(errp, "timer value is too big");
        return;
    }

    if (value == s->stats_poll_interval) {
        return;
    }

    if (value == 0) {
        /* timer=0 disables the timer */
        balloon_stats_destroy_timer(s);
        return;
    }

    if (balloon_stats_enabled(s)) {
        /* timer interval change */
        s->stats_poll_interval = value;
        balloon_stats_change_timer(s, value);
        return;
    }

    /* create a new timer */
    g_assert(s->stats_timer == NULL);
    s->stats_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, balloon_stats_poll_cb, s);
    s->stats_poll_interval = value;
    balloon_stats_change_timer(s, 0);
}

static void virtio_balloon_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;
    MemoryRegionSection section;

    for (;;) {
        size_t offset = 0;
        uint32_t pfn;
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            return;
        }

        while (iov_to_buf(elem->out_sg, elem->out_num, offset, &pfn, 4) == 4) {
            ram_addr_t pa;
            ram_addr_t addr;
            int p = virtio_ldl_p(vdev, &pfn);

            pa = (ram_addr_t) p << VIRTIO_BALLOON_PFN_SHIFT;
            offset += 4;

            /* FIXME: remove get_system_memory(), but how? */
            section = memory_region_find(get_system_memory(), pa, 1);
            if (!int128_nz(section.size) ||
                !memory_region_is_ram(section.mr) ||
                memory_region_is_rom(section.mr) ||
                memory_region_is_romd(section.mr)) {
                trace_virtio_balloon_bad_addr(pa);
                memory_region_unref(section.mr);
                continue;
            }

            trace_virtio_balloon_handle_output(memory_region_name(section.mr),
                                               pa);
            /* Using memory_region_get_ram_ptr is bending the rules a bit, but
               should be OK because we only want a single page.  */
            addr = section.offset_within_region;
            balloon_page(memory_region_get_ram_ptr(section.mr) + addr,
                         !!(vq == s->dvq));
            memory_region_unref(section.mr);
        }

        virtqueue_push(vq, elem, offset);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_balloon_receive_stats(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;
    VirtIOBalloonStat stat;
    size_t offset = 0;
    qemu_timeval tv;

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (!elem) {
        goto out;
    }

    if (s->stats_vq_elem != NULL) {
        /* This should never happen if the driver follows the spec. */
        virtqueue_push(vq, s->stats_vq_elem, 0);
        virtio_notify(vdev, vq);
        g_free(s->stats_vq_elem);
    }

    s->stats_vq_elem = elem;

    /* Initialize the stats to get rid of any stale values.  This is only
     * needed to handle the case where a guest supports fewer stats than it
     * used to (ie. it has booted into an old kernel).
     */
    reset_stats(s);

    while (iov_to_buf(elem->out_sg, elem->out_num, offset, &stat, sizeof(stat))
           == sizeof(stat)) {
        uint16_t tag = virtio_tswap16(vdev, stat.tag);
        uint64_t val = virtio_tswap64(vdev, stat.val);

        offset += sizeof(stat);
        if (tag < VIRTIO_BALLOON_S_NR)
            s->stats[tag] = val;
    }
    s->stats_vq_offset = offset;

    if (qemu_gettimeofday(&tv) < 0) {
        warn_report("%s: failed to get time of day", __func__);
        goto out;
    }

    s->stats_last_update = tv.tv_sec;

out:
    if (balloon_stats_enabled(s)) {
        balloon_stats_change_timer(s, s->stats_poll_interval);
    }
}

static void *virtio_balloon_poll_free_page_hints(void *opaque)
{
    VirtQueueElement *elem;
    VirtIOBalloon *dev = opaque;
    VirtQueue *vq = dev->free_page_vq;
    uint32_t id;
    size_t size;

    /*
     * Poll the vq till the status changed to STOP. This happens when
     * the guest finishes reporting hints or the migration thread actively
     * stops the reporting.
     */
    while (dev->free_page_report_status < FREE_PAGE_REPORT_S_STOP) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            continue;
        }

        if (elem->out_num) {
            size = iov_to_buf(elem->out_sg, elem->out_num, 0, &id, sizeof(id));
            virtqueue_push(vq, elem, size);
            g_free(elem);
            if (unlikely(size != sizeof(id))) {
                warn_report("%s: received an incorrect cmd id", __func__);
                break;
            }
            if (id == dev->free_page_report_cmd_id) {
                dev->free_page_report_status = FREE_PAGE_REPORT_S_START;
            } else if (dev->free_page_report_status ==
                       FREE_PAGE_REPORT_S_START) {
                /*
                 * Stop the optimization only when it has started. This avoids
                 * obsolete stop sign for the previous command.
                 */
                dev->free_page_report_status = FREE_PAGE_REPORT_S_STOP;
                break;
            }
        }

        if (elem->in_num) {
            if (dev->free_page_report_status == FREE_PAGE_REPORT_S_START &&
                !dev->poison_val) {
                qemu_guest_free_page_hint(elem->in_sg[0].iov_base,
                                          elem->in_sg[0].iov_len);
            }
            virtqueue_push(vq, elem, 0);
            g_free(elem);
        }
    }
    return NULL;
}

static bool virtio_balloon_free_page_support(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    return virtio_vdev_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT);
}

static void virtio_balloon_free_page_start(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (unlikely(s->free_page_report_cmd_id == UINT_MAX)) {
        s->free_page_report_cmd_id =
                       VIRTIO_BALLOON_FREE_PAGE_REPORT_CMD_ID_MIN;
    } else {
        s->free_page_report_cmd_id++;
    }

    s->free_page_report_status = FREE_PAGE_REPORT_S_REQUESTED;
    virtio_notify_config(vdev);
    qemu_thread_create(&s->free_page_thread, "free_page_optimization_thread",
                       virtio_balloon_poll_free_page_hints, s,
                       QEMU_THREAD_JOINABLE);
}

static void virtio_balloon_free_page_stop(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    switch (s->free_page_report_status) {
    case FREE_PAGE_REPORT_S_REQUESTED:
    case FREE_PAGE_REPORT_S_START:
        /*
         * The guest hasn't done the reporting, so host sends a notification
         * to the guest to actively stop the reporting before joining the
         * optimization thread.
         */
        s->free_page_report_status = FREE_PAGE_REPORT_S_STOP;
        virtio_notify_config(vdev);
    case FREE_PAGE_REPORT_S_STOP:
        /* The guest has stopped the reporting. Join the optimization thread */
        qemu_thread_join(&s->free_page_thread);
        s->free_page_report_status = FREE_PAGE_REPORT_S_EXIT;
    case FREE_PAGE_REPORT_S_EXIT:
        /* The optimization thread has gone. No actions needded so far. */
        break;
    }
}

static void virtio_balloon_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    struct virtio_balloon_config config;

    config.num_pages = cpu_to_le32(dev->num_pages);
    config.actual = cpu_to_le32(dev->actual);
    config.poison_val = cpu_to_le32(dev->poison_val);

    if (dev->free_page_report_status >= FREE_PAGE_REPORT_S_STOP) {
        config.free_page_report_cmd_id =
                       cpu_to_le32(VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID);
    } else {
        config.free_page_report_cmd_id =
                       cpu_to_le32(dev->free_page_report_cmd_id);
    }

    trace_virtio_balloon_get_config(config.num_pages, config.actual);
    memcpy(config_data, &config, sizeof(struct virtio_balloon_config));
}

static int build_dimm_list(Object *obj, void *opaque)
{
    GSList **list = opaque;

    if (object_dynamic_cast(obj, TYPE_PC_DIMM)) {
        DeviceState *dev = DEVICE(obj);
        if (dev->realized) { /* only realized DIMMs matter */
            *list = g_slist_prepend(*list, dev);
        }
    }

    object_child_foreach(obj, build_dimm_list, opaque);
    return 0;
}

static ram_addr_t get_current_ram_size(void)
{
    GSList *list = NULL, *item;
    ram_addr_t size = ram_size;

    build_dimm_list(qdev_get_machine(), &list);
    for (item = list; item; item = g_slist_next(item)) {
        Object *obj = OBJECT(item->data);
        if (!strcmp(object_get_typename(obj), TYPE_PC_DIMM)) {
            size += object_property_get_int(obj, PC_DIMM_SIZE_PROP,
                                            &error_abort);
        }
    }
    g_slist_free(list);

    return size;
}

static void virtio_balloon_set_config(VirtIODevice *vdev,
                                      const uint8_t *config_data)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    struct virtio_balloon_config config;
    uint32_t oldactual = dev->actual;
    ram_addr_t vm_ram_size = get_current_ram_size();

    memcpy(&config, config_data, sizeof(struct virtio_balloon_config));
    dev->actual = le32_to_cpu(config.actual);
    if (dev->actual != oldactual) {
        qapi_event_send_balloon_change(vm_ram_size -
                        ((ram_addr_t) dev->actual << VIRTIO_BALLOON_PFN_SHIFT),
                        &error_abort);
    }
    dev->poison_val = le32_to_cpu(config.poison_val);
    trace_virtio_balloon_set_config(dev->actual, oldactual);
}

static uint64_t virtio_balloon_get_features(VirtIODevice *vdev, uint64_t f,
                                            Error **errp)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    f |= dev->host_features;
    virtio_add_feature(&f, VIRTIO_BALLOON_F_STATS_VQ);

    if (dev->host_features & 1ULL << VIRTIO_BALLOON_F_FREE_PAGE_HINT) {
        virtio_add_feature(&f, VIRTIO_BALLOON_F_PAGE_POISON);
    }

    return f;
}

static void virtio_balloon_stat(void *opaque, BalloonInfo *info)
{
    VirtIOBalloon *dev = opaque;
    info->actual = get_current_ram_size() - ((uint64_t) dev->actual <<
                                             VIRTIO_BALLOON_PFN_SHIFT);
}

static void virtio_balloon_to_target(void *opaque, ram_addr_t target)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(opaque);
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    ram_addr_t vm_ram_size = get_current_ram_size();

    if (target > vm_ram_size) {
        target = vm_ram_size;
    }
    if (target) {
        dev->num_pages = (vm_ram_size - target) >> VIRTIO_BALLOON_PFN_SHIFT;
        virtio_notify_config(vdev);
    }
    trace_virtio_balloon_to_target(target, dev->num_pages);
}

static int virtio_balloon_post_load_device(void *opaque, int version_id)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(opaque);

    if (balloon_stats_enabled(s)) {
        balloon_stats_change_timer(s, s->stats_poll_interval);
    }
    return 0;
}

static const VMStateDescription vmstate_virtio_balloon_free_page_report = {
    .name = "virtio-balloon-device/free-page-report",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = virtio_balloon_free_page_support,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(free_page_report_cmd_id, VirtIOBalloon),
        VMSTATE_UINT32(poison_val, VirtIOBalloon),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_virtio_balloon_device = {
    .name = "virtio-balloon-device",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = virtio_balloon_post_load_device,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(num_pages, VirtIOBalloon),
        VMSTATE_UINT32(actual, VirtIOBalloon),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * []) {
        &vmstate_virtio_balloon_free_page_report,
        NULL
    }
};

static void virtio_balloon_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);

    virtio_init(vdev, "virtio-balloon", VIRTIO_ID_BALLOON,
                sizeof(struct virtio_balloon_config));

    s->ivq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->dvq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->svq = virtio_add_queue(vdev, 128, virtio_balloon_receive_stats);
    if (virtio_has_feature(s->host_features,
                           VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
        s->free_page_vq = virtio_add_queue(vdev, VIRTQUEUE_MAX_SIZE, NULL);
        s->free_page_report_status = FREE_PAGE_REPORT_S_EXIT;
        s->free_page_report_cmd_id =
                           VIRTIO_BALLOON_FREE_PAGE_REPORT_CMD_ID_MIN - 1;
    }
    reset_stats(s);
}

static void virtio_balloon_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);

    balloon_stats_destroy_timer(s);
    qemu_remove_balloon_handler(s);
    virtio_cleanup(vdev);
}

static void virtio_balloon_device_reset(VirtIODevice *vdev)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);

    if (s->stats_vq_elem != NULL) {
        virtqueue_unpop(s->svq, s->stats_vq_elem, 0);
        g_free(s->stats_vq_elem);
        s->stats_vq_elem = NULL;
    }
}

static void virtio_balloon_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);

    if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
        if (!s->stats_vq_elem && vdev->vm_running &&
            virtqueue_rewind(s->svq, 1)) {
            /*
             * Poll stats queue for the element we have discarded when the VM
             * was stopped.
             */
            virtio_balloon_receive_stats(vdev, s->svq);
        }

        if (virtio_balloon_free_page_support(s)) {
            qemu_add_balloon_handler(virtio_balloon_to_target,
                                     virtio_balloon_stat,
                                     virtio_balloon_free_page_support,
                                     virtio_balloon_free_page_start,
                                     virtio_balloon_free_page_stop,
                                     s);
        } else {
            qemu_add_balloon_handler(virtio_balloon_to_target,
                                     virtio_balloon_stat, NULL, NULL, NULL, s);
        }
    }
}

static void virtio_balloon_instance_init(Object *obj)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(obj);

    object_property_add(obj, "guest-stats", "guest statistics",
                        balloon_stats_get_all, NULL, NULL, s, NULL);

    object_property_add(obj, "guest-stats-polling-interval", "int",
                        balloon_stats_get_poll_interval,
                        balloon_stats_set_poll_interval,
                        NULL, s, NULL);
}

static const VMStateDescription vmstate_virtio_balloon = {
    .name = "virtio-balloon",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_balloon_properties[] = {
    DEFINE_PROP_BIT("deflate-on-oom", VirtIOBalloon, host_features,
                    VIRTIO_BALLOON_F_DEFLATE_ON_OOM, false),
    DEFINE_PROP_BIT("free-page-hint", VirtIOBalloon, host_features,
                    VIRTIO_BALLOON_F_FREE_PAGE_HINT, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_balloon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_balloon_properties;
    dc->vmsd = &vmstate_virtio_balloon;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_balloon_device_realize;
    vdc->unrealize = virtio_balloon_device_unrealize;
    vdc->reset = virtio_balloon_device_reset;
    vdc->get_config = virtio_balloon_get_config;
    vdc->set_config = virtio_balloon_set_config;
    vdc->get_features = virtio_balloon_get_features;
    vdc->set_status = virtio_balloon_set_status;
    vdc->vmsd = &vmstate_virtio_balloon_device;
}

static const TypeInfo virtio_balloon_info = {
    .name = TYPE_VIRTIO_BALLOON,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBalloon),
    .instance_init = virtio_balloon_instance_init,
    .class_init = virtio_balloon_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_balloon_info);
}

type_init(virtio_register_types)
