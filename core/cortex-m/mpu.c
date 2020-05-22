/* Copyright 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* MPU module for Chrome EC */

#include "mpu.h"
#include "console.h"
#include "cpu.h"
#include "registers.h"
#include "task.h"
#include "util.h"

/**
 * @return Number of regions supported by the MPU. 0 means the processor does
 * not implement an MPU.
 */
int mpu_num_regions(void)
{
	return MPU_TYPE_REG_COUNT(mpu_get_type());
}

/**
 * @return true if processor has MPU, false otherwise
 */
bool has_mpu(void)
{
	return mpu_num_regions() != 0;
}

/**
 * @return true if MPU has unified instruction and data maps, false otherwise
 */
bool mpu_is_unified(void)
{
	return (mpu_get_type() & MPU_TYPE_UNIFIED_MASK) == 0;
}


/**
 * Update a memory region.
 *
 * region: index of the region to update
 * addr: base address of the region
 * size_bit: size of the region in power of two.
 * attr: attribute bits. Current value will be overwritten if enable is true.
 * enable: enables the region if non zero. Otherwise, disables the region.
 * srd: subregion mask to partition region into 1/8ths, 0 = subregion enabled.
 *
 * Based on 3.1.4.1 'Updating an MPU Region' of Stellaris LM4F232H5QC Datasheet
 */
int mpu_update_region(uint8_t region, uint32_t addr, uint8_t size_bit,
		      uint16_t attr, uint8_t enable, uint8_t srd)
{
	/*
	 * Note that on the Cortex-M3, Cortex-M4, and Cortex-M7, the base
	 * address used for an MPU region must be aligned to the size of the
	 * region:
	 *
	 * https://developer.arm.com/docs/dui0553/a/cortex-m4-peripherals/optional-memory-protection-unit/mpu-region-base-address-register
	 * https://developer.arm.com/docs/dui0552/a/cortex-m3-peripherals/optional-memory-protection-unit/mpu-region-base-address-register
	 * https://developer.arm.com/docs/dui0646/a/cortex-m7-peripherals/optional-memory-protection-unit/mpu-region-base-address-register#BABDAHJG
	 */
	if (!is_aligned(addr, BIT(size_bit)))
		return -EC_ERROR_INVAL;

	if (region >= mpu_num_regions())
		return -EC_ERROR_INVAL;

	if (size_bit < MPU_SIZE_BITS_MIN)
		return -EC_ERROR_INVAL;

	asm volatile("isb; dsb;");

	MPU_NUMBER = region;
	MPU_SIZE &= ~1;					/* Disable */
	if (enable) {
		MPU_BASE = addr;
		/*
		 * MPU_ATTR = attr;
		 * MPU_SIZE = (srd << 8) | ((size_bit - 1) << 1) | 1;
		 *
		 * WORKAROUND: the 2 half-word accesses above should work
		 * according to the doc, but they don't ..., do a single 32-bit
		 * one.
		 */
		REG32(&MPU_SIZE) = ((uint32_t)attr << 16)
				 | (srd << 8) | ((size_bit - 1) << 1) | 1;
	}

	asm volatile("isb; dsb;");

	return EC_SUCCESS;
}

/**
 * Configure a region
 *
 * region: index of the region to update
 * addr: Base address of the region
 * size: Size of the region in bytes
 * attr: Attribute bits. Current value will be overwritten if enable is set.
 * enable: Enables the region if non zero. Otherwise, disables the region.
 *
 * Returns EC_SUCCESS on success or -EC_ERROR_INVAL if a parameter is invalid.
 */
int mpu_config_region(uint8_t region, uint32_t addr, uint32_t size,
		      uint16_t attr, uint8_t enable)
{
	int rv;
	int size_bit = 0;
	uint8_t blocks, srd1, srd2;

	if (!size)
		return EC_SUCCESS;

	/* Bit position of first '1' in size */
	size_bit = 31 - __builtin_clz(size);
	/* Min. region size is 32 bytes */
	if (size_bit < 5)
		return -EC_ERROR_INVAL;

	/* If size is a power of 2 then represent it with a single MPU region */
	if (POWER_OF_TWO(size))
		return mpu_update_region(region, addr, size_bit, attr, enable,
					 0);

	/* Sub-regions are not supported for region <= 128 bytes */
	if (size_bit < 7)
		return -EC_ERROR_INVAL;
	/* Verify we can represent range with <= 2 regions */
	if (size & ~(0x3f << (size_bit - 5)))
		return -EC_ERROR_INVAL;

	/*
	 * Round up size of first region to power of 2.
	 * Calculate the number of fully occupied blocks (block size =
	 * region size / 8) in the first region.
	 */
	blocks = size >> (size_bit - 2);

	/* Represent occupied blocks of two regions with srd mask. */
	srd1 = BIT(blocks) - 1;
	srd2 = (1 << ((size >> (size_bit - 5)) & 0x7)) - 1;

	/*
	 * Second region not supported for DATA_RAM_TEXT, also verify size of
	 * second region is sufficient to support sub-regions.
	 */
	if (srd2 && (region == REGION_DATA_RAM_TEXT || size_bit < 10))
		return -EC_ERROR_INVAL;

	/* Write first region. */
	rv = mpu_update_region(region, addr, size_bit + 1, attr, enable, ~srd1);
	if (rv != EC_SUCCESS)
		return rv;

	/*
	 * Second protection region (if necessary) begins at the first block
	 * we marked unoccupied in the first region.
	 * Size of the second region is the block size of first region.
	 */
	addr += (1 << (size_bit - 2)) * blocks;

	/*
	 * Now represent occupied blocks in the second region. It's possible
	 * that the first region completely represented the occupied area, if
	 * so then no second protection region is required.
	 */
	if (srd2)
		rv = mpu_update_region(region + 1, addr, size_bit - 2, attr,
				       enable, ~srd2);

	return rv;
}

/**
 * Set a region executable and read-write.
 *
 * region: index of the region
 * addr: base address of the region
 * size: size of the region in bytes
 * texscb: TEX and SCB bit field
 */
static int mpu_unlock_region(uint8_t region, uint32_t addr, uint32_t size,
			     uint8_t texscb)
{
	return mpu_config_region(region, addr, size,
				 MPU_ATTR_RW_RW | texscb, 1);
}

void mpu_enable(void)
{
	MPU_CTRL |= MPU_CTRL_PRIVDEFEN | MPU_CTRL_HFNMIENA | MPU_CTRL_ENABLE;
}

void mpu_disable(void)
{
	MPU_CTRL &= ~(MPU_CTRL_PRIVDEFEN | MPU_CTRL_HFNMIENA | MPU_CTRL_ENABLE);
}

uint32_t mpu_get_type(void)
{
	return MPU_TYPE;
}

int mpu_protect_data_ram(void)
{
	int ret;

	/* Prevent code execution from data RAM */
	ret = mpu_config_region(REGION_DATA_RAM,
				CONFIG_RAM_BASE,
				CONFIG_DATA_RAM_SIZE,
				MPU_ATTR_XN |
				MPU_ATTR_RW_RW |
				MPU_ATTR_INTERNAL_SRAM,
				1);
	if (ret != EC_SUCCESS)
		return ret;

	/* Exempt the __iram_text section */
	return mpu_unlock_region(
		REGION_DATA_RAM_TEXT, (uint32_t)&__iram_text_start,
		(uint32_t)(&__iram_text_end - &__iram_text_start),
		MPU_ATTR_INTERNAL_SRAM);
}

#if defined(CONFIG_EXTERNAL_STORAGE) || !defined(CONFIG_FLASH_PHYSICAL)
int mpu_protect_code_ram(void)
{
	/* Prevent write access to code RAM */
	return mpu_config_region(REGION_STORAGE,
				 CONFIG_PROGRAM_MEMORY_BASE + CONFIG_RO_MEM_OFF,
				 CONFIG_CODE_RAM_SIZE,
				 MPU_ATTR_RO_NO | MPU_ATTR_INTERNAL_SRAM,
				 1);
}
#else
int mpu_lock_ro_flash(void)
{
	/* Prevent execution from internal mapped RO flash */
	return mpu_config_region(REGION_STORAGE,
				 CONFIG_MAPPED_STORAGE_BASE + CONFIG_RO_MEM_OFF,
				 CONFIG_RO_SIZE,
				 MPU_ATTR_XN | MPU_ATTR_RW_RW |
				 MPU_ATTR_FLASH_MEMORY, 1);
}

int mpu_lock_rw_flash(void)
{
	/* Prevent execution from internal mapped RW flash */
	const uint16_t mpu_attr = MPU_ATTR_XN | MPU_ATTR_RW_RW |
				  MPU_ATTR_FLASH_MEMORY;
	const uint32_t rw_start_address =
		CONFIG_MAPPED_STORAGE_BASE + CONFIG_RW_MEM_OFF;

	/*
	 * Least significant set bit of the address determines the max size of
	 * the region because on the Cortex-M3, Cortex-M4 and Cortex-M7, the
	 * address used for an MPU region must be aligned to the size.
	 */
	const int aligned_size_bit =
		__fls(rw_start_address & -rw_start_address);
	const uint32_t first_region_size =
		MIN(BIT(aligned_size_bit), CONFIG_RW_SIZE);
	const uint32_t second_region_address =
		rw_start_address + first_region_size;
	const uint32_t second_region_size = CONFIG_RW_SIZE - first_region_size;
	int rv;

	rv = mpu_config_region(REGION_STORAGE, rw_start_address,
			       first_region_size, mpu_attr, 1);
	if ((rv != EC_SUCCESS) || (second_region_size == 0))
		return rv;

	/* If this fails then it's impossible to represent with two regions. */
	return mpu_config_region(REGION_STORAGE2, second_region_address,
				 second_region_size, mpu_attr, 1);
}
#endif /* !CONFIG_EXTERNAL_STORAGE */

#ifdef CONFIG_ROLLBACK_MPU_PROTECT
int mpu_lock_rollback(int lock)
{
	int rv;
	int num_mpu_regions = mpu_num_regions();

	const uint32_t rollback_region_start_address =
		CONFIG_MAPPED_STORAGE_BASE + CONFIG_ROLLBACK_OFF;
	const uint32_t rollback_region_total_size = CONFIG_ROLLBACK_SIZE;
	const uint16_t mpu_attr =
		MPU_ATTR_XN /* Execute never */ |
		MPU_ATTR_NO_NO /* No access (privileged or unprivileged */;

	/*
	 * Originally rollback MPU support was added on Cortex-M7, which
	 * supports 16 MPU regions and has rollback region aligned in a way
	 * that we can use a single region.
	 */
	uint8_t rollback_mpu_region = REGION_ROLLBACK;

	if (rollback_mpu_region < num_mpu_regions) {
		rv = mpu_config_region(rollback_mpu_region,
				       rollback_region_start_address,
				       rollback_region_total_size, mpu_attr,
				       lock);
		return rv;
	}

	/*
	 * If we get here, we can't use REGION_ROLLBACK because our MPU doesn't
	 * have enough regions. Instead, we choose unused MPU regions.
	 *
	 * Note that on the Cortex-M3, Cortex-M4, and Cortex-M7, the base
	 * address used for an MPU region must be aligned to the size of the
	 * region, so it's not possible to use a single region to protect the
	 * entire rollback flash on the STM32F412 (bloonchipper); we have to
	 * use two.
	 *
	 * See mpu_update_region for alignment details.
	 */

	rollback_mpu_region = REGION_CHIP_RESERVED;
	rv = mpu_config_region(rollback_mpu_region,
			       rollback_region_start_address,
			       rollback_region_total_size / 2, mpu_attr, lock);
	if (rv != EC_SUCCESS)
		return rv;

	rollback_mpu_region = REGION_STORAGE2;
	rv = mpu_config_region(rollback_mpu_region,
			       rollback_region_start_address +
				       (rollback_region_total_size / 2),
			       rollback_region_total_size / 2, mpu_attr, lock);
	return rv;
}
#endif

#ifdef CONFIG_CHIP_UNCACHED_REGION
/* Store temporarily the regions ranges to use them for the MPU configuration */
#define REGION(_name, _flag, _start, _size) \
	static const uint32_t CONCAT2(_region_start_, _name) \
		__attribute__((unused, section(".unused"))) = _start; \
	static const uint32_t CONCAT2(_region_size_, _name) \
		__attribute__((unused, section(".unused"))) = _size;
#include "memory_regions.inc"
#undef REGION
#endif /* CONFIG_CHIP_UNCACHED_REGION */

int mpu_pre_init(void)
{
	int i;
	int num_mpu_regions;
	int rv;

	if (!has_mpu())
		return EC_ERROR_HW_INTERNAL;

	num_mpu_regions = mpu_num_regions();

	/* Supports MPU with 8 or 16 unified regions */
	if (!mpu_is_unified() ||
	    (num_mpu_regions != 8 && num_mpu_regions != 16))
		return EC_ERROR_UNIMPLEMENTED;

	mpu_disable();

	for (i = 0; i < num_mpu_regions; ++i) {
		/*
		 * Disable all regions.
		 *
		 * We use the smallest possible size (32 bytes), but it
		 * doesn't really matter since the regions are disabled.
		 *
		 * Use the fixed SRAM region base to ensure base is aligned
		 * to the region size.
		 */
		rv = mpu_update_region(i, CORTEX_M_SRAM_BASE, MPU_SIZE_BITS_MIN,
			0, 0, 0);
		if (rv != EC_SUCCESS)
			return rv;
	}

	if (IS_ENABLED(CONFIG_ROLLBACK_MPU_PROTECT)) {
		rv = mpu_lock_rollback(1);
		if (rv != EC_SUCCESS)
			return rv;
	}

	if (IS_ENABLED(CONFIG_ARMV7M_CACHE)) {
#ifdef CONFIG_CHIP_UNCACHED_REGION
		rv = mpu_config_region(
			REGION_UNCACHED_RAM,
			CONCAT2(_region_start_, CONFIG_CHIP_UNCACHED_REGION),
			CONCAT2(_region_size_, CONFIG_CHIP_UNCACHED_REGION),
			MPU_ATTR_XN | MPU_ATTR_RW_RW, 1);
		if (rv != EC_SUCCESS)
			return rv;

#endif
	}

	mpu_enable();

	if (IS_ENABLED(CONFIG_ARMV7M_CACHE))
		cpu_enable_caches();

	return EC_SUCCESS;
}
