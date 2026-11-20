/*
 * shennong (Xiaomi 14 Pro, Snapdragon 8 Gen 3 / SM8650): pasted into xnuloader/loader.c next to platform_redwood.
 * Values from mainline Linux arch/arm64/boot/dts/qcom/sm8650.dtsi (SoC-level, the same on every SM8650 phone);
 * Confirmed on the phone (OS3.0.308.0.WNBCNXM): GIC and cores from its device tree and /proc/iomem,
 * the framebuffer from its splash_region.
 */
static const struct platform platform_shennong = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "shennong", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17100000, .gic_redist = 0x17180000,   /* intc: GICD, GICR (8 x 0x40000) */
	.gic_redist_stride = 0x40000,                        /* its device tree's redistributor-stride: 4 frames a core */
	.uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,                         /* cpu@0 ... cpu@700: MPIDR Aff1 */
	/*
	 * cpu0-1: Cortex-A520 (capacity 1024), cpu2-6: Cortex-A720 (2909), cpu7: Cortex-X4 (3591).
	 * Only the two A520s count as small for XNU's slow-core partner logic.
	 */
	.small_cores = 0x03,
	.fb_base = 0xFC400000,                               /* splash_region ("cont_splash_region") in its device tree */
};
