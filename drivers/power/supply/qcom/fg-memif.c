// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2018, 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"FG: %s: " fmt, __func__

#include "fg-core.h"
#include "fg-reg.h"

/* Generic definitions */
#define RETRY_COUNT		3
#define DEBUG_PRINT_BUFFER_SIZE	64

enum {
	FG_READ = 0,
	FG_WRITE,
};

static int fg_set_address(struct fg_dev *fg, u16 address)
{
	u8 buffer[2];
	int rc;

	buffer[0] = address & 0xFF;
	buffer[1] = address >> 8;

	/* MSB has to be written zero for GEN3 FG */
	if (fg->version == GEN3_FG)
		buffer[1] = 0;

	rc = fg_write(fg, MEM_IF_ADDR_LSB(fg), buffer, 2);
	if (rc < 0) {
		pr_err("failed to write to 0x%04X, rc=%d\n",
			MEM_IF_ADDR_LSB(fg), rc);
		return rc;
	}

	return rc;
}

static int fg_config_access_mode(struct fg_dev *fg, bool access, bool burst)
{
	int rc;
	u8 intf_ctl = 0;

	fg_dbg(fg, FG_SRAM_READ | FG_SRAM_WRITE, "access: %d burst: %d\n",
		access, burst);

	WARN_ON(burst && fg->use_ima_single_mode);
	intf_ctl = ((access == FG_WRITE) ? IMA_WR_EN_BIT : 0) |
			(burst ? MEM_ACS_BURST_BIT : 0);

	rc = fg_masked_write(fg, MEM_IF_IMA_CTL(fg), IMA_CTL_MASK,
			intf_ctl);
	if (rc < 0) {
		pr_err("failed to write to 0x%04x, rc=%d\n",
			MEM_IF_IMA_CTL(fg), rc);
		return -EIO;
	}

	return rc;
}

static int fg_run_iacs_clear_sequence(struct fg_dev *fg)
{
	u8 val, hw_sts, exp_sts;
	int rc, tries = 250;

	/*
	 * Values to write for running IACS clear sequence comes from
	 * hardware documentation.
	 */
	rc = fg_masked_write(fg, MEM_IF_IMA_CFG(fg),
			IACS_CLR_BIT | STATIC_CLK_EN_BIT,
			IACS_CLR_BIT | STATIC_CLK_EN_BIT);
	if (rc < 0) {
		pr_err("failed to write 0x%04x, rc=%d\n", MEM_IF_IMA_CFG(fg),
			rc);
		return rc;
	}

	rc = fg_config_access_mode(fg, FG_READ, false);
	if (rc < 0) {
		pr_err("failed to write to 0x%04x, rc=%d\n",
			MEM_IF_IMA_CTL(fg), rc);
		return rc;
	}

	rc = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg),
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT,
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT);
	if (rc < 0) {
		pr_err("failed to set ima_req_access bit rc=%d\n", rc);
		return rc;
	}

	/* Delay for the clock to reach FG */
	usleep_range(35, 40);

	while (1) {
		if (fg->version == GEN4_FG) {
			val = 0x4;
			rc = fg_write(fg, MEM_IF_ADDR_MSB(fg), &val, 1);
			if (rc < 0) {
				pr_err("failed to write 0x%04x, rc=%d\n",
					MEM_IF_ADDR_MSB(fg), rc);
				return rc;
			}

			val = 0;
			rc = fg_write(fg, MEM_IF_WR_DATA1(fg), &val, 1);
			if (rc < 0) {
				pr_err("failed to write 0x%04x, rc=%d\n",
					MEM_IF_WR_DATA1(fg), rc);
				return rc;
			}

			rc = fg_read(fg, MEM_IF_RD_DATA1(fg), &val, 1);
			if (rc < 0) {
				pr_err("failed to read 0x%04x, rc=%d\n",
					MEM_IF_RD_DATA1(fg), rc);
				return rc;
			}
		} else { /* GEN3 FG */
			val = 0;
			rc = fg_write(fg, MEM_IF_ADDR_MSB(fg), &val, 1);
			if (rc < 0) {
				pr_err("failed to write 0x%04x, rc=%d\n",
					MEM_IF_ADDR_MSB(fg), rc);
				return rc;
			}

			val = 0;
			rc = fg_write(fg, MEM_IF_WR_DATA3(fg), &val, 1);
			if (rc < 0) {
				pr_err("failed to write 0x%04x, rc=%d\n",
					MEM_IF_WR_DATA3(fg), rc);
				return rc;
			}

			rc = fg_read(fg, MEM_IF_RD_DATA3(fg), &val, 1);
			if (rc < 0) {
				pr_err("failed to read 0x%04x, rc=%d\n",
					MEM_IF_RD_DATA3(fg), rc);
				return rc;
			}
		}

		/* Delay for IMA hardware to clear */
		usleep_range(35, 40);

		rc = fg_read(fg, MEM_IF_IMA_HW_STS(fg), &hw_sts, 1);
		if (rc < 0) {
			pr_err("failed to read ima_hw_sts rc=%d\n", rc);
			return rc;
		}

		if (hw_sts != 0)
			continue;

		rc = fg_read(fg, MEM_IF_IMA_EXP_STS(fg), &exp_sts, 1);
		if (rc < 0) {
			pr_err("failed to read ima_exp_sts rc=%d\n", rc);
			return rc;
		}

		if (exp_sts == 0 || !(--tries))
			break;
	}

	if (!tries)
		pr_err("Failed to clear the error? hw_sts: %x exp_sts: %d\n",
			hw_sts, exp_sts);

	rc = fg_masked_write(fg, MEM_IF_IMA_CFG(fg), IACS_CLR_BIT, 0);
	if (rc < 0) {
		pr_err("failed to write 0x%04x, rc=%d\n", MEM_IF_IMA_CFG(fg),
			rc);
		return rc;
	}

	udelay(5);

	rc = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg),
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT, 0);
	if (rc < 0) {
		pr_err("failed to write to 0x%04x, rc=%d\n",
			MEM_IF_MEM_INTF_CFG(fg), rc);
		return rc;
	}

	/* Delay before next transaction is attempted */
	usleep_range(35, 40);
	fg_dbg(fg, FG_SRAM_READ | FG_SRAM_WRITE, "IACS clear sequence complete\n");
	return rc;
}

int fg_clear_dma_errors_if_any(struct fg_dev *fg)
{
	int rc;
	u8 dma_sts;
	bool error_present;

	rc = fg_read(fg, MEM_IF_DMA_STS(fg), &dma_sts, 1);
	if (rc < 0) {
		pr_err("failed to read addr=0x%04x, rc=%d\n",
			MEM_IF_DMA_STS(fg), rc);
		return rc;
	}
	fg_dbg(fg, FG_STATUS, "dma_sts: %x\n", dma_sts);

	error_present = dma_sts & (DMA_WRITE_ERROR_BIT | DMA_READ_ERROR_BIT);
	rc = fg_masked_write(fg, MEM_IF_DMA_CTL(fg), DMA_CLEAR_LOG_BIT,
			error_present ? DMA_CLEAR_LOG_BIT : 0);
	if (rc < 0) {
		pr_err("failed to write addr=0x%04x, rc=%d\n",
			MEM_IF_DMA_CTL(fg), rc);
		return rc;
	}

	return 0;
}

int fg_clear_ima_errors_if_any(struct fg_dev *fg, bool check_hw_sts)
{
	int rc = 0;
	u8 err_sts, exp_sts = 0, hw_sts = 0;
	bool run_err_clr_seq = false;

	rc = fg_read(fg, MEM_IF_IMA_EXP_STS(fg), &exp_sts, 1);
	if (rc < 0) {
		pr_err("failed to read ima_exp_sts rc=%d\n", rc);
		return rc;
	}

	rc = fg_read(fg, MEM_IF_IMA_HW_STS(fg), &hw_sts, 1);
	if (rc < 0) {
		pr_err("failed to read ima_hw_sts rc=%d\n", rc);
		return rc;
	}

	rc = fg_read(fg, MEM_IF_IMA_ERR_STS(fg), &err_sts, 1);
	if (rc < 0) {
		pr_err("failed to read ima_err_sts rc=%d\n", rc);
		return rc;
	}

	fg_dbg(fg, FG_SRAM_READ | FG_SRAM_WRITE, "ima_err_sts=%x ima_exp_sts=%x ima_hw_sts=%x\n",
		err_sts, exp_sts, hw_sts);

	if (check_hw_sts) {
		/*
		 * Lower nibble should be equal to upper nibble before SRAM
		 * transactions begins from SW side. If they are unequal, then
		 * the error clear sequence should be run irrespective of IMA
		 * exception errors.
		 */
		if ((hw_sts & 0x0F) != hw_sts >> 4) {
			pr_err("IMA HW not in correct state, hw_sts=%x\n",
				hw_sts);
			run_err_clr_seq = true;
		}
	}

	if (exp_sts & (IACS_ERR_BIT | XCT_TYPE_ERR_BIT | DATA_RD_ERR_BIT |
		DATA_WR_ERR_BIT | ADDR_BURST_WRAP_BIT | ADDR_STABLE_ERR_BIT)) {
		pr_err("IMA exception bit set, exp_sts=%x\n", exp_sts);
		run_err_clr_seq = true;
	}

	if (run_err_clr_seq) {
		/* clear the error */
		rc = fg_run_iacs_clear_sequence(fg);
		if (rc < 0) {
			pr_err("failed to run iacs clear sequence rc=%d\n", rc);
			return rc;
		}

		/* Retry again as there was an error in the transaction */
		return -EAGAIN;
	}

	return rc;
}

static int fg_check_iacs_ready(struct fg_dev *fg)
{
	int rc = 0, tries = 250;
	u8 ima_opr_sts = 0;

	/*
	 * Additional delay to make sure IACS ready bit is set after
	 * Read/Write operation.
	 */

	usleep_range(30, 35);
	while (1) {
		rc = fg_read(fg, MEM_IF_IMA_OPR_STS(fg), &ima_opr_sts, 1);
		if (rc < 0) {
			pr_err("failed to read 0x%04x, rc=%d\n",
				MEM_IF_IMA_OPR_STS(fg), rc);
			return rc;
		}

		if (ima_opr_sts & IACS_RDY_BIT)
			break;

		if (!(--tries))
			break;

		/* delay for iacs_ready to be asserted */
		usleep_range(5000, 7000);
	}

	if (!tries) {
		pr_err("IACS_RDY not set, opr_sts: %d\n", ima_opr_sts);
		/* check for error condition */
		rc = fg_clear_ima_errors_if_any(fg, false);
		if (rc < 0) {
			if (rc != -EAGAIN)
				pr_err("Failed to check for ima errors rc=%d\n",
					rc);
			return rc;
		}

		return -EBUSY;
	}

	return 0;
}

static int __fg_interleaved_mem_write(struct fg_dev *fg, u16 address,
				int offset, u8 *val, int len)
{
	int rc = 0, i;
	u8 *ptr = val, byte_enable = 0, num_bytes = 0, dummy_byte = 0;

	fg_dbg(fg, FG_SRAM_WRITE, "length %d addr=%02X offset=%d\n", len,
		address, offset);

	while (len > 0) {
		num_bytes = (offset + len) > fg->sram.num_bytes_per_word ?
				(fg->sram.num_bytes_per_word - offset) : len;

		/* write to byte_enable */
		for (i = offset; i < (offset + num_bytes); i++)
			byte_enable |= BIT(i);

		rc = fg_write(fg, MEM_IF_IMA_BYTE_EN(fg), &byte_enable, 1);
		if (rc < 0) {
			pr_err("Unable to write to byte_en_reg rc=%d\n",
				rc);
			return rc;
		}

		/* write data */
		rc = fg_write(fg, MEM_IF_WR_DATA0(fg) + offset, ptr,
				num_bytes);
		if (rc < 0) {
			pr_err("failed to write to 0x%04x, rc=%d\n",
				MEM_IF_WR_DATA0(fg) + offset, rc);
			return rc;
		}

		/*
		 * The last-byte WR_DATA3/1 starts the write transaction.
		 * Write a dummy value to WR_DATA3/1 if it does not have
		 * valid data. This dummy data is not written to the
		 * SRAM as byte_en for WR_DATA3/1 is not set.
		 */
		if (fg->version == GEN3_FG && !(byte_enable & BIT(3))) {
			rc = fg_write(fg, MEM_IF_WR_DATA3(fg), &dummy_byte,
					1);
			if (rc < 0) {
				pr_err("failed to write dummy-data to WR_DATA3 rc=%d\n",
					rc);
				return rc;
			}
		} else if (fg->version == GEN4_FG && !(byte_enable & BIT(1))) {
			rc = fg_write(fg, MEM_IF_WR_DATA1(fg), &dummy_byte,
					1);
			if (rc < 0) {
				pr_err("failed to write dummy-data to WR_DATA1 rc=%d\n",
					rc);
				return rc;
			}
		}

		/* check for error condition */
		rc = fg_clear_ima_errors_if_any(fg, false);
		if (rc < 0) {
			if (rc == -EAGAIN)
				pr_err("IMA error cleared, address [%d %d] len %d\n",
					address, offset, len);
			else
				pr_err("Failed to check for ima errors rc=%d\n",
					rc);
			return rc;
		}

		ptr += num_bytes;
		len -= num_bytes;
		offset = byte_enable = 0;

		if (fg->use_ima_single_mode && len) {
			address++;
			rc = fg_set_address(fg, address);
			if (rc < 0) {
				pr_err("failed to set address rc = %d\n", rc);
				return rc;
			}
		}

		rc = fg_check_iacs_ready(fg);
		if (rc < 0) {
			pr_debug("IACS_RDY failed rc=%d\n", rc);
			return rc;
		}
	}

	return rc;
}

static int __fg_interleaved_mem_read(struct fg_dev *fg, u16 address,
				int offset, u8 *val, int len)
{
	int rc = 0, total_len;
	u8 *rd_data = val, num_bytes;
	char str[DEBUG_PRINT_BUFFER_SIZE];

	fg_dbg(fg, FG_SRAM_READ, "length %d addr=%02X\n", len, address);

	total_len = len;
	while (len > 0) {
		num_bytes = (offset + len) > fg->sram.num_bytes_per_word ?
				(fg->sram.num_bytes_per_word - offset) : len;
		rc = fg_read(fg, MEM_IF_RD_DATA0(fg) + offset, rd_data,
				num_bytes);
		if (rc < 0) {
			pr_err("failed to read 0x%04x, rc=%d\n",
				MEM_IF_RD_DATA0(fg) + offset, rc);
			return rc;
		}

		rd_data += num_bytes;
		len -= num_bytes;
		offset = 0;

		/* check for error condition */
		rc = fg_clear_ima_errors_if_any(fg, false);
		if (rc < 0) {
			if (rc == -EAGAIN)
				pr_err("IMA error cleared, address [%d %d] len %d\n",
					address, offset, len);
			else
				pr_err("Failed to check for ima errors rc=%d\n",
					rc);
			return rc;
		}

		if (fg->use_ima_single_mode) {
			if (len) {
				address++;
				rc = fg_set_address(fg, address);
				if (rc < 0) {
					pr_err("failed to set address rc = %d\n",
						rc);
					return rc;
				}
			}
		} else {
			if (len && len < fg->sram.num_bytes_per_word) {
				/*
				 * Move to single mode. Changing address is not
				 * required here as it must be in burst mode.
				 * Address will get incremented internally by FG
				 * HW once the MSB of RD_DATA is read.
				 */
				rc = fg_config_access_mode(fg, FG_READ,
								false);
				if (rc < 0) {
					pr_err("failed to move to single mode rc=%d\n",
						rc);
					return -EIO;
				}
			}
		}

		rc = fg_check_iacs_ready(fg);
		if (rc < 0) {
			pr_debug("IACS_RDY failed rc=%d\n", rc);
			return rc;
		}
	}

	if (*fg->debug_mask & FG_SRAM_READ) {
		fill_string(str, DEBUG_PRINT_BUFFER_SIZE, val, total_len);
		pr_info("data read: %s\n", str);
	}

	return rc;
}

static int fg_get_mem_access_status(struct fg_dev *fg, bool *status)
{
	int rc;
	u8 mem_if_sts;

	rc = fg_read(fg, MEM_IF_MEM_INTF_CFG(fg), &mem_if_sts, 1);
	if (rc < 0) {
		pr_err("failed to read rif_mem status rc=%d\n", rc);
		return rc;
	}

	*status = mem_if_sts & MEM_ACCESS_REQ_BIT;
	return 0;
}

static bool is_mem_access_available(struct fg_dev *fg, int access)
{
	bool rif_mem_sts = true;
	int rc, time_count = 0;

	while (1) {
		rc = fg_get_mem_access_status(fg, &rif_mem_sts);
		if (rc < 0)
			return rc;

		/* This is an inverting logic */
		if (!rif_mem_sts)
			break;

		fg_dbg(fg, FG_SRAM_READ | FG_SRAM_WRITE, "MEM_ACCESS_REQ is not clear yet for IMA_%s\n",
			access ? "write" : "read");

		/*
		 * Try this no more than 4 times. If MEM_ACCESS_REQ is not
		 * clear, then return an error instead of waiting for it again.
		 */
		if  (time_count > 4) {
			pr_err("Tried 4 times(~16ms) polling MEM_ACCESS_REQ\n");
			return false;
		}

		/* Wait for 4ms before reading MEM_ACCESS_REQ again */
		usleep_range(4000, 4100);
		time_count++;
	}
	return true;
}

static int fg_interleaved_mem_config(struct fg_dev *fg, u8 *val,
		u16 address, int offset, int len, bool access)
{
	int rc = 0;
	bool burst_mode = false;

	if (!is_mem_access_available(fg, access))
		return -EBUSY;

	/* configure for IMA access */
	rc = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg),
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT,
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT);
	if (rc < 0) {
		pr_err("failed to set ima_req_access bit rc=%d\n", rc);
		return rc;
	}

	/* configure for the read/write, single/burst mode */
	burst_mode = fg->use_ima_single_mode ? false :
			(offset + len) > fg->sram.num_bytes_per_word;
	rc = fg_config_access_mode(fg, access, burst_mode);
	if (rc < 0) {
		pr_err("failed to set memory access rc = %d\n", rc);
		return rc;
	}

	rc = fg_check_iacs_ready(fg);
	if (rc < 0) {
		pr_err_ratelimited("IACS_RDY failed rc=%d\n", rc);
		return rc;
	}

	rc = fg_set_address(fg, address);
	if (rc < 0) {
		pr_err("failed to set address rc = %d\n", rc);
		return rc;
	}

	if (access == FG_READ) {
		rc = fg_check_iacs_ready(fg);
		if (rc < 0) {
			pr_debug("IACS_RDY failed rc=%d\n", rc);
			return rc;
		}
	}

	return rc;
}

static int fg_get_beat_count(struct fg_dev *fg, u8 *count)
{
	int rc;

	rc = fg_read(fg, MEM_IF_FG_BEAT_COUNT(fg), count, 1);
	*count &= BEAT_COUNT_MASK;
	return rc;
}

int fg_interleaved_mem_read(struct fg_dev *fg, u16 address, u8 offset,
				u8 *val, int len)
{
	int rc = 0, ret;
	u8 start_beat_count, end_beat_count, count = 0;
	bool retry = false;

	if (fg->version == GEN4_FG) {
		if (offset > 1) {
			pr_err("offset too large %d\n", offset);
			return -EINVAL;
		}
	} else {
		if (offset > 3) {
			pr_err("offset too large %d\n", offset);
			return -EINVAL;
		}
	}
retry:
	if (count >= RETRY_COUNT) {
		pr_err("Tried %d times\n", RETRY_COUNT);
		retry = false;
		goto out;
	}

	rc = fg_interleaved_mem_config(fg, val, address, offset, len,
					FG_READ);
	if (rc < 0) {
		pr_err("failed to configure SRAM for IMA rc = %d\n", rc);
		count++;
		retry = true;
		goto out;
	}

	/* read the start beat count */
	rc = fg_get_beat_count(fg, &start_beat_count);
	if (rc < 0) {
		pr_err("failed to read beat count rc=%d\n", rc);
		count++;
		retry = true;
		goto out;
	}

	/* read data */
	rc = __fg_interleaved_mem_read(fg, address, offset, val, len);
	if (rc < 0) {
		count++;
		if (rc == -EAGAIN) {
			pr_err("IMA read failed retry_count = %d\n", count);
			goto retry;
		}
		pr_err("failed to read SRAM address rc = %d\n", rc);
		retry = true;
		goto out;
	}

	/* read the end beat count */
	rc = fg_get_beat_count(fg, &end_beat_count);
	if (rc < 0) {
		pr_err("failed to read beat count rc=%d\n", rc);
		count++;
		retry = true;
		goto out;
	}

	fg_dbg(fg, FG_SRAM_READ, "Start beat_count = %x End beat_count = %x\n",
		start_beat_count, end_beat_count);

	if (start_beat_count != end_beat_count) {
		fg_dbg(fg, FG_SRAM_READ, "Beat count(%d/%d) do not match - retry transaction\n",
			start_beat_count, end_beat_count);
		count++;
		retry = true;
	}
out:
	/* Release IMA access */
	ret = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg),
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT, 0);
	if (rc < 0 && ret < 0) {
		pr_err("failed to reset IMA access bit ret = %d\n", ret);
		return ret;
	}

	if (retry) {
		retry = false;
		goto retry;
	}

	return rc;
}

int fg_interleaved_mem_write(struct fg_dev *fg, u16 address, u8 offset,
				u8 *val, int len, bool atomic_access)
{
	int rc = 0, ret;
	u8 start_beat_count, end_beat_count, count = 0;
	bool retry = false;

	if (fg->version == GEN4_FG) {
		if (offset > 1) {
			pr_err("offset too large %d\n", offset);
			return -EINVAL;
		}
	} else {
		if (offset > 3) {
			pr_err("offset too large %d\n", offset);
			return -EINVAL;
		}
	}
retry:
	if (count >= RETRY_COUNT) {
		pr_err("Tried %d times\n", RETRY_COUNT);
		retry = false;
		goto out;
	}

	rc = fg_interleaved_mem_config(fg, val, address, offset, len,
					FG_WRITE);
	if (rc < 0) {
		pr_err("failed to configure SRAM for IMA rc = %d\n", rc);
		count++;
		retry = true;
		goto out;
	}

	/* read the start beat count */
	rc = fg_get_beat_count(fg, &start_beat_count);
	if (rc < 0) {
		pr_err("failed to read beat count rc=%d\n", rc);
		count++;
		retry = true;
		goto out;
	}

	/* write data */
	rc = __fg_interleaved_mem_write(fg, address, offset, val, len);
	if (rc < 0) {
		count++;
		if (rc == -EAGAIN) {
			pr_err("IMA write failed retry_count = %d\n", count);
			goto retry;
		}
		pr_err("failed to write SRAM address rc = %d\n", rc);
		retry = true;
		goto out;
	}

	/* read the end beat count */
	rc = fg_get_beat_count(fg, &end_beat_count);
	if (rc < 0) {
		pr_err("failed to read beat count rc=%d\n", rc);
		count++;
		retry = true;
		goto out;
	}

	if (atomic_access && start_beat_count != end_beat_count)
		pr_err("Start beat_count = %x End beat_count = %x\n",
			start_beat_count, end_beat_count);
out:
	/* Release IMA access */
	ret = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg),
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT, 0);
	if (rc < 0 && ret < 0) {
		pr_err("failed to reset IMA access bit ret = %d\n", ret);
		return ret;
	}

	if (retry) {
		retry = false;
		goto retry;
	}

	/* Return the error we got before releasing memory access */
	return rc;
}

static int fg_poll_alg_active(struct fg_dev *fg)
{
	u32 retries = 35, poll_time_us = 10000;
	int rc;
	u8 val;

	/*
	 * ALG active should be asserted low within ~164 ms mostly however
	 * during ESR pulsing, a worst case delay of ~320 ms is needed.
	 */
	while (retries--) {
		rc = fg_read(fg, BATT_INFO_PEEK_RD(fg), &val, 1);
		if (rc < 0) {
			pr_err("failed to read PEEK_MUX rc=%d\n", rc);
			return rc;
		}

		if (!(val & ALG_ACTIVE_BIT))
			break;

		usleep_range(poll_time_us, poll_time_us + 1);
	}

	if (val & ALG_ACTIVE_BIT)
		return -ETIMEDOUT;

	/* Wait for 1 ms after ALG active is asserted low */
	usleep_range(1000, 1001);
	return rc;
}

static int fg_direct_mem_release(struct fg_dev *fg)
{
	int rc;
	u8 val = 0, mask;

	mask = MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT;
	rc = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg), mask, val);
	if (rc < 0) {
		pr_err("failed to configure mem_if_mem_intf_cfg rc=%d\n", rc);
		return rc;
	}

	mask = MEM_ARB_REQ_BIT;
	rc = fg_masked_write(fg, MEM_IF_MEM_ARB_CFG(fg), mask, val);
	if (rc < 0) {
		pr_err("failed to configure mem_if_mem_arb_cfg rc:%d\n", rc);
		return rc;
	}

	pr_debug("released access\n");
	return rc;
}

#define MEM_GNT_WAIT_TIME_US	10000
#define MEM_GNT_RETRIES		50
static int fg_direct_mem_request(struct fg_dev *fg)
{
	int rc, ret, i = 0;
	u8 val, mask, poll_bit;

	if (fg->wa_flags & PM8150B_V1_DMA_WA) {
		rc = fg_poll_alg_active(fg);
		if (rc < 0) {
			pr_err("Failed to assert ALG active rc=%d\n", rc);
			return rc;
		}
	}

	val = mask = MEM_ARB_REQ_BIT;
	rc = fg_masked_write(fg, MEM_IF_MEM_ARB_CFG(fg), mask, val);
	if (rc < 0) {
		pr_err("failed to configure mem_if_mem_arb_cfg rc:%d\n", rc);
		return rc;
	}

	mask = MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT;
	val = MEM_ACCESS_REQ_BIT;
	rc = fg_masked_write(fg, MEM_IF_MEM_INTF_CFG(fg), mask, val);
	if (rc < 0) {
		pr_err("failed to configure mem_if_mem_intf_cfg rc=%d\n", rc);
		goto release;
	}

	pr_debug("requesting access\n");

	/*
	 * HW takes 5 cycles (200 KHz clock) to grant access after requesting
	 * for DMA. Wait for 40 us before polling for MEM_GNT first time.
	 */
	usleep_range(40, 41);

	poll_bit = MEM_GNT_BIT;
	if (fg->version == GEN4_FG)
		poll_bit = GEN4_MEM_GNT_BIT;

	while (i < MEM_GNT_RETRIES) {
		rc = fg_read(fg, MEM_IF_INT_RT_STS(fg), &val, 1);
		if (rc < 0) {
			pr_err("Error in reading MEM_IF_INT_RT_STS, rc=%d\n",
				rc);
			goto release;
		}

		if (val & poll_bit) {
			/* Delay needed for PM8150B V1 after DMA is granted */
			if (fg->wa_flags & PM8150B_V1_DMA_WA)
				usleep_range(1000, 1001);
			return 0;
		}

		usleep_range(MEM_GNT_WAIT_TIME_US, MEM_GNT_WAIT_TIME_US + 1);
		i++;
	}

	rc = -ETIMEDOUT;
	pr_err("wait for mem_grant timed out, val=0x%x\n", val);
	fg_dump_regs(fg);

release:
	ret = fg_direct_mem_release(fg);
	if (ret < 0)
		return ret;

	return rc;
}

static int fg_get_dma_address(struct fg_dev *fg, u16 sram_addr, u8 offset,
				u16 *addr)
{
	int i;
	u16 start_sram_addr, end_sram_addr;

	for (i = 0; i < fg->sram.num_partitions; i++) {
		start_sram_addr = fg->sram.addr_map[i].partition_start;
		end_sram_addr = fg->sram.addr_map[i].partition_end;
		if (sram_addr >= start_sram_addr &&
			sram_addr <= end_sram_addr) {
			*addr = fg->sram.addr_map[i].spmi_addr_base + offset +
					(sram_addr - start_sram_addr) *
						fg->sram.num_bytes_per_word;
			return 0;
		}
	}

	pr_err("Couldn't find address for %d from address map\n", sram_addr);
	return -ENXIO;
}

static int fg_get_partition_count(struct fg_dev *fg, u16 sram_addr, int len,
				int *count)
{
	int i, start_partn = 0, end_partn = 0;
	u16 end_addr = 0;

	end_addr = sram_addr + len / fg->sram.num_bytes_per_word;
	if (!(len % fg->sram.num_bytes_per_word))
		end_addr -= 1;

	if (sram_addr == end_addr) {
		*count = 1;
		return 0;
	}

	for (i = 0; i < fg->sram.num_partitions; i++) {
		if (sram_addr >= fg->sram.addr_map[i].partition_start
			&& sram_addr <= fg->sram.addr_map[i].partition_end)
			start_partn = i + 1;

		if (end_addr >= fg->sram.addr_map[i].partition_start
			&& end_addr <= fg->sram.addr_map[i].partition_end)
			end_partn = i + 1;
	}

	if (!start_partn || !end_partn) {
		pr_err("Couldn't find number of partitions for address %d\n",
			sram_addr);
		return -ENXIO;
	}

	*count = (end_partn - start_partn) + 1;

	return 0;
}

static int fg_get_partition_avail_bytes(struct fg_dev *fg, u16 sram_addr,
					int len, int *rem_len)
{
	int i, part_len = 0, temp;
	u16 end_addr;

	for (i = 0; i < fg->sram.num_partitions; i++) {
		if (sram_addr >= fg->sram.addr_map[i].partition_start
			&& sram_addr <= fg->sram.addr_map[i].partition_end) {
			part_len = (fg->sram.addr_map[i].partition_end -
					fg->sram.addr_map[i].partition_start +
					1);
			part_len *= fg->sram.num_bytes_per_word;
			end_addr = fg->sram.addr_map[i].partition_end;
			break;
		}
	}

	if (part_len <= 0) {
		pr_err("Bad address? total_len=%d\n", part_len);
		return -ENXIO;
	}

	temp = (end_addr - sram_addr + 1) * fg->sram.num_bytes_per_word;
	if (temp > part_len || !temp) {
		pr_err("Bad length=%d\n", temp);
		return -ENXIO;
	}

	*rem_len = temp;
	pr_debug("address %d len %d rem_len %d\n", sram_addr, len, *rem_len);
	return 0;
}

static int __fg_direct_mem_rw(struct fg_dev *fg, u16 sram_addr, u8 offset,
				u8 *val, int len, bool access)
{
	int rc, ret, num_partitions, num_bytes = 0;
	u16 addr;
	u8 *ptr = val;
	char *temp_str;

	if (offset > 3) {
		pr_err("offset too large %d\n", offset);
		return -EINVAL;
	}

	rc = fg_get_partition_count(fg, sram_addr, len, &num_partitions);
	if (rc < 0)
		return rc;

	pr_debug("number of partitions: %d\n", num_partitions);

	rc = fg_direct_mem_request(fg);
	if (rc < 0) {
		pr_err("Error in requesting direct_mem access rc=%d\n", rc);
		return rc;
	}

	while (num_partitions-- && len) {
		rc = fg_get_dma_address(fg, sram_addr, offset, &addr);
		if (rc < 0) {
			pr_err("Incorrect address %d/offset %d\n", sram_addr,
				offset);
			break;
		}

		rc = fg_get_partition_avail_bytes(fg, sram_addr + offset, len,
						&num_bytes);
		if (rc < 0)
			break;

		if (num_bytes > len)
			num_bytes = len;

		pr_debug("reading from address: [%d %d] dma_address = %x\n",
			sram_addr, offset, addr);

		if (access == FG_READ) {
			rc = fg_read(fg, addr, ptr, num_bytes);
			temp_str = "read";
		} else {
			rc = fg_write(fg, addr, ptr, num_bytes);
			temp_str = "write";
		}

		if (rc < 0) {
			pr_err("Error in %sing address %d rc=%d\n", temp_str,
				sram_addr, rc);
			break;
		}

		ptr += num_bytes;
		len -= num_bytes;
		sram_addr += (num_bytes / fg->sram.num_bytes_per_word);
		offset = 0;
	}

	ret = fg_direct_mem_release(fg);
	if (ret < 0) {
		pr_err("Error in releasing direct_mem access rc=%d\n", rc);
		return ret;
	}

	return rc;
}

int fg_dma_mem_req(struct fg_dev *chip, bool request)
{
	int ret, rc = 0, retry_count  = RETRY_COUNT;
	u8 val;

	if (request) {
		/* configure for DMA access */
		rc = fg_masked_write(chip, MEM_IF_MEM_INTF_CFG(chip),
				MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT,
				MEM_ACCESS_REQ_BIT);
		if (rc < 0) {
			pr_err("failed to set mem_access bit rc=%d\n", rc);
			return rc;
		}

		rc = fg_masked_write(chip, MEM_IF_MEM_ARB_CFG(chip),
				MEM_IF_ARB_REQ_BIT, MEM_IF_ARB_REQ_BIT);
		if (rc < 0) {
			pr_err("failed to set mem_arb bit rc=%d\n", rc);
			goto release_mem;
		}

		while (retry_count--) {
			rc = fg_read(chip, MEM_IF_INT_RT_STS(chip), &val, 1);
			if (rc < 0) {
				pr_err("failed to set ima_rt_sts rc=%d\n", rc);
				goto release_mem;
			}
			if (val & MEM_GNT_BIT)
				break;
			msleep(20);
		}
		if ((retry_count < 0) && !(val & MEM_GNT_BIT)) {
			pr_err("failed to get memory access\n");
			rc = -ETIMEDOUT;
			goto release_mem;
		}

		return 0;
	}

release_mem:
	/* Release access */
	rc = fg_masked_write(chip, MEM_IF_MEM_INTF_CFG(chip),
			MEM_ACCESS_REQ_BIT | IACS_SLCT_BIT, 0);
	if (rc < 0)
		pr_err("failed to reset mem_access bit rc = %d\n", rc);

	ret = fg_masked_write(chip, MEM_IF_MEM_ARB_CFG(chip),
			MEM_IF_ARB_REQ_BIT, 0);
	if (ret < 0) {
		pr_err("failed to release mem_arb bit rc=%d\n", ret);
		return ret;
	}

	return rc;
}

int fg_direct_mem_read(struct fg_dev *fg, u16 sram_addr, u8 offset,
				u8 *val, int len)
{
	return __fg_direct_mem_rw(fg, sram_addr, offset, val, len, FG_READ);
}

int fg_direct_mem_write(struct fg_dev *fg, u16 sram_addr, u8 offset,
				u8 *val, int len, bool atomic_access)
{
	return __fg_direct_mem_rw(fg, sram_addr, offset, val, len, FG_WRITE);
}

static int fg_ima_init(struct fg_dev *fg)
{
	int rc;

	if (fg->version == GEN3_FG) {
		fg->sram.num_bytes_per_word = 4;
		fg->sram.address_max = 255;
	} else if (fg->version == GEN4_FG) {
		fg->sram.num_bytes_per_word = 2;
		fg->sram.address_max = 480;
	} else {
		pr_err("Unknown FG version %d\n", fg->version);
		return -ENXIO;
	}

	/*
	 * Change the FG_MEM_INT interrupt to track IACS_READY
	 * condition instead of end-of-transaction. This makes sure
	 * that the next transaction starts only after the hw is ready.
	 */
	rc = fg_masked_write(fg, MEM_IF_IMA_CFG(fg), IACS_INTR_SRC_SLCT_BIT,
				IACS_INTR_SRC_SLCT_BIT);
	if (rc < 0) {
		pr_err("failed to configure interrupt source %d\n", rc);
		return rc;
	}

	/* Clear DMA errors if any before clearing IMA errors */
	rc = fg_clear_dma_errors_if_any(fg);
	if (rc < 0) {
		pr_err("Error in checking DMA errors rc:%d\n", rc);
		return rc;
	}

	/* Clear IMA errors if any before SRAM transactions can begin */
	rc = fg_clear_ima_errors_if_any(fg, true);
	if (rc < 0 && rc != -EAGAIN) {
		pr_err("Error in checking IMA errors rc:%d\n", rc);
		return rc;
	}

	return 0;
}

/*
 * This SRAM partition to DMA address partition mapping remains identical for
 * PMICs that use GEN3 FG.
 */
static struct fg_dma_address fg_gen3_addr_map[3] = {
	/* system partition */
	{
		.partition_start = 0,
		.partition_end = 23,
		.spmi_addr_base = FG_DMA0_BASE + SRAM_ADDR_OFFSET,
	},
	/* battery profile partition */
	{
		.partition_start = 24,
		.partition_end = 79,
		.spmi_addr_base = FG_DMA1_BASE + SRAM_ADDR_OFFSET,
	},
	/* scratch pad partition */
	{
		.partition_start = 80,
		.partition_end =  125,
		.spmi_addr_base = FG_DMA2_BASE + SRAM_ADDR_OFFSET,
	},
};

static struct fg_dma_address fg_gen4_addr_map[6] = {
	/* system partition */
	{
		.partition_start = 0,
		.partition_end = 63,
		.spmi_addr_base = GEN4_FG_DMA0_BASE + SRAM_ADDR_OFFSET,
	},
	/* battery profile partition */
	{
		.partition_start = 64,
		.partition_end = 169,
		.spmi_addr_base = GEN4_FG_DMA1_BASE + SRAM_ADDR_OFFSET,
	},
	/* battery profile partition continued */
	{
		.partition_start = 170,
		.partition_end = 274,
		.spmi_addr_base = GEN4_FG_DMA2_BASE + SRAM_ADDR_OFFSET,
	},
	/* dp/SW partition */
	{
		.partition_start = 275,
		.partition_end = 299,
		.spmi_addr_base = GEN4_FG_DMA3_BASE + SRAM_ADDR_OFFSET,
	},
	/* wk/scratch pad partition */
	{
		.partition_start = 300,
		.partition_end =  405,
		.spmi_addr_base = GEN4_FG_DMA4_BASE + SRAM_ADDR_OFFSET,
	},
	/* wk/scratch pad partition continued */
	{
		.partition_start = 406,
		.partition_end =  486,
		.spmi_addr_base = GEN4_FG_DMA5_BASE + SRAM_ADDR_OFFSET,
	},
};

static int fg_dma_init(struct fg_dev *fg)
{
	int rc;
	u8 val;

	if (fg->version == GEN3_FG) {
		fg->sram.addr_map = fg_gen3_addr_map;
		fg->sram.num_partitions = 3;
		fg->sram.num_bytes_per_word = 4;
		fg->sram.address_max = 255;
	} else if (fg->version == GEN4_FG) {
		fg->sram.addr_map = fg_gen4_addr_map;
		fg->sram.num_partitions = 6;
		fg->sram.num_bytes_per_word = 2;
		fg->sram.address_max = 485;
	} else {
		pr_err("Unknown FG version %d\n", fg->version);
		return -ENXIO;
	}

	/* Clear DMA errors if any before clearing IMA errors */
	rc = fg_clear_dma_errors_if_any(fg);
	if (rc < 0) {
		pr_err("Error in checking DMA errors rc:%d\n", rc);
		return rc;
	}

	/* Configure the DMA peripheral addressing to partition */
	rc = fg_masked_write(fg, MEM_IF_DMA_CTL(fg), ADDR_KIND_BIT,
				ADDR_KIND_BIT);
	if (rc < 0) {
		pr_err("failed to configure DMA_CTL rc:%d\n", rc);
		return rc;
	}

	/* Release the DMA initially so that request can happen */
	rc = fg_direct_mem_release(fg);
	if (rc < 0) {
		pr_err("Error in releasing direct_mem access rc=%d\n",
			rc);
		return rc;
	}

	/* Set low latency always and clear log bit */
	rc = fg_masked_write(fg, MEM_IF_MEM_ARB_CFG(fg),
		MEM_ARB_LO_LATENCY_EN_BIT | MEM_CLR_LOG_BIT,
		MEM_ARB_LO_LATENCY_EN_BIT);
	if (rc < 0) {
		pr_err("failed to configure mem_if_mem_arb_cfg rc:%d\n", rc);
		return rc;
	}

	/*
	 * Configure PEEK_MUX for ALG active signal always for PM8150B.
	 * For v1.0, it is used for DMA workaround. For v2.0 onwards, it is
	 * used for ADC lockup workaround.
	 */
	val = ALG_ACTIVE_PEEK_CFG;
	rc = fg_write(fg, BATT_INFO_PEEK_MUX4(fg), &val, 1);
	if (rc < 0) {
		pr_err("failed to configure batt_info_peek_mux4 rc:%d\n", rc);
		return rc;
	}

	return 0;
}

int fg_memif_init(struct fg_dev *fg)
{
	if (fg->battery_missing)
		return 0;

	if (fg->use_dma)
		return fg_dma_init(fg);

	return fg_ima_init(fg);
}
