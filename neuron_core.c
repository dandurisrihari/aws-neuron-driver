// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/** Each neuron device has N number of neuron cores. (inf1 has 4 neuron cores).
 *
 * Engines:
 * -------
 * Neuron Core has multiple engines(inf1 has 3) which can do different types of computations.
 * Each engine's instruction stream is feed through DMA.
 *
 * Notifications:
 * -------------
 * As the engines execute instructions they produce messages in notification queue.
 * These messages are used by applications for monitoring completion of program and
 * also for profiling the program.
 *
 * Notification queue is a circular buffer in host memory - hardware writes to the buffer and
 * applications consumes it by memory mapping the area.
 *
 * Semaphores and events:
 * ---------------------
 * For synchronization between hardware blocks and software, NC provides two type synchronization
 * hardware primitives, semaphores and events. Events can be considered simple bitmap which hold
 * either 1 or 0. Semaphores hold any value in signed 32 bit range. Engines can be programmed
 * with instructions which can wait for semaphore to reach a certain value or a particular event
 * is set. Applications can use this to manipulate execution of the program.
 */

#include <asm/io.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/fault-inject.h>

#include "v1/address_map.h"
#include "v1/putils.h"

#include "neuron_mempool.h"
#include "neuron_device.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_nc_mmap);
#endif

#define NC_SEMAPHORE_SIZE 4
#define NC_EVENT_SIZE 4

static u64 nc_get_axi_offset(int nc_index)
{
	return MMAP_P_OFFSET + (nc_index * MMAP_NC_SIZE);
}

static void *nc_get_semaphore_base(struct neuron_device *nd, u8 nc_id)
{
	return nd->npdev.bar2 + nc_get_axi_offset(nc_id);
}

int nc_semaphore_read(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 *result)
{
	void *addr;

	if (semaphore_index > V1_SEMAPHORE_COUNT)
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_READ_OFFSET + (semaphore_index * NC_SEMAPHORE_SIZE);
	return fw_io_read_csr_array((void **)&addr, result, 1);
}

int nc_semaphore_write(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	void *addr;

	if (semaphore_index > V1_SEMAPHORE_COUNT)
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_SET_OFFSET + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_semaphore_increment(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	void *addr;

	if (semaphore_index > V1_SEMAPHORE_COUNT)
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_INCR_OFFSET + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

int nc_semaphore_decrement(struct neuron_device *nd, u8 nc_id, u16 semaphore_index, u32 value)
{
	void *addr;

	if (semaphore_index > V1_SEMAPHORE_COUNT)
		return -EINVAL;

	addr = nc_get_semaphore_base(nd, nc_id);
	addr += MMAP_NC_SEMA_DECR_OFFSET + (semaphore_index * NC_SEMAPHORE_SIZE);
	writel(value, addr);
	return 0;
}

static void *nc_get_event_addr(struct neuron_device *nd, u8 nc_id, u16 event_index)
{
	void *base = nd->npdev.bar2 + nc_get_axi_offset(nc_id) + MMAP_NC_EVENT_OFFSET;
	return (base + (event_index * NC_EVENT_SIZE));
}

int nc_event_get(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 *result)
{
	void *addr;

	if (event_index > V1_EVENTS_COUNT)
		return -EINVAL;

	addr = nc_get_event_addr(nd, nc_id, event_index);
	return fw_io_read_csr_array(&addr, result, 1);
}

int nc_event_set(struct neuron_device *nd, u8 nc_id, u16 event_index, u32 value)
{
	u32 *addr;

	if (event_index > V1_EVENTS_COUNT)
		return -EINVAL;

	addr = nc_get_event_addr(nd, nc_id, event_index);
	writel(value, addr);
	return 0;
}

enum NQ_TYPE {
	NQ_TYPE_TRACE = 0, /**< Implicit notifications generated during execution. */
	NQ_TYPE_NOTIFY, /**< Explicit notifications generated by NOTIFY instruction */
	NQ_TYPE_EVENT, /**< Notifications triggered by event set/clear operations. */
	NQ_TYPE_ERROR, /**< Notifications triggered by an error condition. */
	NQ_TYPE_MAX
};

/* Neuron notification queues can be memory mapped to read notifications from the device.
 *
 * Each device has 64(V1_NC_PER_DEVICE * MAX_NQ_ENGINE * NQ_TYPE_PER_ENGINE) notification queues.
 * Each queue is mapped to 1GB(separate vma) of space.
 */

/** Max size of a notification queue mapping.
 */
#define NC_NQ_MMAP_SIZE_PER_NQ (1 * 1024 * 1024 * 1024UL)
#define NC_NQ_MMAP_SIZE_PER_ENGINE (NC_NQ_MMAP_SIZE_PER_NQ * NQ_TYPE_PER_ENGINE)
#define NC_NQ_MMAP_SIZE_PER_NC (NC_NQ_MMAP_SIZE_PER_ENGINE * MAX_NQ_ENGINE)
#define NC_NQ_MMAP_SIZE_PER_ND (NC_NQ_MMAP_SIZE_PER_NC * V1_NC_PER_DEVICE)

/* offset in the devnode file */
#define NC_NQ_MMAP_START_OFFSET (0)
#define NC_NQ_MMAP_END_OFFSET (NC_NQ_MMAP_START_OFFSET + NC_NQ_MMAP_SIZE_PER_ND)

int nc_get_nq_mmap_offset(int nc_id, int engine_index, int nq_type, u64 *offset)
{
	if (nc_id > V1_NC_PER_DEVICE)
		return -EINVAL;
	if (engine_index > MAX_NQ_ENGINE)
		return -EINVAL;
	if (nq_type > NQ_TYPE_PER_ENGINE)
		return -EINVAL;

	*offset = NC_NQ_MMAP_START_OFFSET;
	*offset += (nc_id * NC_NQ_MMAP_SIZE_PER_NC);
	*offset += (engine_index * NC_NQ_MMAP_SIZE_PER_ENGINE);
	*offset += (nq_type * NC_NQ_MMAP_SIZE_PER_NQ);

	return 0;
}

int nc_get_nq_from_mmap_offset(u64 offset, int *nc_id, int *engine_index, int *nq_type)
{
	if (offset < NC_NQ_MMAP_START_OFFSET)
		return -EINVAL;
	if (offset >= NC_NQ_MMAP_END_OFFSET)
		return -EINVAL;

	offset -= NC_NQ_MMAP_START_OFFSET;

	*nc_id = offset / NC_NQ_MMAP_SIZE_PER_NC;
	offset %= NC_NQ_MMAP_SIZE_PER_NC;

	*engine_index = offset / NC_NQ_MMAP_SIZE_PER_ENGINE;
	offset %= NC_NQ_MMAP_SIZE_PER_ENGINE;

	*nq_type = offset / NC_NQ_MMAP_SIZE_PER_NQ;

	return 0;
}

int nc_nq_init(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type, u32 size)
{
	struct mem_chunk *mc, **mc_ptr;
	u64 queue_pa;
	void *apb_base;
	int ret;
	u8 nq_id;
	u32 low, high;

	if (nd == NULL || nc_id >= V1_NC_PER_DEVICE)
		return -EINVAL;

	nq_id = (nq_type * NQ_TYPE_PER_ENGINE) + eng_index;
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	mc_ptr = &nd->nq_mc[nc_id][nq_id];
	if (*mc_ptr == NULL) {
		ret = mc_alloc(&nd->mpset, mc_ptr, size, MEM_LOC_HOST, 0, 0, nc_id);
		if (ret)
			return ret;
	}
	mc = *mc_ptr;

	apb_base = nd->npdev.bar0 + pu_get_relative_offset(nc_id);
	queue_pa = mc->pa | PCIEX8_0_BASE;

	low = (u32)(queue_pa & 0xffffffff);
	high = (u32)(queue_pa >> 32U);
	switch (nq_type) {
	case NQ_TYPE_ERROR:
		pu_write_error_notification_cfg_0(apb_base, low);
		pu_write_error_notification_cfg_1(apb_base, high);
		pu_write_error_notification_cfg_2(apb_base, size);
		break;
	case NQ_TYPE_EVENT:
		pu_write_event_notification_cfg_0(apb_base, low);
		pu_write_event_notification_cfg_1(apb_base, high);
		pu_write_event_notification_cfg_2(apb_base, size);
		break;
	case NQ_TYPE_NOTIFY:
		pu_write_expl_notification_cfg_0(apb_base, eng_index, 0, low);
		pu_write_expl_notification_cfg_1(apb_base, eng_index, 0, high);
		pu_write_expl_notification_cfg_2(apb_base, eng_index, 0, size);
		break;
	case NQ_TYPE_TRACE:
		pu_write_impl_notification_cfg_0(apb_base, eng_index, 0, low);
		pu_write_impl_notification_cfg_1(apb_base, eng_index, 0, high);
		pu_write_impl_notification_cfg_2(apb_base, eng_index, 0, size);
		break;
	default:
		return -1;
	}

	return 0;
}

int nc_nq_destroy(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type)
{
	u8 nq_id;
	void *apb_base;

	nq_id = (nq_type * NQ_TYPE_PER_ENGINE) + eng_index;
	if (nd == NULL || nc_id >= V1_NC_PER_DEVICE || nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;
	if (nd == NULL || nc_id >= V1_NC_PER_DEVICE)
		return -EINVAL;

	nq_id = (nq_type * NQ_TYPE_PER_ENGINE) + eng_index;
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	if (nd->nq_mc[nc_id][nq_id] == NULL) {
		return 0;
	}

	apb_base = nd->npdev.bar0 + pu_get_relative_offset(nc_id);
	switch (nq_type) {
	case NQ_TYPE_ERROR:
		pu_write_error_notification_cfg_2(apb_base, 0);
		pu_write_error_notification_cfg_0(apb_base, 0);
		pu_write_error_notification_cfg_1(apb_base, 0);
		break;
	case NQ_TYPE_EVENT:
		pu_write_event_notification_cfg_2(apb_base, 0);
		pu_write_event_notification_cfg_0(apb_base, 0);
		pu_write_event_notification_cfg_1(apb_base, 0);
		break;
	case NQ_TYPE_NOTIFY:
		pu_write_expl_notification_cfg_2(apb_base, eng_index, 0, 0);
		pu_write_expl_notification_cfg_0(apb_base, eng_index, 0, 0);
		pu_write_expl_notification_cfg_1(apb_base, eng_index, 0, 0);
		break;
	case NQ_TYPE_TRACE:
		pu_write_impl_notification_cfg_2(apb_base, eng_index, 0, 0);
		pu_write_impl_notification_cfg_0(apb_base, eng_index, 0, 0);
		pu_write_impl_notification_cfg_1(apb_base, eng_index, 0, 0);
		break;
	default:
		return -1;
	}

	// sleep 1msec so that hw can drain
	msleep(1);

	mc_free(&nd->nq_mc[nc_id][nq_id]);
	return 0;
}

void nc_nq_destroy_all(struct neuron_device *nd)
{
	u8 nc_id;
	u8 eng_index;
	u8 nq_type;

	for (nc_id = 0; nc_id < V1_NC_PER_DEVICE; nc_id++) {
		for (eng_index = 0; eng_index < MAX_NQ_ENGINE; eng_index++) {
			for (nq_type = 0; nq_type < NQ_TYPE_PER_ENGINE; nq_type++) {
				nc_nq_destroy(nd, nc_id, eng_index, nq_type);
			}
		}
	}
}

int nc_nq_mmap(struct neuron_device *nd, u8 nc_id, u8 eng_index, u32 nq_type,
	       struct vm_area_struct *vma)
{
	struct mem_chunk *mc;
	u8 nq_id;
	int ret;

	if (nd == NULL || nc_id >= V1_NC_PER_DEVICE)
		return -EINVAL;
	nq_id = (nq_type * NQ_TYPE_PER_ENGINE) + eng_index;
	if (nq_id >= MAX_NQ_SUPPORTED)
		return -EINVAL;

	mc = nd->nq_mc[nc_id][nq_id];
	if (mc == NULL)
		return -EINVAL;

#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_nc_mmap, 1))
		return -ENOSPC;
#endif

	ret = remap_pfn_range(vma, vma->vm_start, PHYS_PFN(mc->pa), mc->size, vma->vm_page_prot);
	if (ret != 0)
		return ret;

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY;

	return 0;
}
