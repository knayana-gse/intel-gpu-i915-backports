/* i915_irq.c -- IRQ support for the I915 -*- linux-c -*-
 */
/*
 * Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/circ_buf.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/sysrq.h>

#include <drm/drm_drv.h>

#include "display/icl_dsi_regs.h"
#include "display/intel_de.h"
#include "display/intel_display_trace.h"
#include "display/intel_display_types.h"
#include "display/intel_hotplug.h"
#include "display/intel_psr.h"

#include "gt/intel_breadcrumbs.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_irq.h"
#include "gt/intel_gt_pm_irq.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_rps.h"
#include "gt/iov/intel_iov_memirq.h"

#include "i915_driver.h"
#include "i915_drv.h"
#include "i915_irq.h"
#include "intel_pm.h"

/**
 * DOC: interrupt handling
 *
 * These functions provide the basic support for enabling and disabling the
 * interrupt handling support. There's a lot more functionality in i915_irq.c
 * and related files, but that will be described in separate chapters.
 */

/*
 * Interrupt statistic for PMU. Increments the counter only if the
 * interrupt originated from the GPU so interrupts from a device which
 * shares the interrupt line are not accounted.
 */
static inline void pmu_irq_stats(struct drm_i915_private *i915,
				 irqreturn_t res)
{
	if (unlikely(res != IRQ_HANDLED))
		return;

	/*
	 * A clever compiler translates that into INC. A not so clever one
	 * should at least prevent store tearing.
	 */
	WRITE_ONCE(i915->pmu.irq_count, i915->pmu.irq_count + 1);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
typedef bool (*long_pulse_detect_func)(enum hpd_pin pin, u32 val);
typedef u32 (*hotplug_enables_func)(struct intel_encoder *encoder);

static const u32 hpd_gen11[HPD_NUM_PINS] = {
	[HPD_PORT_TC1] = GEN11_TC_HOTPLUG(HPD_PORT_TC1) | GEN11_TBT_HOTPLUG(HPD_PORT_TC1),
	[HPD_PORT_TC2] = GEN11_TC_HOTPLUG(HPD_PORT_TC2) | GEN11_TBT_HOTPLUG(HPD_PORT_TC2),
	[HPD_PORT_TC3] = GEN11_TC_HOTPLUG(HPD_PORT_TC3) | GEN11_TBT_HOTPLUG(HPD_PORT_TC3),
	[HPD_PORT_TC4] = GEN11_TC_HOTPLUG(HPD_PORT_TC4) | GEN11_TBT_HOTPLUG(HPD_PORT_TC4),
	[HPD_PORT_TC5] = GEN11_TC_HOTPLUG(HPD_PORT_TC5) | GEN11_TBT_HOTPLUG(HPD_PORT_TC5),
	[HPD_PORT_TC6] = GEN11_TC_HOTPLUG(HPD_PORT_TC6) | GEN11_TBT_HOTPLUG(HPD_PORT_TC6),
};

static const u32 hpd_xelpdp[HPD_NUM_PINS] = {
	[HPD_PORT_TC1] = XELPDP_TBT_HOTPLUG(HPD_PORT_TC1) | XELPDP_DP_ALT_HOTPLUG(HPD_PORT_TC1),
	[HPD_PORT_TC2] = XELPDP_TBT_HOTPLUG(HPD_PORT_TC2) | XELPDP_DP_ALT_HOTPLUG(HPD_PORT_TC2),
	[HPD_PORT_TC3] = XELPDP_TBT_HOTPLUG(HPD_PORT_TC3) | XELPDP_DP_ALT_HOTPLUG(HPD_PORT_TC3),
	[HPD_PORT_TC4] = XELPDP_TBT_HOTPLUG(HPD_PORT_TC4) | XELPDP_DP_ALT_HOTPLUG(HPD_PORT_TC4),
};

static const u32 hpd_icp[HPD_NUM_PINS] = {
	[HPD_PORT_A] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_A),
	[HPD_PORT_B] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_B),
	[HPD_PORT_C] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_C),
	[HPD_PORT_TC1] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC1),
	[HPD_PORT_TC2] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC2),
	[HPD_PORT_TC3] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC3),
	[HPD_PORT_TC4] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC4),
	[HPD_PORT_TC5] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC5),
	[HPD_PORT_TC6] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC6),
};

static const u32 hpd_sde_dg1[HPD_NUM_PINS] = {
	[HPD_PORT_A] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_A),
	[HPD_PORT_B] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_B),
	[HPD_PORT_C] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_C),
	[HPD_PORT_D] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_D),
	[HPD_PORT_TC1] = SDE_TC_HOTPLUG_DG2(HPD_PORT_TC1),
};

static const u32 hpd_mtp[HPD_NUM_PINS] = {
	[HPD_PORT_A] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_A),
	[HPD_PORT_B] = SDE_DDI_HOTPLUG_ICP(HPD_PORT_B),
	[HPD_PORT_TC1] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC1),
	[HPD_PORT_TC2] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC2),
	[HPD_PORT_TC3] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC3),
	[HPD_PORT_TC4] = SDE_TC_HOTPLUG_ICP(HPD_PORT_TC4),
};

static void intel_hpd_init_pins(struct drm_i915_private *dev_priv)
{
	struct i915_hotplug *hpd = &dev_priv->hotplug;

	if (DISPLAY_VER(dev_priv) >= 14)
		hpd->hpd = hpd_xelpdp;
	else
		hpd->hpd = hpd_gen11;

	if ((INTEL_PCH_TYPE(dev_priv) < PCH_DG1) &&
	    (!HAS_PCH_SPLIT(dev_priv) || HAS_PCH_NOP(dev_priv)))
		return;

	if (INTEL_PCH_TYPE(dev_priv) >= PCH_DG1)
		hpd->pch_hpd = hpd_sde_dg1;
	else if (INTEL_PCH_TYPE(dev_priv) >= PCH_MTP)
		hpd->pch_hpd = hpd_mtp;
	else
		hpd->pch_hpd = hpd_icp;
}

static void
intel_handle_vblank(struct drm_i915_private *dev_priv, enum pipe pipe)
{
	struct intel_crtc *crtc = intel_crtc_for_pipe(dev_priv, pipe);

	drm_crtc_handle_vblank(&crtc->base);
}
#else
static void intel_hpd_init_pins(struct drm_i915_private *dev_priv) {}
#endif

void gen3_irq_reset(struct intel_uncore *uncore, i915_reg_t imr,
		    i915_reg_t iir, i915_reg_t ier)
{
	intel_uncore_write(uncore, imr, 0xffffffff);
	intel_uncore_posting_read(uncore, imr);

	intel_uncore_write(uncore, ier, 0);

	/* IIR can theoretically queue up two events. Be paranoid. */
	intel_uncore_write(uncore, iir, 0xffffffff);
	intel_uncore_posting_read(uncore, iir);
	intel_uncore_write(uncore, iir, 0xffffffff);
	intel_uncore_posting_read(uncore, iir);
}

/*
 * We should clear IMR at preinstall/uninstall, and just check at postinstall.
 */
static void gen3_assert_iir_is_zero(struct intel_uncore *uncore, i915_reg_t reg)
{
	u32 val = intel_uncore_read(uncore, reg);

	if (val == 0)
		return;

	drm_WARN(&uncore->i915->drm, 1,
		 "Interrupt register 0x%x is not zero: 0x%08x\n",
		 i915_mmio_reg_offset(reg), val);
	intel_uncore_write(uncore, reg, 0xffffffff);
	intel_uncore_posting_read(uncore, reg);
	intel_uncore_write(uncore, reg, 0xffffffff);
	intel_uncore_posting_read(uncore, reg);
}

void gen3_irq_init(struct intel_uncore *uncore,
		   i915_reg_t imr, u32 imr_val,
		   i915_reg_t ier, u32 ier_val,
		   i915_reg_t iir)
{
	gen3_assert_iir_is_zero(uncore, iir);

	intel_uncore_write(uncore, ier, ier_val);
	intel_uncore_write(uncore, imr, imr_val);
	intel_uncore_posting_read(uncore, imr);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
/* For display hotplug interrupt */
static inline void
i915_hotplug_interrupt_update_locked(struct drm_i915_private *dev_priv,
				     u32 mask,
				     u32 bits)
{
	u32 val;

	lockdep_assert_held(&dev_priv->irq_lock);
	drm_WARN_ON(&dev_priv->drm, bits & ~mask);

	val = intel_uncore_read(&dev_priv->uncore, PORT_HOTPLUG_EN);
	val &= ~mask;
	val |= bits;
	intel_uncore_write(&dev_priv->uncore, PORT_HOTPLUG_EN, val);
}

/**
 * i915_hotplug_interrupt_update - update hotplug interrupt enable
 * @dev_priv: driver private
 * @mask: bits to update
 * @bits: bits to enable
 * NOTE: the HPD enable bits are modified both inside and outside
 * of an interrupt context. To avoid that read-modify-write cycles
 * interfer, these bits are protected by a spinlock. Since this
 * function is usually not called from a context where the lock is
 * held already, this function acquires the lock itself. A non-locking
 * version is also available.
 */
void i915_hotplug_interrupt_update(struct drm_i915_private *dev_priv,
				   u32 mask,
				   u32 bits)
{
	spin_lock_irq(&dev_priv->irq_lock);
	i915_hotplug_interrupt_update_locked(dev_priv, mask, bits);
	spin_unlock_irq(&dev_priv->irq_lock);
}

/**
 * bdw_update_pipe_irq - update DE pipe interrupt
 * @dev_priv: driver private
 * @pipe: pipe whose interrupt to update
 * @interrupt_mask: mask of interrupt bits to update
 * @enabled_irq_mask: mask of interrupt bits to enable
 */
static void bdw_update_pipe_irq(struct drm_i915_private *dev_priv,
				enum pipe pipe, u32 interrupt_mask,
				u32 enabled_irq_mask)
{
	u32 new_val;

	lockdep_assert_held(&dev_priv->irq_lock);

	drm_WARN_ON(&dev_priv->drm, enabled_irq_mask & ~interrupt_mask);

	if (drm_WARN_ON(&dev_priv->drm, !intel_irqs_enabled(dev_priv)))
		return;

	new_val = dev_priv->de_irq_mask[pipe];
	new_val &= ~interrupt_mask;
	new_val |= (~enabled_irq_mask & interrupt_mask);

	if (new_val != dev_priv->de_irq_mask[pipe]) {
		dev_priv->de_irq_mask[pipe] = new_val;
		intel_uncore_write(&dev_priv->uncore, GEN8_DE_PIPE_IMR(pipe), dev_priv->de_irq_mask[pipe]);
		intel_uncore_posting_read(&dev_priv->uncore, GEN8_DE_PIPE_IMR(pipe));
	}
}

void bdw_enable_pipe_irq(struct drm_i915_private *i915,
			 enum pipe pipe, u32 bits)
{
	bdw_update_pipe_irq(i915, pipe, bits, bits);
}

void bdw_disable_pipe_irq(struct drm_i915_private *i915,
			  enum pipe pipe, u32 bits)
{
	bdw_update_pipe_irq(i915, pipe, bits, 0);
}

/**
 * ibx_display_interrupt_update - update SDEIMR
 * @dev_priv: driver private
 * @interrupt_mask: mask of interrupt bits to update
 * @enabled_irq_mask: mask of interrupt bits to enable
 */
static void ibx_display_interrupt_update(struct drm_i915_private *dev_priv,
					 u32 interrupt_mask,
					 u32 enabled_irq_mask)
{
	u32 sdeimr = intel_uncore_read(&dev_priv->uncore, SDEIMR);
	sdeimr &= ~interrupt_mask;
	sdeimr |= (~enabled_irq_mask & interrupt_mask);

	drm_WARN_ON(&dev_priv->drm, enabled_irq_mask & ~interrupt_mask);

	lockdep_assert_held(&dev_priv->irq_lock);

	if (drm_WARN_ON(&dev_priv->drm, !intel_irqs_enabled(dev_priv)))
		return;

	intel_uncore_write(&dev_priv->uncore, SDEIMR, sdeimr);
	intel_uncore_posting_read(&dev_priv->uncore, SDEIMR);
}

void ibx_enable_display_interrupt(struct drm_i915_private *i915, u32 bits)
{
	ibx_display_interrupt_update(i915, bits, bits);
}

void ibx_disable_display_interrupt(struct drm_i915_private *i915, u32 bits)
{
	ibx_display_interrupt_update(i915, bits, 0);
}

/*
 * This timing diagram depicts the video signal in and
 * around the vertical blanking period.
 *
 * Assumptions about the fictitious mode used in this example:
 *  vblank_start >= 3
 *  vsync_start = vblank_start + 1
 *  vsync_end = vblank_start + 2
 *  vtotal = vblank_start + 3
 *
 *           start of vblank:
 *           latch double buffered registers
 *           increment frame counter (ctg+)
 *           generate start of vblank interrupt (gen4+)
 *           |
 *           |          frame start:
 *           |          generate frame start interrupt (aka. vblank interrupt) (gmch)
 *           |          may be shifted forward 1-3 extra lines via PIPECONF
 *           |          |
 *           |          |  start of vsync:
 *           |          |  generate vsync interrupt
 *           |          |  |
 * ___xxxx___    ___xxxx___    ___xxxx___    ___xxxx___    ___xxxx___    ___xxxx
 *       .   \hs/   .      \hs/          \hs/          \hs/   .      \hs/
 * ----va---> <-----------------vb--------------------> <--------va-------------
 *       |          |       <----vs----->                     |
 * -vbs-----> <---vbs+1---> <---vbs+2---> <-----0-----> <-----1-----> <-----2--- (scanline counter gen2)
 * -vbs-2---> <---vbs-1---> <---vbs-----> <---vbs+1---> <---vbs+2---> <-----0--- (scanline counter gen3+)
 * -vbs-2---> <---vbs-2---> <---vbs-1---> <---vbs-----> <---vbs+1---> <---vbs+2- (scanline counter hsw+ hdmi)
 *       |          |                                         |
 *       last visible pixel                                   first visible pixel
 *                  |                                         increment frame counter (gen3/4)
 *                  pixel counter = vblank_start * htotal     pixel counter = 0 (gen3/4)
 *
 * x  = horizontal active
 * _  = horizontal blanking
 * hs = horizontal sync
 * va = vertical active
 * vb = vertical blanking
 * vs = vertical sync
 * vbs = vblank_start (number)
 *
 * Summary:
 * - most events happen at the start of horizontal sync
 * - frame start happens at the start of horizontal blank, 1-4 lines
 *   (depending on PIPECONF settings) after the start of vblank
 * - gen3/4 pixel and frame counter are synchronized with the start
 *   of horizontal active on the first line of vertical active
 */

u32 g4x_get_vblank_counter(struct drm_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->dev);
	struct drm_vblank_crtc *vblank = &dev_priv->drm.vblank[drm_crtc_index(crtc)];
	enum pipe pipe = to_intel_crtc(crtc)->pipe;

	if (!vblank->max_vblank_count)
		return 0;

	return intel_uncore_read(&dev_priv->uncore, PIPE_FRMCOUNT_G4X(pipe));
}

static u32 intel_crtc_scanlines_since_frame_timestamp(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct drm_vblank_crtc *vblank =
		&crtc->base.dev->vblank[drm_crtc_index(&crtc->base)];
	const struct drm_display_mode *mode = &vblank->hwmode;
	u32 htotal = mode->crtc_htotal;
	u32 clock = mode->crtc_clock;
	u32 scan_prev_time, scan_curr_time, scan_post_time;

	/*
	 * To avoid the race condition where we might cross into the
	 * next vblank just between the PIPE_FRMTMSTMP and TIMESTAMP_CTR
	 * reads. We make sure we read PIPE_FRMTMSTMP and TIMESTAMP_CTR
	 * during the same frame.
	 */
	do {
		/*
		 * This field provides read back of the display
		 * pipe frame time stamp. The time stamp value
		 * is sampled at every start of vertical blank.
		 */
		scan_prev_time = intel_de_read_fw(dev_priv,
						  PIPE_FRMTMSTMP(crtc->pipe));

		/*
		 * The TIMESTAMP_CTR register has the current
		 * time stamp value.
		 */
		scan_curr_time = intel_de_read_fw(dev_priv, IVB_TIMESTAMP_CTR);

		scan_post_time = intel_de_read_fw(dev_priv,
						  PIPE_FRMTMSTMP(crtc->pipe));
	} while (scan_post_time != scan_prev_time);

	return div_u64(mul_u32_u32(scan_curr_time - scan_prev_time,
				   clock), 1000 * htotal);
}

/*
 * On certain encoders on certain platforms, pipe
 * scanline register will not work to get the scanline,
 * since the timings are driven from the PORT or issues
 * with scanline register updates.
 * This function will use Framestamp and current
 * timestamp registers to calculate the scanline.
 */
static u32 __intel_get_crtc_scanline_from_timestamp(struct intel_crtc *crtc)
{
	struct drm_vblank_crtc *vblank =
		&crtc->base.dev->vblank[drm_crtc_index(&crtc->base)];
	const struct drm_display_mode *mode = &vblank->hwmode;
	u32 vblank_start = mode->crtc_vblank_start;
	u32 vtotal = mode->crtc_vtotal;
	u32 scanline;

	scanline = intel_crtc_scanlines_since_frame_timestamp(crtc);
	scanline = min(scanline, vtotal - 1);
	scanline = (scanline + vblank_start) % vtotal;

	return scanline;
}

/*
 * intel_de_read_fw(), only for fast reads of display block, no need for
 * forcewake etc.
 */
static int __intel_get_crtc_scanline(struct intel_crtc *crtc)
{
	struct drm_device *dev = crtc->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	const struct drm_display_mode *mode;
	struct drm_vblank_crtc *vblank;
	enum pipe pipe = crtc->pipe;
	int position, vtotal;

	if (!crtc->active)
		return 0;

	vblank = &crtc->base.dev->vblank[drm_crtc_index(&crtc->base)];
	mode = &vblank->hwmode;

	if (crtc->mode_flags & I915_MODE_FLAG_GET_SCANLINE_FROM_TIMESTAMP)
		return __intel_get_crtc_scanline_from_timestamp(crtc);

	vtotal = mode->crtc_vtotal;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		vtotal /= 2;

	position = intel_de_read_fw(dev_priv, PIPEDSL(pipe)) & PIPEDSL_LINE_MASK;

	/*
	 * On HSW, the DSL reg (0x70000) appears to return 0 if we
	 * read it just before the start of vblank.  So try it again
	 * so we don't accidentally end up spanning a vblank frame
	 * increment, causing the pipe_update_end() code to squak at us.
	 *
	 * The nature of this problem means we can't simply check the ISR
	 * bit and return the vblank start value; nor can we use the scanline
	 * debug register in the transcoder as it appears to have the same
	 * problem.  We may need to extend this to include other platforms,
	 * but so far testing only shows the problem on HSW.
	 */
	if (!position) {
		int i, temp;

		for (i = 0; i < 100; i++) {
			udelay(1);
			temp = intel_de_read_fw(dev_priv, PIPEDSL(pipe)) & PIPEDSL_LINE_MASK;
			if (temp != position) {
				position = temp;
				break;
			}
		}
	}

	/*
	 * See update_scanline_offset() for the details on the
	 * scanline_offset adjustment.
	 */
	return (position + crtc->scanline_offset) % vtotal;
}

static bool i915_get_crtc_scanoutpos(struct drm_crtc *_crtc,
				     bool in_vblank_irq,
				     int *vpos, int *hpos,
				     ktime_t *stime, ktime_t *etime,
				     const struct drm_display_mode *mode)
{
	struct drm_device *dev = _crtc->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_crtc *crtc = to_intel_crtc(_crtc);
	enum pipe pipe = crtc->pipe;
	int position;
	int vbl_start, vbl_end, hsync_start, htotal, vtotal;
	unsigned long irqflags;

	if (drm_WARN_ON(&dev_priv->drm, !mode->crtc_clock)) {
		drm_dbg(&dev_priv->drm,
			"trying to get scanoutpos for disabled "
			"pipe %c\n", pipe_name(pipe));
		return false;
	}

	htotal = mode->crtc_htotal;
	hsync_start = mode->crtc_hsync_start;
	vtotal = mode->crtc_vtotal;
	vbl_start = mode->crtc_vblank_start;
	vbl_end = mode->crtc_vblank_end;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
		vbl_start = DIV_ROUND_UP(vbl_start, 2);
		vbl_end /= 2;
		vtotal /= 2;
	}

	/*
	 * Lock uncore.lock, as we will do multiple timing critical raw
	 * register reads, potentially with preemption disabled, so the
	 * following code must not block on uncore.lock.
	 */
	spin_lock_irqsave(&dev_priv->uncore.lock, irqflags);

	/* preempt_disable_rt() should go right here in PREEMPT_RT patchset. */

	/* Get optional system timestamp before query. */
	if (stime)
		*stime = ktime_get();

	position = __intel_get_crtc_scanline(crtc);
	if (crtc->mode_flags & I915_MODE_FLAG_VRR) {
		int scanlines = intel_crtc_scanlines_since_frame_timestamp(crtc);

		/*
		 * Already exiting vblank? If so, shift our position
		 * so it looks like we're already apporaching the full
		 * vblank end. This should make the generated timestamp
		 * more or less match when the active portion will start.
		 */
		if (position >= vbl_start && scanlines < position)
			position = min(crtc->vmax_vblank_start + scanlines, vtotal - 1);
	}

	/* Get optional system timestamp after query. */
	if (etime)
		*etime = ktime_get();

	/* preempt_enable_rt() should go right here in PREEMPT_RT patchset. */

	spin_unlock_irqrestore(&dev_priv->uncore.lock, irqflags);

	/*
	 * While in vblank, position will be negative
	 * counting up towards 0 at vbl_end. And outside
	 * vblank, position will be positive counting
	 * up since vbl_end.
	 */
	if (position >= vbl_start)
		position -= vbl_end;
	else
		position += vtotal - vbl_end;

	*vpos = position;
	*hpos = 0;
	return true;
}

bool intel_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
				     ktime_t *vblank_time, bool in_vblank_irq)
{
	return drm_crtc_vblank_helper_get_vblank_timestamp_internal(
		crtc, max_error, vblank_time, in_vblank_irq,
		i915_get_crtc_scanoutpos);
}

int intel_get_crtc_scanline(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	unsigned long irqflags;
	int position;

	spin_lock_irqsave(&dev_priv->uncore.lock, irqflags);
	position = __intel_get_crtc_scanline(crtc);
	spin_unlock_irqrestore(&dev_priv->uncore.lock, irqflags);

	return position;
}
#endif

/**
 * ivb_parity_work - Workqueue called when a parity error interrupt
 * occurred.
 * @work: workqueue struct
 *
 * Doesn't actually do anything except notify userspace. As a consequence of
 * this event, userspace should try to remap the bad rows since statistically
 * it is likely the same row is more likely to go bad again.
 */
static void ivb_parity_work(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv), l3_parity.error_work);
	struct intel_gt *gt = to_gt(dev_priv);
	u32 error_status, row, bank, subbank;
	char *parity_event[6];
	u32 misccpctl;
	u8 slice = 0;

	/* We must turn off DOP level clock gating to access the L3 registers.
	 * In order to prevent a get/put style interface, acquire struct mutex
	 * any time we access those registers.
	 */
	mutex_lock(&dev_priv->drm.struct_mutex);

	/* If we've screwed up tracking, just let the interrupt fire again */
	if (drm_WARN_ON(&dev_priv->drm, !dev_priv->l3_parity.which_slice))
		goto out;

	misccpctl = intel_uncore_read(&dev_priv->uncore, GEN7_MISCCPCTL);
	intel_uncore_write(&dev_priv->uncore, GEN7_MISCCPCTL, misccpctl & ~GEN7_DOP_CLOCK_GATE_ENABLE);
	intel_uncore_posting_read(&dev_priv->uncore, GEN7_MISCCPCTL);

	while ((slice = ffs(dev_priv->l3_parity.which_slice)) != 0) {
		i915_reg_t reg;

		slice--;
		if (drm_WARN_ON_ONCE(&dev_priv->drm,
				     slice >= NUM_L3_SLICES(dev_priv)))
			break;

		dev_priv->l3_parity.which_slice &= ~(1<<slice);

		reg = GEN7_L3CDERRST1(slice);

		error_status = intel_uncore_read(&dev_priv->uncore, reg);
		row = GEN7_PARITY_ERROR_ROW(error_status);
		bank = GEN7_PARITY_ERROR_BANK(error_status);
		subbank = GEN7_PARITY_ERROR_SUBBANK(error_status);

		intel_uncore_write(&dev_priv->uncore, reg, GEN7_PARITY_ERROR_VALID | GEN7_L3CDERRST1_ENABLE);
		intel_uncore_posting_read(&dev_priv->uncore, reg);

		parity_event[0] = I915_L3_PARITY_UEVENT "=1";
		parity_event[1] = kasprintf(GFP_KERNEL, "ROW=%d", row);
		parity_event[2] = kasprintf(GFP_KERNEL, "BANK=%d", bank);
		parity_event[3] = kasprintf(GFP_KERNEL, "SUBBANK=%d", subbank);
		parity_event[4] = kasprintf(GFP_KERNEL, "SLICE=%d", slice);
		parity_event[5] = NULL;

		kobject_uevent_env(&dev_priv->drm.primary->kdev->kobj,
				   KOBJ_CHANGE, parity_event);

		DRM_DEBUG("Parity error: Slice = %d, Row = %d, Bank = %d, Sub bank = %d.\n",
			  slice, row, bank, subbank);

		kfree(parity_event[4]);
		kfree(parity_event[3]);
		kfree(parity_event[2]);
		kfree(parity_event[1]);
	}

	intel_uncore_write(&dev_priv->uncore, GEN7_MISCCPCTL, misccpctl);

out:
	drm_WARN_ON(&dev_priv->drm, dev_priv->l3_parity.which_slice);
	spin_lock_irq(gt->irq_lock);
	gen5_gt_enable_irq(gt, GT_PARITY_ERROR(dev_priv));
	spin_unlock_irq(gt->irq_lock);

	mutex_unlock(&dev_priv->drm.struct_mutex);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static bool gen11_port_hotplug_long_detect(enum hpd_pin pin, u32 val)
{
	switch (pin) {
	case HPD_PORT_TC1:
	case HPD_PORT_TC2:
	case HPD_PORT_TC3:
	case HPD_PORT_TC4:
	case HPD_PORT_TC5:
	case HPD_PORT_TC6:
		return val & GEN11_HOTPLUG_CTL_LONG_DETECT(pin);
	default:
		return false;
	}
}

/*
 * Get a bit mask of pins that have triggered, and which ones may be long.
 * This can be called multiple times with the same masks to accumulate
 * hotplug detection results from several registers.
 *
 * Note that the caller is expected to zero out the masks initially.
 */
static void intel_get_hpd_pins(struct drm_i915_private *dev_priv,
			       u32 *pin_mask, u32 *long_mask,
			       u32 hotplug_trigger, u32 dig_hotplug_reg,
			       const u32 hpd[HPD_NUM_PINS],
			       bool long_pulse_detect(enum hpd_pin pin, u32 val))
{
	enum hpd_pin pin;

	BUILD_BUG_ON(BITS_PER_TYPE(*pin_mask) < HPD_NUM_PINS);

	for_each_hpd_pin(pin) {
		if ((hpd[pin] & hotplug_trigger) == 0)
			continue;

		*pin_mask |= BIT(pin);

		if (long_pulse_detect(pin, dig_hotplug_reg))
			*long_mask |= BIT(pin);
	}

	drm_dbg(&dev_priv->drm,
		"hotplug event received, stat 0x%08x, dig 0x%08x, pins 0x%08x, long 0x%08x\n",
		hotplug_trigger, dig_hotplug_reg, *pin_mask, *long_mask);

}

static u32 intel_hpd_enabled_irqs(struct drm_i915_private *dev_priv,
				  const u32 hpd[HPD_NUM_PINS])
{
	struct intel_encoder *encoder;
	u32 enabled_irqs = 0;

	for_each_intel_encoder(&dev_priv->drm, encoder)
		if (dev_priv->hotplug.stats[encoder->hpd_pin].state == HPD_ENABLED)
			enabled_irqs |= hpd[encoder->hpd_pin];

	return enabled_irqs;
}

static u32 intel_hpd_hotplug_irqs(struct drm_i915_private *dev_priv,
				  const u32 hpd[HPD_NUM_PINS])
{
	struct intel_encoder *encoder;
	u32 hotplug_irqs = 0;

	for_each_intel_encoder(&dev_priv->drm, encoder)
		hotplug_irqs |= hpd[encoder->hpd_pin];

	return hotplug_irqs;
}

static u32 intel_hpd_hotplug_enables(struct drm_i915_private *i915,
				     hotplug_enables_func hotplug_enables)
{
	struct intel_encoder *encoder;
	u32 hotplug = 0;

	for_each_intel_encoder(&i915->drm, encoder)
		hotplug |= hotplug_enables(encoder);

	return hotplug;
}

static void gmbus_irq_handler(struct drm_i915_private *dev_priv)
{
	wake_up_all(&dev_priv->gmbus_wait_queue);
}

static void dp_aux_irq_handler(struct drm_i915_private *dev_priv)
{
	wake_up_all(&dev_priv->gmbus_wait_queue);
}

#if defined(CONFIG_DEBUG_FS)
static void display_pipe_crc_irq_handler(struct drm_i915_private *dev_priv,
					 enum pipe pipe,
					 u32 crc0, u32 crc1,
					 u32 crc2, u32 crc3,
					 u32 crc4)
{
	struct intel_crtc *crtc = intel_crtc_for_pipe(dev_priv, pipe);
	struct intel_pipe_crc *pipe_crc = &crtc->pipe_crc;
	u32 crcs[5] = { crc0, crc1, crc2, crc3, crc4 };

	trace_intel_pipe_crc(crtc, crcs);

	spin_lock(&pipe_crc->lock);
	/*
	 * For some not yet identified reason, the first CRC is
	 * bonkers. So let's just wait for the next vblank and read
	 * out the buggy result.
	 *
	 * On GEN8+ sometimes the second CRC is bonkers as well, so
	 * don't trust that one either.
	 */
	if (pipe_crc->skipped <= 1) {
		pipe_crc->skipped++;
		spin_unlock(&pipe_crc->lock);
		return;
	}
	spin_unlock(&pipe_crc->lock);

	drm_crtc_add_crc_entry(&crtc->base, true,
				drm_crtc_accurate_vblank_count(&crtc->base),
				crcs);
}
#else
static inline void
display_pipe_crc_irq_handler(struct drm_i915_private *dev_priv,
			     enum pipe pipe,
			     u32 crc0, u32 crc1,
			     u32 crc2, u32 crc3,
			     u32 crc4) {}
#endif

static void flip_done_handler(struct drm_i915_private *i915,
			      enum pipe pipe)
{
	struct intel_crtc *crtc = intel_crtc_for_pipe(i915, pipe);
	struct drm_crtc_state *crtc_state = crtc->base.state;
	struct drm_pending_vblank_event *e = crtc_state->event;
	struct drm_device *dev = &i915->drm;
	unsigned long irqflags;

	spin_lock_irqsave(&dev->event_lock, irqflags);

	crtc_state->event = NULL;

	drm_crtc_send_vblank_event(&crtc->base, e);

	spin_unlock_irqrestore(&dev->event_lock, irqflags);
}

static void hsw_pipe_crc_irq_handler(struct drm_i915_private *dev_priv,
				     enum pipe pipe)
{
	display_pipe_crc_irq_handler(dev_priv, pipe,
				     intel_uncore_read(&dev_priv->uncore, PIPE_CRC_RES_1_IVB(pipe)),
				     0, 0, 0, 0);
}

#endif

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void xelpdp_pica_irq_handler(struct drm_i915_private *i915, u32 iir)
{
	enum hpd_pin pin;
	u32 hotplug_trigger = iir & (XELPDP_DP_ALT_HOTPLUG_MASK | XELPDP_TBT_HOTPLUG_MASK);
	u32 trigger_aux = iir & XELPDP_AUX_TC_MASK;
	u32 pin_mask = 0, long_mask = 0;

	for (pin = HPD_PORT_TC1; pin <= HPD_PORT_TC4; pin++) {
		u32 val;

		if (!(i915->hotplug.hpd[pin] & hotplug_trigger))
			continue;

		pin_mask |= BIT(pin);

		val = intel_de_read(i915, XELPDP_PORT_HOTPLUG_CTL(pin));
		intel_de_write(i915, XELPDP_PORT_HOTPLUG_CTL(pin), val);

		if (val & (XELPDP_DP_ALT_HPD_LONG_DETECT | XELPDP_TBT_HPD_LONG_DETECT))
			long_mask |= BIT(pin);
	}

	if (pin_mask) {
		drm_dbg(&i915->drm,
			"pica hotplug event received, stat 0x%08x, pins 0x%08x, long 0x%08x\n",
			hotplug_trigger, pin_mask, long_mask);

		intel_hpd_irq_handler(i915, pin_mask, long_mask);
	}

	if (trigger_aux)
		dp_aux_irq_handler(i915);

	if (!pin_mask && !trigger_aux)
		drm_err(&i915->drm,
			"Unexpected DE HPD/AUX interrupt 0x%08x\n", iir);
}
#endif

/*
 * To handle irqs with the minimum potential races with fresh interrupts, we:
 * 1 - Disable Master Interrupt Control.
 * 2 - Find the source(s) of the interrupt.
 * 3 - Clear the Interrupt Identity bits (IIR).
 * 4 - Process the interrupt(s) that had bits set in the IIRs.
 * 5 - Re-enable Master Interrupt Control.
 */

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void gen11_hpd_irq_handler(struct drm_i915_private *dev_priv, u32 iir)
{
	u32 pin_mask = 0, long_mask = 0;
	u32 trigger_tc = iir & GEN11_DE_TC_HOTPLUG_MASK;
	u32 trigger_tbt = iir & GEN11_DE_TBT_HOTPLUG_MASK;

	if (trigger_tc) {
		u32 dig_hotplug_reg;

		dig_hotplug_reg = intel_uncore_read(&dev_priv->uncore, GEN11_TC_HOTPLUG_CTL);
		intel_uncore_write(&dev_priv->uncore, GEN11_TC_HOTPLUG_CTL, dig_hotplug_reg);

		intel_get_hpd_pins(dev_priv, &pin_mask, &long_mask,
				   trigger_tc, dig_hotplug_reg,
				   dev_priv->hotplug.hpd,
				   gen11_port_hotplug_long_detect);
	}

	if (trigger_tbt) {
		u32 dig_hotplug_reg;

		dig_hotplug_reg = intel_uncore_read(&dev_priv->uncore, GEN11_TBT_HOTPLUG_CTL);
		intel_uncore_write(&dev_priv->uncore, GEN11_TBT_HOTPLUG_CTL, dig_hotplug_reg);

		intel_get_hpd_pins(dev_priv, &pin_mask, &long_mask,
				   trigger_tbt, dig_hotplug_reg,
				   dev_priv->hotplug.hpd,
				   gen11_port_hotplug_long_detect);
	}

	if (pin_mask)
		intel_hpd_irq_handler(dev_priv, pin_mask, long_mask);
	else
		drm_err(&dev_priv->drm,
			"Unexpected DE HPD interrupt 0x%08x\n", iir);
}

static u32 gen8_de_port_aux_mask(struct drm_i915_private *dev_priv)
{
	if (DISPLAY_VER(dev_priv) >= 14)
		return TGL_DE_PORT_AUX_DDIA |
			TGL_DE_PORT_AUX_DDIB;
	else if (DISPLAY_VER(dev_priv) >= 13)
		return TGL_DE_PORT_AUX_DDIA |
			TGL_DE_PORT_AUX_DDIB |
			TGL_DE_PORT_AUX_DDIC |
			XELPD_DE_PORT_AUX_DDID |
			XELPD_DE_PORT_AUX_DDIE |
			TGL_DE_PORT_AUX_USBC1 |
			TGL_DE_PORT_AUX_USBC2 |
			TGL_DE_PORT_AUX_USBC3 |
			TGL_DE_PORT_AUX_USBC4;
	else
		return TGL_DE_PORT_AUX_DDIA |
			TGL_DE_PORT_AUX_DDIB |
			TGL_DE_PORT_AUX_DDIC |
			TGL_DE_PORT_AUX_USBC1 |
			TGL_DE_PORT_AUX_USBC2 |
			TGL_DE_PORT_AUX_USBC3 |
			TGL_DE_PORT_AUX_USBC4 |
			TGL_DE_PORT_AUX_USBC5 |
			TGL_DE_PORT_AUX_USBC6;
}

static u32 gen8_de_pipe_fault_mask(struct drm_i915_private *dev_priv)
{
	if (DISPLAY_VER(dev_priv) >= 13 || HAS_D12_PLANE_MINIMIZATION(dev_priv))
		return RKL_DE_PIPE_IRQ_FAULT_ERRORS;
	else
		return GEN11_DE_PIPE_IRQ_FAULT_ERRORS;
}

static void intel_pmdemand_irq_handler(struct drm_i915_private *dev_priv)
{
	wake_up_all(&dev_priv->pmdemand.waitqueue);
}

static void
gen8_de_misc_irq_handler(struct drm_i915_private *dev_priv, u32 iir)
{
	bool found = false;

	if (iir & GEN8_DE_MISC_GSE) {
		intel_opregion_asle_intr(dev_priv);
		found = true;
	}

	if (iir & GEN8_DE_EDP_PSR) {
		struct intel_encoder *encoder;
		u32 psr_iir;
		i915_reg_t iir_reg;

		for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
			struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

			iir_reg = TRANS_PSR_IIR(intel_dp->psr.transcoder);

			psr_iir = intel_uncore_read(&dev_priv->uncore, iir_reg);
			intel_uncore_write(&dev_priv->uncore, iir_reg, psr_iir);

			if (psr_iir)
				found = true;

			intel_psr_irq_handler(intel_dp, psr_iir);
		}
	}

	if (iir & XELPDP_PMDEMAND_RSPTOUT_ERR) {
		drm_dbg(&dev_priv->drm,
			"Error waiting for Punit PM Demand Response\n");
		intel_pmdemand_irq_handler(dev_priv);
		found = true;
	}

	if (iir & XELPDP_PMDEMAND_RSP) {
		intel_pmdemand_irq_handler(dev_priv);
		found = true;
	}

	if (!found)
		drm_err(&dev_priv->drm, "Unexpected DE Misc interrupt\n");
}

static void gen11_dsi_te_interrupt_handler(struct drm_i915_private *dev_priv,
					   u32 te_trigger)
{
	enum pipe pipe = INVALID_PIPE;
	enum transcoder dsi_trans;
	enum port port;
	u32 val, tmp;

	/*
	 * Incase of dual link, TE comes from DSI_1
	 * this is to check if dual link is enabled
	 */
	val = intel_uncore_read(&dev_priv->uncore, TRANS_DDI_FUNC_CTL2(TRANSCODER_DSI_0));
	val &= PORT_SYNC_MODE_ENABLE;

	/*
	 * if dual link is enabled, then read DSI_0
	 * transcoder registers
	 */
	port = ((te_trigger & DSI1_TE && val) || (te_trigger & DSI0_TE)) ?
						  PORT_A : PORT_B;
	dsi_trans = (port == PORT_A) ? TRANSCODER_DSI_0 : TRANSCODER_DSI_1;

	/* Check if DSI configured in command mode */
	val = intel_uncore_read(&dev_priv->uncore, DSI_TRANS_FUNC_CONF(dsi_trans));
	val = val & OP_MODE_MASK;

	if (val != CMD_MODE_NO_GATE && val != CMD_MODE_TE_GATE) {
		drm_err(&dev_priv->drm, "DSI trancoder not configured in command mode\n");
		return;
	}

	/* Get PIPE for handling VBLANK event */
	val = intel_uncore_read(&dev_priv->uncore, TRANS_DDI_FUNC_CTL(dsi_trans));
	switch (val & TRANS_DDI_EDP_INPUT_MASK) {
	case TRANS_DDI_EDP_INPUT_A_ON:
		pipe = PIPE_A;
		break;
	case TRANS_DDI_EDP_INPUT_B_ONOFF:
		pipe = PIPE_B;
		break;
	case TRANS_DDI_EDP_INPUT_C_ONOFF:
		pipe = PIPE_C;
		break;
	default:
		drm_err(&dev_priv->drm, "Invalid PIPE\n");
		return;
	}

	intel_handle_vblank(dev_priv, pipe);

	/* clear TE in dsi IIR */
	port = (te_trigger & DSI1_TE) ? PORT_B : PORT_A;
	tmp = intel_uncore_read(&dev_priv->uncore, DSI_INTR_IDENT_REG(port));
	intel_uncore_write(&dev_priv->uncore, DSI_INTR_IDENT_REG(port), tmp);
}

static u32 gen8_de_pipe_flip_done_mask(struct drm_i915_private *i915)
{
	return GEN9_PIPE_PLANE1_FLIP_DONE;
}

static void gen8_read_and_ack_pch_irqs(struct drm_i915_private *i915, u32 *pch_iir, u32 *pica_iir)
{
	u32 pica_ier = 0;

	*pica_iir = 0;
	*pch_iir = intel_de_read(i915, SDEIIR);
	if (!*pch_iir)
		return;

	/**
	 * PICA IER must be disabled/re-enabled around clearing PICA IIR and
	 * SDEIIR, to avoid losing PICA IRQs and to ensure that such IRQs set
	 * their flags both in the PICA and SDE IIR.
	 */
	if (*pch_iir & SDE_PICAINTERRUPT) {
		drm_WARN_ON(&i915->drm, INTEL_PCH_TYPE(i915) < PCH_MTP);

		pica_ier = intel_de_rmw(i915, PICAINTERRUPT_IER, ~0, 0);
		*pica_iir = intel_de_read(i915, PICAINTERRUPT_IIR);
		intel_de_write(i915, PICAINTERRUPT_IIR, *pica_iir);
	}

	intel_de_write(i915, SDEIIR, *pch_iir);

	if (pica_ier)
		intel_de_write(i915, PICAINTERRUPT_IER, pica_ier);
}

static bool icp_ddi_port_hotplug_long_detect(enum hpd_pin pin, u32 val)
{
	switch (pin) {
	case HPD_PORT_A:
	case HPD_PORT_B:
	case HPD_PORT_C:
	case HPD_PORT_D:
		return val & SHOTPLUG_CTL_DDI_HPD_LONG_DETECT(pin);
	default:
		return false;
	}
}

static bool icp_tc_port_hotplug_long_detect(enum hpd_pin pin, u32 val)
{
	switch (pin) {
	case HPD_PORT_TC1:
	case HPD_PORT_TC2:
	case HPD_PORT_TC3:
	case HPD_PORT_TC4:
	case HPD_PORT_TC5:
	case HPD_PORT_TC6:
		return val & ICP_TC_HPD_LONG_DETECT(pin);
	default:
		return false;
	}
}

static void icp_irq_handler(struct drm_i915_private *dev_priv, u32 pch_iir)
{
	u32 ddi_hotplug_trigger = pch_iir & SDE_DDI_HOTPLUG_MASK_ICP;
	u32 tc_hotplug_trigger = pch_iir & SDE_TC_HOTPLUG_MASK_ICP;
	u32 pin_mask = 0, long_mask = 0;

	if (ddi_hotplug_trigger) {
		u32 dig_hotplug_reg;

		dig_hotplug_reg = intel_uncore_read(&dev_priv->uncore, SHOTPLUG_CTL_DDI);
		intel_uncore_write(&dev_priv->uncore, SHOTPLUG_CTL_DDI, dig_hotplug_reg);

		intel_get_hpd_pins(dev_priv, &pin_mask, &long_mask,
				   ddi_hotplug_trigger, dig_hotplug_reg,
				   dev_priv->hotplug.pch_hpd,
				   icp_ddi_port_hotplug_long_detect);
	}

	if (tc_hotplug_trigger) {
		u32 dig_hotplug_reg;

		dig_hotplug_reg = intel_uncore_read(&dev_priv->uncore, SHOTPLUG_CTL_TC);
		intel_uncore_write(&dev_priv->uncore, SHOTPLUG_CTL_TC, dig_hotplug_reg);

		intel_get_hpd_pins(dev_priv, &pin_mask, &long_mask,
				   tc_hotplug_trigger, dig_hotplug_reg,
				   dev_priv->hotplug.pch_hpd,
				   icp_tc_port_hotplug_long_detect);
	}

	if (pin_mask)
		intel_hpd_irq_handler(dev_priv, pin_mask, long_mask);

	if (pch_iir & SDE_GMBUS_ICP)
		gmbus_irq_handler(dev_priv);
}

static irqreturn_t
gen8_de_irq_handler(struct drm_i915_private *dev_priv, u32 master_ctl)
{
	irqreturn_t ret = IRQ_NONE;
	u32 iir;
	enum pipe pipe;

	drm_WARN_ON_ONCE(&dev_priv->drm, !HAS_DISPLAY(dev_priv));

	if (master_ctl & GEN8_DE_MISC_IRQ) {
		iir = intel_uncore_read(&dev_priv->uncore, GEN8_DE_MISC_IIR);
		if (iir) {
			intel_uncore_write(&dev_priv->uncore, GEN8_DE_MISC_IIR, iir);
			ret = IRQ_HANDLED;
			gen8_de_misc_irq_handler(dev_priv, iir);
		} else {
			drm_err_ratelimited(&dev_priv->drm,
					    "The master control interrupt lied (DE MISC)!\n");
		}
	}

	if (master_ctl & GEN11_DE_HPD_IRQ) {
		iir = intel_uncore_read(&dev_priv->uncore, GEN11_DE_HPD_IIR);
		if (iir) {
			intel_uncore_write(&dev_priv->uncore, GEN11_DE_HPD_IIR, iir);
			ret = IRQ_HANDLED;
			gen11_hpd_irq_handler(dev_priv, iir);
		} else {
			drm_err_ratelimited(&dev_priv->drm,
					    "The master control interrupt lied, (DE HPD)!\n");
		}
	}

	if (master_ctl & GEN8_DE_PORT_IRQ) {
		iir = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PORT_IIR);
		if (iir) {
			bool found = false;

			intel_uncore_write(&dev_priv->uncore, GEN8_DE_PORT_IIR, iir);
			ret = IRQ_HANDLED;

			if (iir & gen8_de_port_aux_mask(dev_priv)) {
				dp_aux_irq_handler(dev_priv);
				found = true;
			}

			if (iir & (DSI0_TE | DSI1_TE)) {
				gen11_dsi_te_interrupt_handler(dev_priv, iir & (DSI0_TE | DSI1_TE));
				found = true;
			}

			if (!found)
				drm_err_ratelimited(&dev_priv->drm,
						    "Unexpected DE Port interrupt\n");
		}
		else
			drm_err_ratelimited(&dev_priv->drm,
					    "The master control interrupt lied (DE PORT)!\n");
	}

	for_each_pipe(dev_priv, pipe) {
		u32 fault_errors;

		if (!(master_ctl & GEN8_DE_PIPE_IRQ(pipe)))
			continue;

		iir = intel_uncore_read(&dev_priv->uncore, GEN8_DE_PIPE_IIR(pipe));
		if (!iir) {
			drm_err_ratelimited(&dev_priv->drm,
					    "The master control interrupt lied (DE PIPE)!\n");
			continue;
		}

		ret = IRQ_HANDLED;
		intel_uncore_write(&dev_priv->uncore, GEN8_DE_PIPE_IIR(pipe), iir);

		if (iir & GEN8_PIPE_VBLANK)
			intel_handle_vblank(dev_priv, pipe);

		if (iir & gen8_de_pipe_flip_done_mask(dev_priv))
			flip_done_handler(dev_priv, pipe);

		if (iir & GEN8_PIPE_CDCLK_CRC_DONE)
			hsw_pipe_crc_irq_handler(dev_priv, pipe);

		fault_errors = iir & gen8_de_pipe_fault_mask(dev_priv);
		if (fault_errors)
			drm_err_ratelimited(&dev_priv->drm,
					    "Fault errors on pipe %c: 0x%08x\n",
					    pipe_name(pipe),
					    fault_errors);
	}

	if (HAS_PCH_SPLIT(dev_priv) && !HAS_PCH_NOP(dev_priv) &&
	    master_ctl & GEN8_DE_PCH_IRQ) {
		u32 pica_iir;

		/*
		 * FIXME(BDW): Assume for now that the new interrupt handling
		 * scheme also closed the SDE interrupt handling race we've seen
		 * on older pch-split platforms. But this needs testing.
		 */
		gen8_read_and_ack_pch_irqs(dev_priv, &iir, &pica_iir);
		if (iir) {
			ret = IRQ_HANDLED;

			if (pica_iir)
				xelpdp_pica_irq_handler(dev_priv, pica_iir);

			icp_irq_handler(dev_priv, iir);
		} else {
			/*
			 * Like on previous PCH there seems to be something
			 * fishy going on with forwarding PCH interrupts.
			 */
			drm_dbg(&dev_priv->drm,
				"The master control interrupt lied (SDE)!\n");
		}
	}

	return ret;
}
#endif

static const char *
hardware_error_type_to_str(const enum hardware_error hw_err)
{
	switch (hw_err) {
	case HARDWARE_ERROR_CORRECTABLE:
		return "CORRECTABLE";
	case HARDWARE_ERROR_NONFATAL:
		return "NONFATAL";
	case HARDWARE_ERROR_FATAL:
		return "FATAL";
	default:
		return "UNKNOWN";
	}
}

#define log_gt_hw_err(gt, fmt, ...) \
	drm_err_ratelimited(&(gt)->i915->drm, HW_ERR "GT%d detected " fmt, \
			    (gt)->info.id, ##__VA_ARGS__)

static const char *
soc_err_index_to_str(unsigned long index)
{
	switch (index) {
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, SOC_PSF_CSC_0):
		return "Invalid CSC PSF Command Parity";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, SOC_PSF_CSC_1):
		return "Invalid CSC PSF Unexpected Completion";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, SOC_PSF_CSC_2):
		return "Invalid CSC PSF Unsupported Request";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_PSF_0):
		return "Invalid PCIe PSF Command Parity";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_PSF_1):
		return "PCIe PSF Unexpected Completion";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_PSF_2):
		return "PCIe PSF Unsupported Request";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_CD0_MDFI):
		return "ANR MDFI";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_PCIAER):
		return "Local IEH internal: Malformed PCIe AER";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_PCIERR):
		return "Local IEH internal: Malformed PCIe ERR";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_UR):
		return "Local IEH internal: UR conditions in IEH";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_SERR_SRCS):
		return "Local IEH internal: From SERR Sources";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_MDFI_EAST):
		return "Base Die MDFI T2T";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_FATAL, PVC_SOC_MDFI_SOUTH):
		return "Base Die MDFI T2C";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_PUNIT):
		return "PUNIT";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_0):
		return "HBM SS0: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_1):
		return "HBM SS0: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_2):
		return "HBM SS0: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_3):
		return "HBM SS0: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_4):
		return "HBM SS0: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_5):
		return "HBM SS0: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_6):
		return "HBM SS0: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, SOC_HBM_SS0_7):
		return "HBM SS0: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_0):
		return "HBM SS1: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_1):
		return "HBM SS1: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_2):
		return "HBM SS1: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_3):
		return "HBM SS1: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_4):
		return "HBM SS1: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_5):
		return "HBM SS1: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_6):
		return "HBM SS1: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS1_7):
		return "HBM SS1: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_0):
		return "HBM SS2: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_1):
		return "HBM SS2: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_2):
		return "HBM SS2: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_3):
		return "HBM SS2: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_4):
		return "HBM SS2: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_5):
		return "HBM SS2: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_6):
		return "HBM SS2: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS2_7):
		return "HBM SS2: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_0):
		return "HBM SS3: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_1):
		return "HBM SS3: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_2):
		return "HBM SS3: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_3):
		return "HBM SS3: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_4):
		return "HBM SS3: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_5):
		return "HBM SS3: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_6):
		return "HBM SS3: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_FATAL, PVC_SOC_HBM_SS3_7):
		return "HBM SS3: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_NONFATAL, SOC_PSF_CSC_0):
		return "Invalid CSC PSF Command Parity";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_NONFATAL, SOC_PSF_CSC_1):
		return "Invalid CSC PSF Unexpected Completion";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_NONFATAL, SOC_PSF_CSC_2):
		return "Invalid CSC PSF Unsupported Request";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_CD0_MDFI):
		return "ANR MDFI";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_MDFI_EAST):
		return "Base Die MDFI T2T";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_MDFI_SOUTH):
		return "Base Die MDFI T2C";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_0):
		return "Invalid HBM SS0: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_1):
		return "Invalid HBM SS0: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_2):
		return "Invalid HBM SS0: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_3):
		return "Invalid HBM SS0: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_4):
		return "Invalid HBM SS0: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_5):
		return "Invalid HBM SS0: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_6):
		return "Invalid HBM SS0: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, SOC_HBM_SS0_7):
		return "Invalid HBM SS0: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_0):
		return "Invalid HBM SS1: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_1):
		return "Invalid HBM SS1: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_2):
		return "Invalid HBM SS1: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_3):
		return "Invalid HBM SS1: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_4):
		return "Invalid HBM SS1: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_5):
		return "Invalid HBM SS1: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_6):
		return "Invalid HBM SS1: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS1_7):
		return "Invalid HBM SS1: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_0):
		return "Invalid HBM SS2: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_1):
		return "Invalid HBM SS2: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_2):
		return "Invalid HBM SS2: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_3):
		return "Invalid HBM SS2: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_4):
		return "Invalid HBM SS2: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_5):
		return "Invalid HBM SS2: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_6):
		return "Invalid HBM SS2: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS2_7):
		return "Invalid HBM SS2: Channel7";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_0):
		return "Invalid HBM SS3: Channel0";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_1):
		return "Invalid HBM SS3: Channel1";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_2):
		return "Invalid HBM SS3: Channel2";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_3):
		return "Invalid HBM SS3: Channel3";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_4):
		return "Invalid HBM SS3: Channel4";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_5):
		return "Invalid HBM SS3: Channel5";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_6):
		return "Invalid HBM SS3: Channel6";
	case SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, HARDWARE_ERROR_NONFATAL, PVC_SOC_HBM_SS3_7):
		return "Invalid HBM SS3: Channel7";
	default:
		return "Undefined";
	}
}

static void log_soc_hw_error(struct intel_gt *gt, unsigned long index,
			     const enum hardware_error hw_err)
{
	const char *hwerr_to_str = hardware_error_type_to_str(hw_err);
	const char *error_name;

	if (!IS_PONTEVECCHIO(gt->i915))
		return;

	error_name = soc_err_index_to_str(index);
	if (!strcmp(error_name, "Undefined") || strstr(error_name, "Invalid"))
		intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT, "%s SOC %s error\n",
					  error_name, hwerr_to_str);
	else
		log_gt_hw_err(gt, "%s SOC %s error\n", error_name, hwerr_to_str);
}

static void update_soc_hw_error_cnt(struct intel_gt *gt, unsigned long index)
{
	unsigned long flags;
	void *entry;

	entry = xa_load(&gt->errors.soc, index);
	entry = xa_mk_value(xa_to_value(entry) + 1);

	xa_lock_irqsave(&gt->errors.soc, flags);
	if (xa_is_err(__xa_store(&gt->errors.soc, index, entry, GFP_ATOMIC)))
		drm_err_ratelimited(&gt->i915->drm,
				    HW_ERR "SOC error reported by IEH%lu on GT %d lost\n",
				   (index >> IEH_SHIFT) & IEH_MASK,
				    gt->info.id);
	xa_unlock_irqrestore(&gt->errors.soc, flags);
}

static void
gen12_soc_hw_error_handler(struct intel_gt *gt,
			  const enum hardware_error hw_err)
{
	void __iomem * const regs = gt->uncore->regs;
	unsigned long mst_glb_errstat, slv_glb_errstat, lcl_errstat, index;
	u32 ieh_header;
	u32 errbit;
	u32 base = SOC_XEHPSDV_BASE;
	u32 slave_base = SOC_XEHPSDV_SLAVE_BASE;
	int i;

	lockdep_assert_held(gt->irq_lock);
	if (!IS_PONTEVECCHIO(gt->i915))
		return;

	base = SOC_PVC_BASE;
	slave_base = SOC_PVC_SLAVE_BASE;

	log_gt_hw_err(gt, "SOC %s error\n", hardware_error_type_to_str(hw_err));

	if (hw_err == HARDWARE_ERROR_CORRECTABLE ||
	    (hw_err == HARDWARE_ERROR_NONFATAL && !IS_PONTEVECCHIO(gt->i915))) {
		for (i = 0; i < INTEL_GT_SOC_NUM_IEH; i++)
			raw_reg_write(regs, SOC_GSYSEVTCTL_REG(base, slave_base, i),
				      ~REG_BIT(hw_err));

		raw_reg_write(regs, SOC_GLOBAL_ERR_STAT_MASTER_REG(base, hw_err),
			      REG_GENMASK(31, 0));
		raw_reg_write(regs, SOC_LOCAL_ERR_STAT_MASTER_REG(base, hw_err),
			      REG_GENMASK(31, 0));
		raw_reg_write(regs, SOC_GLOBAL_ERR_STAT_SLAVE_REG(slave_base, hw_err),
			      REG_GENMASK(31, 0));
		raw_reg_write(regs, SOC_LOCAL_ERR_STAT_SLAVE_REG(slave_base, hw_err),
			      REG_GENMASK(31, 0));

		intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT, "Invalid SOC %s error\n", hardware_error_type_to_str(hw_err));

		goto unmask_gsysevtctl;
	}

	/*
	 * Mask error type in GSYSEVTCTL so that no new errors of the type
	 * will be reported. Read the master global IEH error register if
	 * BIT 1 is set then process the slave IEH first. If BIT 0 in
	 * global error register is set then process the corresponding
	 * Local error registers
	 */
	for (i = 0; i < INTEL_GT_SOC_NUM_IEH; i++)
		raw_reg_write(regs, SOC_GSYSEVTCTL_REG(base, slave_base, i), ~REG_BIT(hw_err));

	mst_glb_errstat = raw_reg_read(regs,
				       SOC_GLOBAL_ERR_STAT_MASTER_REG(base, hw_err));
	log_gt_hw_err(gt, "SOC_GLOBAL_ERR_STAT_MASTER_REG_%s:0x%08lx\n",
		      hardware_error_type_to_str(hw_err), mst_glb_errstat);
	if (mst_glb_errstat & REG_BIT(SOC_SLAVE_IEH)) {
		slv_glb_errstat = raw_reg_read(regs,
					       SOC_GLOBAL_ERR_STAT_SLAVE_REG(slave_base,
									     hw_err));
		log_gt_hw_err(gt, "SOC_GLOBAL_ERR_STAT_SLAVE_REG_%s:0x%08lx\n",
			      hardware_error_type_to_str(hw_err), slv_glb_errstat);

		if (slv_glb_errstat & REG_BIT(SOC_IEH1_LOCAL_ERR_STATUS)) {
			lcl_errstat = raw_reg_read(regs,
						   SOC_LOCAL_ERR_STAT_SLAVE_REG(slave_base,
										hw_err));
			log_gt_hw_err(gt, "SOC_LOCAL_ERR_STAT_SLAVE_REG_%s:0x%08lx\n",
				      hardware_error_type_to_str(hw_err), lcl_errstat);

			for_each_set_bit(errbit, &lcl_errstat,
					 SOC_HW_ERR_MAX_BITS) {
				/*
				 * SOC errors have global and local error
				 * registers for each correctable non-fatal
				 * and fatal categories and these are per IEH
				 * on platform. XEHPSDV and PVC have two IEHs
				 */
				index = SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_LOCAL, hw_err, errbit);
				update_soc_hw_error_cnt(gt, index);
				if (IS_PONTEVECCHIO(gt->i915))
					intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
								  "Invalid SOC %s error\n", hardware_error_type_to_str(hw_err));
			}
			raw_reg_write(regs, SOC_LOCAL_ERR_STAT_SLAVE_REG(slave_base, hw_err),
				      lcl_errstat);
		}

		for_each_set_bit(errbit, &slv_glb_errstat, SOC_HW_ERR_MAX_BITS) {
			/* Skip reprocessing of SOC_IEH1_LOCAL_ERR_STATUS bit */
			if (errbit == SOC_IEH1_LOCAL_ERR_STATUS)
				continue;

			index = SOC_ERR_INDEX(INTEL_GT_SOC_IEH1, INTEL_SOC_REG_GLOBAL, hw_err, errbit);
			update_soc_hw_error_cnt(gt, index);
			log_soc_hw_error(gt, index, hw_err);
		}
		raw_reg_write(regs, SOC_GLOBAL_ERR_STAT_SLAVE_REG(slave_base, hw_err),
			      slv_glb_errstat);
	}

	if (mst_glb_errstat & REG_BIT(SOC_IEH0_LOCAL_ERR_STATUS)) {
		lcl_errstat = raw_reg_read(regs,
					   SOC_LOCAL_ERR_STAT_MASTER_REG(base, hw_err));
		log_gt_hw_err(gt, "SOC_LOCAL_ERR_STAT_MASTER_REG_%s:0x%08lx\n",
			      hardware_error_type_to_str(hw_err), lcl_errstat);
		for_each_set_bit(errbit, &lcl_errstat, SOC_HW_ERR_MAX_BITS) {
			if (errbit == PVC_SOC_MDFI_EAST || errbit == PVC_SOC_MDFI_SOUTH) {
			       ieh_header = raw_reg_read(regs, LOCAL_FIRST_IEH_HEADER_LOG_REG);
			       log_gt_hw_err(gt, "LOCAL_FIRST_IEH_HEADER_LOG_REG:0x%08x\n",
					     ieh_header);

			       if (ieh_header != MDFI_SEVERITY(hw_err)) {
				       lcl_errstat &= ~REG_BIT(errbit);
				       continue;
			       }
			}
			index = SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_LOCAL, hw_err, errbit);
			update_soc_hw_error_cnt(gt, index);
			log_soc_hw_error(gt, index, hw_err);
		}
		raw_reg_write(regs, SOC_LOCAL_ERR_STAT_MASTER_REG(base, hw_err),
			      lcl_errstat);
	}

	for_each_set_bit(errbit, &mst_glb_errstat, SOC_HW_ERR_MAX_BITS) {
		/* Skip reprocessing of SOC_SLAVE_IEH and SOC_IEH0_LOCAL_ERR_STATUS */
		if (errbit == SOC_SLAVE_IEH || errbit == SOC_IEH0_LOCAL_ERR_STATUS)
			continue;

		index = SOC_ERR_INDEX(INTEL_GT_SOC_IEH0, INTEL_SOC_REG_GLOBAL, hw_err, errbit);
		update_soc_hw_error_cnt(gt, index);
		log_soc_hw_error(gt, index, hw_err);
	}
	raw_reg_write(regs, SOC_GLOBAL_ERR_STAT_MASTER_REG(base, hw_err),
		      mst_glb_errstat);

unmask_gsysevtctl:
	for (i = 0; i < INTEL_GT_SOC_NUM_IEH; i++)
		raw_reg_write(regs, SOC_GSYSEVTCTL_REG(base, slave_base, i),
			      (HARDWARE_ERROR_MAX << 1) + 1);

}

static void gen12_gt_fatal_hw_error_stats_update(struct intel_gt *gt,
						 unsigned long errstat)
{
	u32 errbit, cnt;

	if (!errstat && HAS_GT_ERROR_VECTORS(gt->i915))
		return;

	for_each_set_bit(errbit, &errstat, GT_HW_ERROR_MAX_ERR_BITS) {
		if (IS_PONTEVECCHIO(gt->i915) && !(REG_BIT(errbit) & PVC_FAT_ERR_MASK)) {
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
						  "Undefined FATAL error\n");
			continue;
		}

		switch (errbit) {
		case ARRAY_BIST_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_ARR_BIST]++;
			log_gt_hw_err(gt, "Array BIST FATAL error\n");
			break;
		case FPU_UNCORR_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_FPU]++;
			log_gt_hw_err(gt, "FPU FATAL error\n");
			break;
		case L3_DOUBLE_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_L3_DOUB]++;
			log_gt_hw_err(gt, "L3 Double FATAL error\n");
			break;
		case L3_ECC_CHK_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_L3_ECC_CHK]++;
			log_gt_hw_err(gt, "L3 ECC Checker FATAL error\n");
			break;
		case GUC_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_GUC]++;
			log_gt_hw_err(gt, "GUC SRAM FATAL error\n");
			break;
		case IDI_PAR_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_IDI_PAR]++;
			log_gt_hw_err(gt, "IDI PARITY FATAL error\n");
			break;
		case SQIDI_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_SQIDI]++;
			log_gt_hw_err(gt, "SQIDI FATAL error\n");
			break;
		case SAMPLER_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_SAMPLER]++;
			log_gt_hw_err(gt, "SAMPLER FATAL error\n");
			break;
		case SLM_FAT_ERR:
			if (!IS_PONTEVECCHIO(gt->i915)) {
				cnt = intel_uncore_read(gt->uncore,
							SLM_ECC_ERROR_CNTR(HARDWARE_ERROR_FATAL));
				gt->errors.hw[INTEL_GT_HW_ERROR_FAT_SLM] = cnt;
			} else {
				gt->errors.hw[INTEL_GT_HW_ERROR_FAT_SLM]++;
			}
			log_gt_hw_err(gt, "LSC Uncorrectable Fatal error\n");
			break;
		case EU_IC_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_EU_IC]++;
			log_gt_hw_err(gt, "EU IC FATAL error\n");
			break;
		case EU_GRF_FAT_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_FAT_EU_GRF]++;
			log_gt_hw_err(gt, "EU GRF FATAL error\n");
			break;
		default:
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
						  "Undefined FATAL error\n");
			break;
		}
	}
}

static void
gen12_gt_correctable_hw_error_stats_update(struct intel_gt *gt,
					   unsigned long errstat)
{
	u32 errbit, cnt;

	if (!errstat && HAS_GT_ERROR_VECTORS(gt->i915))
		return;

	for_each_set_bit(errbit, &errstat, GT_HW_ERROR_MAX_ERR_BITS) {
		if (IS_PONTEVECCHIO(gt->i915) && !(REG_BIT(errbit) & PVC_COR_ERR_MASK)) {
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
						  "Undefined CORRECTABLE error\n");
			continue;
		}

		switch (errbit) {
		case L3_SNG_COR_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_COR_L3_SNG]++;
			log_gt_hw_err(gt, "l3 single correctable error\n");
			break;
		case GUC_COR_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_COR_GUC]++;
			log_gt_hw_err(gt, "SINGLE BIT GUC SRAM CORRECTABLE error\n");
			break;
		case SAMPLER_COR_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_COR_SAMPLER]++;
			log_gt_hw_err(gt, "SINGLE BIT SAMPLER CORRECTABLE error\n");
			break;
		case SLM_COR_ERR:
			if (!IS_PONTEVECCHIO(gt->i915)) {
				cnt = intel_uncore_read(gt->uncore,
							SLM_ECC_ERROR_CNTR(HARDWARE_ERROR_CORRECTABLE));
				gt->errors.hw[INTEL_GT_HW_ERROR_COR_SLM] = cnt;
			} else {
				gt->errors.hw[INTEL_GT_HW_ERROR_COR_SLM]++;
			}

			log_gt_hw_err(gt, "SINGLE BIT SLM CORRECTABLE error\n");
			break;
		case EU_IC_COR_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_COR_EU_IC]++;
			log_gt_hw_err(gt, "SINGLE BIT EU IC CORRECTABLE error\n");
			break;
		case EU_GRF_COR_ERR:
			gt->errors.hw[INTEL_GT_HW_ERROR_COR_EU_GRF]++;
			log_gt_hw_err(gt, "SINGLE BIT EU GRF CORRECTABLE error\n");
			break;
		default:
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
						  "Undefined CORRECTABLE error\n");
			break;
		}
	}
}

static void gen12_gsc_hw_error_work(struct work_struct *work)
{
	struct intel_gt *gt =
		container_of(work, typeof(*gt), gsc_hw_error_work);
	char *csc_hw_error_event[3];

	csc_hw_error_event[0] = PRELIM_I915_MEMORY_HEALTH_UEVENT "=1";
	csc_hw_error_event[1] = "SPARING_STATUS_UNKNOWN=1 RESET_REQUIRED=1";
	csc_hw_error_event[2] = NULL;
	gt->mem_sparing.health_status = MEM_HEALTH_UNKNOWN;

	dev_notice(gt->i915->drm.dev, "Unknown memory health status, Reset Required\n");
	kobject_uevent_env(&gt->i915->drm.primary->kdev->kobj, KOBJ_CHANGE,
			   csc_hw_error_event);
}

static void gen12_mem_health_work(struct work_struct *work)
{
	struct intel_gt *gt =
		container_of(work, typeof(*gt), mem_sparing.mem_health_work);
	u32 cause;
	int event_idx = 0;
	char *sparing_event[3];

	spin_lock_irq(gt->irq_lock);
	cause = fetch_and_zero(&gt->mem_sparing.cause);
	spin_unlock_irq(gt->irq_lock);
	if (!cause)
		return;

	sparing_event[event_idx++] = PRELIM_I915_MEMORY_HEALTH_UEVENT "=1";
	switch (cause) {
	case BANK_SPARNG_ERR_MITIGATION_DOWNGRADED:
		gt->mem_sparing.health_status = MEM_HEALTH_ALARM;
		sparing_event[event_idx++] = "MEM_HEALTH_ALARM=1";

		break;
	case BANK_SPARNG_DIS_PCLS_EXCEEDED:
		/* We get this correctable error notification only
		 * after a threshold in the firmware for correctable
		 * errors has been reached. Hence the recommendation
		 * is to run through PPR which happens after the
		 * card is reset.
		 */
		gt->mem_sparing.health_status = MEM_HEALTH_EC_PENDING;
		sparing_event[event_idx++] = "RESET_REQUIRED=1 EC_PENDING=1";

		break;
	case BANK_SPARNG_ENA_PCLS_UNCORRECTABLE:
		gt->mem_sparing.health_status = MEM_HEALTH_DEGRADED;
		sparing_event[event_idx++] = "DEGRADED=1 EC_FAILED=1";

		add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
		break;
	case BANK_CORRECTABLE_ERROR:
		return;
	default:
		gt->mem_sparing.health_status = MEM_HEALTH_UNKNOWN;
		sparing_event[event_idx++] = "SPARING_STATUS_UNKNOWN=1";

	}

	sparing_event[event_idx++] = NULL;

	kobject_uevent_env(&gt->i915->drm.primary->kdev->kobj, KOBJ_CHANGE,
			   sparing_event);
}

static void log_hbm_err_info(struct intel_gt *gt, u32 cause,
			     u32 reg_swf0, u32 reg_swf1)
{
	struct swf0_bitfields {
		u32 event_num:5;
		u32 tile:1;
		u32 channel:5;
		u32 pseudochannel:1;
		u32 row:18;
	};

	struct swf1_bitfields {
		u32 column:10;
		u32 bank:6;
		u32 old_state:4;
		u32 new_state:4;
	};

	bool report_state_change = false;
	struct swf0_bitfields bfswf0;
	struct swf1_bitfields bfswf1;
	const char *event;

	bfswf0.event_num = REG_FIELD_GET(EVENT_MASK, reg_swf0);
	bfswf0.tile = REG_FIELD_GET(TILE_MASK, reg_swf0);
	bfswf0.channel = REG_FIELD_GET(CHANNEL_MASK, reg_swf0);
	bfswf0.pseudochannel = REG_FIELD_GET(PSEUDOCHANNEL_MASK, reg_swf0);
	bfswf0.row = REG_FIELD_GET(ROW_MASK, reg_swf0);
	bfswf1.column = REG_FIELD_GET(COLUMN_MASK, reg_swf1);
	bfswf1.bank = REG_FIELD_GET(BANK_MASK, reg_swf1);
	bfswf1.old_state = REG_FIELD_GET(OLDSTATE_MASK, reg_swf1);
	bfswf1.new_state = REG_FIELD_GET(NEWSTATE_MASK, reg_swf1);

	switch (cause) {
	case BANK_CORRECTABLE_ERROR:
		event = "Correctable Error Received on";
		drm_err_ratelimited(&gt->i915->drm, HW_ERR
				    "[HBM ERROR]: %s HBM Tile%u, Channel%u, Pseudo Channel %u, Bank%u, Row%u, Column%u\n",
				    event, bfswf0.tile, bfswf0.channel, bfswf0.pseudochannel,
				    bfswf1.bank, bfswf0.row, bfswf1.column);

		if (bfswf1.old_state != bfswf1.new_state)
			report_state_change = true;

		break;
	case BANK_SPARNG_ERR_MITIGATION_DOWNGRADED:
		event = "PCLS Applied";
		drm_err_ratelimited(&gt->i915->drm, HW_ERR
				    "[HBM ERROR]: %s on HBM Tile%u, Channel%u, Pseudo Channel%u, Bank%u, Row%u, Column%u\n",
				    event, bfswf0.tile, bfswf0.channel, bfswf0.pseudochannel,
				    bfswf1.bank, bfswf0.row, bfswf1.column);
		break;
	case BANK_SPARNG_DIS_PCLS_EXCEEDED:
		switch (bfswf0.event_num) {
		case UC_DEMAND_ACCESS:
			event = "Uncorrectable Error on Demand Access received";
			drm_err_ratelimited(&gt->i915->drm, HW_ERR
					    "[HBM ERROR]: %s of HBM Tile%u, Channel%u, Pseudo Channel%u, Bank%u, Row%u\n",
					    event, bfswf0.tile, bfswf0.channel,
					    bfswf0.pseudochannel, bfswf1.bank, bfswf0.row);
			break;
		case PATROL_SCRUB_ERROR:
			event = "Uncorrectable Error on Patrol Scrub";
			drm_err_ratelimited(&gt->i915->drm, HW_ERR
					    "[HBM ERROR]: %s of HBM Tile%u, Channel%u, Pseudo Channel%u, Bank%u, Row%u\n",
					    event, bfswf0.tile, bfswf0.channel,
					    bfswf0.pseudochannel, bfswf1.bank, bfswf0.row);

			dev_crit(gt->i915->drm.dev,"[Hardware Info:] Its advisable to run HBM test/repair \
					    Cycle to repair any potential permanent fault in HBM.");
			break;
		case PCLS_EXCEEDED:
			event = "Exceeded PCLS Threshold";
			drm_err_ratelimited(&gt->i915->drm, HW_ERR
					    "[HBM ERROR]: %s on HBM Tile%u, Channel%u, Pseudo Channel%u\n",
					    event, bfswf0.tile, bfswf0.channel,
					    bfswf0.pseudochannel);

			dev_crit(gt->i915->drm.dev, "[Hardware Info:] Its advisable to run HBM test/repair \
					    Cycle to repair any potential permanent fault in HBM.");

			if (bfswf1.old_state != bfswf1.new_state)
				report_state_change = true;

			break;
		case PCLS_SAME_CACHELINE:
			event = "Cannot Apply PCLS, PCLS Already Applied to This Line";
			drm_err_ratelimited(&gt->i915->drm, HW_ERR
					    "[HBM ERROR]: %s on HBM Tile%u, Channel%u, Pseudo Channel%u\n",
					    event, bfswf0.tile, bfswf0.channel,
					    bfswf0.pseudochannel);

			dev_crit(gt->i915->drm.dev, "[Hardware Info:] Its advisable to run HBM test/repair \
					    Cycle to repair any potential permanent fault in HBM.");

			break;
		default:
			event = "Unknown event for Error Cause:";
			drm_err_ratelimited(&gt->i915->drm, HW_ERR "%s 0x%x\n",
					    event, cause);
			break;
		}
		break;
	case BANK_SPARNG_ENA_PCLS_UNCORRECTABLE:
		dev_crit(gt->i915->drm.dev, HW_ERR
				    "[HBM ERROR]: Unrepairable fault has been detected, replace the PVC Card\n");
		break;
	default:
		event = "Unknown Error Cause";
		drm_err_ratelimited(&gt->i915->drm, HW_ERR
				    "%s: 0x%x\n", event, cause);
		break;
	}
	if (report_state_change)
		drm_err_ratelimited(&gt->i915->drm, HW_ERR
				    "[HBM ERROR]: Old_State%u to New_State%u for HBM TILE%u\n",
				    bfswf1.old_state, bfswf1.new_state, bfswf0.tile);
}

static void
gen12_gsc_hw_error_handler(struct intel_gt *gt,
			   const enum hardware_error hw_err)
{
	void __iomem * const regs = gt->uncore->regs;
	u32 base = DG1_GSC_HECI1_BASE;
	unsigned long err_status;
	u32 errbit;

	if (!HAS_MEM_SPARING_SUPPORT(gt->i915))
		return;

	lockdep_assert_held(gt->irq_lock);

	base = PVC_GSC_HECI1_BASE;
	err_status = raw_reg_read(regs, GSC_HEC_CORR_UNCORR_ERR_STATUS(base, hw_err));
	if (unlikely(!err_status))
		return;

	switch (hw_err) {
	case HARDWARE_ERROR_CORRECTABLE:
		for_each_set_bit(errbit, &err_status, GSC_HW_ERROR_MAX_ERR_BITS) {
			u32 err_type = GSC_HW_ERROR_MAX_ERR_BITS;
			u32 sw0_reg, sw1_reg;
			const char *name;

			switch (errbit) {
			case GSC_COR_SRAM_ECC_SINGLE_BIT_ERR:
				name = "Single bit error on SRAM";
				err_type = INTEL_GSC_HW_ERROR_COR_SRAM_ECC;
				break;
			case GSC_COR_FW_REPORTED_ERR:
				gt->mem_sparing.cause |=
					raw_reg_read(regs,
						     GSC_HEC_CORR_FW_ERR_DW0(base));

				drm_err_ratelimited(&gt->i915->drm, HW_ERR
						    "GSC %s FW Error, GSC_HEC_CORR_FW_ERR_DW0::0x%08x\n",
						    (gt->mem_sparing.cause == BANK_CORRECTABLE_ERROR) ?
						    "CORRECTABLE" : "UNCORRECTABLE",
						     gt->mem_sparing.cause);

				if (unlikely(!gt->mem_sparing.cause))
					goto re_enable_interrupt;

				if (IS_PONTEVECCHIO(gt->i915)) {
					sw0_reg = raw_reg_read(regs, SWF_0);
					sw1_reg = raw_reg_read(regs, SWF_1);
					log_hbm_err_info(gt, gt->mem_sparing.cause,
							 sw0_reg, sw1_reg);

					/* These registers are written by FSP,
					 * so write 0 to clear
					 */
					raw_reg_write(regs, SWF_0, 0);
					raw_reg_write(regs, SWF_1, 0);
				}

				schedule_work(&gt->mem_sparing.mem_health_work);
				break;
			default:
				name = "Undefined";
				break;
			}

			if (err_type != GSC_HW_ERROR_MAX_ERR_BITS)
				gt->errors.gsc_hw[err_type]++;

			if (errbit != GSC_COR_FW_REPORTED_ERR)
				drm_err_ratelimited(&gt->i915->drm, HW_ERR
						    "%s GSC Correctable Error, GSC_HEC_CORR_ERR_STATUS:0x%08lx\n",
						    name, err_status);
		}
		break;
	case HARDWARE_ERROR_NONFATAL:
		for_each_set_bit(errbit, &err_status, GSC_HW_ERROR_MAX_ERR_BITS) {
			u32 err_type = GSC_HW_ERROR_MAX_ERR_BITS;
			const char *name;

			switch (errbit) {
			case GSC_UNCOR_MIA_SHUTDOWN_ERR:
				name = "MinuteIA Unexpected Shutdown";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_MIA_SHUTDOWN;
				break;
			case GSC_UNCOR_MIA_INT_ERR:
				name = "MinuteIA Internal Error";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_MIA_INT;
				break;
			case GSC_UNCOR_SRAM_ECC_ERR:
				name = "Double bit error on SRAM";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_SRAM_ECC;
				break;
			case GSC_UNCOR_WDG_TIMEOUT_ERR:
				name = "WDT 2nd Timeout";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_WDG_TIMEOUT;
				break;
			case GSC_UNCOR_ROM_PARITY_ERR:
				name = "ROM has a parity error";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_ROM_PARITY;
				break;
			case GSC_UNCOR_UCODE_PARITY_ERR:
				name = "Ucode has a parity error";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_UCODE_PARITY;
				break;
			case GSC_UNCOR_FW_REPORTED_ERR:
				name = "Errors Reported to FW and Detected by FW";
				break;
			case GSC_UNCOR_GLITCH_DET_ERR:
				name = "Glitch is detected on voltage rail";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_GLITCH_DET;
				break;
			case GSC_UNCOR_FUSE_PULL_ERR:
				name = "Fuse Pull Error";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_FUSE_PULL;
				break;
			case GSC_UNCOR_FUSE_CRC_CHECK_ERR:
				name = "Fuse CRC Check Failed on Fuse Pull";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_FUSE_CRC_CHECK;
				break;
			case GSC_UNCOR_SELFMBIST_ERR:
				name = "Self Mbist Failed";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_SELFMBIST;
				break;
			case GSC_UNCOR_AON_PARITY_ERR:
				name = "AON RF has parity error";
				err_type = INTEL_GSC_HW_ERROR_UNCOR_AON_PARITY;
				break;
			default:
				name = "Undefined";
				break;
			}

			if (err_type != GSC_HW_ERROR_MAX_ERR_BITS)
				gt->errors.gsc_hw[err_type]++;

			schedule_work(&gt->gsc_hw_error_work);
			drm_err_ratelimited(&gt->i915->drm, HW_ERR
					    "%s GSC NON_FATAL Error, GSC_HEC_UNCORR_ERR_STATUS:0x%08lx\n",
					    name, err_status);

			drm_err_ratelimited(&gt->i915->drm, "[Hardware Info:] \
					    CSC services may have stopped running. \
					    Recommend resetting PVC card to recover CSC services.");
		}
		break;
	case HARDWARE_ERROR_FATAL:
		/* GSC error not handled for Fatal Error status */
		drm_err_ratelimited(&gt->i915->drm,
				    HW_ERR "Fatal GSC Error Detected\n");
	default:
		break;
	}

re_enable_interrupt:
	raw_reg_write(regs, GSC_HEC_CORR_UNCORR_ERR_STATUS(base, hw_err), err_status);
}

static void
log_correctable_err(struct intel_gt *gt, const char *name, int i, u32 err)
{
	log_gt_hw_err(gt,
		      "%s CORRECTABLE error, ERR_VECT_GT_CORRECTABLE_%d:0x%08x\n",
		      name, i, err);
}

static void
gt_l3fabric_error_handler(struct intel_gt *gt, unsigned long l3fabric_vctr_reg)
{
	const char *l3bankout_prel3_srcs = "\tlbi_lbcf_ras_event_cmd\n"
					   "\tlbi_lbcf_ras_event_data_be\n"
					   "\tlbi_lbcf_ras_event_memrdrtn_data\n"
					   "\tlbi_lbcf_ras_event_memrdrtn_tag\n"
					   "\tSQDB buffer parity err";

	const char *lnep_error_srcs = "\tSQIDI: CMI parity errors, CMI poison, Memory"
				      " completion error code, CC->SQ parity errors\n"
				      " \tLCUnit: ccs_update_fifo_parity_error,"
				      " csc_parity_error, sq_cc_rtn_parity_error,"
				      " lcint_cs_miss_lat_fifo_rd_parity_error,"
				      " lnep_lcunit_cmd_error,"
				      " lnep_lcunit_data_parity_error, sq_0_fatal"
				      " sq_1_fatal";

	const char *bmcb_blce_srcs = "\ttag/data error";
	const u32 max_l3_nodepair = 8;

	const char *lnep_err_srcs_mdfit2t;
	const char *lnep_err_srcs_mdfit2c;
	const char *lnep_err_srcs_mert;
	u32 errbit;

	for_each_set_bit(errbit, &l3fabric_vctr_reg, max_l3_nodepair) {
		lnep_err_srcs_mdfit2t = "";
		lnep_err_srcs_mdfit2c = "";
		lnep_err_srcs_mert = "";

		/* mert LNEP error valid only on 0th L3 Nodepair */
		if (errbit == 0)
			lnep_err_srcs_mert = "\tMERT: IOSF Poison, IOSF UR/CA\n";

		/*
		 * T2T LNEP error valid only on 0th and
		 * 2nd L3 Nodepair.
		 */
		if (errbit == 0 || errbit == 2)
			lnep_err_srcs_mdfit2t = "\tMDFI T2T: MDFI poison/writefail/rsppktparity\n";

		/* T2C LNEP error valid only on 6th L3 Nodepair */
		if (errbit == 6)
			lnep_err_srcs_mdfit2c = "\tMDFI T2C: MDFI poison/writefail/rsppktparity\n";

		log_gt_hw_err(gt, "L3 Fabric Error seen on L3Nodepair[%d].\n"
				  "Possible causes of errors are:\n"
				  "lngp_rdrtn_parity_error.\n"
				  "l3bankout_prel3_fatal_error. Sources\n[%s\n]\n"
				  "rr_fatal_error.\n"
				  "lnep_fatal_error. Sources\n[%s %s %s %s\n]\n"
				  "bmcb. Sources\n[%s\n]\n"
				  "blce. Sources\n[%s\n]\n",
				  errbit, l3bankout_prel3_srcs, lnep_err_srcs_mdfit2t,
				  lnep_err_srcs_mdfit2c, lnep_err_srcs_mert, lnep_error_srcs,
				  bmcb_blce_srcs, bmcb_blce_srcs);
	}
}

static void
gen12_gt_hw_error_handler(struct intel_gt *gt,
			  const enum hardware_error hw_err)
{
	void __iomem * const regs = gt->uncore->regs;
	const char *hw_err_str = hardware_error_type_to_str(hw_err);

	unsigned long errstat;

	lockdep_assert_held(gt->irq_lock);

	if (!HAS_GT_ERROR_VECTORS(gt->i915)) {
		errstat = raw_reg_read(regs, ERR_STAT_GT_REG(hw_err));
		if (unlikely(!errstat)) {
			intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
						  "ERR_STAT_GT_REG_%s blank!\n", hw_err_str);
			return;
		}
	}

	switch (hw_err) {
	case HARDWARE_ERROR_CORRECTABLE:
		log_gt_hw_err(gt, "GT CORRECTABLE error\n");
		if (HAS_GT_ERROR_VECTORS(gt->i915)) {
			bool error = false;
			int i;

			errstat = 0;
			for (i = 0; i < ERR_STAT_GT_COR_VCTR_LEN; i++) {
				u32 err_type = ERR_STAT_GT_COR_VCTR_LEN;
				unsigned long vctr;

				vctr = raw_reg_read(regs, ERR_STAT_GT_COR_VCTR_REG(i));
				if (!vctr)
					continue;

				switch (i) {
				case ERR_STAT_GT_VCTR0:
				case ERR_STAT_GT_VCTR1:
					err_type = INTEL_GT_HW_ERROR_COR_SUBSLICE;
					gt->errors.hw[err_type] += hweight32(vctr);
					log_correctable_err(gt, "SUBSLICE", i, vctr);

					/* Avoid second read/write to error status register*/
					if (errstat)
						break;

					errstat = raw_reg_read(regs, ERR_STAT_GT_REG(hw_err));
					log_gt_hw_err(gt, "ERR_STAT_GT_CORRECTABLE:0x%08lx\n",
						      errstat);
					gen12_gt_correctable_hw_error_stats_update(gt, errstat);
					if (errstat)
						raw_reg_write(regs, ERR_STAT_GT_REG(hw_err), errstat);
					break;

				case ERR_STAT_GT_VCTR2:
				case ERR_STAT_GT_VCTR3:
					err_type = INTEL_GT_HW_ERROR_COR_L3BANK;
					gt->errors.hw[err_type] += hweight32(vctr);
					log_correctable_err(gt, "L3 BANK", i, vctr);
					break;

				default:
					intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
								  "%s CORRECTABLE error, ERR_VECT_GT_CORRECTABLE_%d:0x%08lx\n",
								  "Undefined", i, vctr);
					break;
				}
				raw_reg_write(regs, ERR_STAT_GT_COR_VCTR_REG(i), vctr);
				error = true;
			}

			if (!error)
				intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
							  "Undefined CORRECTABLE error, no vectr reg is set\n");
		} else {
			gen12_gt_correctable_hw_error_stats_update(gt, errstat);
			log_gt_hw_err(gt, "ERR_STAT_GT_CORRECTABLE:0x%08lx\n", errstat);
		}
		break;
	case HARDWARE_ERROR_NONFATAL:
		/*
		 * TODO: The GT Non Fatal Error Status Register
		 * only has reserved bitfields defined.
		 * Remove once there is something to service.
		 */
		intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
					  "Undefined GT NonFatal error\n");
		break;
	case HARDWARE_ERROR_FATAL:
		log_gt_hw_err(gt, "GT FATAL error\n");
		if (HAS_GT_ERROR_VECTORS(gt->i915)) {
			bool error = false;
			int i;

			errstat = 0;
			for (i = 0; i < ERR_STAT_GT_FATAL_VCTR_LEN; i++) {
				u32 err_type = ERR_STAT_GT_FATAL_VCTR_LEN;
				unsigned long vctr;
				const char *name;

				vctr = raw_reg_read(regs, ERR_STAT_GT_FATAL_VCTR_REG(i));
				if (!vctr)
					continue;

				/* i represents the vector register index */
				switch (i) {
				case ERR_STAT_GT_VCTR0:
				case ERR_STAT_GT_VCTR1:
					err_type = INTEL_GT_HW_ERROR_FAT_SUBSLICE;
					gt->errors.hw[err_type] += hweight32(vctr);
					name = "SUBSLICE";
					log_gt_hw_err(gt, "%s FATAL error, ERR_VECT_GT_FATAL_%d:0x%08lx\n",
						      name, i, vctr);
					/*Avoid second read/write to error status register.*/
					if (errstat)
						break;

					errstat = raw_reg_read(regs, ERR_STAT_GT_REG(hw_err));
					log_gt_hw_err(gt, "ERR_STAT_GT_FATAL:0x%08lx\n", errstat);
					gen12_gt_fatal_hw_error_stats_update(gt, errstat);
					if (errstat)
						raw_reg_write(regs, ERR_STAT_GT_REG(hw_err), errstat);
					break;

				case ERR_STAT_GT_VCTR2:
				case ERR_STAT_GT_VCTR3:
					err_type = INTEL_GT_HW_ERROR_FAT_L3BANK;
					gt->errors.hw[err_type] += hweight32(vctr);
					name = "L3 BANK";
					break;
				case ERR_STAT_GT_VCTR6:
					gt->errors.hw[INTEL_GT_HW_ERROR_FAT_TLB] += hweight16(vctr);
					name = "TLB";
					break;
				case ERR_STAT_GT_VCTR7:
					gt->errors.hw[INTEL_GT_HW_ERROR_FAT_L3_FABRIC] += hweight8(vctr);
					name = "L3 FABRIC";
					log_gt_hw_err(gt, "%s FATAL error, ERR_VECT_GT_FATAL_%d:0x%08lx.\n",
						      name, i, vctr);
					gt_l3fabric_error_handler(gt, vctr);

					break;
				default:
					name = "Undefined";
					break;
				}
				raw_reg_write(regs, ERR_STAT_GT_FATAL_VCTR_REG(i), vctr);

				if (!strcmp(name, "Undefined"))
					intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
								  "%s FATAL error, ERR_VECT_GT_FATAL_%d:0x%08lx\n",
								  name, i, vctr);
				else if (strcmp(name, "SUBSLICE") && strcmp(name, "L3 FABRIC"))
					log_gt_hw_err(gt, "%s FATAL error, ERR_VECT_GT_FATAL_%d:0x%08lx\n",
						      name, i, vctr);
				error = true;
			}
			if (!error)
				intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
							  "Undefined FATAL error, no vectr reg is set\n");
		} else {
			gen12_gt_fatal_hw_error_stats_update(gt, errstat);
			log_gt_hw_err(gt, "ERR_STAT_GT_FATAL:0x%08lx\n", errstat);
		}
		break;
	default:
		break;
	}

	if (!HAS_GT_ERROR_VECTORS(gt->i915))
		raw_reg_write(regs, ERR_STAT_GT_REG(hw_err), errstat);
}

static void log_errors(struct intel_gt *gt, const enum hardware_error hw_err,
		       bool is_error_valid, const char *err_msg)
{
	const char *hw_err_str = hardware_error_type_to_str(hw_err);

	if (!IS_PONTEVECCHIO(gt->i915))
		return;

	if (is_error_valid)
		log_gt_hw_err(gt, "%s %s error\n", err_msg, hw_err_str);
	else
		intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
					  "%s %s error\n", err_msg, hw_err_str);
}

static void
gen12_hw_error_source_handler(struct intel_gt *gt,
			      const enum hardware_error hw_err)
{
	void __iomem * const regs = gt->uncore->regs;
	const char *hw_err_str = hardware_error_type_to_str(hw_err);
	unsigned long errsrc;
	unsigned long flags;
	u32 errbit;

	spin_lock_irqsave(gt->irq_lock, flags);
	errsrc = raw_reg_read(regs, DEV_ERR_STAT_REG(hw_err));

	if (unlikely(!errsrc)) {
		intel_gt_log_driver_error(gt, INTEL_GT_DRIVER_ERROR_INTERRUPT,
					  "DEV_ERR_STAT_REG_%s blank!\n", hw_err_str);
		goto out_unlock;
	}

	if (IS_PONTEVECCHIO(gt->i915))
		log_gt_hw_err(gt, "DEV_ERR_STAT_REG_%s:0x%08lx\n", hw_err_str, errsrc);

	for_each_set_bit(errbit, &errsrc, DEV_ERR_STAT_MAX_BITS) {
		bool is_valid = false;
		const char *name;

		switch (errbit) {
		case DEV_ERR_STAT_GT_ERROR:
			gen12_gt_hw_error_handler(gt, hw_err);
			break;
		case DEV_ERR_STAT_SGGI_ERROR:
			switch (hw_err) {
			case HARDWARE_ERROR_FATAL:
				is_valid = true;
				name = "SGGI Cmd Parity";
				break;
			case HARDWARE_ERROR_NONFATAL:
				is_valid = true;
				name = "SGGI Data Parity";
				break;
			default:
				name = "Undefined";
				break;
			}
			log_errors(gt, hw_err, is_valid, name);
			break;
		case DEV_ERR_STAT_GSC_ERROR:
			if (gt->info.id == 0) {
				/* Memory health status is being tracked on root tile only */
				gen12_gsc_hw_error_handler(gt, hw_err);
			} else {
				name = "Undefined GSC";
				log_errors(gt, hw_err, is_valid, name);
			}
			break;
		case DEV_ERR_STAT_SGUNIT_ERROR:
			name = "Undefined SG UNIT";
			log_errors(gt, hw_err, is_valid, name);

			/* Remove counter for fatal error after removal from sysman side */
			gt->errors.sgunit[hw_err]++;
			break;
		case DEV_ERR_STAT_SGCI_ERROR:
			switch (hw_err) {
			case HARDWARE_ERROR_FATAL:
				is_valid = true;
				name = "SGCI Cmd Parity";
				break;
			case HARDWARE_ERROR_NONFATAL:
				is_valid = true;
				name = "SGCI Data Parity";
				break;
			default:
				name = "Undefined";
				break;
			}
			log_errors(gt, hw_err, is_valid, name);
			break;
		case DEV_ERR_STAT_SOC_ERROR:
			gen12_soc_hw_error_handler(gt, hw_err);
			break;
		case DEV_ERR_STAT_MERT_ERROR:
			switch (hw_err) {
			case HARDWARE_ERROR_FATAL:
				is_valid = true;
				name = "MERT Cmd Parity";
				break;
			case HARDWARE_ERROR_NONFATAL:
				is_valid = true;
				name = "MERT Data Parity";
				break;
			default:
				name = "Undefined";
				break;
			}
			log_errors(gt, hw_err, is_valid, name);
			break;

		default:
			name = "Undefined";
			log_errors(gt, hw_err, is_valid, name);
			break;
		}
	}

	raw_reg_write(regs, DEV_ERR_STAT_REG(hw_err), errsrc);

out_unlock:
	spin_unlock_irqrestore(gt->irq_lock, flags);
}

/*
 * gen12_iaf_irq_handler - handle accelerator fabric IRQs
 *
 * Gen12+ can have an accelerator fabric attached.  Handle the IRQs that are
 * sourced by the device supporting the fabric.
 */
static void gen12_iaf_irq_handler(struct intel_gt *gt, const u32 master_ctl)
{
	if (master_ctl & GEN12_IAF_IRQ)
		generic_handle_irq(gt->iaf_irq);
}

/*
 * GEN12+ adds three Error bits to the Master Interrupt
 * Register to support dgfx card error handling.
 * These three bits are used to convey the class of error:
 * FATAL, NONFATAL, or CORRECTABLE.
 *
 * To process an interrupt:
 *	1. Determine source of error (IP block) by reading
 *	   the Device Error Source Register (RW1C) that
 *	   corresponds to the class of error being serviced.
 *	2. For GT as the generating IP block, read and log
 *	   the GT Error Register (RW1C) that corresponds to
 *	   the class of error being serviced.
 */
static void
gen12_hw_error_irq_handler(struct intel_gt *gt, const u32 master_ctl)
{
	enum hardware_error hw_err;

	for (hw_err = 0; hw_err < HARDWARE_ERROR_MAX; hw_err++) {
		if (master_ctl & GEN12_ERROR_IRQ(hw_err)) {
			if (IS_PONTEVECCHIO(gt->i915))
				log_gt_hw_err(gt, "%s error GFX_MSTR_INTR:0x%08x\n",
					      hardware_error_type_to_str(hw_err), master_ctl);
			gen12_hw_error_source_handler(gt, hw_err);
		}
	}
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static u32
gen11_gu_misc_irq_ack(struct drm_i915_private *i915, const u32 master_ctl)
{
	void __iomem * const regs = i915->uncore.regs;
	u32 iir;

	if (!(master_ctl & GEN11_GU_MISC_IRQ))
		return 0;

	iir = raw_reg_read(regs, GEN11_GU_MISC_IIR);
	if (likely(iir))
		raw_reg_write(regs, GEN11_GU_MISC_IIR, iir);

	return iir;
}

static void
gen11_gu_misc_irq_handler(struct drm_i915_private *i915, const u32 iir)
{
	if (iir & GEN11_GU_MISC_GSE)
		intel_opregion_asle_intr(i915);
}
#else
static u32
gen11_gu_misc_irq_ack(struct drm_i915_private *i915, const u32 master_ctl) { return 0; }
static void
gen11_gu_misc_irq_handler(struct drm_i915_private *i915, const u32 iir) {}
#endif

static inline u32 gen11_master_intr_disable(void __iomem * const regs)
{
	raw_reg_write(regs, GEN11_GFX_MSTR_IRQ, 0);

	/*
	 * Now with master disabled, get a sample of level indications
	 * for this interrupt. Indications will be cleared on related acks.
	 * New indications can and will light up during processing,
	 * and will generate new interrupt after enabling master.
	 */
	return raw_reg_read(regs, GEN11_GFX_MSTR_IRQ);
}

static inline void gen11_master_intr_enable(void __iomem * const regs)
{
	raw_reg_write(regs, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void
gen11_display_irq_handler(struct drm_i915_private *i915)
{
	void __iomem * const regs = i915->uncore.regs;
	const u32 disp_ctl = raw_reg_read(regs, GEN11_DISPLAY_INT_CTL);

	disable_rpm_wakeref_asserts(&i915->runtime_pm);
	/*
	 * GEN11_DISPLAY_INT_CTL has same format as GEN8_MASTER_IRQ
	 * for the display related bits.
	 */
	raw_reg_write(regs, GEN11_DISPLAY_INT_CTL, 0x0);
	gen8_de_irq_handler(i915, disp_ctl);
	raw_reg_write(regs, GEN11_DISPLAY_INT_CTL,
		      GEN11_DISPLAY_IRQ_ENABLE);

	enable_rpm_wakeref_asserts(&i915->runtime_pm);
}
#else
static void
gen11_display_irq_handler(struct drm_i915_private *i915) {}
#endif

static irqreturn_t gen11_irq_handler(int irq, void *arg)
{
	struct drm_i915_private *i915 = arg;
	void __iomem * const regs = i915->uncore.regs;
	struct intel_gt *gt = to_gt(i915);
	u32 master_ctl;
	u32 gu_misc_iir;

	if (!intel_irqs_enabled(i915))
		return IRQ_NONE;

	master_ctl = gen11_master_intr_disable(regs);
	if (!master_ctl) {
		gen11_master_intr_enable(regs);
		return IRQ_NONE;
	}

	/* Find, queue (onto bottom-halves), then clear each source */
	gen11_gt_irq_handler(gt, master_ctl);

	/* IRQs are synced during runtime_suspend, we don't require a wakeref */
	if (master_ctl & GEN11_DISPLAY_IRQ)
		gen11_display_irq_handler(i915);

	gu_misc_iir = gen11_gu_misc_irq_ack(i915, master_ctl);

	gen11_master_intr_enable(regs);

	gen11_gu_misc_irq_handler(i915, gu_misc_iir);

	pmu_irq_stats(i915, IRQ_HANDLED);

	return IRQ_HANDLED;
}

static inline u32 dg1_master_intr_disable(void __iomem * const regs)
{
	u32 val;

	/* First disable interrupts */
	raw_reg_write(regs, DG1_MSTR_TILE_INTR, 0);

	/* Get the indication levels and ack the master unit */
	val = raw_reg_read(regs, DG1_MSTR_TILE_INTR);
	if (unlikely(!val))
		return 0;

	raw_reg_write(regs, DG1_MSTR_TILE_INTR, val);

	return val;
}

static inline void dg1_master_intr_enable(void __iomem * const regs)
{
	raw_reg_write(regs, DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
}

static irqreturn_t dg1_irq_handler(int irq, void *arg)
{
	struct drm_i915_private * const i915 = arg;
	struct intel_gt *gt = to_gt(i915);
	void __iomem * const t0_regs = gt->uncore->regs;
	u32 master_tile_ctl, master_ctl;
	u32 gu_misc_iir = 0;
	unsigned int i;

	if (!intel_irqs_enabled(i915))
		return IRQ_NONE;

	master_tile_ctl = dg1_master_intr_disable(t0_regs);
	if (!master_tile_ctl) {
		dg1_master_intr_enable(t0_regs);
		return IRQ_NONE;
	}

	for_each_gt(gt, i915, i) {
		void __iomem *const regs = gt->uncore->regs;

		if ((master_tile_ctl & DG1_MSTR_TILE(i)) == 0)
			continue;

		/*
		 * All interrupts for standalone media come in through
		 * the primary GT.  We deal with them lower in the handler
		 * stack while processing the primary GT's interrupts.
		 */
		if (drm_WARN_ON_ONCE(&i915->drm, gt->type == GT_MEDIA))
			continue;

		master_ctl = raw_reg_read(regs, GEN11_GFX_MSTR_IRQ);

		/*
		 * We might be in irq handler just when PCIe DPC is initiated and all
		 * MMIO reads will be returned with all 1's. Ignore this irq as device
		 * is inaccessible.
		 */
		if (master_ctl == REG_GENMASK(31, 0)) {
			dev_dbg(gt->i915->drm.dev, "Ignore this IRQ as device might be in DPC containment.\n");
			return IRQ_HANDLED;
		}

		raw_reg_write(regs, GEN11_GFX_MSTR_IRQ, master_ctl);

		gen11_gt_irq_handler(gt, master_ctl);
		gen12_iaf_irq_handler(gt, master_ctl);
		gen12_hw_error_irq_handler(gt, master_ctl);

		/*
		 * We'll probably only get display interrupts on tile 0, but
		 * it doesn't hurt to check the bit on each tile just to be
		 * safe.
		 */
		if (master_ctl & GEN11_DISPLAY_IRQ)
			gen11_display_irq_handler(i915);

		gu_misc_iir |= gen11_gu_misc_irq_ack(i915, master_ctl);
	}

	dg1_master_intr_enable(t0_regs);

	gen11_gu_misc_irq_handler(i915, gu_misc_iir);

	pmu_irq_stats(i915, IRQ_HANDLED);

	return IRQ_HANDLED;
}

static irqreturn_t vf_mem_irq_handler(int irq, void *arg)
{
	struct drm_i915_private * const i915 = arg;
	struct intel_gt *gt;
	unsigned int i;

	if (!intel_irqs_enabled(i915))
		return IRQ_NONE;

	for_each_gt(gt, i915, i)
		intel_iov_memirq_handler(&gt->iov);

	pmu_irq_stats(i915, IRQ_HANDLED);

	return IRQ_HANDLED;
}

static void vf_mem_irq_reset(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i)
		intel_iov_memirq_reset(&gt->iov);
}

static int vf_mem_irq_postinstall(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i)
		intel_iov_memirq_postinstall(&gt->iov);

	return 0;
}

/* Called from drm generic code, passed 'crtc' which
 * we use as a pipe index
 */
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static bool gen11_dsi_configure_te(struct intel_crtc *intel_crtc,
				   bool enable)
{
	struct drm_i915_private *dev_priv = to_i915(intel_crtc->base.dev);
	enum port port;
	u32 tmp;

	if (!(intel_crtc->mode_flags &
	    (I915_MODE_FLAG_DSI_USE_TE1 | I915_MODE_FLAG_DSI_USE_TE0)))
		return false;

	/* for dual link cases we consider TE from slave */
	if (intel_crtc->mode_flags & I915_MODE_FLAG_DSI_USE_TE1)
		port = PORT_B;
	else
		port = PORT_A;

	tmp =  intel_uncore_read(&dev_priv->uncore, DSI_INTR_MASK_REG(port));
	if (enable)
		tmp &= ~DSI_TE_EVENT;
	else
		tmp |= DSI_TE_EVENT;

	intel_uncore_write(&dev_priv->uncore, DSI_INTR_MASK_REG(port), tmp);

	tmp = intel_uncore_read(&dev_priv->uncore, DSI_INTR_IDENT_REG(port));
	intel_uncore_write(&dev_priv->uncore, DSI_INTR_IDENT_REG(port), tmp);

	return true;
}

int bdw_enable_vblank(struct drm_crtc *_crtc)
{
	struct intel_crtc *crtc = to_intel_crtc(_crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	unsigned long irqflags;

	if (gen11_dsi_configure_te(crtc, true))
		return 0;

	spin_lock_irqsave(&dev_priv->irq_lock, irqflags);
	bdw_enable_pipe_irq(dev_priv, pipe, GEN8_PIPE_VBLANK);
	spin_unlock_irqrestore(&dev_priv->irq_lock, irqflags);

	/* Even if there is no DMC, frame counter can get stuck when
	 * PSR is active as no frames are generated, so check only for PSR.
	 */
	if (HAS_PSR(dev_priv))
		drm_crtc_vblank_restore(&crtc->base);

	return 0;
}

void bdw_disable_vblank(struct drm_crtc *_crtc)
{
	struct intel_crtc *crtc = to_intel_crtc(_crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	unsigned long irqflags;

	if (gen11_dsi_configure_te(crtc, false))
		return;

	spin_lock_irqsave(&dev_priv->irq_lock, irqflags);
	bdw_disable_pipe_irq(dev_priv, pipe, GEN8_PIPE_VBLANK);
	spin_unlock_irqrestore(&dev_priv->irq_lock, irqflags);
}
#endif

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void gen11_display_irq_reset(struct drm_i915_private *dev_priv)
{
	struct intel_uncore *uncore = &dev_priv->uncore;
	u32 trans_mask = BIT(TRANSCODER_A) | BIT(TRANSCODER_B) |
		BIT(TRANSCODER_C) | BIT(TRANSCODER_D);
	enum transcoder trans;
	enum pipe pipe;

	if (!HAS_DISPLAY(dev_priv))
		return;

	intel_uncore_write(uncore, GEN11_DISPLAY_INT_CTL, 0);

	for_each_cpu_transcoder_masked(dev_priv, trans, trans_mask) {
		enum intel_display_power_domain domain;

		domain = POWER_DOMAIN_TRANSCODER(trans);
		if (!intel_display_power_is_enabled(dev_priv, domain))
			continue;

		intel_uncore_write(uncore, TRANS_PSR_IMR(trans), 0xffffffff);
		intel_uncore_write(uncore, TRANS_PSR_IIR(trans), 0xffffffff);
	}

	for_each_pipe(dev_priv, pipe)
		if (intel_display_power_is_enabled(dev_priv,
						   POWER_DOMAIN_PIPE(pipe)))
			GEN8_IRQ_RESET_NDX(uncore, DE_PIPE, pipe);

	GEN3_IRQ_RESET(uncore, GEN8_DE_PORT_);
	GEN3_IRQ_RESET(uncore, GEN8_DE_MISC_);

	if (DISPLAY_VER(dev_priv) >= 14)
		GEN3_IRQ_RESET(uncore, PICAINTERRUPT_);
	else
		GEN3_IRQ_RESET(uncore, GEN11_DE_HPD_);

	GEN3_IRQ_RESET(uncore, SDE);
}

#else
static void gen11_display_irq_reset(struct drm_i915_private *dev_priv) {}
#endif

static void gen11_irq_reset(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt = to_gt(dev_priv);
	struct intel_uncore *uncore = gt->uncore;

	gen11_master_intr_disable(dev_priv->uncore.regs);

	gen11_gt_irq_reset(gt);
	gen11_display_irq_reset(dev_priv);

	if (!IS_SRIOV_VF(dev_priv)) {
		GEN3_IRQ_RESET(uncore, GEN11_GU_MISC_);
		GEN3_IRQ_RESET(uncore, GEN8_PCU_);
	}
}

static void dg1_irq_reset(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt = to_gt(dev_priv);
	struct intel_uncore *uncore = gt->uncore;
	unsigned int i;

	dg1_master_intr_disable(dev_priv->uncore.regs);

	for_each_gt(gt, dev_priv, i) {
		gen11_gt_irq_reset(gt);

		uncore = gt->uncore;
		GEN3_IRQ_RESET(uncore, GEN11_GU_MISC_);
		GEN3_IRQ_RESET(uncore, GEN8_PCU_);
	}

	gen11_display_irq_reset(dev_priv);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
void gen8_irq_power_well_post_enable(struct drm_i915_private *dev_priv,
				     u8 pipe_mask)
{
	struct intel_uncore *uncore = &dev_priv->uncore;
	u32 extra_ier = GEN8_PIPE_VBLANK |
		gen8_de_pipe_flip_done_mask(dev_priv);
	enum pipe pipe;

	spin_lock_irq(&dev_priv->irq_lock);

	if (!intel_irqs_enabled(dev_priv)) {
		spin_unlock_irq(&dev_priv->irq_lock);
		return;
	}

	for_each_pipe_masked(dev_priv, pipe, pipe_mask)
		GEN8_IRQ_INIT_NDX(uncore, DE_PIPE, pipe,
				  dev_priv->de_irq_mask[pipe],
				  ~dev_priv->de_irq_mask[pipe] | extra_ier);

	spin_unlock_irq(&dev_priv->irq_lock);
}

void gen8_irq_power_well_pre_disable(struct drm_i915_private *dev_priv,
				     u8 pipe_mask)
{
	struct intel_uncore *uncore = &dev_priv->uncore;
	enum pipe pipe;

	spin_lock_irq(&dev_priv->irq_lock);

	if (!intel_irqs_enabled(dev_priv)) {
		spin_unlock_irq(&dev_priv->irq_lock);
		return;
	}

	for_each_pipe_masked(dev_priv, pipe, pipe_mask)
		GEN8_IRQ_RESET_NDX(uncore, DE_PIPE, pipe);

	spin_unlock_irq(&dev_priv->irq_lock);

	/* make sure we're done processing display irqs */
	intel_synchronize_irq(dev_priv);
}
#endif

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static u32 icp_ddi_hotplug_enables(struct intel_encoder *encoder)
{
	switch (encoder->hpd_pin) {
	case HPD_PORT_A:
	case HPD_PORT_B:
	case HPD_PORT_C:
	case HPD_PORT_D:
		return SHOTPLUG_CTL_DDI_HPD_ENABLE(encoder->hpd_pin);
	default:
		return 0;
	}
}

static u32 icp_tc_hotplug_enables(struct intel_encoder *encoder)
{
	switch (encoder->hpd_pin) {
	case HPD_PORT_TC1:
	case HPD_PORT_TC2:
	case HPD_PORT_TC3:
	case HPD_PORT_TC4:
	case HPD_PORT_TC5:
	case HPD_PORT_TC6:
		return ICP_TC_HPD_ENABLE(encoder->hpd_pin);
	default:
		return 0;
	}
}

static void icp_ddi_hpd_detection_setup(struct drm_i915_private *dev_priv)
{
	u32 hotplug;

	hotplug = intel_uncore_read(&dev_priv->uncore, SHOTPLUG_CTL_DDI);
	hotplug &= ~(SHOTPLUG_CTL_DDI_HPD_ENABLE(HPD_PORT_A) |
		     SHOTPLUG_CTL_DDI_HPD_ENABLE(HPD_PORT_B) |
		     SHOTPLUG_CTL_DDI_HPD_ENABLE(HPD_PORT_C) |
		     SHOTPLUG_CTL_DDI_HPD_ENABLE(HPD_PORT_D));
	hotplug |= intel_hpd_hotplug_enables(dev_priv, icp_ddi_hotplug_enables);
	intel_uncore_write(&dev_priv->uncore, SHOTPLUG_CTL_DDI, hotplug);
}

static void icp_tc_hpd_detection_setup(struct drm_i915_private *dev_priv)
{
	u32 hotplug;

	hotplug = intel_uncore_read(&dev_priv->uncore, SHOTPLUG_CTL_TC);
	hotplug &= ~(ICP_TC_HPD_ENABLE(HPD_PORT_TC1) |
		     ICP_TC_HPD_ENABLE(HPD_PORT_TC2) |
		     ICP_TC_HPD_ENABLE(HPD_PORT_TC3) |
		     ICP_TC_HPD_ENABLE(HPD_PORT_TC4) |
		     ICP_TC_HPD_ENABLE(HPD_PORT_TC5) |
		     ICP_TC_HPD_ENABLE(HPD_PORT_TC6));
	hotplug |= intel_hpd_hotplug_enables(dev_priv, icp_tc_hotplug_enables);
	intel_uncore_write(&dev_priv->uncore, SHOTPLUG_CTL_TC, hotplug);
}

static void icp_hpd_irq_setup(struct drm_i915_private *dev_priv)
{
	u32 hotplug_irqs, enabled_irqs;

	enabled_irqs = intel_hpd_enabled_irqs(dev_priv, dev_priv->hotplug.pch_hpd);
	hotplug_irqs = intel_hpd_hotplug_irqs(dev_priv, dev_priv->hotplug.pch_hpd);

	if (INTEL_PCH_TYPE(dev_priv) <= PCH_TGP)
		intel_uncore_write(&dev_priv->uncore, SHPD_FILTER_CNT, SHPD_FILTER_CNT_500_ADJ);

	ibx_display_interrupt_update(dev_priv, hotplug_irqs, enabled_irqs);

	icp_ddi_hpd_detection_setup(dev_priv);
	icp_tc_hpd_detection_setup(dev_priv);
}

static u32 gen11_hotplug_enables(struct intel_encoder *encoder)
{
	switch (encoder->hpd_pin) {
	case HPD_PORT_TC1:
	case HPD_PORT_TC2:
	case HPD_PORT_TC3:
	case HPD_PORT_TC4:
	case HPD_PORT_TC5:
	case HPD_PORT_TC6:
		return GEN11_HOTPLUG_CTL_ENABLE(encoder->hpd_pin);
	default:
		return 0;
	}
}

static void dg1_hpd_invert(struct drm_i915_private *i915)
{
	u32 val = (INVERT_DDIA_HPD |
		   INVERT_DDIB_HPD |
		   INVERT_DDIC_HPD |
		   INVERT_DDID_HPD);
	intel_uncore_rmw(&i915->uncore, SOUTH_CHICKEN1, 0, val);
}

static void dg1_hpd_irq_setup(struct drm_i915_private *dev_priv)
{
	dg1_hpd_invert(dev_priv);
	icp_hpd_irq_setup(dev_priv);
}

static void gen11_tc_hpd_detection_setup(struct drm_i915_private *dev_priv)
{
	u32 hotplug;

	hotplug = intel_uncore_read(&dev_priv->uncore, GEN11_TC_HOTPLUG_CTL);
	hotplug &= ~(GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC1) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC2) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC3) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC4) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC5) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC6));
	hotplug |= intel_hpd_hotplug_enables(dev_priv, gen11_hotplug_enables);
	intel_uncore_write(&dev_priv->uncore, GEN11_TC_HOTPLUG_CTL, hotplug);
}

static void gen11_tbt_hpd_detection_setup(struct drm_i915_private *dev_priv)
{
	u32 hotplug;

	hotplug = intel_uncore_read(&dev_priv->uncore, GEN11_TBT_HOTPLUG_CTL);
	hotplug &= ~(GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC1) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC2) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC3) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC4) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC5) |
		     GEN11_HOTPLUG_CTL_ENABLE(HPD_PORT_TC6));
	hotplug |= intel_hpd_hotplug_enables(dev_priv, gen11_hotplug_enables);
	intel_uncore_write(&dev_priv->uncore, GEN11_TBT_HOTPLUG_CTL, hotplug);
}

static void gen11_hpd_irq_setup(struct drm_i915_private *dev_priv)
{
	u32 hotplug_irqs, enabled_irqs;
	u32 val;

	enabled_irqs = intel_hpd_enabled_irqs(dev_priv, dev_priv->hotplug.hpd);
	hotplug_irqs = intel_hpd_hotplug_irqs(dev_priv, dev_priv->hotplug.hpd);

	val = intel_uncore_read(&dev_priv->uncore, GEN11_DE_HPD_IMR);
	val &= ~hotplug_irqs;
	val |= ~enabled_irqs & hotplug_irqs;
	intel_uncore_write(&dev_priv->uncore, GEN11_DE_HPD_IMR, val);
	intel_uncore_posting_read(&dev_priv->uncore, GEN11_DE_HPD_IMR);

	gen11_tc_hpd_detection_setup(dev_priv);
	gen11_tbt_hpd_detection_setup(dev_priv);

	icp_hpd_irq_setup(dev_priv);
}

static u32 mtp_ddi_hotplug_enables(struct intel_encoder *encoder)
{
	switch (encoder->hpd_pin) {
	case HPD_PORT_A:
	case HPD_PORT_B:
		return SHOTPLUG_CTL_DDI_HPD_ENABLE(encoder->hpd_pin);
	default:
		return 0;
	}
}

static u32 mtp_tc_hotplug_enables(struct intel_encoder *encoder)
{
	switch (encoder->hpd_pin) {
	case HPD_PORT_TC1:
	case HPD_PORT_TC2:
	case HPD_PORT_TC3:
	case HPD_PORT_TC4:
		return ICP_TC_HPD_ENABLE(encoder->hpd_pin);
	default:
		return 0;
	}
}

static void mtp_ddi_hpd_detection_setup(struct drm_i915_private *i915)
{
	intel_de_rmw(i915, SHOTPLUG_CTL_DDI,
		     (SHOTPLUG_CTL_DDI_HPD_ENABLE(HPD_PORT_A) |
		      SHOTPLUG_CTL_DDI_HPD_ENABLE(HPD_PORT_B)),
		     intel_hpd_hotplug_enables(i915, mtp_ddi_hotplug_enables));
}

static void mtp_tc_hpd_detection_setup(struct drm_i915_private *i915)
{
	intel_de_rmw(i915, SHOTPLUG_CTL_TC,
		     (ICP_TC_HPD_ENABLE(HPD_PORT_TC1) |
		      ICP_TC_HPD_ENABLE(HPD_PORT_TC2) |
		      ICP_TC_HPD_ENABLE(HPD_PORT_TC3) |
		      ICP_TC_HPD_ENABLE(HPD_PORT_TC4)),
		     intel_hpd_hotplug_enables(i915, mtp_tc_hotplug_enables));
}

static void mtp_hpd_invert(struct drm_i915_private *i915)
{
	u32 val = (INVERT_DDIA_HPD |
		   INVERT_DDIB_HPD |
		   INVERT_DDIC_HPD |
		   INVERT_TC1_HPD |
		   INVERT_TC2_HPD |
		   INVERT_TC3_HPD |
		   INVERT_TC4_HPD |
		   INVERT_DDID_HPD_MTP |
		   INVERT_DDIE_HPD);
	intel_de_rmw(i915, SOUTH_CHICKEN1, 0, val);
}

static void mtp_hpd_irq_setup(struct drm_i915_private *i915)
{
	u32 hotplug_irqs, enabled_irqs;

	enabled_irqs = intel_hpd_enabled_irqs(i915, i915->hotplug.pch_hpd);
	hotplug_irqs = intel_hpd_hotplug_irqs(i915, i915->hotplug.pch_hpd);

	intel_de_write(i915, SHPD_FILTER_CNT, SHPD_FILTER_CNT_500_ADJ);

	mtp_hpd_invert(i915);
	ibx_display_interrupt_update(i915, hotplug_irqs, enabled_irqs);

	mtp_ddi_hpd_detection_setup(i915);
	mtp_tc_hpd_detection_setup(i915);
}

static void xelpdp_pica_hpd_detection_setup(struct drm_i915_private *i915)
{
	struct intel_encoder *encoder;
	enum hpd_pin pin;
	u32 available_pins = 0;

	BUILD_BUG_ON(BITS_PER_TYPE(available_pins) < HPD_NUM_PINS);

	for_each_intel_encoder(&i915->drm, encoder)
		available_pins |= BIT(encoder->hpd_pin);

	for (pin = HPD_PORT_TC1; pin <= HPD_PORT_TC4; pin++) {
		u32 mask = XELPDP_TBT_HOTPLUG_ENABLE |
			   XELPDP_DP_ALT_HOTPLUG_ENABLE;

		intel_de_rmw(i915, XELPDP_PORT_HOTPLUG_CTL(pin),
			     mask,
			     available_pins & BIT(pin) ?  mask : 0);
	}
}

static void xelpdp_hpd_irq_setup(struct drm_i915_private *i915)
{
	u32 hotplug_irqs, enabled_irqs;

	enabled_irqs = intel_hpd_enabled_irqs(i915, i915->hotplug.hpd);
	hotplug_irqs = intel_hpd_hotplug_irqs(i915, i915->hotplug.hpd);

	intel_de_rmw(i915, PICAINTERRUPT_IMR, hotplug_irqs,
		     ~enabled_irqs & hotplug_irqs);
	intel_uncore_posting_read(&i915->uncore, PICAINTERRUPT_IMR);

	xelpdp_pica_hpd_detection_setup(i915);

	if (INTEL_PCH_TYPE(i915) >= PCH_MTP)
		mtp_hpd_irq_setup(i915);
}

#endif

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void gen8_de_irq_postinstall(struct drm_i915_private *dev_priv)
{
	struct intel_uncore *uncore = &dev_priv->uncore;

	u32 de_pipe_masked = gen8_de_pipe_fault_mask(dev_priv) |
		GEN8_PIPE_CDCLK_CRC_DONE;
	u32 de_pipe_enables;
	u32 de_port_masked = gen8_de_port_aux_mask(dev_priv);
	u32 de_port_enables;
	u32 de_misc_masked = GEN8_DE_EDP_PSR;
	u32 trans_mask = BIT(TRANSCODER_A) | BIT(TRANSCODER_B) |
		BIT(TRANSCODER_C) | BIT(TRANSCODER_D);
	enum transcoder trans;
	enum pipe pipe;

	if (!HAS_DISPLAY(dev_priv))
		return;

	if (DISPLAY_VER(dev_priv) >= 14)
		de_misc_masked |= XELPDP_PMDEMAND_RSPTOUT_ERR |
				  XELPDP_PMDEMAND_RSP;
	else {
		enum port port;

		if (intel_bios_is_dsi_present(dev_priv, &port))
			de_port_masked |= DSI0_TE | DSI1_TE;
	}

	de_pipe_enables = de_pipe_masked |
		GEN8_PIPE_VBLANK |
		gen8_de_pipe_flip_done_mask(dev_priv);

	de_port_enables = de_port_masked;
	for_each_cpu_transcoder_masked(dev_priv, trans, trans_mask) {
		enum intel_display_power_domain domain;

		domain = POWER_DOMAIN_TRANSCODER(trans);
		if (!intel_display_power_is_enabled(dev_priv, domain))
			continue;

		gen3_assert_iir_is_zero(uncore, TRANS_PSR_IIR(trans));
	}

	for_each_pipe(dev_priv, pipe) {
		dev_priv->de_irq_mask[pipe] = ~de_pipe_masked;

		if (intel_display_power_is_enabled(dev_priv,
				POWER_DOMAIN_PIPE(pipe)))
			GEN8_IRQ_INIT_NDX(uncore, DE_PIPE, pipe,
					  dev_priv->de_irq_mask[pipe],
					  de_pipe_enables);
	}

	GEN3_IRQ_INIT(uncore, GEN8_DE_PORT_, ~de_port_masked, de_port_enables);
	GEN3_IRQ_INIT(uncore, GEN8_DE_MISC_, ~de_misc_masked, de_misc_masked);

	if (IS_DISPLAY_VER(dev_priv, 11, 13)) {
		u32 de_hpd_masked = 0;
		u32 de_hpd_enables = GEN11_DE_TC_HOTPLUG_MASK |
				     GEN11_DE_TBT_HOTPLUG_MASK;

		GEN3_IRQ_INIT(uncore, GEN11_DE_HPD_, ~de_hpd_masked,
			      de_hpd_enables);
	}
}

static void mtp_irq_postinstall(struct drm_i915_private *i915)
{
	struct intel_uncore *uncore = &i915->uncore;
	u32 sde_mask = SDE_GMBUS_ICP | SDE_PICAINTERRUPT;
	u32 de_hpd_mask = XELPDP_AUX_TC_MASK;
	u32 de_hpd_enables = de_hpd_mask | XELPDP_DP_ALT_HOTPLUG_MASK |
			     XELPDP_TBT_HOTPLUG_MASK;

	GEN3_IRQ_INIT(uncore, PICAINTERRUPT_, ~de_hpd_mask,
		      de_hpd_enables);

	GEN3_IRQ_INIT(uncore, SDE, ~sde_mask, 0xffffffff);
}

static void icp_irq_postinstall(struct drm_i915_private *dev_priv)
{
	struct intel_uncore *uncore = &dev_priv->uncore;
	u32 mask = SDE_GMBUS_ICP;

	GEN3_IRQ_INIT(uncore, SDE, ~mask, 0xffffffff);
}
#else
static void gen8_de_irq_postinstall(struct drm_i915_private *dev_priv) {}
static void mtp_irq_postinstall(struct drm_i915_private *i915) {}
static void icp_irq_postinstall(struct drm_i915_private *dev_priv) {}
#endif

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void gen11_de_irq_postinstall(struct drm_i915_private *dev_priv)
{
	if (!HAS_DISPLAY(dev_priv))
		return;

	gen8_de_irq_postinstall(dev_priv);

	intel_uncore_write(&dev_priv->uncore, GEN11_DISPLAY_INT_CTL,
			   GEN11_DISPLAY_IRQ_ENABLE);
}
#else
static void gen11_de_irq_postinstall(struct drm_i915_private *dev_priv) {}
#endif

static void gen11_irq_postinstall(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt = to_gt(dev_priv);
	struct intel_uncore *uncore = gt->uncore;
	u32 gu_misc_masked = GEN11_GU_MISC_GSE;

	icp_irq_postinstall(dev_priv);

	gen11_gt_irq_postinstall(gt);
	gen11_de_irq_postinstall(dev_priv);

	if (!IS_SRIOV_VF(dev_priv))
		GEN3_IRQ_INIT(uncore, GEN11_GU_MISC_, ~gu_misc_masked, gu_misc_masked);

	gen11_master_intr_enable(uncore->regs);
	intel_uncore_posting_read(&dev_priv->uncore, GEN11_GFX_MSTR_IRQ);
}

static void clear_all_soc_errors(struct intel_gt *gt)
{
	void __iomem * const regs = gt->uncore->regs;
	enum hardware_error hw_err;
	u32 base = SOC_PVC_BASE;
	u32 slave_base = SOC_PVC_SLAVE_BASE;
	unsigned int i;


	hw_err = HARDWARE_ERROR_CORRECTABLE;
	while (hw_err < HARDWARE_ERROR_MAX) {
		for (i = 0; i < INTEL_GT_SOC_NUM_IEH; i++)
			raw_reg_write(regs, SOC_GSYSEVTCTL_REG(base, slave_base, i),
				      ~REG_BIT(hw_err));

		raw_reg_write(regs, SOC_GLOBAL_ERR_STAT_MASTER_REG(base, hw_err),
			      REG_GENMASK(31, 0));
		raw_reg_write(regs, SOC_LOCAL_ERR_STAT_MASTER_REG(base, hw_err),
			      REG_GENMASK(31, 0));
		raw_reg_write(regs, SOC_GLOBAL_ERR_STAT_SLAVE_REG(slave_base, hw_err),
			      REG_GENMASK(31, 0));
		raw_reg_write(regs, SOC_LOCAL_ERR_STAT_SLAVE_REG(slave_base, hw_err),
			      REG_GENMASK(31, 0));
		hw_err++;
	}

	for (i = 0; i < INTEL_GT_SOC_NUM_IEH; i++)
		raw_reg_write(regs, SOC_GSYSEVTCTL_REG(base, slave_base, i),
			      (HARDWARE_ERROR_MAX << 1) + 1);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
static void display_dg1_irq_postinstall(struct drm_i915_private *dev_priv)
{
		intel_uncore_write(&dev_priv->uncore, GEN11_DISPLAY_INT_CTL,
				   GEN11_DISPLAY_IRQ_ENABLE);
}
#else
static void display_dg1_irq_postinstall(struct drm_i915_private *dev_priv) {}
#endif

static void dg1_irq_postinstall(struct drm_i915_private *dev_priv)
{
	u32 gu_misc_masked = GEN11_GU_MISC_GSE;
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, dev_priv, i) {
		/*
		 * All Soc error correctable, non fatal and fatal are reported
		 * to IEH registers only. To be safe we are clearing these errors as well.
		 */
		if (IS_PONTEVECCHIO(gt->i915))
			clear_all_soc_errors(gt);

		gen11_gt_irq_postinstall(gt);

		GEN3_IRQ_INIT(gt->uncore, GEN11_GU_MISC_, ~gu_misc_masked,
			      gu_misc_masked);
		intel_uncore_write(gt->uncore, GEN11_GFX_MSTR_IRQ, REG_GENMASK(30, 0));
	}

	if (HAS_DISPLAY(dev_priv)) {
		if (DISPLAY_VER(dev_priv) >= 14)
			mtp_irq_postinstall(dev_priv);
		else
			icp_irq_postinstall(dev_priv);

		gen8_de_irq_postinstall(dev_priv);
		display_dg1_irq_postinstall(dev_priv);
	}

	intel_uncore_write(&dev_priv->uncore, DG1_MSTR_TILE_INTR, REG_GENMASK(3, 0));
	dg1_master_intr_enable(to_gt(dev_priv)->uncore->regs);
	intel_uncore_posting_read(to_gt(dev_priv)->uncore, DG1_MSTR_TILE_INTR);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
struct intel_hotplug_funcs {
	void (*hpd_irq_setup)(struct drm_i915_private *i915);
};

#define HPD_FUNCS(platform)					 \
static const struct intel_hotplug_funcs platform##_hpd_funcs = { \
	.hpd_irq_setup = platform##_hpd_irq_setup,		 \
}

HPD_FUNCS(xelpdp);
HPD_FUNCS(dg1);
HPD_FUNCS(gen11);
HPD_FUNCS(icp);
#undef HPD_FUNCS

void intel_hpd_irq_setup(struct drm_i915_private *i915)
{
	if (i915->display_irqs_enabled && i915->hotplug_funcs)
		i915->hotplug_funcs->hpd_irq_setup(i915);
}

static void display_intel_irq_init(struct drm_i915_private *dev_priv)
{

	struct drm_device *dev = &dev_priv->drm;

	dev->vblank_disable_immediate = true;

	/* Most platforms treat the display irq block as an always-on
	 * power domain. vlv/chv can disable it at runtime and need
	 * special care to avoid writing any of the display block registers
	 * outside of the power domain. We defer setting up the display irqs
	 * in this case to the runtime pm.
	 */
	dev_priv->display_irqs_enabled = true;

	dev_priv->hotplug.hpd_storm_threshold = HPD_STORM_DEFAULT_THRESHOLD;
	/* If we have MST support, we want to avoid doing short HPD IRQ storm
	 * detection, as short HPD storms will occur as a natural part of
	 * sideband messaging with MST.
	 * On older platforms however, IRQ storms can occur with both long and
	 * short pulses, as seen on some G4x systems.
	 */
	dev_priv->hotplug.hpd_short_storm_enabled = false;

	if (HAS_PCH_DG2(dev_priv))
		dev_priv->hotplug_funcs = &icp_hpd_funcs;
	else if (HAS_PCH_DG1(dev_priv))
		dev_priv->hotplug_funcs = &dg1_hpd_funcs;
	else if (DISPLAY_VER(dev_priv) >= 14)
		dev_priv->hotplug_funcs = &xelpdp_hpd_funcs;
	else
		dev_priv->hotplug_funcs = &gen11_hpd_funcs;

}
#else
static void display_intel_irq_init(struct drm_i915_private *dev_priv) {}
#endif

/**
 * intel_irq_init - initializes irq support
 * @dev_priv: i915 device instance
 *
 * This function initializes all the irq support including work items, timers
 * and all the vtables. It does not setup the interrupt itself though.
 */
void intel_irq_init(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt = to_root_gt(dev_priv);
	int i;

	INIT_WORK(&dev_priv->l3_parity.error_work, ivb_parity_work);
	for (i = 0; i < MAX_L3_SLICES; ++i)
		dev_priv->l3_parity.remap_info[i] = NULL;

	if (HAS_MEM_SPARING_SUPPORT(dev_priv))
		INIT_WORK(&gt->gsc_hw_error_work, gen12_gsc_hw_error_work);

	if (HAS_MEM_SPARING_SUPPORT(dev_priv)) {
		INIT_WORK(&gt->mem_sparing.mem_health_work,
			  gen12_mem_health_work);
	}

	if (!HAS_DISPLAY(dev_priv))
		return;
	intel_hpd_init_pins(dev_priv);

	intel_hpd_init_work(dev_priv);

	display_intel_irq_init(dev_priv);
}

/**
 * intel_irq_fini - deinitializes IRQ support
 * @i915: i915 device instance
 *
 * This function deinitializes all the IRQ support.
 */
void intel_irq_fini(struct drm_i915_private *i915)
{
	int i;

	for (i = 0; i < MAX_L3_SLICES; ++i)
		kfree(i915->l3_parity.remap_info[i]);
}

static irq_handler_t intel_irq_handler(struct drm_i915_private *dev_priv)
{
	if (HAS_MEMORY_IRQ_STATUS(dev_priv))
		return vf_mem_irq_handler;
	else if (GRAPHICS_VER_FULL(dev_priv) >= IP_VER(12, 10))
		return dg1_irq_handler;
	else
		return gen11_irq_handler;
}

static void intel_irq_reset(struct drm_i915_private *dev_priv)
{
	if (dev_priv->quiesce_gpu)
		return;

	if (HAS_MEMORY_IRQ_STATUS(dev_priv))
		vf_mem_irq_reset(dev_priv);
	else if (GRAPHICS_VER_FULL(dev_priv) >= IP_VER(12, 10))
		dg1_irq_reset(dev_priv);
	else
		gen11_irq_reset(dev_priv);
}

static void intel_irq_postinstall(struct drm_i915_private *dev_priv)
{
	if (HAS_MEMORY_IRQ_STATUS(dev_priv))
		vf_mem_irq_postinstall(dev_priv);
	else if (GRAPHICS_VER_FULL(dev_priv) >= IP_VER(12, 10))
		dg1_irq_postinstall(dev_priv);
	else
		gen11_irq_postinstall(dev_priv);
}

/**
 * process_hw_errors - checks for the occurrence of HW errors
 * @dev_priv: i915 device instance
 *
 * This checks for the HW Errors including FATAL error that might
 * have occurred in the previous boot of the driver which will
 * initiate PCIe FLR reset of the device and cause the
 * driver to reload.
 */
static void process_hw_errors(struct drm_i915_private *dev_priv)
{
	struct intel_gt *gt = to_gt(dev_priv);
	void __iomem * const t0_regs = gt->uncore->regs;
	u32 dev_pcieerr_status, master_ctl;
	int i;

	dev_pcieerr_status = raw_reg_read(t0_regs, DEV_PCIEERR_STATUS);

	for_each_gt(gt, dev_priv, i) {
		void __iomem *const regs = gt->uncore->regs;

		if (dev_pcieerr_status & DEV_PCIEERR_IS_FATAL(i)) {
			if (IS_PONTEVECCHIO(gt->i915))
				log_gt_hw_err(gt, "DEV_PCIEERR_STATUS_FATAL:0x%08x\n",
					      dev_pcieerr_status);
			gen12_hw_error_source_handler(gt, HARDWARE_ERROR_FATAL);
		}

		master_ctl = raw_reg_read(regs, GEN11_GFX_MSTR_IRQ);
		raw_reg_write(regs, GEN11_GFX_MSTR_IRQ, master_ctl);
		gen12_hw_error_irq_handler(gt, master_ctl);
	}
	if (dev_pcieerr_status)
		raw_reg_write(t0_regs, DEV_PCIEERR_STATUS, dev_pcieerr_status);
}

/**
 * intel_irq_install - enables the hardware interrupt
 * @dev_priv: i915 device instance
 *
 * This function enables the hardware interrupt handling, but leaves the hotplug
 * handling still disabled. It is called after intel_irq_init().
 *
 * In the driver load and resume code we need working interrupts in a few places
 * but don't want to deal with the hassle of concurrent probe and hotplug
 * workers. Hence the split into this two-stage approach.
 */
int intel_irq_install(struct drm_i915_private *dev_priv)
{
	int irq = to_pci_dev(dev_priv->drm.dev)->irq;
	int ret;

	if (IS_DGFX(dev_priv))
		process_hw_errors(dev_priv);

	/*
	 * We enable some interrupt sources in our postinstall hooks, so mark
	 * interrupts as enabled _before_ actually enabling them to avoid
	 * special cases in our ordering checks.
	 */
	dev_priv->runtime_pm.irqs_enabled = true;
#ifdef BPM_DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED
	dev_priv->irq_enabled = true;
#else
	dev_priv->drm.irq_enabled = true;
#endif 
	intel_irq_reset(dev_priv);

	ret = request_irq(irq, intel_irq_handler(dev_priv),
			  IRQF_SHARED, DRIVER_NAME, dev_priv);
	if (ret < 0) {
#ifdef BPM_DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED
		dev_priv->irq_enabled = false;
#else
		dev_priv->drm.irq_enabled = false;
#endif 
		return ret;
	}

	intel_irq_postinstall(dev_priv);

	return ret;
}

/**
 * intel_irq_uninstall - finilizes all irq handling
 * @dev_priv: i915 device instance
 *
 * This stops interrupt and hotplug handling and unregisters and frees all
 * resources acquired in the init functions.
 */
void intel_irq_uninstall(struct drm_i915_private *dev_priv)
{
	int irq = to_pci_dev(dev_priv->drm.dev)->irq;

	/*
	 * FIXME we can get called twice during driver probe
	 * error handling as well as during driver remove due to
	 * intel_modeset_driver_remove() calling us out of sequence.
	 * Would be nice if it didn't do that...
	 */
#ifdef BPM_DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED
	if (!dev_priv->irq_enabled)
#else
	if (!dev_priv->drm.irq_enabled)
#endif 
		return;

#ifdef BPM_DRM_DEVICE_IRQ_ENABLED_INSIDE_LEGACY_ADDED
	dev_priv->irq_enabled = false;
#else
	dev_priv->drm.irq_enabled = false;
#endif 

	intel_irq_reset(dev_priv);

	free_irq(irq, dev_priv);

	intel_hpd_cancel_work(dev_priv);

	dev_priv->runtime_pm.irqs_enabled = false;
}

/**
 * intel_runtime_pm_disable_interrupts - runtime interrupt disabling
 * @dev_priv: i915 device instance
 *
 * This function is used to disable interrupts at runtime, both in the runtime
 * pm and the system suspend/resume code.
 */
void intel_runtime_pm_disable_interrupts(struct drm_i915_private *dev_priv)
{
	if (dev_priv->quiesce_gpu)
		return;

	intel_irq_reset(dev_priv);
	dev_priv->runtime_pm.irqs_enabled = false;
	intel_synchronize_irq(dev_priv);
}

/**
 * intel_runtime_pm_enable_interrupts - runtime interrupt enabling
 * @dev_priv: i915 device instance
 *
 * This function is used to enable interrupts at runtime, both in the runtime
 * pm and the system suspend/resume code.
 */
void intel_runtime_pm_enable_interrupts(struct drm_i915_private *dev_priv)
{
	dev_priv->runtime_pm.irqs_enabled = true;
	intel_irq_reset(dev_priv);
	intel_irq_postinstall(dev_priv);
}

bool intel_irqs_enabled(struct drm_i915_private *dev_priv)
{
	return dev_priv->runtime_pm.irqs_enabled;
}

void intel_synchronize_irq(struct drm_i915_private *i915)
{
	synchronize_irq(to_pci_dev(i915->drm.dev)->irq);
}

void intel_synchronize_hardirq(struct drm_i915_private *i915)
{
	synchronize_hardirq(to_pci_dev(i915->drm.dev)->irq);
}
