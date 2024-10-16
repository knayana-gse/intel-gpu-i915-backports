// SPDX-License-Identifier: MIT
/*
 * Copyright © 2016 Intel Corporation
 */

#include <linux/string_helpers.h>

#include <drm/drm_print.h>

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_internal.h"
#include "gt/intel_gt_regs.h"

#include "i915_drv.h"
#include "intel_breadcrumbs.h"
#include "intel_context.h"
#include "intel_engine.h"
#include "intel_engine_pm.h"
#include "intel_engine_regs.h"
#include "intel_engine_user.h"
#include "intel_execlists_submission.h"
#include "intel_gpu_commands.h"
#include "intel_gt.h"
#include "intel_gt_mcr.h"
#include "intel_gt_pm.h"
#include "intel_gt_print.h"
#include "intel_gt_requests.h"
#include "intel_lrc.h"
#include "intel_lrc_reg.h"
#include "intel_reset.h"
#include "intel_ring.h"
#include "uc/intel_guc_submission.h"

/* Haswell does have the CXT_SIZE register however it does not appear to be
 * valid. Now, docs explain in dwords what is in the context object. The full
 * size is 70720 bytes, however, the power context and execlist context will
 * never be saved (power context is stored elsewhere, and execlists don't work
 * on HSW) - so the final size, including the extra state required for the
 * Resource Streamer, is 66944 bytes, which rounds to 17 pages.
 */
#define HSW_CXT_TOTAL_SIZE		(17 * PAGE_SIZE)

#define GEN8_LR_CONTEXT_RENDER_SIZE	(20 * PAGE_SIZE)
#define GEN9_LR_CONTEXT_RENDER_SIZE	(22 * PAGE_SIZE)
#define GEN11_LR_CONTEXT_RENDER_SIZE	(14 * PAGE_SIZE)

#define GEN8_LR_CONTEXT_OTHER_SIZE	( 2 * PAGE_SIZE)

static void
intel_engine_reset_failed_uevent_work(struct work_struct *work);

#define MAX_MMIO_BASES 3
struct engine_info {
	u8 class;
	u8 instance;
	u8 irq_offset;
	/* mmio bases table *must* be sorted in reverse graphics_ver order */
	struct engine_mmio_base {
		u32 graphics_ver : 8;
		u32 base : 24;
	} mmio_bases[MAX_MMIO_BASES];
};

static const struct engine_info intel_engines[] = {
	[RCS0] = {
		.class = RENDER_CLASS,
		.instance = 0,
		.irq_offset = GEN11_RCS0,
		.mmio_bases = {
			{ .graphics_ver = 1, .base = RENDER_RING_BASE }
		},
	},
	[BCS0] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 0,
		.irq_offset = GEN11_BCS,
		.mmio_bases = {
			{ .graphics_ver = 6, .base = BLT_RING_BASE }
		},
	},
	[BCS1] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 1,
		.irq_offset = XEHPC_BCS1,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS1_RING_BASE }
		},
	},
	[BCS2] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 2,
		.irq_offset = XEHPC_BCS2,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS2_RING_BASE }
		},
	},
	[BCS3] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 3,
		.irq_offset = XEHPC_BCS3,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS3_RING_BASE }
		},
	},
	[BCS4] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 4,
		.irq_offset = XEHPC_BCS4,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS4_RING_BASE }
		},
	},
	[BCS5] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 5,
		.irq_offset = XEHPC_BCS5,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS5_RING_BASE }
		},
	},
	[BCS6] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 6,
		.irq_offset = XEHPC_BCS6,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS6_RING_BASE }
		},
	},
	[BCS7] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 7,
		.irq_offset = XEHPC_BCS7,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS7_RING_BASE }
		},
	},
	[BCS8] = {
		.class = COPY_ENGINE_CLASS,
		.instance = 8,
		.irq_offset = XEHPC_BCS8,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHPC_BCS8_RING_BASE }
		},
	},
	[VCS0] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 0,
		.irq_offset = 32 + GEN11_VCS(0),
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD_RING_BASE },
			{ .graphics_ver = 6, .base = GEN6_BSD_RING_BASE },
			{ .graphics_ver = 4, .base = BSD_RING_BASE }
		},
	},
	[VCS1] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 1,
		.irq_offset = 32 + GEN11_VCS(1),
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD2_RING_BASE },
			{ .graphics_ver = 8, .base = GEN8_BSD2_RING_BASE }
		},
	},
	[VCS2] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 2,
		.irq_offset = 32 + GEN11_VCS(2),
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD3_RING_BASE }
		},
	},
	[VCS3] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 3,
		.irq_offset = 32 + GEN11_VCS(3),
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD4_RING_BASE }
		},
	},
	[VCS4] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 4,
		.irq_offset = 32 + GEN11_VCS(4),
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD5_RING_BASE }
		},
	},
	[VCS5] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 5,
		.irq_offset = 32 + GEN11_VCS(5),
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD6_RING_BASE }
		},
	},
	[VCS6] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 6,
		.irq_offset = 32 + GEN11_VCS(6),
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD7_RING_BASE }
		},
	},
	[VCS7] = {
		.class = VIDEO_DECODE_CLASS,
		.instance = 7,
		.irq_offset = 32 + GEN11_VCS(7),
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD8_RING_BASE }
		},
	},
	[VECS0] = {
		.class = VIDEO_ENHANCEMENT_CLASS,
		.instance = 0,
		.irq_offset = 32 + GEN11_VECS(0),
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_VEBOX_RING_BASE },
			{ .graphics_ver = 7, .base = VEBOX_RING_BASE }
		},
	},
	[VECS1] = {
		.class = VIDEO_ENHANCEMENT_CLASS,
		.instance = 1,
		.irq_offset = 32 + GEN11_VECS(1),
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_VEBOX2_RING_BASE }
		},
	},
	[VECS2] = {
		.class = VIDEO_ENHANCEMENT_CLASS,
		.instance = 2,
		.irq_offset = 32 + GEN11_VECS(2),
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_VEBOX3_RING_BASE }
		},
	},
	[VECS3] = {
		.class = VIDEO_ENHANCEMENT_CLASS,
		.instance = 3,
		.irq_offset = 32 + GEN11_VECS(3),
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_VEBOX4_RING_BASE }
		},
	},
	[CCS0] = {
		.class = COMPUTE_CLASS,
		.instance = 0,
		.irq_offset = GEN12_CCS0,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = GEN12_COMPUTE0_RING_BASE }
		}
	},
	[CCS1] = {
		.class = COMPUTE_CLASS,
		.instance = 1,
		.irq_offset = GEN12_CCS1,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = GEN12_COMPUTE1_RING_BASE }
		}
	},
	[CCS2] = {
		.class = COMPUTE_CLASS,
		.instance = 2,
		.irq_offset = GEN12_CCS2,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = GEN12_COMPUTE2_RING_BASE }
		}
	},
	[CCS3] = {
		.class = COMPUTE_CLASS,
		.instance = 3,
		.irq_offset = GEN12_CCS3,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = GEN12_COMPUTE3_RING_BASE }
		}
	},
	[GSC0] = {
		.class = OTHER_CLASS,
		.instance = OTHER_GSC_INSTANCE,
		.irq_offset = GEN11_CSME,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = MTL_GSC_RING_BASE }
		}
	},
};

/**
 * intel_engine_context_size() - return the size of the context for an engine
 * @gt: the gt
 * @class: engine class
 *
 * Each engine class may require a different amount of space for a context
 * image.
 *
 * Return: size (in bytes) of an engine class specific context image
 *
 * Note: this size includes the HWSP, which is part of the context image
 * in LRC mode, but does not include the "shared data page" used with
 * GuC submission. The caller should account for this if using the GuC.
 */
u32 intel_engine_context_size(struct intel_gt *gt, u8 class)
{
	BUILD_BUG_ON(I915_GTT_PAGE_SIZE != PAGE_SIZE);

	switch (class) {
	case COMPUTE_CLASS:
	case RENDER_CLASS:
		return GEN11_LR_CONTEXT_RENDER_SIZE;
	default:
		MISSING_CASE(class);
		fallthrough;
	case VIDEO_DECODE_CLASS:
	case VIDEO_ENHANCEMENT_CLASS:
	case COPY_ENGINE_CLASS:
	case OTHER_CLASS:
		return GEN8_LR_CONTEXT_OTHER_SIZE;
	}
}

static u32 __engine_mmio_base(struct drm_i915_private *i915,
			      const struct engine_mmio_base *bases)
{
	int i;

	for (i = 0; i < MAX_MMIO_BASES; i++)
		if (GRAPHICS_VER(i915) >= bases[i].graphics_ver)
			break;

	GEM_BUG_ON(i == MAX_MMIO_BASES);
	GEM_BUG_ON(!bases[i].base);

	return bases[i].base;
}

static void __sprint_engine_name(struct intel_engine_cs *engine)
{
	/*
	 * Before we know what the uABI name for this engine will be,
	 * we still would like to keep track of this engine in the debug logs.
	 * We throw in a ' here as a reminder that this isn't its final name.
	 */
	GEM_WARN_ON(snprintf(engine->name, sizeof(engine->name), "%s'%u.%u",
			     intel_engine_class_repr(engine->class),
			     engine->instance,
			     engine->gt->info.id) >= sizeof(engine->name));
}

void intel_engine_set_hwsp_writemask(struct intel_engine_cs *engine, u32 mask)
{
	if (IS_SRIOV_VF(engine->i915))
		return;

	if (engine->i915->quiesce_gpu)
		return;
	ENGINE_WRITE(engine, RING_HWSTAM, mask);
}

static void intel_engine_sanitize_mmio(struct intel_engine_cs *engine)
{
	/* Mask off all writes into the unknown HWSP */
	intel_engine_set_hwsp_writemask(engine, ~0u);
}

static void nop_irq_handler(struct intel_engine_cs *engine, u16 iir)
{
	GEM_DEBUG_WARN_ON(iir);
}

static u32 get_reset_domain(u8 ver, enum intel_engine_id id)
{
	static const u32 engine_reset_domains[] = {
		[RCS0]  = GEN11_GRDOM_RENDER,
		[BCS0]  = GEN11_GRDOM_BLT,
		[BCS1]  = XEHPC_GRDOM_BLT1,
		[BCS2]  = XEHPC_GRDOM_BLT2,
		[BCS3]  = XEHPC_GRDOM_BLT3,
		[BCS4]  = XEHPC_GRDOM_BLT4,
		[BCS5]  = XEHPC_GRDOM_BLT5,
		[BCS6]  = XEHPC_GRDOM_BLT6,
		[BCS7]  = XEHPC_GRDOM_BLT7,
		[BCS8]  = XEHPC_GRDOM_BLT8,
		[VCS0]  = GEN11_GRDOM_MEDIA,
		[VCS1]  = GEN11_GRDOM_MEDIA2,
		[VCS2]  = GEN11_GRDOM_MEDIA3,
		[VCS3]  = GEN11_GRDOM_MEDIA4,
		[VCS4]  = GEN11_GRDOM_MEDIA5,
		[VCS5]  = GEN11_GRDOM_MEDIA6,
		[VCS6]  = GEN11_GRDOM_MEDIA7,
		[VCS7]  = GEN11_GRDOM_MEDIA8,
		[VECS0] = GEN11_GRDOM_VECS,
		[VECS1] = GEN11_GRDOM_VECS2,
		[VECS2] = GEN11_GRDOM_VECS3,
		[VECS3] = GEN11_GRDOM_VECS4,
		[CCS0]  = GEN11_GRDOM_RENDER,
		[CCS1]  = GEN11_GRDOM_RENDER,
		[CCS2]  = GEN11_GRDOM_RENDER,
		[CCS3]  = GEN11_GRDOM_RENDER,
		[GSC0]  = GEN12_GRDOM_GSC,
	};

	GEM_BUG_ON(id >= ARRAY_SIZE(engine_reset_domains) ||
		   !engine_reset_domains[id]);
	return engine_reset_domains[id];
}

static void reset_work(struct work_struct *work)
{
	struct intel_engine_cs *engine = container_of(work, struct intel_engine_cs, reset.work);

	intel_gt_reset(engine->gt, engine->mask, engine->reset.msg);
}

static int intel_engine_setup(struct intel_gt *gt, enum intel_engine_id id,
			      u8 logical_instance)
{
	const struct engine_info *info = &intel_engines[id];
	struct drm_i915_private *i915 = gt->i915;
	struct intel_engine_cs *engine;
	u8 guc_class;

	BUILD_BUG_ON(MAX_ENGINE_CLASS >= BIT(GEN11_ENGINE_CLASS_WIDTH));
	BUILD_BUG_ON(MAX_ENGINE_INSTANCE >= BIT(GEN11_ENGINE_INSTANCE_WIDTH));
	BUILD_BUG_ON(I915_MAX_VCS > (MAX_ENGINE_INSTANCE + 1));
	BUILD_BUG_ON(I915_MAX_VECS > (MAX_ENGINE_INSTANCE + 1));

	if (GEM_DEBUG_WARN_ON(id >= ARRAY_SIZE(gt->engine)))
		return -EINVAL;

	if (GEM_DEBUG_WARN_ON(info->class > MAX_ENGINE_CLASS))
		return -EINVAL;

	if (GEM_DEBUG_WARN_ON(info->instance > MAX_ENGINE_INSTANCE))
		return -EINVAL;

	if (GEM_DEBUG_WARN_ON(gt->engine_class[info->class][info->instance]))
		return -EINVAL;

	engine = kzalloc(sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return -ENOMEM;

	BUILD_BUG_ON(BITS_PER_TYPE(engine->mask) < I915_NUM_ENGINES);

	engine->id = id;
	engine->legacy_idx = INVALID_ENGINE;
	engine->mask = BIT(id);
	engine->reset_domain = get_reset_domain(GRAPHICS_VER(gt->i915),
						id);
	engine->i915 = i915;
	engine->gt = gt;
	engine->uncore = gt->uncore;
	guc_class = engine_class_to_guc_class(info->class);
	engine->guc_id = MAKE_GUC_ID(guc_class, info->instance);
	engine->mmio_base = __engine_mmio_base(i915, info->mmio_bases);

	engine->irq_handler = nop_irq_handler;

	engine->class = info->class;
	engine->instance = info->instance;
	engine->logical_mask = BIT(logical_instance);
	engine->irq_offset = info->irq_offset;
	__sprint_engine_name(engine);

	INIT_WORK(&engine->reset.work, reset_work);

	engine->ppgtt_size = INTEL_INFO(i915)->ppgtt_size;
	if (IS_PONTEVECCHIO(i915) && engine->class == VIDEO_DECODE_CLASS)
		engine->ppgtt_size = min_t(u8, engine->ppgtt_size, 48);

	if ((engine->class == COMPUTE_CLASS && !RCS_MASK(engine->gt) &&
	     __ffs(CCS_MASK(engine->gt)) == engine->instance) ||
	     engine->class == RENDER_CLASS)
		engine->flags |= I915_ENGINE_FIRST_RENDER_COMPUTE;

	/* features common between engines sharing EUs */
	if (engine->class == RENDER_CLASS || engine->class == COMPUTE_CLASS) {
		engine->flags |= I915_ENGINE_HAS_RCS_REG_STATE;
		engine->flags |= I915_ENGINE_HAS_EU_PRIORITY;

		/* EU attention is not available on VFs */
		if(!IS_SRIOV_VF(gt->i915))
			engine->flags |= I915_ENGINE_HAS_EU_ATTENTION;

		/* we only care about run alone on platforms that have a CCS */
		if (CCS_MASK(gt))
			engine->flags |= I915_ENGINE_HAS_RUN_ALONE_MODE;
	}

	engine->props.heartbeat_interval_ms =
		CPTCFG_DRM_I915_HEARTBEAT_INTERVAL;
	engine->props.max_busywait_duration_ns =
		CPTCFG_DRM_I915_MAX_REQUEST_BUSYWAIT;
	engine->props.preempt_timeout_ms =
		CPTCFG_DRM_I915_PREEMPT_TIMEOUT;
	engine->props.stop_timeout_ms =
		CPTCFG_DRM_I915_STOP_TIMEOUT;
	engine->props.timeslice_duration_ms =
		CPTCFG_DRM_I915_TIMESLICE_DURATION;

	/* FIXME: Balancer IGT test starts to fail below 5ms timeslice */
	if (intel_guc_submission_is_wanted(&gt->uc.guc) &&
	    (engine->props.timeslice_duration_ms < 5))
		engine->props.timeslice_duration_ms = 5;

	/*
	 * Mid-thread pre-emption is not available in Gen12. Unfortunately,
	 * some compute workloads run quite long threads. That means they get
	 * reset due to not pre-empting in a timely manner. So, bump the
	 * pre-emption timeout value to be much higher for compute engines.
	 */
	if (GRAPHICS_VER(i915) == 12 && (engine->flags & I915_ENGINE_HAS_RCS_REG_STATE))
		engine->props.preempt_timeout_ms = CPTCFG_DRM_I915_PREEMPT_TIMEOUT_COMPUTE;

	/*
	 * With their many BCS engines (and CCS engines), PVC systems can overload
	 * the PCIe bus. That results in all operations crawling along. Forward
	 * progress is made but it can take a while to complete each individual
	 * copy. So need to bump the pre-emption timeout to compensate and not
	 * kill such copies off prematurely.
	 */
	if (GRAPHICS_VER(i915) == 12 && engine->class == COPY_ENGINE_CLASS &&
	    (hweight32(BCS_MASK(gt)) >= 2))
		engine->props.preempt_timeout_ms = CPTCFG_DRM_I915_PREEMPT_TIMEOUT_COMPUTE_COPY;

	if (!RCS_MASK(gt) || engine->class == COMPUTE_CLASS) { /* non-interactive, compute-only */
		/*
		 * No interactive 3D pipeline, set compute-only datacentre
		 * defaults. Disable heartbeat checking and any accidental
		 * preemption resets; the compute workloads are very long
		 * running and desire to run uninterrupted.
		 */
		engine->props.heartbeat_interval_ms = 0; /* no heartbeats */
		engine->props.preempt_timeout_ms = 0; /* no engine resets */
	}

	/* Cap properties according to any system limits */
#define CLAMP_PROP(field) \
	do { \
		u64 clamp = intel_clamp_##field(engine, engine->props.field); \
		if (clamp != engine->props.field) { \
			drm_notice(&engine->i915->drm, \
				   "Warning, clamping %s to %lld to prevent overflow\n", \
				   #field, clamp); \
			engine->props.field = clamp; \
		} \
	} while (0)

	CLAMP_PROP(heartbeat_interval_ms);
	CLAMP_PROP(max_busywait_duration_ns);
	CLAMP_PROP(preempt_timeout_ms);
	CLAMP_PROP(stop_timeout_ms);
	CLAMP_PROP(timeslice_duration_ms);

#undef CLAMP_PROP

	engine->defaults = engine->props; /* never to change again */

	engine->context_size = intel_engine_context_size(gt, engine->class);
	if (WARN_ON(engine->context_size > BIT(20)))
		engine->context_size = 0;
	if (engine->context_size)
		DRIVER_CAPS(i915)->has_logical_contexts = true;

	ewma__engine_latency_init(&engine->latency);

	/* Scrub mmio state on takeover */
	intel_engine_sanitize_mmio(engine);

	gt->engine_class[info->class][info->instance] = engine;
	gt->engine[id] = engine;

	/* Store last instance of copy engine */
	if (engine->class == COPY_ENGINE_CLASS)
		gt->rsvd_bcs = id;

	return 0;
}

u64 intel_clamp_heartbeat_interval_ms(struct intel_engine_cs *engine, u64 value)
{
	value = min_t(u64, value, jiffies_to_msecs(MAX_SCHEDULE_TIMEOUT));

	return value;
}

u64 intel_clamp_max_busywait_duration_ns(struct intel_engine_cs *engine, u64 value)
{
	value = min(value, jiffies_to_nsecs(2));

	return value;
}

u64 intel_clamp_preempt_timeout_ms(struct intel_engine_cs *engine, u64 value)
{
	/*
	 * NB: The GuC API only supports 32bit values. However, the limit is further
	 * reduced due to internal calculations which would otherwise overflow.
	 */
	if (intel_guc_submission_is_wanted(&engine->gt->uc.guc))
		value = min_t(u64, value, guc_policy_max_preempt_timeout_ms());

	value = min_t(u64, value, jiffies_to_msecs(MAX_SCHEDULE_TIMEOUT));

	return value;
}

u64 intel_clamp_stop_timeout_ms(struct intel_engine_cs *engine, u64 value)
{
	value = min_t(u64, value, jiffies_to_msecs(MAX_SCHEDULE_TIMEOUT));

	return value;
}

u64 intel_clamp_timeslice_duration_ms(struct intel_engine_cs *engine, u64 value)
{
	/*
	 * NB: The GuC API only supports 32bit values. However, the limit is further
	 * reduced due to internal calculations which would otherwise overflow.
	 */
	if (intel_guc_submission_is_wanted(&engine->gt->uc.guc))
		value = min_t(u64, value, guc_policy_max_exec_quantum_ms());

	value = min_t(u64, value, jiffies_to_msecs(MAX_SCHEDULE_TIMEOUT));

	return value;
}

static void __setup_bcs_capabilities(struct intel_engine_cs *engine)
{
	if (HAS_LINK_COPY_ENGINES(engine->i915)) {
		engine->uabi_capabilities |=
			PRELIM_I915_COPY_CLASS_CAP_SATURATE_PCIE;
		if (engine->instance == 0)
			engine->uabi_capabilities |=
				PRELIM_I915_COPY_CLASS_CAP_BLOCK_COPY |
				PRELIM_I915_COPY_CLASS_CAP_SATURATE_LMEM |
				PRELIM_I915_COPY_CLASS_CAP_SATURATE_LINK;
		else if (engine->instance >= 3)
			engine->uabi_capabilities |=
				PRELIM_I915_COPY_CLASS_CAP_SATURATE_LINK;
	} else if (GRAPHICS_VER(engine->i915) >= 12) {
		engine->uabi_capabilities |=
			PRELIM_I915_COPY_CLASS_CAP_BLOCK_COPY;
	}
}

static void __setup_engine_capabilities(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;

	if (engine->class == VIDEO_DECODE_CLASS) {
		/*
		 * HEVC support is present on first engine instance
		 * before Gen11 and on all instances afterwards.
		 */
		engine->uabi_capabilities |= I915_VIDEO_CLASS_CAPABILITY_HEVC;

		/*
		 * SFC block is present only on even logical engine
		 * instances.
		 */
		if (engine->gt->info.vdbox_sfc_access & BIT(engine->instance))
			engine->uabi_capabilities |=
				I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC;

		/* PVC does not have VDENC on all except the first engine */
		if (!HAS_SLIM_VDBOX(i915) || engine->instance == 0)
			engine->uabi_capabilities |=
				PRELIM_I915_VIDEO_CLASS_CAPABILITY_VDENC;
	} else if (engine->class == VIDEO_ENHANCEMENT_CLASS) {
		if (engine->gt->info.sfc_mask & BIT(engine->instance))
			engine->uabi_capabilities |=
				I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC;
	} else if (engine->class == COPY_ENGINE_CLASS) {
		__setup_bcs_capabilities(engine);
	}
}

static void intel_setup_engine_capabilities(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	for_each_engine(engine, gt, id)
		__setup_engine_capabilities(engine);
}

static void destroy_pinned_context(struct intel_context *ce)
{
	if (!list_empty(&ce->timeline->engine_link)) {
		struct intel_engine_cs *engine = ce->engine;
		struct i915_vma *hwsp = engine->status_page.vma;

		mutex_lock(&hwsp->vm->mutex);
		list_del(&ce->timeline->engine_link);
		mutex_unlock(&hwsp->vm->mutex);
	}

	list_del(&ce->pinned_contexts_link);
	intel_context_unpin(ce);
	intel_context_put(ce);
}

/**
 * intel_engines_release() - free the resources allocated for Command Streamers
 * @gt: pointer to struct intel_gt
 */
void intel_engines_release(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	struct intel_context *ce, *cn;
	enum intel_engine_id id;

	/*
	 * Before we release the resources held by engine, we must be certain
	 * that the HW is no longer accessing them -- having the GPU scribble
	 * to or read from a page being used for something else causes no end
	 * of fun.
	 *
	 * The GPU should be reset by this point, but assume the worst just
	 * in case we aborted before completely initialising the engines.
	 */

	/* Disable any further use of the engines */
	memset(&gt->i915->uabi_engines, 0, sizeof(gt->i915->uabi_engines));
	for_each_engine(engine, gt, id) {
		if (!engine->kernel_context)
			continue;

		intel_engine_pm_wait_for_idle(engine);
		GEM_BUG_ON(intel_engine_pm_is_awake(engine));

		memset(&engine->reset, 0, sizeof(engine->reset));
		engine->kernel_context = NULL;
	}

	list_for_each_entry_safe(ce, cn, &gt->pinned_contexts, pinned_contexts_link)
		destroy_pinned_context(ce);

	/* Decouple the backend; but keep the layout for late GPU resets */
	for_each_engine(engine, gt, id) {
		if (!engine->release)
			continue;

		engine->release(engine);
		engine->release = NULL;
	}
}

void intel_engine_free_request_pool(struct intel_engine_cs *engine)
{
	if (!engine->request_pool)
		return;

	kmem_cache_free(i915_request_slab_cache(), engine->request_pool);
}

void intel_engines_free(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	/* Free the requests! dma-resv keeps fences around for an eternity */
	rcu_barrier();

	for_each_engine(engine, gt, id) {
		intel_engine_free_request_pool(engine);
		kfree(engine);
		gt->engine[id] = NULL;
	}
}

static
bool gen11_vdbox_has_sfc(struct intel_gt *gt,
			 unsigned int physical_vdbox,
			 unsigned int logical_vdbox, u16 vdbox_mask)
{
	struct drm_i915_private *i915 = gt->i915;

	/*
	 * In Gen11, only even numbered logical VDBOXes are hooked
	 * up to an SFC (Scaler & Format Converter) unit.
	 * In Gen12, Even numbered physical instance always are connected
	 * to an SFC. Odd numbered physical instances have SFC only if
	 * previous even instance is fused off.
	 * In PVC, none of the VDBOXes have access to an SFC.
	 *
	 * Starting with Xe_HP, there's also a dedicated SFC_ENABLE field
	 * in the fuse register that tells us whether a specific SFC is present.
	 */
	if ((gt->info.sfc_mask & BIT(physical_vdbox / 2)) == 0)
		return false;
	else if (drm_WARN_ON(&i915->drm, HAS_SLIM_VDBOX(i915)))
		 /* Should already be caught by the SFC fuse check */
		return false;
	else if (MEDIA_VER(i915) >= 12)
		return (physical_vdbox % 2 == 0) ||
			!(BIT(physical_vdbox - 1) & vdbox_mask);
	else if (MEDIA_VER(i915) == 11)
		return logical_vdbox % 2 == 0;

	return false;
}

static void engine_mask_apply_media_fuses(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	unsigned int logical_vdbox = 0;
	unsigned int i;
	u32 media_fuse, fuse1;
	u16 vdbox_mask;
	u16 vebox_mask;

	if (MEDIA_VER(gt->i915) < 11)
		return;

	/*
	 * On newer platforms the fusing register is called 'enable' and has
	 * enable semantics, while on older platforms it is called 'disable'
	 * and bits have disable semantices.
	 */
	media_fuse = intel_uncore_read(gt->uncore, GEN11_GT_VEBOX_VDBOX_DISABLE);
	if (MEDIA_VER_FULL(i915) < IP_VER(12, 50))
		media_fuse = ~media_fuse;

	vdbox_mask = media_fuse & GEN11_GT_VDBOX_DISABLE_MASK;
	vebox_mask = (media_fuse & GEN11_GT_VEBOX_DISABLE_MASK) >>
		      GEN11_GT_VEBOX_DISABLE_SHIFT;

	if (HAS_SLIM_VDBOX(i915)) {
		/*
		 * The SFC_ENABLE bits are broken on PVC and indicate that
		 * SFC is present, even though PVC has no SFC support.
		 */
		gt->info.sfc_mask = 0;
	} else if (MEDIA_VER_FULL(i915) >= IP_VER(12, 50)) {
		fuse1 = intel_uncore_read(gt->uncore, HSW_PAVP_FUSE1);
		gt->info.sfc_mask = REG_FIELD_GET(XEHP_SFC_ENABLE_MASK, fuse1);
	} else {
		gt->info.sfc_mask = ~0;
	}

	for (i = 0; i < I915_MAX_VCS; i++) {
		if (!HAS_ENGINE(gt, _VCS(i))) {
			vdbox_mask &= ~BIT(i);
			continue;
		}

		if (!(BIT(i) & vdbox_mask)) {
			gt->info.engine_mask &= ~BIT(_VCS(i));
			drm_dbg(&i915->drm, "vcs%u fused off\n", i);
			continue;
		}

		if (gen11_vdbox_has_sfc(gt, i, logical_vdbox, vdbox_mask))
			gt->info.vdbox_sfc_access |= BIT(i);
		logical_vdbox++;
	}
	drm_dbg(&i915->drm, "vdbox enable: %04x, instances: %04lx\n",
		vdbox_mask, VDBOX_MASK(gt));
	if (vdbox_mask != VDBOX_MASK(gt))
		DRM_WARN("%s: unexpected VDbox fuse value\n", __func__);

	for (i = 0; i < I915_MAX_VECS; i++) {
		if (!HAS_ENGINE(gt, _VECS(i))) {
			vebox_mask &= ~BIT(i);
			continue;
		}

		if (!(BIT(i) & vebox_mask)) {
			gt->info.engine_mask &= ~BIT(_VECS(i));
			drm_dbg(&i915->drm, "vecs%u fused off\n", i);
		}
	}
	drm_dbg(&i915->drm, "vebox enable: %04x, instances: %04lx\n",
		vebox_mask, VEBOX_MASK(gt));
	if (vebox_mask != VEBOX_MASK(gt))
		DRM_WARN("%s: unexpected VEbox fuse value\n", __func__);
}

static void engine_mask_apply_compute_fuses(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_gt_info *info = &gt->info;
	int ss_per_ccs = info->sseu.max_subslices / I915_MAX_CCS;
	unsigned long ccs_mask;
	unsigned int i;

	if (hweight32(CCS_MASK(gt)) <= 1)
		return;

	ccs_mask = intel_slicemask_from_xehp_dssmask(info->sseu.compute_subslice_mask,
						     ss_per_ccs);

	/*
	 * As per HSD:22012626112 on PVC when using only quad0, we
	 * are allowed to use only CCS0 engine and all other engines
	 * should be masked out, as it GAM daisy chain is broken.
	 */
	if (IS_PONTEVECCHIO(i915)) {
		u32 meml3_mask;

		meml3_mask = intel_uncore_read(gt->uncore, GEN10_MIRROR_FUSE3);
		if (REG_FIELD_GET(GEN12_MEML3_EN_MASK, meml3_mask) == 1)
			ccs_mask &= ~(GENMASK(I915_MAX_CCS, 1));
	}

	/*
	 * If all DSS in a quadrant are fused off, the corresponding CCS
	 * engine is not available for use.
	 */
	for_each_clear_bit(i, &ccs_mask, I915_MAX_CCS) {
		info->engine_mask &= ~BIT(_CCS(i));
		drm_dbg(&i915->drm, "ccs%u fused off\n", i);
	}
}

static void engine_mask_apply_copy_fuses(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_gt_info *info = &gt->info;
	unsigned long meml3_mask;
	unsigned long quad;

	if (!(GRAPHICS_VER_FULL(i915) >= IP_VER(12, 60) &&
	      GRAPHICS_VER_FULL(i915) < IP_VER(12, 70)))
		return;

	meml3_mask = intel_uncore_read(gt->uncore, GEN10_MIRROR_FUSE3);
	meml3_mask = REG_FIELD_GET(GEN12_MEML3_EN_MASK, meml3_mask);

	/*
	 * Link Copy engines may be fused off according to meml3_mask. Each
	 * bit is a quad that houses 2 Link Copy and two Sub Copy engines.
	 */
	for_each_clear_bit(quad, &meml3_mask, GEN12_MAX_MSLICES) {
		unsigned int instance = quad * 2 + 1;
		intel_engine_mask_t mask = GENMASK(_BCS(instance + 1),
						   _BCS(instance));

		if (mask & info->engine_mask) {
			drm_dbg(&i915->drm, "bcs%u fused off\n", instance);
			drm_dbg(&i915->drm, "bcs%u fused off\n", instance + 1);

			info->engine_mask &= ~mask;
		}
	}
}

/*
 * Determine which engines are fused off in our particular hardware.
 * Note that we have a catch-22 situation where we need to be able to access
 * the blitter forcewake domain to read the engine fuses, but at the same time
 * we need to know which engines are available on the system to know which
 * forcewake domains are present. We solve this by intializing the forcewake
 * domains based on the full engine mask in the platform capabilities before
 * calling this function and pruning the domains for fused-off engines
 * afterwards.
 */
static intel_engine_mask_t init_engine_mask(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_gt_info *info = &gt->info;

	GEM_BUG_ON(!info->engine_mask);

	info->engine_mask &= i915->params.ring_mask;

	/* only print the message once for GT0 */
	if (gt->info.id == 0 &&
	    info->engine_mask != INTEL_INFO(i915)->platform_engine_mask)
		DRM_INFO("loading with reduced engine mask 0x%x\n",
			 info->engine_mask);

	engine_mask_apply_media_fuses(gt);
	engine_mask_apply_compute_fuses(gt);
	engine_mask_apply_copy_fuses(gt);

	if (!intel_uc_wants_gsc_uc(&gt->uc))
		info->engine_mask &= ~BIT(GSC0);

	return info->engine_mask;
}

static void populate_logical_ids(struct intel_gt *gt, u8 *logical_ids,
				 u8 class, const u8 *map, u8 num_instances)
{
	int i, j;
	u8 current_logical_id = 0;

	for (j = 0; j < num_instances; ++j) {
		for (i = 0; i < ARRAY_SIZE(intel_engines); ++i) {
			if (!HAS_ENGINE(gt, i) ||
			    intel_engines[i].class != class)
				continue;

			if (intel_engines[i].instance == map[j]) {
				logical_ids[intel_engines[i].instance] =
					current_logical_id++;
				break;
			}
		}
	}
}

static void setup_logical_ids(struct intel_gt *gt, u8 *logical_ids, u8 class)
{
	/*
	 * Logical to physical mapping is needed for proper support
	 * to split-frame feature.
	 */
	if (HAS_SLIM_VDBOX(gt->i915) && class == VIDEO_DECODE_CLASS) {
		const u8 map[] = { 0, 2, 1 };

		populate_logical_ids(gt, logical_ids, class,
				     map, ARRAY_SIZE(map));
	} else if (MEDIA_VER(gt->i915) >= 11 && class == VIDEO_DECODE_CLASS) {
		const u8 map[] = { 0, 2, 4, 6, 1, 3, 5, 7 };

		populate_logical_ids(gt, logical_ids, class,
				     map, ARRAY_SIZE(map));
	} else {
		int i;
		u8 map[MAX_ENGINE_INSTANCE + 1];

		for (i = 0; i < MAX_ENGINE_INSTANCE + 1; ++i)
			map[i] = i;
		populate_logical_ids(gt, logical_ids, class,
				     map, ARRAY_SIZE(map));
	}
}

/**
 * intel_engines_init_mmio() - allocate and prepare the Engine Command Streamers
 * @gt: pointer to struct intel_gt
 *
 * Return: non-zero if the initialization failed.
 */
int intel_engines_init_mmio(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	const unsigned int engine_mask = init_engine_mask(gt);
	unsigned int mask = 0;
	unsigned int i, class;
	u8 logical_ids[MAX_ENGINE_INSTANCE + 1];
	int err;

	if (engine_mask == 0)
		DRM_WARN("%s: no engines enabled\n", __func__);
	drm_WARN_ON(&i915->drm, engine_mask &
		    GENMASK(BITS_PER_TYPE(mask) - 1, I915_NUM_ENGINES));

	if (i915_inject_probe_failure(i915))
		return -ENODEV;

	for (class = 0; class < MAX_ENGINE_CLASS + 1; ++class) {
		setup_logical_ids(gt, logical_ids, class);

		for (i = 0; i < ARRAY_SIZE(intel_engines); ++i) {
			u8 instance = intel_engines[i].instance;

			if (intel_engines[i].class != class ||
			    !HAS_ENGINE(gt, i))
				continue;

			err = intel_engine_setup(gt, i,
						 logical_ids[instance]);
			if (err)
				goto cleanup;

			mask |= BIT(i);
		}
	}

	/*
	 * Catch failures to update intel_engines table when the new engines
	 * are added to the driver by a warning and disabling the forgotten
	 * engines.
	 */
	if (drm_WARN_ON(&i915->drm, mask != engine_mask))
		gt->info.engine_mask = mask;

	gt->info.num_engines = hweight32(mask);

	intel_gt_check_and_clear_faults(gt);

	intel_setup_engine_capabilities(gt);

	intel_uncore_prune_engine_fw_domains(gt->uncore, gt);

	return 0;

cleanup:
	intel_engines_free(gt);
	return err;
}

void intel_engine_init_execlists(struct intel_engine_cs *engine)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;

	execlists->port_mask = 1;
	GEM_BUG_ON(!is_power_of_2(execlists_num_ports(execlists)));
	GEM_BUG_ON(execlists_num_ports(execlists) > EXECLIST_MAX_PORTS);

	memset(execlists->pending, 0, sizeof(execlists->pending));
	execlists->active =
		memset(execlists->inflight, 0, sizeof(execlists->inflight));
}

static void cleanup_status_page(struct intel_engine_cs *engine)
{
	struct i915_vma *vma;

	/* Prevent writes into HWSP after returning the page to the system */
	intel_engine_set_hwsp_writemask(engine, ~0u);

	vma = fetch_and_zero(&engine->status_page.vma);
	if (!vma)
		return;

	if (!HWS_NEEDS_PHYSICAL(engine->i915))
		i915_vma_unpin(vma);

	i915_gem_object_unpin_map(vma->obj);
	i915_gem_object_put(vma->obj);

	/* no longer in control, nothing left to sanitize */
	engine->status_page.sanitize = NULL;
}

static int pin_ggtt_status_page(struct intel_engine_cs *engine,
				struct i915_gem_ww_ctx *ww,
				struct i915_vma *vma)
{
	return i915_ggtt_pin_for_gt(vma, ww);
}

static int init_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;
	struct i915_gem_ww_ctx ww;
	struct i915_vma *vma;
	void *vaddr;
	int ret;

	INIT_LIST_HEAD(&engine->status_page.timelines);

	/*
	 * Though the HWS register does support 36bit addresses, historically
	 * we have had hangs and corruption reported due to wild writes if
	 * the HWS is placed above 4G. We only allow objects to be allocated
	 * in GFP_DMA32 for i965, and no earlier physical address users had
	 * access to more than 4G.
	 */
	obj = i915_gem_object_create_internal(engine->i915, PAGE_SIZE);
	if (IS_ERR(obj)) {
		gt_err(engine->gt,
			"Failed to allocate %s status page\n",
			engine->name);
		return PTR_ERR(obj);
	}

	i915_gem_object_set_cache_coherency(obj, I915_CACHE_LLC);

	vma = i915_vma_instance(obj, &engine->gt->ggtt->vm, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_put;
	}

	i915_gem_ww_ctx_init(&ww, true);
retry:
	ret = i915_gem_object_lock(obj, &ww);
	if (!ret && !HWS_NEEDS_PHYSICAL(engine->i915))
		ret = pin_ggtt_status_page(engine, &ww, vma);
	if (ret)
		goto err;

	vaddr = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto err_unpin;
	}

	engine->status_page.addr = memset(vaddr, 0, PAGE_SIZE);
	engine->status_page.vma = vma;

	ret = i915_vma_wait_for_bind(vma);
err_unpin:
	if (ret)
		i915_vma_unpin(vma);
err:
	if (ret == -EDEADLK) {
		ret = i915_gem_ww_ctx_backoff(&ww);
		if (!ret)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
err_put:
	if (ret)
		i915_gem_object_put(obj);
	return ret;
}

static int engine_setup_common(struct intel_engine_cs *engine)
{
	int err;

	spin_lock_init(&engine->barrier_lock);
	INIT_LIST_HEAD(&engine->barrier_tasks);

	err = init_status_page(engine);
	if (err)
		return err;

	engine->breadcrumbs = intel_breadcrumbs_create(engine);
	if (!engine->breadcrumbs) {
		err = -ENOMEM;
		goto err_status;
	}

	engine->sched_engine = i915_sched_engine_create(ENGINE_PHYSICAL);
	if (!engine->sched_engine) {
		err = -ENOMEM;
		goto err_sched_engine;
	}
#ifndef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
	engine->sched_engine->private_data = engine;
#endif

	intel_engine_init_execlists(engine);
	intel_engine_init__pm(engine);
	intel_engine_init_retire(engine);

	/* Use the whole device by default */
	engine->sseu =
		intel_sseu_from_device_info(&engine->gt->info.sseu);

	intel_engine_init_workarounds(engine);
	intel_engine_init_whitelist(engine);
	intel_engine_init_ctx_wa(engine);

	engine->flags |= I915_ENGINE_HAS_RELATIVE_MMIO;

	INIT_WORK(&engine->reset.notify_reset_failed, intel_engine_reset_failed_uevent_work);
	return 0;

err_sched_engine:
	intel_breadcrumbs_put(fetch_and_zero(&engine->breadcrumbs));
err_status:
	cleanup_status_page(engine);
	return err;
}

struct measure_breadcrumb {
	struct i915_request rq;
	struct intel_ring ring;
	u32 cs[2048];
};

static int measure_breadcrumb_dw(struct intel_context *ce)
{
	struct intel_engine_cs *engine = ce->engine;
	struct measure_breadcrumb *frame;
	int dw;

	frame = kzalloc(sizeof(*frame), GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	frame->rq.i915 = engine->i915;
	frame->rq.engine = engine;
	frame->rq.context = ce;
	rcu_assign_pointer(frame->rq.timeline, ce->timeline);
	frame->rq.hwsp_seqno = ce->timeline->hwsp_seqno;
	frame->rq.execution_mask = engine->mask;

	frame->ring.vaddr = frame->cs;
	frame->ring.size = sizeof(frame->cs);
	frame->ring.wrap =
		BITS_PER_TYPE(frame->ring.size) - ilog2(frame->ring.size);
	frame->ring.effective_size = frame->ring.size;
	intel_ring_update_space(&frame->ring);
	frame->rq.ring = &frame->ring;

	mutex_lock(&ce->timeline->mutex);
	spin_lock_irq(&engine->sched_engine->lock);

	dw = engine->emit_fini_breadcrumb(&frame->rq, frame->cs) - frame->cs;

	spin_unlock_irq(&engine->sched_engine->lock);
	mutex_unlock(&ce->timeline->mutex);

	GEM_BUG_ON(dw & 1); /* RING_TAIL must be qword aligned */

	kfree(frame);
	return dw;
}

struct intel_context *
intel_engine_create_pinned_context(struct intel_engine_cs *engine,
				   struct i915_address_space *vm,
				   unsigned int ring_size,
				   unsigned int hwsp,
				   struct lock_class_key *key,
				   const char *name)
{
	struct intel_context *ce;
	int err;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return ce;

	__set_bit(CONTEXT_BARRIER_BIT, &ce->flags);
	ce->timeline = page_pack_bits(NULL, hwsp);
	ce->ring = NULL;
	ce->ring_size = ring_size;

	i915_vm_put(ce->vm);
	ce->vm = i915_vm_get(vm);

	err = intel_context_pin(ce); /* perma-pin so it is always available */
	if (err) {
		intel_context_put(ce);
		return ERR_PTR(err);
	}

	list_add_tail(&ce->pinned_contexts_link, &engine->gt->pinned_contexts);
	if (ce->state)
		ce->state->obj->flags |= I915_BO_ALLOC_VOLATILE;

	/*
	 * Give our perma-pinned kernel timelines a separate lockdep class,
	 * so that we can use them from within the normal user timelines
	 * should we need to inject GPU operations during their request
	 * construction.
	 */
	lockdep_set_class_and_name(&ce->timeline->mutex, key, name);

	return ce;
}

static struct intel_context *
create_kernel_context(struct intel_engine_cs *engine)
{
	static struct lock_class_key kernel;

	return intel_engine_create_pinned_context(engine, engine->gt->vm, SZ_4K,
						  I915_GEM_HWS_SEQNO_ADDR,
						  &kernel, "kernel_context");
}

/*
 * engines_init_common - initialize cengine state which might require hw access
 * @engine: Engine to initialize.
 *
 * Initializes @engine@ structure members shared between legacy and execlists
 * submission modes which do require hardware access.
 *
 * Typcally done at later stages of submission mode specific engine setup.
 *
 * Returns zero on success or an error code on failure.
 */
static int engine_init_common(struct intel_engine_cs *engine)
{
	struct intel_context *ce;
	int ret;

	engine->set_default_submission(engine);

	/*
	 * We may need to do things with the shrinker which
	 * require us to immediately switch back to the default
	 * context. This can cause a problem as pinning the
	 * default context also requires GTT space which may not
	 * be available. To avoid this we always pin the default
	 * context.
	 */
	ce = create_kernel_context(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	engine->kernel_context = ce;
	ret = measure_breadcrumb_dw(ce);
	if (ret < 0)
		return ret;

	engine->emit_fini_breadcrumb_dw = ret;
	return 0;
}

int intel_engines_init(struct intel_gt *gt)
{
	int (*setup)(struct intel_engine_cs *engine);
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err;

	if (intel_uc_uses_guc_submission(&gt->uc)) {
		gt->submission_method = INTEL_SUBMISSION_GUC;
		setup = intel_guc_submission_setup;
	} else {
		gt->submission_method = INTEL_SUBMISSION_ELSP;
		setup = intel_execlists_submission_setup;
	}

	for_each_engine(engine, gt, id) {
		err = engine_setup_common(engine);
		if (err)
			return err;

		err = setup(engine);
		if (err) {
			intel_engine_cleanup_common(engine);
			return err;
		}

		/* The backend should now be responsible for cleanup */
		GEM_BUG_ON(engine->release == NULL);

		err = engine_init_common(engine);
		if (err)
			return err;
		/*
		 * If we have only one instance of blitter engine then it will be
		 * use for kernel operations as well as exposed to user.
		 * If more instances are available do not expose it to use.
		 */
		if (engine->id == gt->rsvd_bcs && engine->instance)
			continue;

		/*
		 * As per HSD:16016805146 on SR-IOV PF/VF we only have CCS-1 mode
		 * thus only first ccs engine will be operational and can be exposed.
		 */
		if (IS_PONTEVECCHIO(gt->i915) && IS_SRIOV(gt->i915) &&
		    engine->class == COMPUTE_CLASS &&
		    engine->mask != FIRST_ENGINE_MASK(gt, CCS))
			continue;

		intel_engine_add_user(engine);
	}

	return 0;
}

void intel_engine_quiesce(struct intel_engine_cs *engine)
{
	cleanup_status_page(engine);
}

/*
 * intel_engine_cleanup_common - cleans up the engine state created by
 *                               the common initiailizers.
 * @engine: Engine to cleanup.
 *
 * This cleans up everything created by the common helpers.
 */
void intel_engine_cleanup_common(struct intel_engine_cs *engine)
{
	GEM_BUG_ON(!list_empty(&engine->sched_engine->requests));

	i915_sched_engine_put(fetch_and_zero(&engine->sched_engine));
	intel_breadcrumbs_put(fetch_and_zero(&engine->breadcrumbs));

	intel_engine_fini_retire(engine);

	if (engine->default_state)
		fput(engine->default_state);

	if (!engine->i915->quiesce_gpu)
		intel_engine_quiesce(engine);

	GEM_BUG_ON(!list_empty(&engine->barrier_tasks));

	intel_wa_list_free(&engine->ctx_wa_list);
	intel_wa_list_free(&engine->wa_list);
	intel_wa_list_free(&engine->whitelist);
}

/**
 * intel_engine_resume - re-initializes the HW state of the engine
 * @engine: Engine to resume.
 *
 * Returns zero on success or an error code on failure.
 */
int intel_engine_resume(struct intel_engine_cs *engine)
{
	intel_engine_apply_workarounds(engine);
	intel_engine_apply_whitelist(engine);

	return engine->resume(engine);
}

u64 intel_engine_get_active_head(const struct intel_engine_cs *engine)
{
	return ENGINE_READ64(engine, RING_ACTHD, RING_ACTHD_UDW);
}

u64 intel_engine_get_last_batch_head(const struct intel_engine_cs *engine)
{
	return ENGINE_READ64(engine, RING_BBADDR, RING_BBADDR_UDW);
}

static unsigned long stop_timeout(const struct intel_engine_cs *engine)
{
	if (in_atomic() || irqs_disabled()) /* inside atomic preempt-reset? */
		return 0;

	/*
	 * If we are doing a normal GPU reset, we can take our time and allow
	 * the engine to quiesce. We've stopped submission to the engine, and
	 * if we wait long enough an innocent context should complete and
	 * leave the engine idle. So they should not be caught unaware by
	 * the forthcoming GPU reset (which usually follows the stop_cs)!
	 */
	return READ_ONCE(engine->props.stop_timeout_ms);
}

static int __intel_engine_stop_cs(struct intel_engine_cs *engine,
				  int fast_timeout_us,
				  int slow_timeout_ms)
{
	struct intel_uncore *uncore = engine->uncore;
	const i915_reg_t mode = RING_MI_MODE(engine->mmio_base);
	int err;

	intel_uncore_write_fw(uncore, mode, _MASKED_BIT_ENABLE(STOP_RING));

	/*
	 * Wa_22011802037: Prior to doing a reset, ensure CS is
	 * stopped, set ring stop bit and prefetch disable bit to halt CS
	 */
	if (IS_MTL_GRAPHICS_STEP(engine->i915, M, STEP_A0, STEP_B0) ||
	    (GRAPHICS_VER(engine->i915) >= 11 &&
	    GRAPHICS_VER_FULL(engine->i915) < IP_VER(12, 70)))
		intel_uncore_write_fw(uncore, RING_MODE_GEN7(engine->mmio_base),
				      _MASKED_BIT_ENABLE(GEN12_GFX_PREFETCH_DISABLE));

	err = __intel_wait_for_register_fw(engine->uncore, mode,
					   MODE_IDLE, MODE_IDLE,
					   fast_timeout_us,
					   slow_timeout_ms,
					   NULL);

	/* A final mmio read to let GPU writes be hopefully flushed to memory */
	intel_uncore_posting_read_fw(uncore, mode);
	return err;
}

int intel_engine_stop_cs(struct intel_engine_cs *engine)
{
	int err = 0;

	if (GRAPHICS_VER(engine->i915) < 3)
		return -ENODEV;

	ENGINE_TRACE(engine, "\n");
	/*
	 * TODO: Find out why occasionally stopping the CS times out. Seen
	 * especially with gem_eio tests.
	 *
	 * Occasionally trying to stop the cs times out, but does not adversely
	 * affect functionality. The timeout is set as a config parameter that
	 * defaults to 100ms. In most cases the follow up operation is to wait
	 * for pending MI_FORCE_WAKES. The assumption is that this timeout is
	 * sufficient for any pending MI_FORCEWAKEs to complete. Once root
	 * caused, the caller must check and handle the return from this
	 * function.
	 */
	if (__intel_engine_stop_cs(engine, 1000, stop_timeout(engine))) {
		ENGINE_TRACE(engine,
			     "timed out on STOP_RING -> IDLE; HEAD:%04x, TAIL:%04x\n",
			     ENGINE_READ_FW(engine, RING_HEAD) & HEAD_ADDR,
			     ENGINE_READ_FW(engine, RING_TAIL) & TAIL_ADDR);

		/*
		 * Sometimes we observe that the idle flag is not
		 * set even though the ring is empty. So double
		 * check before giving up.
		 */
		if ((ENGINE_READ_FW(engine, RING_HEAD) & HEAD_ADDR) !=
		    (ENGINE_READ_FW(engine, RING_TAIL) & TAIL_ADDR))
			err = -ETIMEDOUT;
	}

	return err;
}

void intel_engine_cancel_stop_cs(struct intel_engine_cs *engine)
{
	ENGINE_TRACE(engine, "\n");

	ENGINE_WRITE_FW(engine, RING_MI_MODE, _MASKED_BIT_DISABLE(STOP_RING));
}

static u32 __cs_pending_mi_force_wakes(struct intel_engine_cs *engine)
{
	static const i915_reg_t _reg[I915_NUM_ENGINES] = {
		[RCS0] = MSG_IDLE_CS,
		[BCS0] = MSG_IDLE_BCS,
		[VCS0] = MSG_IDLE_VCS0,
		[VCS1] = MSG_IDLE_VCS1,
		[VCS2] = MSG_IDLE_VCS2,
		[VCS3] = MSG_IDLE_VCS3,
		[VCS4] = MSG_IDLE_VCS4,
		[VCS5] = MSG_IDLE_VCS5,
		[VCS6] = MSG_IDLE_VCS6,
		[VCS7] = MSG_IDLE_VCS7,
		[VECS0] = MSG_IDLE_VECS0,
		[VECS1] = MSG_IDLE_VECS1,
		[VECS2] = MSG_IDLE_VECS2,
		[VECS3] = MSG_IDLE_VECS3,
		[CCS0] = MSG_IDLE_CS,
		[CCS1] = MSG_IDLE_CS,
		[CCS2] = MSG_IDLE_CS,
		[CCS3] = MSG_IDLE_CS,
	};
	u32 val;

	if (!_reg[engine->id].reg)
		return 0;

	val = intel_uncore_read(engine->uncore, _reg[engine->id]);

	/* bits[29:25] & bits[13:9] >> shift */
	return (val & (val >> 16) & MSG_IDLE_FW_MASK) >> MSG_IDLE_FW_SHIFT;
}

static void __gpm_wait_for_fw_complete(struct intel_gt *gt, u32 fw_mask)
{
	int ret;

	/* Ensure GPM receives fw up/down after CS is stopped */
	udelay(1);

	/* Wait for forcewake request to complete in GPM */
	ret =  __intel_wait_for_register_fw(gt->uncore,
					    GEN9_PWRGT_DOMAIN_STATUS,
					    fw_mask, fw_mask, 5000, 0, NULL);

	/* Ensure CS receives fw ack from GPM */
	udelay(1);

	if (ret)
		GT_TRACE(gt, "Failed to complete pending forcewake %d\n", ret);
}

/*
 * Wa_22011802037:gen12: In addition to stopping the cs, we need to wait for any
 * pending MI_FORCE_WAKEUP requests that the CS has initiated to complete. The
 * pending status is indicated by bits[13:9] (masked by bits[29:25]) in the
 * MSG_IDLE register. There's one MSG_IDLE register per reset domain. Since we
 * are concerned only with the gt reset here, we use a logical OR of pending
 * forcewakeups from all reset domains and then wait for them to complete by
 * querying PWRGT_DOMAIN_STATUS.
 */
void intel_engine_wait_for_pending_mi_fw(struct intel_engine_cs *engine)
{
	u32 fw_pending = __cs_pending_mi_force_wakes(engine);

	if (fw_pending)
		__gpm_wait_for_fw_complete(engine->gt, fw_pending);
}

/* NB: please notice the memset */
void intel_engine_get_instdone(const struct intel_engine_cs *engine,
			       struct intel_instdone *instdone)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_uncore *uncore = engine->uncore;
	u32 mmio_base = engine->mmio_base;
	int slice;
	int subslice;
	int iter;

	memset(instdone, 0, sizeof(*instdone));

	instdone->instdone =
		intel_uncore_read(uncore, RING_INSTDONE(mmio_base));

	if (engine->id != RCS0)
		return;

	instdone->slice_common =
		intel_uncore_read(uncore, GEN7_SC_INSTDONE);
	instdone->slice_common_extra[0] =
		intel_uncore_read(uncore, GEN12_SC_INSTDONE_EXTRA);
	instdone->slice_common_extra[1] =
		intel_uncore_read(uncore, GEN12_SC_INSTDONE_EXTRA2);

	for_each_ss_steering(iter, engine->gt, slice, subslice) {
		instdone->sampler[slice][subslice] =
			intel_gt_mcr_read(engine->gt,
					  GEN8_SAMPLER_INSTDONE,
					  slice, subslice);
		instdone->row[slice][subslice] =
			intel_gt_mcr_read(engine->gt,
					  GEN8_ROW_INSTDONE,
					  slice, subslice);
	}

	if (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 55)) {
		for_each_ss_steering(iter, engine->gt, slice, subslice)
			instdone->geom_svg[slice][subslice] =
			intel_gt_mcr_read(engine->gt,
					  XEHPG_INSTDONE_GEOM_SVG,
					  slice, subslice);
	}
}

static bool ring_is_idle(struct intel_engine_cs *engine)
{
	bool idle = true;

	/* GuC submission shouldn't access HEAD & TAIL via MMIO */
	GEM_BUG_ON(intel_engine_uses_guc(engine));

	if (I915_SELFTEST_ONLY(!engine->mmio_base))
		return true;

	if (!intel_engine_pm_get_if_awake(engine))
		return true;

	/* First check that no commands are left in the ring */
	if ((ENGINE_READ(engine, RING_HEAD) & HEAD_ADDR) !=
	    (ENGINE_READ(engine, RING_TAIL) & TAIL_ADDR))
		idle = false;

	/* No bit for gen2, so assume the CS parser is idle */
	if (!(ENGINE_READ(engine, RING_MI_MODE) & MODE_IDLE))
		idle = false;

	intel_engine_pm_put_async(engine);

	return idle;
}

void __intel_engine_flush_submission(struct intel_engine_cs *engine, bool sync)
{
	struct tasklet_struct *t = &engine->sched_engine->tasklet;

#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
	if (!t->func)
#else
	if (!t->callback)
#endif
		return;

	local_bh_disable();
	if (tasklet_trylock(t)) {
		/* Must wait for any GPU reset in progress. */
		if (__tasklet_is_enabled(t))
#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
			t->func(t->data);
#else
			t->callback(t);
#endif
		tasklet_unlock(t);
	}
	local_bh_enable();

	/* Synchronise and wait for the tasklet on another CPU */
	if (sync)
		tasklet_unlock_spin_wait(t);
}

/**
 * intel_engine_is_idle() - Report if the engine has finished process all work
 * @engine: the intel_engine_cs
 *
 * Return true if there are no requests pending, nothing left to be submitted
 * to hardware, and that the engine is idle.
 */
bool intel_engine_is_idle(struct intel_engine_cs *engine)
{
	/* More white lies, if wedged, hw state is inconsistent */
	if (intel_gt_is_wedged(engine->gt))
		return true;

	if (!intel_engine_pm_is_awake(engine))
		return true;

	/* Waiting to drain ELSP? */
	intel_synchronize_hardirq(engine->i915);
	intel_engine_flush_submission(engine);

	/* ELSP is empty, but there are ready requests? E.g. after reset */
	if (!i915_sched_engine_is_empty(engine->sched_engine))
		return false;

	/*
	 * We shouldn't touch engine registers with GuC submission as the GuC
	 * owns the registers. Let's tie the idle to engine pm, at worst this
	 * function sometimes will falsely report non-idle when idle during the
	 * delay to retire requests or with virtual engines and a request
	 * running on another instance within the same class / submit mask.
	 */
	if (intel_engine_uses_guc(engine))
		return false;

	/* Ring stopped? */
	return ring_is_idle(engine);
}

bool intel_engine_irq_enable(struct intel_engine_cs *engine)
{
	if (!engine->irq_enable)
		return false;

	/* Caller disables interrupts */
	spin_lock(engine->gt->irq_lock);
	engine->irq_enable(engine);
	spin_unlock(engine->gt->irq_lock);

	return true;
}

void intel_engine_irq_disable(struct intel_engine_cs *engine)
{
	if (!engine->irq_disable)
		return;

	/* Caller disables interrupts */
	spin_lock(engine->gt->irq_lock);
	engine->irq_disable(engine);
	spin_unlock(engine->gt->irq_lock);
}

void intel_engines_reset_default_submission(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	for_each_engine(engine, gt, id) {
		if (engine->status_page.sanitize)
			engine->status_page.sanitize(engine);

		engine->set_default_submission(engine);
	}
}

bool intel_engine_can_store_dword(struct intel_engine_cs *engine)
{
	return true;
}

static void hexdump(struct drm_printer *m, int indent, const void *buf, size_t len)
{
	const size_t rowsize = 8 * sizeof(u32);
	const void *prev = NULL;
	bool skip = false;
	size_t pos;

	for (pos = 0; pos < len; pos += rowsize) {
		char line[128];

		if (prev && !memcmp(prev, buf + pos, rowsize)) {
			if (!skip) {
				i_printf(m, indent, "*\n");
				skip = true;
			}
			continue;
		}

		WARN_ON_ONCE(hex_dump_to_buffer(buf + pos, len - pos,
						rowsize, sizeof(u32),
						line, sizeof(line),
						false) >= sizeof(line));
		i_printf(m, indent, "[%04zx] %s\n", pos, line);

		prev = buf + pos;
		skip = false;
	}
}

static void intel_engine_print_registers(struct intel_engine_cs *engine,
					 struct drm_printer *m,
					 int indent)
{
	struct drm_i915_private *dev_priv = engine->i915;
	u64 addr;

	/* VF can't access these registers */
	if (IS_SRIOV_VF(dev_priv))
		return;

	i_printf(m, indent, "Registers: MMIO base: 0x%08x\n", engine->mmio_base);
	indent += 2;

	if (HAS_EXECLISTS(dev_priv)) {
		i_printf(m, indent, "EL_STAT_HI: 0x%08x\n",
			 ENGINE_READ(engine, RING_EXECLIST_STATUS_HI));
		i_printf(m, indent, "EL_STAT_LO: 0x%08x\n",
			 ENGINE_READ(engine, RING_EXECLIST_STATUS_LO));
	}
	i_printf(m, indent, "RING_START: 0x%08x\n",
		 ENGINE_READ(engine, RING_START));
	i_printf(m, indent, "RING_HEAD:  0x%08x\n",
		 ENGINE_READ(engine, RING_HEAD) & HEAD_ADDR);
	i_printf(m, indent, "RING_TAIL:  0x%08x\n",
		 ENGINE_READ(engine, RING_TAIL) & TAIL_ADDR);
	i_printf(m, indent, "RING_CTL:   0x%08x%s\n",
		 ENGINE_READ(engine, RING_CTL),
		 ENGINE_READ(engine, RING_CTL) & (RING_WAIT | RING_WAIT_SEMAPHORE) ? " [waiting]" : "");
	i_printf(m, indent, "RING_MODE:  0x%08x%s\n",
		 ENGINE_READ(engine, RING_MI_MODE),
		 ENGINE_READ(engine, RING_MI_MODE) & (MODE_IDLE) ? " [idle]" : "");
	i_printf(m, indent, "RING_NOPID: 0x%08x\n",
		 ENGINE_READ(engine, RING_NOPID));

	i_printf(m, indent, "RING_IMR:   0x%08x\n",
		 ENGINE_READ(engine, RING_IMR));
	i_printf(m, indent, "RING_ESR:   0x%08x\n",
		 ENGINE_READ(engine, RING_ESR));
	i_printf(m, indent, "RING_EMR:   0x%08x\n",
		 ENGINE_READ(engine, RING_EMR));
	i_printf(m, indent, "RING_EIR:   0x%08x\n",
		 ENGINE_READ(engine, RING_EIR));

	addr = intel_engine_get_active_head(engine);
	i_printf(m, indent, "ACTHD:  0x%08x_%08x\n",
		 upper_32_bits(addr), lower_32_bits(addr));
	addr = intel_engine_get_last_batch_head(engine);
	i_printf(m, indent, "BBADDR: 0x%08x_%08x\n",
		 upper_32_bits(addr), lower_32_bits(addr));
	addr = ENGINE_READ64(engine, RING_DMA_FADD, RING_DMA_FADD_UDW);
	i_printf(m, indent, "DMA_FADDR: 0x%08x_%08x\n",
		 upper_32_bits(addr), lower_32_bits(addr));
	i_printf(m, indent, "IPEIR: 0x%08x\n",
		 ENGINE_READ(engine, RING_IPEIR));
	i_printf(m, indent, "IPEHR: 0x%08x\n",
		 ENGINE_READ(engine, RING_IPEHR));
}

static void print_request_ring(struct drm_printer *m, int indent, struct i915_request *rq)
{
	const unsigned int limit = 1024;
	unsigned int size;
	void *ring;

	i_printf(m, indent,
		 "Ring: { head %04x, postfix %04x, tail %04x, batch 0x%08x_%08x }\n",
		 rq->head, rq->postfix, rq->tail,
		 rq->batch ? upper_32_bits(rq->batch->node.start) : ~0u,
		 rq->batch ? lower_32_bits(rq->batch->node.start) : ~0u);

	size = rq->tail - rq->head;
	if (rq->tail < rq->head)
		size += rq->ring->size;
	size = min(size, limit);

	ring = kmalloc(size, GFP_ATOMIC);
	if (ring) {
		const void *vaddr = rq->ring->vaddr;
		unsigned int head = rq->head;
		unsigned int len = 0;

		if (rq->tail < head) {
			len = min(rq->ring->size - head, limit);
			memcpy(ring, vaddr + head, len);
			head = 0;
		}
		memcpy(ring + len, vaddr + head, size - len);

		hexdump(m, indent + 2, ring, size);
		kfree(ring);
	}
}

static unsigned long list_count(struct list_head *list)
{
	struct list_head *pos;
	unsigned long count = 0;

	list_for_each(pos, list)
		count++;

	return count;
}

static unsigned long read_ul(void *p, size_t x)
{
	return *(unsigned long *)(p + x);
}

static void print_properties(struct intel_engine_cs *engine,
			     struct drm_printer *m,
			     int indent)
{
	static const struct pmap {
		size_t offset;
		const char *name;
	} props[] = {
#define P(x) { \
	.offset = offsetof(typeof(engine->props), x), \
	.name = #x \
}
		P(heartbeat_interval_ms),
		P(max_busywait_duration_ns),
		P(preempt_timeout_ms),
		P(stop_timeout_ms),
		P(timeslice_duration_ms),

		{},
#undef P
	};
	const struct pmap *p;

	i_printf(m, indent, "Properties:\n");
	for (p = props; p->name; p++)
		i_printf(m, indent + 2, "%s: %lu [default %lu]\n",
			 p->name,
			 read_ul(&engine->props, p->offset),
			 read_ul(&engine->defaults, p->offset));
}

static void engine_dump_request(struct intel_engine_cs *engine, struct i915_request *rq,
				struct drm_printer *m, int indent)
{
	u32 lrc;

	i915_request_show(m, rq, "", indent);
	if (!__i915_request_has_started(rq) || i915_request_signaled(rq))
		return;

	lrc = 0;
	if (!IS_SRIOV_VF(engine->i915))
		lrc = ENGINE_READ(engine, RING_CURRENT_LRCA);
	if (lrc & CURRENT_LRCA_VALID &&
	    (rq->context->lrc.lrca ^ lrc) & GENMASK(31, 12))
		return;

	indent += 2;
	print_request_ring(m, indent, rq);
	intel_context_show(rq->context, m, indent);
}

static void
engine_dump_active_requests(struct intel_engine_cs *engine,
			    struct drm_printer *m,
			    int indent)
{
	struct i915_request *rq, *last;
	struct i915_sched_engine *se;
	const unsigned int max = 8;
	unsigned long flags;
	unsigned int count;

	se = engine->sched_engine;
	if (!se)
		return;

	/*
	 * No need for an engine->irq_seqno_barrier() before the seqno reads.
	 * The GPU is still running so requests are still executing and any
	 * hardware reads will be out of date by the time they are reported.
	 * But the intention here is just to report an instantaneous snapshot
	 * so that's fine.
	 */

	spin_lock_irqsave(&se->lock, flags);

	count = 0;
	last = NULL;
	i_printf(m, indent, "Requests:\n");
	list_for_each_entry(rq, &se->requests, sched.link) {
		if (!(rq->execution_mask & engine->mask))
			continue;

		if (i915_request_signaled(rq))
			continue;

		if (__i915_request_is_complete(rq))
			continue;

		if (count++ < max - 1)
			engine_dump_request(engine, rq, m, indent + 2);
		else
			last = rq;
	}
	if (last) {
		if (count > max) {
			i_printf(m, indent + 2,
				   "... skipping %d executing requests... \n",
				   count - max);
		}
		engine_dump_request(engine, last, m, indent + 2);
	}

	i_printf(m, indent, "On hold?: %lu\n", list_count(&se->hold));
	spin_unlock_irqrestore(&se->lock, flags);
}

void intel_engine_dump(struct intel_engine_cs *engine,
		       struct drm_printer *m,
		       int indent)
{
	struct i915_gpu_error * const error = &engine->i915->gpu_error;
	struct i915_request *rq;
	intel_wakeref_t wf;
	ktime_t dummy;

	i_printf(m, indent, "%s:\n", engine->name);
	indent += 2;

	i_printf(m, indent, "Awake? %d\n", atomic_read(&engine->wakeref.count));
	i_printf(m, indent, "Interrupts: { count: %lu, total: %lluns, avg: %luns, max: %luns }\n",
		 READ_ONCE(engine->stats.irq.count),
		 READ_ONCE(engine->stats.irq.total),
		 ewma_irq_time_read(&engine->stats.irq.avg),
		 READ_ONCE(engine->stats.irq.max));
	if (HAS_RECOVERABLE_PAGE_FAULT(engine->i915)) {
		i_printf(m, indent, "Pagefault: { depth: %d }\n",
			 atomic_read(&engine->in_pagefault));
	}
	i_printf(m, indent, "Barriers?: %s\n",
		 str_yes_no(!list_empty(&engine->barrier_tasks)));
	i_printf(m, indent, "Latency: %luus\n",
		 ewma__engine_latency_read(&engine->latency));
	if (intel_engine_supports_stats(engine))
		i_printf(m, indent, "Runtime: %llums\n",
			 ktime_to_ms(intel_engine_get_busy_time(engine,
								&dummy)));
	i_printf(m, indent, "Forcewake: %x domains, %d active\n",
		 engine->fw_domain, READ_ONCE(engine->fw_active));

	rcu_read_lock();
	rq = READ_ONCE(engine->heartbeat.systole);
	if (rq)
		i_printf(m, indent, "Heartbeat: %d ms ago\n",
			 jiffies_to_msecs(jiffies - rq->emitted_jiffies));
	else if (work_pending(&engine->heartbeat.work.work))
		i_printf(m, indent, "Heartbeat: pending @ %lu interrupts\n",
			 engine->heartbeat.interrupts);
	else
		i_printf(m, indent, "Heartbeat: idle\n");
	rcu_read_unlock();
	i_printf(m, indent, "Reset count: %d (global %d)\n",
		 i915_reset_engine_count(engine),
		 i915_reset_count(error));
	print_properties(engine, m, indent);

	wf = intel_runtime_pm_get_if_in_use(engine->uncore->rpm);
	if (wf) {
		intel_engine_print_registers(engine, m, indent);
		intel_runtime_pm_put(engine->uncore->rpm, wf);
	}

	engine_dump_active_requests(engine, m, indent);

	if (engine->status_page.vma && (wf = intel_gt_pm_get_if_awake(engine->gt))) {
		i_printf(m, indent, "HWSP [0x%08x,0x%08llx):\n",
			 i915_ggtt_offset(engine->status_page.vma),
			 i915_ggtt_offset(engine->status_page.vma) + engine->status_page.vma->node.size);
		hexdump(m, indent + 2, engine->status_page.addr, PAGE_SIZE);
		intel_gt_pm_put_async(engine->gt, wf);
	}

	i_printf(m, indent, "Idle? %s\n", str_yes_no(intel_engine_is_idle(engine)));
}

/**
 * intel_engine_get_busy_time() - Return current accumulated engine busyness
 * @engine: engine to report on
 * @now: monotonic timestamp of sampling
 *
 * Returns accumulated time @engine was busy since engine stats were enabled.
 */
ktime_t intel_engine_get_busy_time(struct intel_engine_cs *engine, ktime_t *now)
{
	if (!engine->busyness || !intel_engine_supports_stats(engine))
		return 0;

	return engine->busyness(engine, now);
}

/**
 * intel_engine_get_busy_ticks() - Return current accumulated engine busyness
 * ticks
 * @engine: engine to report on
 * @vf_id: function to report on
 *
 * Returns accumulated ticks @engine was busy since engine stats were enabled.
 */
u64 intel_engine_get_busy_ticks(struct intel_engine_cs *engine, unsigned int vf_id)
{
	if (!engine->busyness_ticks ||
	    !(engine->flags & I915_ENGINE_SUPPORTS_TICKS_STATS))
		return 0;

	return engine->busyness_ticks(engine, vf_id);
}

struct intel_context *
intel_engine_create_virtual(struct intel_engine_cs **siblings,
			    unsigned int count, unsigned long flags)
{
	if (count == 0)
		return ERR_PTR(-EINVAL);

	if (count == 1 && !(flags & FORCE_VIRTUAL))
		return intel_context_create(siblings[0]);

	GEM_BUG_ON(!siblings[0]->cops->create_virtual);
	return siblings[0]->cops->create_virtual(siblings, count, flags);
}

struct i915_request *
intel_engine_find_active_request(struct intel_engine_cs *engine)
{
	struct i915_request *request, *active = NULL;
	struct i915_sched_engine *se;
	unsigned long flags;

	/*
	 * We are called by the error capture, reset and to dump engine
	 * state at random points in time. In particular, note that neither is
	 * crucially ordered with an interrupt. After a hang, the GPU is dead
	 * and we assume that no more writes can happen (we waited long enough
	 * for all writes that were in transaction to be flushed) - adding an
	 * extra delay for a recent interrupt is pointless. Hence, we do
	 * not need an engine->irq_seqno_barrier() before the seqno reads.
	 * At all other times, we must assume the GPU is still running, but
	 * we only care about the snapshot of this moment.
	 */

	rcu_read_lock();
	request = execlists_active(&engine->execlists);
	if (request) {
		struct intel_timeline *tl = request->context->timeline;

		list_for_each_entry_from_reverse(request, &tl->requests, link) {
			if (__i915_request_is_complete(request))
				break;

			if (request->execution_mask & engine->mask)
				active = request;
		}
	}
	rcu_read_unlock();
	if (active)
		return active;

	se = engine->sched_engine;
	if (!se)
		return active;

	spin_lock_irqsave(&se->lock, flags);
	list_for_each_entry(request, &se->requests, sched.link) {
		if (!(request->execution_mask & engine->mask))
			continue;

		if (__i915_request_is_complete(request))
			continue;

		if (__i915_request_has_started(request)) {
			active = request;
			break;
		}
	}
	spin_unlock_irqrestore(&se->lock, flags);

	return active;
}

void xehp_enable_ccs_engines(struct intel_engine_cs *engine)
{
	/*
	 * If there are any non-fused-off CCS engines, we need to enable CCS
	 * support in the RCU_MODE register.  This only needs to be done once,
	 * so for simplicity we'll take care of this in the RCS engine's
	 * resume handler; since the RCS and all CCS engines belong to the
	 * same reset domain and are reset together, this will also take care
	 * of re-applying the setting after i915-triggered resets.
	 */
	if (!CCS_MASK(engine->gt))
		return;

	intel_uncore_write(engine->uncore, GEN12_RCU_MODE,
			   _MASKED_BIT_ENABLE(GEN12_RCU_MODE_CCS_ENABLE));
}

static void
intel_engine_reset_failed_uevent_work(struct work_struct *work)
{
	struct intel_engine_cs *engine =
		container_of(work, typeof(*engine), reset.notify_reset_failed);
	struct kobject *kobj = &engine->i915->drm.primary->kdev->kobj;
	char *reset_event[5];

	reset_event[0] = PRELIM_I915_RESET_FAILED_UEVENT "=1";
	reset_event[1] = kasprintf(GFP_KERNEL, "RESET_ENABLED=%d",
				   intel_has_reset_engine(engine->gt) ? 1 : 0);
	reset_event[2] = "RESET_UNIT=engine";
	reset_event[3] = kasprintf(GFP_KERNEL, "RESET_ID=%s", engine->name);
	reset_event[4] = NULL;
	kobject_uevent_env(kobj, KOBJ_CHANGE, reset_event);

	kfree(reset_event[1]);
	kfree(reset_event[3]);
}

void
intel_engine_reset_failed_uevent(struct intel_engine_cs *engine)
{
	schedule_work(&engine->reset.notify_reset_failed);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "mock_engine.c"
#include "selftest_engine.c"
#include "selftest_engine_cs.c"
#include "selftest_engine_mi.c"
#endif
