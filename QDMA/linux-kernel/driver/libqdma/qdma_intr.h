/*
 * This file is part of the Xilinx DMA IP Core driver for Linux
 *
 * Copyright (c) 2017-2022, Xilinx, Inc. All rights reserved.
 * Copyright (c) 2022-2024, Advanced Micro Devices, Inc. All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#ifndef LIBQDMA_QDMA_INTR_H_
#define LIBQDMA_QDMA_INTR_H_
/**
 * @file
 * @brief This file contains the declarations for qdma dev interrupt handlers
 *
 */
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/bitops.h>
#include <asm/byteorder.h>
#include "qdma_descq.h"
/**
 * forward declaration for xlnx_dma_dev
 */
struct xlnx_dma_dev;

/*
 * Interrupt aggregation ring entries are written by hardware. Keep the wire
 * entry as one little-endian 64-bit word; C bitfield layout is not portable
 * across ARM/x86 compilers or CPU endian modes.
 */
union qdma_intr_ring {
	__le64 word;
} __packed;

#define QDMA_INTR_RING_COAL_COLOR_MASK		BIT_ULL(63)
#define QDMA_INTR_RING_CPM_INTR_TYPE_MASK	BIT_ULL(51)
#define QDMA_INTR_RING_CPM_QID_MASK		GENMASK_ULL(62, 52)
#define QDMA_INTR_RING_GENERIC_INTR_TYPE_MASK	BIT_ULL(38)
#define QDMA_INTR_RING_GENERIC_QID_MASK		GENMASK_ULL(62, 39)

static inline u64 qdma_intr_ring_word(const union qdma_intr_ring *entry)
{
#ifdef __READ_ONCE_DEFINED__
	return le64_to_cpu(READ_ONCE(entry->word));
#else
	return le64_to_cpu(entry->word);
#endif
}

static inline u8 qdma_intr_ring_color(u64 word)
{
	return (word & QDMA_INTR_RING_COAL_COLOR_MASK) ? 1 : 0;
}

static inline u8 qdma_intr_ring_intr_type(u64 word, bool cpm)
{
	return cpm ? ((word & QDMA_INTR_RING_CPM_INTR_TYPE_MASK) ? 1 : 0) :
		     ((word & QDMA_INTR_RING_GENERIC_INTR_TYPE_MASK) ? 1 : 0);
}

static inline u32 qdma_intr_ring_qid(u64 word, bool cpm)
{
	if (cpm)
		return (word & QDMA_INTR_RING_CPM_QID_MASK) >> 52;

	return (word & QDMA_INTR_RING_GENERIC_QID_MASK) >> 39;
}


/*****************************************************************************/
/**
 * intr_teardown() - un register the interrupts for the device
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	none
 *****************************************************************************/
void intr_teardown(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * intr_setup() - register the interrupts for the device
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	0: success
 * @return	<0: failure
 *****************************************************************************/
int intr_setup(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * intr_ring_teardown() - delete the interrupt ring
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	none
 *****************************************************************************/
void intr_ring_teardown(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * intr_context_setup() - set up the interrupt context
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	0: success
 * @return	<0: failure
 *****************************************************************************/
int intr_context_setup(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * intr_ring_setup() - create the interrupt ring
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	0: success
 * @return	<0: failure
 *****************************************************************************/
int intr_ring_setup(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * intr_legacy_setup() - setup the legacy interrupt handler
 *
 * @param[in]	descq:	descq on which the interrupt needs to be setup
 *
 * @return	0: success
 * @return	<0: failure
 *****************************************************************************/
int intr_legacy_setup(struct qdma_descq *descq);

/*****************************************************************************/
/**
 * intr_legacy_clear() - clear the legacy interrupt handler
 *
 * @param[in]	descq:	descq on which the interrupt needs to be cleared
 *
 * @return	0: success
 * @return	<0: failure
 *****************************************************************************/
void intr_legacy_clear(struct qdma_descq *descq);


/*****************************************************************************/
/**
 * intr_work() - attach the top half for the interrupt
 *
 * @param[in]	work:		pointer to struct work_struct
 *
 * @return	none
 *****************************************************************************/
void intr_work(struct work_struct *work);

/*****************************************************************************/
/**
 * qdma_err_intr_setup() - set up the error interrupt
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	none
 *****************************************************************************/
int qdma_err_intr_setup(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * qdma_enable_hw_err() - enable the hw errors
 *
 * @param[in]	xdev:		pointer to xdev
 * @param[in]	hw_err_type:	hw error type
 *
 * @return	none
 *****************************************************************************/
void qdma_enable_hw_err(struct xlnx_dma_dev *xdev, u8 hw_err_type);

/*****************************************************************************/
/**
 * get_intr_ring_index() - get the interrupt ring index based on vector index
 *
 * @param[in]	xdev:		pointer to xdev
 * @param[in]	vector_index:	vector index
 *
 * @return	0: success
 * @return	<0: failure
 *****************************************************************************/
int get_intr_ring_index(struct xlnx_dma_dev *xdev, u32 vector_index);

#ifndef __QDMA_VF__
#ifdef ERR_DEBUG
/*****************************************************************************/
/**
 * err_stat_handler() - error interrupt handler
 *
 * @param[in]	xdev:		pointer to xdev
 *
 * @return	none
 *****************************************************************************/
void err_stat_handler(struct xlnx_dma_dev *xdev);
#endif
#endif

#endif /* LIBQDMA_QDMA_DEVICE_H_ */

