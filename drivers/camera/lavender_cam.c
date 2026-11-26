/*
 * lavender: camera (CAMSS SDM660) platform probe, F0. Boot-arg gated.
 * 1001b: the camera pins live in the TLMM south tile (0x03100000); 1001
 * read 0x03000000 and got zeros. SMMU IDR fields decoded per arm-smmu.h.
 *
 *   lvcam=1         run the F0 read-only probe: the MMCC camera power
 *                   domains, clock branches, MMPLL10 and the camera RCGs as
 *                   the bootloader left them, the camera pins, the MMSS SMMU
 *                   identity, and (only when the CAMSS domain and its AHB
 *                   clock are already on) the blocks' hardware versions
 *   lvcam=2         ... and when the CAMSS domain is off, switch it on the
 *                   way Linux does (the camss_top GDSC, then the AHB
 *                   branches), read the CSI clock muxes and the CSIPHY
 *                   register interfaces, and switch back off exactly what
 *                   this probe switched on
 *   lvcam=3         ... and power camera@0 (reset low, VANA on gpio46, VDIG
 *                   on PM660L pad 3, reset high) and walk the sensor bus by
 *                   hand on the CCI pins, printing every address that answers
 *                   and the two chip IDs it can read; the rails go off again
 *                   afterwards. Only camera@0 (lvcamsel=0) in this build.
 *   lvcam=4         the same rails without the PM660L step (VANA and the
 *                   reset only, VDIG left as found; lvcamsmoke=1 forces
 *                   this at lvcam=3). 1011 prints one line before every
 *                   access and gives the flusher 300 ms, so the last line
 *                   of a frozen boot names the access that wedged it.
 *   lvcamdelay=S    seconds to wait after boot before the probe (default 25),
 *                   so the console ring flusher and the USB live log are up
 *   lvcamspmi=S     (1012) spawn a helper thread that reads the SPMI
 *   lvcamspmi2=S    arbiter version S seconds after the probe thread starts
 *                   (the second one too, if given); a stalled read loses the
 *                   helper only, so one boot shows whether the arbiter
 *                   answers before this probe touches anything. 1013 stops
 *                   unmapping: ml_io_unmap() panics in this port
 *                   (pmap_remove_options: invalid address range).
 *   lvcamsel=0/1/2  the camera the driver works on from F1 on: 0 the rear
 *                   main (camera@0, csiphy0/csid0, CCI master 0), 1 the
 *                   second rear (camera@1, csiphy1/csid1, master 1, has
 *                   VAF), 2 the front (camera@2, csiphy2/csid2, master 1,
 *                   mount 270). F0 only prints the choice.
 *
 * lvcam=1 READS ONLY. The CAMSS blocks sit behind the MMCC power domain
 * camss_top and its AHB clock; a block read with either one off can hang
 * the bus, so CAMSS registers are only read when the GDSCR reports PWR_ON
 * without SW_COLLAPSE and the AHB branches are running, the rule the
 * audio probe applies to LPASS. MMCC itself, TLMM and the SMMU's AHB clock
 * bit are always readable. CCI, CSID and VFE are not read at either step:
 * each needs its own clocks, and in SDM660 the CSID is inside the VFE's
 * power domain (F1 and F2 bring them up with those).
 *
 * lvcam=2 changes five MMCC registers, each change printed and read back:
 * the camss_top GDSCR (SW_COLLAPSE cleared), camss_top_ahb, camss_ahb,
 * ispif_ahb and csiphy_ahb2crif (CLOCK_ENABLE set). Whatever was already
 * on is left alone. At the end the branches this probe enabled are
 * disabled again and, when this probe powered the domain, it is
 * collapsed again. The VFE GDSCs, every RCG, the bimc_smmu GDSC (which
 * mmcc-sdm660.c says hangs the multimedia subsystem) and the sensor's
 * rails, pins and MCLK are not touched: those are F1.
 *
 * The questions F0 answers decide the port:
 *   - did the bootloader leave camss_top (and the VFE GDSCs) on?
 *   - which camera clock branches are enabled, and is MMPLL10 (the 24 MHz
 *     MCLK source) locked?
 *   - does the camss_top domain come up when asked, do its AHB branches
 *     stop, and do the CSI clock muxes and the CSIPHY register interfaces
 *     answer (their hardware versions, CCI/CSID/VFE, come in F1 and F2)?
 *   - how are the camera GPIOs left (MCLK 32/33/35, CCI 36-39, resets
 *     47/48/52, standby 45, VAF 50, all in the TLMM's south tile)?
 *   - is the MMSS SMMU (0xcd00000, the one the VFE writes through) up?
 *
 * Register meanings: Linux gdsc.c, clk-branch.c, clk-rcg2.c,
 * clk-alpha-pll.c, mmcc-sdm660.c, pinctrl-msm.c/pinctrl-sdm660.c and the
 * mainline camss driver for SDM660, read as a behaviour reference (values
 * only, no code copied); DTB values from resources/lavender_dtb.bin (the
 * phone's DTBO is a stub, so the base DTB is the only truth).
 */
#if defined(ARM64_BOARD_CONFIG_LAVENDER)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define CAM_NITEMS(a)   (sizeof(a) / sizeof((a)[0]))

/* MMCC (clock-controller@c8c0000): every camera clock lives here, none in
 * GCC. */
#define CAM_MMCC_PA             0x0c8c0000u
#define CAM_MMCC_SIZE           0x00040000u

/* GDSCR (gdsc.c): PWR_ON bit 31, SW_COLLAPSE bit 0. */
#define CAM_GDSC_PWR_ON         (1u << 31)
#define CAM_GDSC_SW_COLLAPSE    (1u << 0)
#define CAM_MMCC_CAMSS_TOP_GDSCR    0x34a0u
#define CAM_MMCC_CAMSS_VFE0_GDSCR   0x3664u
#define CAM_MMCC_CAMSS_VFE1_GDSCR   0x3674u
#define CAM_MMCC_CAMSS_CPP_GDSCR    0x36d4u
/* Commented out of mmcc-sdm660.c because it hangs the subsystem: read only. */
#define CAM_MMCC_BIMC_SMMU_GDSCR    0xe020u

/* CBCR (clk-branch.h): CLOCK_ENABLE bit 0, CLK_OFF bit 31. camss_ahb's
 * hardware clock gating bit is bit 1 of its own CBCR. */
#define CAM_CBCR_EN             (1u << 0)
#define CAM_CBCR_HWCG           (1u << 1)
#define CAM_CBCR_CLK_OFF        (1u << 31)
#define CAM_MMCC_CAMSS_AHB_CBCR     0x348cu
#define CAM_MMCC_BIMC_SMMU_AHB_CBCR 0xe004u
#define CAM_MMCC_BIMC_SMMU_AXI_CBCR 0xe008u

/* MMPLL10: MODE at the PLL offset, STATUS at +0x24 (clk-alpha-pll.c, the
 * DEFAULT type); LOCK_DET is STATUS bit 31, MODE bits 0/1/2 are
 * OUTCTRL/BYPASSNL/RESET_N. */
#define CAM_MMCC_MMPLL10_MODE       0x0190u
#define CAM_MMCC_MMPLL10_STATUS     0x01b4u
#define CAM_PLL_LOCK_DET            (1u << 31)

/* RCG2: CMD at cmd_rcgr (UPDATE bit 0, ROOT_EN bit 1, ROOT_OFF bit 31),
 * CFG at +4 (SRC_DIV bits 7:0, SRC_SEL bits 10:8, MODE bits 13:12). */
#define CAM_RCG_CMD_UPDATE      (1u << 0)
#define CAM_RCG_CMD_ROOT_EN     (1u << 1)
#define CAM_RCG_CMD_ROOT_OFF    (1u << 31)

/* CAMSS blocks on one bus window, 0xca00000: the ispif's csi_clk_mux
 * 0x00020, the CSIPHYs' clk muxes 0x00120-128, and (F1/F2) cci 0x0c000,
 * vfe0/1 0x10000/0x14000, csid 0x30000+0x400*i. */
#define CAM_CAMSS_PA            0x0ca00000u
#define CAM_CAMSS_SIZE          0x00050000u
#define CAM_CAMSS_CSI_CLK_MUX   0x00020u
#define CAM_CAMSS_CSIPHY_CLK_MUX(n)  (0x00120u + 4u * (n))

/* CSIPHY0..2 (v3.5) at 0xc824000, 0x1000 apart; common registers at +0x800. */
#define CAM_CSIPHY_PA           0x0c824000u
#define CAM_CSIPHY_SIZE         0x00003000u
#define CAM_CSIPHY_STRIDE       0x1000u
#define CAM_CSIPHY_CTRL5        0x00814u     /* data lanes + CLK_EN bit 7 */
#define CAM_CSIPHY_CTRL6        0x00818u     /* bit 0 PWRDN_B */
#define CAM_CSIPHY_STATUS0      0x008b0u     /* interrupt status 0 */

/* TLMM: three 4 MB tiles looked up by name (mainline sdm630.dtsi):
 * south 0x03100000, center 0x03500000, north 0x03900000. The camera pins
 * (32-52) are all in the south tile: 0x1000 per GPIO, CTL at +0 (pull 1:0,
 * func 4:2, drive 8:6, OE 9), IO at +4 (in bit 0, out bit 1).
 * Device result (1001, lvcam=1): the vendor DTB declares one 0xc00000 range
 * from 0x03000000, but the pin registers start at the tile bases; reading
 * 0x03000000 + 0x1000*n returned zeros, so 1001b moves to the south base. */
#define CAM_TLMM_SOUTH_PA       0x03100000u
#define CAM_TLMM_SOUTH_SIZE     0x00040000u

/* MMSS SMMU (qcom,smmu-v2 at 0xcd00000): sCR0, IDR0..IDR1, s2cr0. */
#define CAM_SMMU_PA             0x0cd00000u
#define CAM_SMMU_SIZE           0x00040000u
#define CAM_SMMU_SCR0           0x00000u
#define CAM_SMMU_IDR0           0x00020u
#define CAM_SMMU_IDR1           0x00024u
#define CAM_SMMU_S2CR0          0x00c00u

/*
 * The AHB branches Linux enables before it reads a CAMSS block, in its
 * order (camss.c, the csiphy/ispif resources of SDM660). BRANCH_HALT ones
 * report CLK_OFF; camss_ahb also has the hardware clock gating bit.
 */
static const struct {
	uint32_t off;
	const char *name;
	uint32_t hwcg;
} cam_ahb[] = {
	{ 0x3484u, "camss_top_ahb", 0 },
	{ 0x348cu, "camss_ahb", CAM_CBCR_HWCG },
	{ 0x3224u, "ispif_ahb", 0 },
	{ 0x374cu, "csiphy_ahb2crif", 0 },
};

static vm_offset_t mmcc, camss, csiphy, tlmm, smmu_mmss;
static int cam_domain_on;       /* camss_top was on when first read */
static int cam_domain_by_us;    /* this probe switched camss_top on */
static int cam_ahb_on;          /* the AHB path to the blocks is running */
static uint32_t cam_ahb_saved[CAM_NITEMS(cam_ahb)];
static int cam_ahb_by_us[CAM_NITEMS(cam_ahb)];
static uint32_t cam_step, cam_sel;
static int cam_chan;

static inline uint32_t
cam_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static void
cam_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
cam_wait(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&cam_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
cam_announce(const char *what)
{
	printf("SWEET CAM: about to %s\n", what);
	cam_wait(2000);
}

/* Wait up to ms milliseconds for a register field to read "want". */
static int
cam_poll(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want,
    unsigned ms)
{
	unsigned i;

	for (i = 0; i < ms; i++) {
		if ((cam_rd(base, off) & mask) == want) {
			return 0;
		}
		cam_wait(1);
	}
	return ETIMEDOUT;
}

static void
cam_dt_facts(void)
{
	static const struct { const char *name; const char *desc; } s[] = {
		{ "camera@0", "rear main: csiphy0/csid0, cci-master 0, mclk0 "
		    "gpio32 reset0 gpio48, eeproms ov02a10/s5k5e8" },
		{ "camera@1", "rear 2: csiphy1/csid1, cci-master 1, mclk3 "
		    "gpio35 reset3 gpio52 vaf gpio50, eeprom s5k5e8" },
		{ "camera@2", "front: csiphy2/csid2, cci-master 1, mclk1 "
		    "gpio33 reset1 gpio47 stby1 gpio45, mount 270" },
	};

	printf("SWEET CAM: lvcamsel=%u -> %s (%s)\n", cam_sel,
	    s[cam_sel].name, s[cam_sel].desc);
}

static void
cam_gdsc(void)
{
	static const struct { uint32_t off; const char *name; } d[] = {
		{ CAM_MMCC_CAMSS_TOP_GDSCR, "camss_top" },
		{ CAM_MMCC_CAMSS_VFE0_GDSCR, "camss_vfe0" },
		{ CAM_MMCC_CAMSS_VFE1_GDSCR, "camss_vfe1" },
		{ CAM_MMCC_CAMSS_CPP_GDSCR, "camss_cpp" },
		{ CAM_MMCC_BIMC_SMMU_GDSCR, "bimc_smmu (read only)" },
	};
	uint32_t gdscr;
	unsigned i;

	cam_announce("read the MMCC camera power domains (read only)");
	for (i = 0; i < CAM_NITEMS(d); i++) {
		gdscr = cam_rd(mmcc, d[i].off);
		printf("SWEET CAM: mmcc %s gdscr %08x (pwr_on %u, collapse %u)\n",
		    d[i].name, gdscr, (gdscr & CAM_GDSC_PWR_ON) != 0,
		    (gdscr & CAM_GDSC_SW_COLLAPSE) != 0);
	}
	gdscr = cam_rd(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR);
	cam_domain_on = (gdscr & CAM_GDSC_PWR_ON) != 0 &&
	    (gdscr & CAM_GDSC_SW_COLLAPSE) == 0;
	printf("SWEET CAM: camss_top domain is %s\n",
	    cam_domain_on ? "ON" : "OFF");
}

static void
cam_branches(void)
{
	static const struct { uint32_t off; const char *name; } b[] = {
		{ 0x5024u, "mnoc_ahb" },
		{ CAM_MMCC_BIMC_SMMU_AHB_CBCR, "bimc_smmu_ahb" },
		{ CAM_MMCC_BIMC_SMMU_AXI_CBCR, "bimc_smmu_axi" },
		{ 0x3484u, "camss_top_ahb" },
		{ CAM_MMCC_CAMSS_AHB_CBCR, "camss_ahb" },
		{ 0x3224u, "ispif_ahb" },
		{ 0x374cu, "csiphy_ahb2crif" },
		{ 0x3344u, "cci" },
		{ 0x3348u, "cci_ahb" },
		{ 0x3384u, "mclk0" },
		{ 0x33b4u, "mclk1" },
		{ 0x3414u, "mclk3" },
		{ 0x3740u, "csiphy0" },
		{ 0x3744u, "csiphy1" },
		{ 0x3748u, "csiphy2" },
		{ 0x30b4u, "csi0" },
		{ 0x30bcu, "csi0_ahb" },
		{ 0x30d4u, "csi0_rdi" },
		{ 0x30e4u, "csi0_pix" },
		{ 0x3730u, "cphy_csid0" },
		{ 0x3668u, "vfe0_ahb" },
		{ 0x36a8u, "vfe0" },
		{ 0x3720u, "vfe0_stream" },
		{ 0x3704u, "csi_vfe0" },
		{ 0x3678u, "vfe1_ahb" },
		{ 0x36acu, "vfe1" },
		{ 0x3724u, "vfe1_stream" },
		{ 0x3714u, "csi_vfe1" },
		{ 0x36b8u, "vfe_vbif_ahb" },
		{ 0x36bcu, "vfe_vbif_axi" },
		{ 0x3c3cu, "throttle_camss_axi" },
	};
	uint32_t cbcr;
	unsigned i;

	cam_announce("read the MMCC camera branches (read only)");
	for (i = 0; i < CAM_NITEMS(b); i++) {
		cbcr = cam_rd(mmcc, b[i].off);
		printf("SWEET CAM: mmcc %s cbcr %08x (en %u, %s)\n", b[i].name,
		    cbcr, cbcr & CAM_CBCR_EN,
		    (cbcr & CAM_CBCR_CLK_OFF) ? "OFF" : "on");
	}
}

static void
cam_plls_rcg(void)
{
	static const struct { uint32_t off; const char *name; } r[] = {
		{ 0x3360u, "mclk0_src" },
		{ 0x3390u, "mclk1_src" },
		{ 0x33f0u, "mclk3_src" },
		{ 0x3300u, "cci_src" },
		{ 0x3090u, "csi0_src" },
		{ 0x3160u, "csi2_src" },
		{ 0x3800u, "csiphy_src" },
		{ 0x3000u, "csiphy0_timer_src" },
		{ 0x3600u, "vfe0_src" },
	};
	uint32_t mode, status, cmd, cfg;
	unsigned i;

	cam_announce("read MMPLL10 and the camera RCGs (read only)");
	mode = cam_rd(mmcc, CAM_MMCC_MMPLL10_MODE);
	status = cam_rd(mmcc, CAM_MMCC_MMPLL10_STATUS);
	printf("SWEET CAM: mmpll10 mode %08x status %08x (outctrl %u, "
	    "bypassnl %u, reset_n %u, lock %u)\n", mode, status, mode & 1u,
	    (mode >> 1) & 1u, (mode >> 2) & 1u,
	    (status & CAM_PLL_LOCK_DET) != 0);
	for (i = 0; i < CAM_NITEMS(r); i++) {
		cmd = cam_rd(mmcc, r[i].off);
		cfg = cam_rd(mmcc, r[i].off + 4);
		printf("SWEET CAM: mmcc %s cmd %08x cfg %08x (root_en %u, "
		    "root_off %u, src %u, div %u, mode %u)\n", r[i].name, cmd,
		    cfg, (cmd & CAM_RCG_CMD_ROOT_EN) != 0,
		    (cmd & CAM_RCG_CMD_ROOT_OFF) != 0, (cfg >> 8) & 0x7u,
		    cfg & 0xffu, (cfg >> 12) & 0x3u);
	}
}

/* Is the AHB path to the CAMSS blocks running? camss_ahb in hardware clock
 * gating mode may read CLK_OFF while idle, so its gating bit counts too. */
static int
cam_ahb_running(void)
{
	uint32_t top = cam_rd(mmcc, cam_ahb[0].off);
	uint32_t ahb = cam_rd(mmcc, cam_ahb[1].off);

	return (top & CAM_CBCR_CLK_OFF) == 0 &&
	    ((ahb & CAM_CBCR_CLK_OFF) == 0 || (ahb & cam_ahb[1].hwcg) != 0);
}

/*
 * Linux gdsc_toggle_logic(on): clear SW_COLLAPSE, then poll the status.
 * If the domain never reports on, the collapse bit goes back and the
 * caller skips every write and read of the CAMSS blocks.
 */
static int
cam_domain_power_up(void)
{
	uint32_t gdscr;

	if (cam_domain_on) {
		printf("SWEET CAM: camss_top was already on; nothing to "
		    "write\n");
		return 0;
	}
	cam_announce("clear camss_top SW_COLLAPSE (lvcam>=2)");
	gdscr = cam_rd(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR);
	printf("SWEET CAM: camss_top gdscr before %08x\n", gdscr);
	cam_wr(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR, gdscr & ~CAM_GDSC_SW_COLLAPSE);
	if (cam_poll(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR,
	    CAM_GDSC_PWR_ON | CAM_GDSC_SW_COLLAPSE, CAM_GDSC_PWR_ON, 200)) {
		gdscr = cam_rd(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR);
		printf("SWEET CAM: camss_top gdscr %08x (STILL OFF); putting "
		    "the collapse back\n", gdscr);
		cam_wr(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR,
		    gdscr | CAM_GDSC_SW_COLLAPSE);
		return EIO;
	}
	gdscr = cam_rd(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR);
	printf("SWEET CAM: camss_top gdscr after %08x (on)\n", gdscr);
	cam_domain_by_us = 1;
	return 0;
}

/* Linux clk_branch_enable: set CLOCK_ENABLE and, unless the branch is in
 * hardware clock gating mode, wait for CLK_OFF to clear. */
static void
cam_ahb_power_up(void)
{
	uint32_t cbcr;
	unsigned i;

	cam_announce("enable the CAMSS AHB clock branches (lvcam>=2)");
	for (i = 0; i < CAM_NITEMS(cam_ahb); i++) {
		cbcr = cam_rd(mmcc, cam_ahb[i].off);
		cam_ahb_saved[i] = cbcr;
		if (cbcr & CAM_CBCR_EN) {
			printf("SWEET CAM: mmcc %s already enabled (cbcr "
			    "%08x)\n", cam_ahb[i].name, cbcr);
			continue;
		}
		cam_wr(mmcc, cam_ahb[i].off, cbcr | CAM_CBCR_EN);
		if (((cbcr & cam_ahb[i].hwcg) == 0) &&
		    cam_poll(mmcc, cam_ahb[i].off, CAM_CBCR_CLK_OFF, 0, 20)) {
			printf("SWEET CAM: mmcc %s did not come on (cbcr "
			    "%08x); putting it back\n", cam_ahb[i].name,
			    cam_rd(mmcc, cam_ahb[i].off));
			cam_wr(mmcc, cam_ahb[i].off, cbcr);
			continue;
		}
		cam_ahb_by_us[i] = 1;
		printf("SWEET CAM: mmcc %s on (cbcr %08x)\n", cam_ahb[i].name,
		    cam_rd(mmcc, cam_ahb[i].off));
	}
	cam_ahb_on = cam_ahb_running() ||
	    (cam_ahb_by_us[0] && cam_ahb_by_us[1]);
}

/* Linux clk_branch_disable: clear CLOCK_ENABLE, wait for CLK_OFF (not in
 * hardware clock gating mode), then the register goes back as found. */
static void
cam_ahb_power_down(void)
{
	uint32_t cbcr;
	unsigned i;

	cam_announce("put the CAMSS AHB clock branches back (lvcam>=2)");
	for (i = CAM_NITEMS(cam_ahb); i-- > 0; ) {
		if (!cam_ahb_by_us[i]) {
			continue;
		}
		cbcr = cam_rd(mmcc, cam_ahb[i].off);
		cam_wr(mmcc, cam_ahb[i].off, cbcr & ~CAM_CBCR_EN);
		if ((cam_ahb_saved[i] & cam_ahb[i].hwcg) == 0) {
			(void)cam_poll(mmcc, cam_ahb[i].off, CAM_CBCR_CLK_OFF,
			    CAM_CBCR_CLK_OFF, 20);
		}
		cam_wr(mmcc, cam_ahb[i].off, cam_ahb_saved[i]);
		cam_ahb_by_us[i] = 0;
		printf("SWEET CAM: mmcc %s back to %08x\n", cam_ahb[i].name,
		    cam_rd(mmcc, cam_ahb[i].off));
	}
	cam_ahb_on = 0;
}

static void
cam_domain_power_down(void)
{
	uint32_t gdscr;

	if (!cam_domain_by_us) {
		return;
	}
	cam_announce("collapse camss_top again (lvcam>=2)");
	gdscr = cam_rd(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR);
	cam_wr(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR,
	    gdscr | CAM_GDSC_SW_COLLAPSE);
	(void)cam_poll(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR, CAM_GDSC_PWR_ON, 0,
	    200);
	gdscr = cam_rd(mmcc, CAM_MMCC_CAMSS_TOP_GDSCR);
	printf("SWEET CAM: camss_top gdscr after %08x (%s)\n", gdscr,
	    (gdscr & CAM_GDSC_PWR_ON) ? "STILL ON" : "off");
	cam_domain_by_us = 0;
	cam_domain_on = 0;
}

/*
 * Only what camss_top and the AHB branches feed is read here: the CSI
 * clock muxes (the ispif's 0xca00020 and the CSIPHYs' 0xca00120-128) and
 * the CSIPHY register interfaces. CCI, CSID and VFE each need their own
 * clocks on top, and in SDM660 the CSID sits in the VFE's power domain
 * (mainline csid_set_power powers the VFE first), so their registers wait
 * for F1 and F2.
 */
static void
cam_muxes(void)
{
	uint32_t mux;
	unsigned i;

	cam_announce("read the CAMSS clock muxes (read only)");
	/* Each mux keeps a CSID index in bits 3:0 (camss-ispif.c,
	 * camss-csiphy.c). Only the registers the DTB declares are read. */
	mux = cam_rd(camss, CAM_CAMSS_CSI_CLK_MUX);
	printf("SWEET CAM: csi_pix_clk_mux %08x (csid %u)\n", mux,
	    mux & 0xfu);
	for (i = 0; i < 3; i++) {
		mux = cam_rd(camss, CAM_CAMSS_CSIPHY_CLK_MUX(i));
		printf("SWEET CAM: csiphy%u_clk_mux %08x (csid %u)\n", i, mux,
		    mux & 0xfu);
	}
}

static void
cam_csiphy(void)
{
	uint32_t ctrl5, ctrl6, status0;
	unsigned i;

	cam_announce("read the CSIPHY controls (read only)");
	for (i = 0; i < 3; i++) {
		ctrl5 = cam_rd(csiphy, i * CAM_CSIPHY_STRIDE + CAM_CSIPHY_CTRL5);
		ctrl6 = cam_rd(csiphy, i * CAM_CSIPHY_STRIDE + CAM_CSIPHY_CTRL6);
		status0 = cam_rd(csiphy, i * CAM_CSIPHY_STRIDE +
		    CAM_CSIPHY_STATUS0);
		printf("SWEET CAM: csiphy%u ctrl5 %08x (clk_en %u, lanes "
		    "%u%u%u%u) ctrl6 %08x (pwrdn_b %u) status0 %08x\n", i,
		    ctrl5, (ctrl5 >> 7) & 1u, ctrl5 & 1u, (ctrl5 >> 1) & 1u,
		    (ctrl5 >> 2) & 1u, (ctrl5 >> 3) & 1u, ctrl6, ctrl6 & 1u,
		    status0);
	}
}

static void
cam_gpios(void)
{
	static const uint32_t pins[] = { 32, 33, 35, 36, 37, 38, 39, 45, 47,
	    48, 50, 52 };
	static const char *const names[] = { "MCLK0", "MCLK1", "MCLK3",
	    "CCI_D0", "CCI_C0", "CCI_D1", "CCI_C1", "STBY1", "RESET1",
	    "RESET0", "VAF", "RESET3" };
	uint32_t ctl, io;
	unsigned i;

	cam_announce("read the camera TLMM pins (read only)");
	printf("SWEET CAM: tlmm south tile @%08x\n", CAM_TLMM_SOUTH_PA);
	for (i = 0; i < CAM_NITEMS(pins); i++) {
		ctl = cam_rd(tlmm, pins[i] * 0x1000u);
		io = cam_rd(tlmm, pins[i] * 0x1000u + 4u);
		printf("SWEET CAM: gpio%u %s ctl %08x io %08x (func %u, out "
		    "%u, drv %u, pull %u)\n", pins[i], names[i], ctl, io,
		    (ctl >> 2) & 0x7u, (ctl >> 9) & 1u, (ctl >> 6) & 0x7u,
		    ctl & 0x3u);
	}
}

static void
cam_smmu(void)
{
	uint32_t cbcr, scr0, idr0, idr1, s2cr0;

	cbcr = cam_rd(mmcc, CAM_MMCC_BIMC_SMMU_AHB_CBCR);
	if (cbcr & CAM_CBCR_CLK_OFF) {
		printf("SWEET CAM: mmss smmu: bimc_smmu_ahb is off (cbcr "
		    "%08x); SMMU reads skipped\n", cbcr);
		return;
	}
	cam_announce("read the MMSS SMMU identity (read only)");
	scr0 = cam_rd(smmu_mmss, CAM_SMMU_SCR0);
	idr0 = cam_rd(smmu_mmss, CAM_SMMU_IDR0);
	idr1 = cam_rd(smmu_mmss, CAM_SMMU_IDR1);
	s2cr0 = cam_rd(smmu_mmss, CAM_SMMU_S2CR0);
	/* sCR0 bit 0 CLIENTPD: 1 = the SMMU is not translating. IDR0: S1TS
	 * bit 30, S2TS 29, NTS 28, NUMSIDB 12:9, NUMSMRG 7:0; IDR1: NUMCB
	 * 7:0, NUMS2CB 23:16 (arm-smmu.h). */
	printf("SWEET CAM: mmss smmu scr0 %08x (clientpd %u) idr0 %08x "
	    "(s1ts %u, s2ts %u, nts %u, smr %u) idr1 %08x (numcb %u, "
	    "nums2cb %u) s2cr0 %08x\n", scr0, scr0 & 1u, idr0,
	    (idr0 >> 30) & 1u, (idr0 >> 29) & 1u, (idr0 >> 28) & 1u,
	    idr0 & 0xffu, idr1, idr1 & 0xffu, (idr1 >> 16) & 0xffu, s2cr0);
}

/*
 * ---------------------------------------------------------------------------
 * F1 (lvcam=3): the camera@0 rails, its reset and the sensor bus.
 *
 * The rails are not LDOs written through SPMI: in the DTB cam_vana is a fixed
 * 2.8 V regulator switched by TLMM gpio46, cam_vdig is a fixed 1.35 V
 * regulator switched by the PM660L's own GPIO3 (SPMI SID 2, pad 0xc2; the
 * consumer cell is 3, and qpnp-pin numbers pads from 1), and cam_vio is
 * pm660_l11, an RPM-controlled LDO the bootloader already votes (it is shared
 * with other peripherals). The sensor's reset is TLMM gpio48, active low; the
 * DTB's power sequence is reset low, VANA, VDIG, VIO, MCLK, reset high.
 *
 * The sensor bus is driven by hand on the CCI's pins, the way the hakios port
 * does it: gpio36 (SDA) and gpio37 (SCL) of CCI master 0 are taken as plain
 * GPIOs, open drain emulated with the internal pull-up (high = input with
 * pull-up, low = output 0; 5 us per half bit, 100 kHz), clock stretching
 * honoured, and the pins given back exactly as they were found. A bus held
 * down by a sensor that stopped mid-byte is unwedged with nine clocks, as
 * Linux's bit-banged master does. This reads the chip IDs of whatever answers
 * without programming the CCI block.
 *
 * The SPMI protocol (arbiter v2/v3, APID map at core+0x800, observer channels
 * for reads, RW channels for writes, APPS-owned APIDs only) is the one the
 * audio driver verified on this phone; it lives here so the camera probe does
 * not touch that shared file.
 */
#define CAM_SPMI_CORE_PA        0x0800f000u
#define CAM_SPMI_CHNLS_PA       0x08400000u      /* RW channels */
#define CAM_SPMI_OBS_PA         0x09400000u      /* observer (read) channels */
#define CAM_SPMI_CFG_PA         0x0800a000u
#define CAM_SPMI_APID_MAP       0x800u
#define CAM_SPMI_STRIDE         0x8000u
#define CAM_SPMI_NO_APID        0xffffu

/* 1013: never call ml_io_unmap() here. In this port it panics in
 * pmap_remove_options() ("invalid address range") on a mapping that came
 * from ml_io_map(), and no other driver unmaps. The probe runs once per
 * boot, so its few page mappings stay for the life of the system. */

/* PM660L (SID 2) camera@0 VDIG pad, and the qpnp-pin register offsets
 * pinctrl-spmi-gpio.c uses (there is no 0x08 STATUS on this peripheral). */
#define CAM_PM660L_SID          2u
#define CAM_PM660L_DVDD0        0x00c2u          /* pad 3 -> 0xc000 + 2*0x100 */
#define CAM_PMIC_GPIO_TYPE      0x04u
#define CAM_PMIC_GPIO_TYPE_GPIO 0x10u
#define CAM_PMIC_GPIO_RT_STS    0x10u            /* bit0: digital input level */
#define CAM_PMIC_GPIO_MODE      0x40u            /* bit0 out, 3:1 fun, 6:4 dir */
#define CAM_PMIC_GPIO_VIN       0x41u
#define CAM_PMIC_GPIO_EN        0x46u            /* bit7 master enable */
#define CAM_PMIC_GPIO_EN_BIT    0x80u
#define CAM_PMIC_GPIO_OUT_HIGH  0x11u            /* dir out (1<<4) | value */

/* TLMM bit layout of an sdm660 pad (see the top of the file). */
#define CAM_TLMM_OE             (1u << 9)
#define CAM_TLMM_PULL_UP        0x3u
#define CAM_TLMM_OUT_HIGH       0x2u             /* IO bit 1 = output value */

#define CAM_GPIO_RESET0         48u              /* camera@0 reset, low active */
#define CAM_GPIO_AVDD0          46u              /* camera@0 cam_vana, high */
#define CAM_GPIO_CCI_SDA0       36u              /* CCI master 0 data */
#define CAM_GPIO_CCI_SCL0       37u              /* CCI master 0 clock */


struct cam_pad {
	uint8_t type, rtsts, mode, vin, en;
};

static vm_offset_t spmi_core;
static int spmi_ready;
static uint32_t cam_avdd_cfg, cam_avdd_io, cam_rst_cfg, cam_rst_io;
static uint32_t cam_sda_cfg, cam_sda_io, cam_scl_cfg, cam_scl_io;
static struct cam_pad cam_dvdd;
static int cam_dvdd_saved;
static int cam_rails_on;
static int cam_no_pmic;

/* A short busy wait: the bit-banged bus needs microseconds, cam_wait() has
 * the thread sleep for whole milliseconds. */
static void
cam_spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static int
cam_spmi_init(void)
{
	uint32_t version;

	if (spmi_ready) {
		return 0;
	}
	printf("SWEET CAM: spmi: mapping the core\n");
	cam_wait(300);
	spmi_core = ml_io_map(CAM_SPMI_CORE_PA, 0x1000);
	if (spmi_core == 0) {
		printf("SWEET CAM: SPMI core map failed\n");
		return EIO;
	}
	printf("SWEET CAM: spmi: core mapped at %#lx\n", (unsigned long)spmi_core);
	cam_wait(300);
	printf("SWEET CAM: spmi: reading the arbiter version\n");
	version = cam_rd(spmi_core, 0);
	printf("SWEET CAM: spmi: version %#x\n", version);
	if (version < 0x20000000u || version >= 0x50000000u) {
		printf("SWEET CAM: SPMI arbiter %#x unsupported\n", version);
		return EIO;
	}
	spmi_ready = 1;
	printf("SWEET CAM: SPMI arbiter %#x\n", version);
	return 0;
}

/* PPID = (sid << 8) | peripheral, found in the 512-entry APID map. */
static uint16_t
cam_spmi_apid(unsigned sid, unsigned periph)
{
	uint32_t apid;

	for (apid = 0; apid < 512u; apid++) {
		uint32_t map = cam_rd(spmi_core, CAM_SPMI_APID_MAP + 4u * apid);

		if (map != 0 && ((map >> 8) & 0xfffu) ==
		    ((sid << 8) | periph)) {
			return (uint16_t)apid;
		}
	}
	return CAM_SPMI_NO_APID;
}

static int
cam_spmi_read(uint8_t sid, uint16_t addr, uint8_t *out)
{
	uint16_t apid;
	vm_offset_t chan;
	uint32_t cmd, status;
	unsigned i;

	if (cam_spmi_init() != 0) {
		return EIO;
	}
	apid = cam_spmi_apid(sid, addr >> 8);
	if (apid == CAM_SPMI_NO_APID) {
		printf("SWEET CAM: SPMI sid %u periph %#x has no APID\n", sid,
		    addr >> 8);
		return EIO;
	}
	chan = ml_io_map(CAM_SPMI_OBS_PA + CAM_SPMI_STRIDE * apid, 0x1000);
	if (chan == 0) {
		return EIO;
	}
	cmd = (1u << 27) | ((addr & 0xffu) << 4);       /* EXT_READL */
	cam_wr(chan, 0x00, cmd);
	for (i = 0; i < 1000; i++) {
		status = cam_rd(chan, 0x08);
		if (status & 1u) {
			if (status & 0x0eu) {
				printf("SWEET CAM: SPMI read error %08x addr "
				    "%#x\n", status, addr);
				return EIO;
			}
			*out = (uint8_t)cam_rd(chan, 0x18);
			return 0;
		}
		cam_wait(1);
	}
	printf("SWEET CAM: SPMI read timeout addr %#x\n", addr);
	return ETIMEDOUT;
}

/* Writes are only allowed where the APID belongs to APPS (EE 0). */
static int
cam_spmi_write(uint8_t sid, uint16_t addr, uint8_t val)
{
	uint16_t apid;
	vm_offset_t cfg, chan;
	uint32_t cmd, status;
	unsigned i;

	if (cam_spmi_init() != 0) {
		return EIO;
	}
	apid = cam_spmi_apid(sid, addr >> 8);
	if (apid == CAM_SPMI_NO_APID) {
		printf("SWEET CAM: SPMI sid %u periph %#x has no APID\n", sid,
		    addr >> 8);
		return EIO;
	}
	cfg = ml_io_map(CAM_SPMI_CFG_PA, 0x1000);
	if (cfg == 0) {
		return EIO;
	}
	if ((cam_rd(cfg, 0x700u + 4u * apid) & 0x7u) != 0) {
		printf("SWEET CAM: SPMI APID %u is not APPS-owned\n", apid);
		return EACCES;
	}
	chan = ml_io_map(CAM_SPMI_CHNLS_PA + CAM_SPMI_STRIDE * apid, 0x1000);
	if (chan == 0) {
		return EIO;
	}
	cam_wr(chan, 0x10, val);                        /* WDATA0 */
	cmd = (0u << 27) | ((addr & 0xffu) << 4);       /* EXT_WRITEL */
	cam_wr(chan, 0x00, cmd);
	for (i = 0; i < 1000; i++) {
		status = cam_rd(chan, 0x08);
		if (status & 1u) {
			if (status & 0x0eu) {
				printf("SWEET CAM: SPMI write error %08x addr "
				    "%#x\n", status, addr);
				return EIO;
			}
			return 0;
		}
		cam_wait(1);
	}
	printf("SWEET CAM: SPMI write timeout addr %#x\n", addr);
	return ETIMEDOUT;
}

/* One PMIC GPIO register write, read back. */
static int
cam_pad_write(uint8_t sid, uint16_t periph, uint32_t off, uint8_t val,
    const char *what)
{
	uint8_t back = 0;

	printf("SWEET CAM: spmi: pad %#x write reg %02x = %02x (%s)\n", periph,
	    off, val, what);
	if (cam_spmi_write(sid, (uint16_t)((periph << 8) | off), val) != 0) {
		return EIO;
	}
	if (cam_spmi_read(sid, (uint16_t)((periph << 8) | off), &back) == 0 &&
	    back != val) {
		printf("SWEET CAM: pm660l %s wrote %02x read back %02x\n", what,
		    val, back);
		return EIO;
	}
	return 0;
}

static int
cam_pad_save(uint8_t sid, uint16_t periph, struct cam_pad *pad)
{
	uint8_t *p = (uint8_t *)pad;
	static const uint8_t offs[] = { CAM_PMIC_GPIO_TYPE,
	    CAM_PMIC_GPIO_RT_STS, CAM_PMIC_GPIO_MODE, CAM_PMIC_GPIO_VIN,
	    CAM_PMIC_GPIO_EN };
	unsigned i;

	for (i = 0; i < 5; i++) {
		printf("SWEET CAM: spmi: pad %#x read reg %02x\n", periph,
		    offs[i]);
		cam_wait(300);
		if (cam_spmi_read(sid, (uint16_t)((periph << 8) | offs[i]),
		    &p[i]) != 0) {
			return EIO;
		}
	}
	printf("SWEET CAM: pm660l pad %#x as found: type %02x rt_sts %02x "
	    "mode %02x vin %02x en %02x\n", periph, pad->type, pad->rtsts,
	    pad->mode, pad->vin, pad->en);
	if (pad->type != CAM_PMIC_GPIO_TYPE_GPIO) {
		printf("SWEET CAM: pm660l pad %#x is not a GPIO (type %02x, "
		    "want %02x); not touched\n", periph, pad->type,
		    CAM_PMIC_GPIO_TYPE_GPIO);
		return EIO;
	}
	return 0;
}

static int
cam_pad_drive(uint8_t sid, uint16_t periph, int high)
{
	uint8_t level = 0;

	if (cam_pad_write(sid, periph, CAM_PMIC_GPIO_VIN, 0x00, "vin") != 0 ||
	    cam_pad_write(sid, periph, CAM_PMIC_GPIO_MODE, high ?
	    CAM_PMIC_GPIO_OUT_HIGH : 0x10u, "mode") != 0 ||
	    cam_pad_write(sid, periph, CAM_PMIC_GPIO_EN,
	    (uint8_t)(cam_dvdd.en | CAM_PMIC_GPIO_EN_BIT), "en") != 0) {
		return EIO;
	}
	if (cam_spmi_read(sid, (uint16_t)((periph << 8) |
	    CAM_PMIC_GPIO_RT_STS), &level) == 0) {
		printf("SWEET CAM: pm660l pad %#x rt_sts %02x after driving it "
		    "%s\n", periph, level, high ? "high" : "low");
	}
	return 0;
}

static void
cam_pad_restore(uint8_t sid, uint16_t periph, const struct cam_pad *pad)
{
	(void)cam_pad_write(sid, periph, CAM_PMIC_GPIO_VIN, pad->vin, "vin");
	(void)cam_pad_write(sid, periph, CAM_PMIC_GPIO_MODE, pad->mode, "mode");
	(void)cam_pad_write(sid, periph, CAM_PMIC_GPIO_EN, pad->en, "en");
}

/* Drive one TLMM pin as a plain GPIO (function 0, 2 mA, no pull). */
static void
cam_tlmm_drive(uint32_t pin, int high)
{
	cam_wr(tlmm, pin * 0x1000u + 4u,
	    high ? CAM_TLMM_OUT_HIGH : 0u);
	cam_wr(tlmm, pin * 0x1000u, CAM_TLMM_OE);
}

/*
 * The camera@0 rail sequence of the DTB: reset low while the rails come up,
 * then VANA (TLMM gpio46), VDIG (PM660L pad 3), and the reset released 5 ms
 * later; the sensor's I2C answers without MCLK, so no clock is programmed in
 * this step.
 */
static int
cam_f1_rails_up(void)
{
	int err;

	if (cam_sel != 0) {
		printf("SWEET CAM: lvcam>=3 drives camera@0 only (lvcamsel=0)\n");
		return EINVAL;
	}
	printf("SWEET CAM: rails step 1: reading the AVDD and reset pins\n");
	cam_wait(300);
	cam_avdd_cfg = cam_rd(tlmm, CAM_GPIO_AVDD0 * 0x1000u);
	cam_avdd_io = cam_rd(tlmm, CAM_GPIO_AVDD0 * 0x1000u + 4u);
	cam_rst_cfg = cam_rd(tlmm, CAM_GPIO_RESET0 * 0x1000u);
	cam_rst_io = cam_rd(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u);
	printf("SWEET CAM: gpio%u avdd cfg %08x io %08x, gpio%u reset cfg %08x "
	    "io %08x\n", CAM_GPIO_AVDD0, cam_avdd_cfg, cam_avdd_io,
	    CAM_GPIO_RESET0, cam_rst_cfg, cam_rst_io);

	printf("SWEET CAM: rails step 2: driving reset low\n");
	cam_wait(300);
	cam_tlmm_drive(CAM_GPIO_RESET0, 0);
	printf("SWEET CAM: rails step 3: reset io now %08x\n",
	    cam_rd(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u));
	cam_wait(300);
	cam_tlmm_drive(CAM_GPIO_AVDD0, 1);
	printf("SWEET CAM: rails step 4: VANA io now %08x\n",
	    cam_rd(tlmm, CAM_GPIO_AVDD0 * 0x1000u + 4u));
	if (cam_no_pmic) {
		printf("SWEET CAM: rails: VDIG left alone (lvcamsmoke)\n");
		cam_wait(5);
		cam_tlmm_drive(CAM_GPIO_RESET0, 1);
		cam_wait(10);
		printf("SWEET CAM: rails: reset io now %08x (no VDIG)\n",
		    cam_rd(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u));
		cam_rails_on = 1;
		printf("SWEET CAM: camera@0 powered without VDIG\n");
		return 0;
	}
	printf("SWEET CAM: rails step 5: reading the PM660L pad\n");
	cam_wait(300);
	cam_dvdd_saved = cam_pad_save(CAM_PM660L_SID, CAM_PM660L_DVDD0,
	    &cam_dvdd) == 0;
	if (!cam_dvdd_saved) {
		printf("SWEET CAM: pm660l pad read failed; VDIG cannot be "
		    "driven, aborting the rails\n");
		cam_tlmm_drive(CAM_GPIO_AVDD0, 0);
		cam_wr(tlmm, CAM_GPIO_AVDD0 * 0x1000u + 4u, cam_avdd_io);
		cam_wr(tlmm, CAM_GPIO_AVDD0 * 0x1000u, cam_avdd_cfg);
		cam_wr(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u, cam_rst_io);
		cam_wr(tlmm, CAM_GPIO_RESET0 * 0x1000u, cam_rst_cfg);
		return EIO;
	}
	printf("SWEET CAM: rails step 6: driving VDIG high\n");
	cam_wait(300);
	err = cam_pad_drive(CAM_PM660L_SID, CAM_PM660L_DVDD0, 1);
	if (err != 0) {
		printf("SWEET CAM: VDIG did not take (%d); powering down\n",
		    err);
		cam_tlmm_drive(CAM_GPIO_RESET0, 0);
		cam_wr(tlmm, CAM_GPIO_AVDD0 * 0x1000u + 4u, cam_avdd_io);
		cam_wr(tlmm, CAM_GPIO_AVDD0 * 0x1000u, cam_avdd_cfg);
		cam_wr(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u, cam_rst_io);
		cam_wr(tlmm, CAM_GPIO_RESET0 * 0x1000u, cam_rst_cfg);
		cam_pad_restore(CAM_PM660L_SID, CAM_PM660L_DVDD0, &cam_dvdd);
		return err;
	}
	printf("SWEET CAM: rails step 7: releasing the reset\n");
	cam_wait(5);
	cam_tlmm_drive(CAM_GPIO_RESET0, 1);
	cam_wait(10);
	printf("SWEET CAM: rails: reset io now %08x\n",
	    cam_rd(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u));
	cam_rails_on = 1;
	printf("SWEET CAM: camera@0 powered (reset released)\n");
	return 0;
}

static void
cam_f1_rails_down(void)
{
	if (!cam_rails_on) {
		return;
	}
	printf("SWEET CAM: rails down: reset low, then the rails\n");
	cam_wait(300);
	cam_tlmm_drive(CAM_GPIO_RESET0, 0);
	cam_wait(1);
	if (cam_dvdd_saved) {
		printf("SWEET CAM: rails down: restoring the PM660L pad\n");
		cam_pad_restore(CAM_PM660L_SID, CAM_PM660L_DVDD0, &cam_dvdd);
	}
	cam_wr(tlmm, CAM_GPIO_AVDD0 * 0x1000u + 4u, cam_avdd_io);
	cam_wr(tlmm, CAM_GPIO_AVDD0 * 0x1000u, cam_avdd_cfg);
	cam_wr(tlmm, CAM_GPIO_RESET0 * 0x1000u + 4u, cam_rst_io);
	cam_wr(tlmm, CAM_GPIO_RESET0 * 0x1000u, cam_rst_cfg);
	cam_rails_on = 0;
	printf("SWEET CAM: rails down done\n");
}

/* Open drain on the CCI pins: high is input with the internal pull-up. */
static void
cam_i2c_line(uint32_t pin, int high)
{
	cam_wr(tlmm, pin * 0x1000u + 4u, 0);
	cam_wr(tlmm, pin * 0x1000u, CAM_TLMM_PULL_UP |
	    (high ? 0u : CAM_TLMM_OE));
}

static int
cam_i2c_level(uint32_t pin)
{
	return (cam_rd(tlmm, pin * 0x1000u + 4u) & 1u) != 0;
}

static void
cam_i2c_half(void)
{
	cam_spin_us(5);
}

static void
cam_i2c_start(void)
{
	cam_i2c_line(CAM_GPIO_CCI_SDA0, 1);
	cam_i2c_line(CAM_GPIO_CCI_SCL0, 1);
	cam_i2c_half();
	cam_i2c_line(CAM_GPIO_CCI_SDA0, 0);
	cam_i2c_half();
	cam_i2c_line(CAM_GPIO_CCI_SCL0, 0);
	cam_i2c_half();
}

static void
cam_i2c_stop(void)
{
	cam_i2c_line(CAM_GPIO_CCI_SDA0, 0);
	cam_i2c_half();
	cam_i2c_line(CAM_GPIO_CCI_SCL0, 1);
	cam_i2c_half();
	cam_i2c_line(CAM_GPIO_CCI_SDA0, 1);
	cam_i2c_half();
}

/* One bit; returns the level the slave put on SDA (ack/read data). */
static int
cam_i2c_bit(int out)
{
	int in;

	cam_i2c_line(CAM_GPIO_CCI_SDA0, out);
	cam_i2c_half();
	cam_i2c_line(CAM_GPIO_CCI_SCL0, 1);
	for (unsigned stretched = 0; !cam_i2c_level(CAM_GPIO_CCI_SCL0) &&
	    stretched < 100; stretched++) {
		cam_i2c_half();
	}
	cam_i2c_half();
	in = cam_i2c_level(CAM_GPIO_CCI_SDA0);
	cam_i2c_line(CAM_GPIO_CCI_SCL0, 0);
	return in;
}

static int
cam_i2c_write(uint8_t byte)
{
	int bit;

	for (bit = 7; bit >= 0; bit--) {
		cam_i2c_bit((byte >> bit) & 1);
	}
	return !cam_i2c_bit(1);
}

static uint8_t
cam_i2c_read(int more)
{
	uint8_t byte = 0;
	int bit;

	for (bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | cam_i2c_bit(1));
	}
	cam_i2c_bit(!more);
	return byte;
}

/* Nine clocks free a bus whose data line a sensor is still holding down. */
static int
cam_i2c_unwedge(void)
{
	unsigned clocks;

	for (clocks = 1; clocks <= 9; clocks++) {
		cam_i2c_line(CAM_GPIO_CCI_SCL0, 0);
		cam_i2c_half();
		cam_i2c_line(CAM_GPIO_CCI_SCL0, 1);
		cam_i2c_half();
		if (cam_i2c_level(CAM_GPIO_CCI_SDA0)) {
			cam_i2c_stop();
			printf("SWEET CAM: i2c data line came back after %u "
			    "clock(s)\n", clocks);
			return 0;
		}
	}
	return EIO;
}

static int
cam_i2c_take(void)
{
	cam_sda_cfg = cam_rd(tlmm, CAM_GPIO_CCI_SDA0 * 0x1000u);
	cam_sda_io = cam_rd(tlmm, CAM_GPIO_CCI_SDA0 * 0x1000u + 4u);
	cam_scl_cfg = cam_rd(tlmm, CAM_GPIO_CCI_SCL0 * 0x1000u);
	cam_scl_io = cam_rd(tlmm, CAM_GPIO_CCI_SCL0 * 0x1000u + 4u);
	printf("SWEET CAM: i2c take sda gpio%u cfg %08x io %08x, scl gpio%u "
	    "cfg %08x io %08x\n", CAM_GPIO_CCI_SDA0, cam_sda_cfg, cam_sda_io,
	    CAM_GPIO_CCI_SCL0, cam_scl_cfg, cam_scl_io);
	cam_i2c_line(CAM_GPIO_CCI_SDA0, 1);
	cam_i2c_line(CAM_GPIO_CCI_SCL0, 1);
	cam_i2c_half();
	if (cam_i2c_level(CAM_GPIO_CCI_SDA0) &&
	    cam_i2c_level(CAM_GPIO_CCI_SCL0)) {
		return 0;
	}
	if (cam_i2c_level(CAM_GPIO_CCI_SCL0) &&
	    !cam_i2c_level(CAM_GPIO_CCI_SDA0) && cam_i2c_unwedge() == 0 &&
	    cam_i2c_level(CAM_GPIO_CCI_SCL0)) {
		return 0;
	}
	printf("SWEET CAM: i2c bus is not at rest (sda %u scl %u): not "
	    "touched\n", cam_i2c_level(CAM_GPIO_CCI_SDA0),
	    cam_i2c_level(CAM_GPIO_CCI_SCL0));
	return EIO;
}

static void
cam_i2c_give(void)
{
	cam_wr(tlmm, CAM_GPIO_CCI_SDA0 * 0x1000u + 4u, cam_sda_io);
	cam_wr(tlmm, CAM_GPIO_CCI_SCL0 * 0x1000u + 4u, cam_scl_io);
	cam_wr(tlmm, CAM_GPIO_CCI_SDA0 * 0x1000u, cam_sda_cfg);
	cam_wr(tlmm, CAM_GPIO_CCI_SCL0 * 0x1000u, cam_scl_cfg);
}

/* Write the out bytes, then read in_count after a repeated start. */
static int
cam_i2c_xfer(uint8_t address, const uint8_t *out, unsigned out_count,
    uint8_t *in, unsigned in_count)
{
	int answered;
	unsigned i;

	cam_i2c_start();
	answered = cam_i2c_write((uint8_t)(address << 1));
	for (i = 0; answered && i < out_count; i++) {
		answered = cam_i2c_write(out[i]);
	}
	if (answered && in_count != 0) {
		cam_i2c_start();
		answered = cam_i2c_write((uint8_t)(address << 1 | 1));
		for (i = 0; answered && i < in_count; i++) {
			in[i] = cam_i2c_read(i + 1 < in_count);
		}
	}
	cam_i2c_stop();
	return answered ? 0 : EIO;
}

static void
cam_f1_scan(void)
{
	uint8_t id[2];
	uint8_t reg[2];
	uint32_t addr;
	unsigned found = 0;

	cam_announce("scan the camera bus by hand on the CCI pins (lvcam>=3)");
	if (cam_i2c_take() != 0) {
		cam_i2c_give();
		return;
	}
	for (addr = 0x08; addr < 0x78; addr++) {
		uint8_t one = 0;

		if (cam_i2c_xfer((uint8_t)addr, NULL, 0, &one, 1) != 0) {
			continue;
		}
		found++;
		printf("SWEET CAM: i2c 0x%02x answers (a read gives %02x)\n",
		    addr, one);
		reg[0] = 0x00;
		reg[1] = 0x00;
		if (cam_i2c_xfer((uint8_t)addr, reg, 2, id, 2) == 0) {
			printf("SWEET CAM:   0x0000 = %02x %02x\n", id[0],
			    id[1]);
		}
		reg[0] = 0x30;
		reg[1] = 0x0a;
		if (cam_i2c_xfer((uint8_t)addr, reg, 2, id, 2) == 0) {
			printf("SWEET CAM:   0x300a = %02x %02x\n", id[0],
			    id[1]);
		}
	}
	cam_i2c_give();
	printf("SWEET CAM: scan done, %u address(es) answered\n", found);
}

static void
cam_f1(void)
{
	if (cam_f1_rails_up() != 0) {
		return;
	}
	cam_f1_scan();
	cam_f1_rails_down();
}

/*
 * 1012: read the SPMI arbiter version from a helper thread at a chosen
 * moment, before the probe touches anything. A stalled read loses only this
 * thread; the rest of the system (and the main camera thread) keeps running.
 */
static void
cam_spmi_vantage_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t at = (uint32_t)(uintptr_t)arg;
	vm_offset_t core;

	printf("SWEET CAM: vantage %u s: started\n", at);
	cam_wait(at * 1000);
	printf("SWEET CAM: vantage %u s: mapping the SPMI core\n", at);
	core = ml_io_map(CAM_SPMI_CORE_PA, 0x1000);
	if (core == 0) {
		printf("SWEET CAM: vantage %u s: map failed\n", at);
		return;
	}
	printf("SWEET CAM: vantage %u s: reading the arbiter\n", at);
	printf("SWEET CAM: vantage %u s: arbiter %#x\n", at, cam_rd(core, 0));
	printf("SWEET CAM: vantage %u s: done\n", at);
}

static void
cam_spmi_vantage_start(const char *name)
{
	uint32_t at = 0;
	thread_t thread;

	if (!PE_parse_boot_argn(name, &at, sizeof(at))) {
		return;
	}
	printf("SWEET CAM: %s=%u s\n", name, at);
	if (kernel_thread_start(cam_spmi_vantage_thread,
	    (void *)(uintptr_t)at, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
lavender_cam_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t delay_s = 25;
	int powered = 0;

	cam_spmi_vantage_start("lvcamspmi");
	cam_spmi_vantage_start("lvcamspmi2");

	(void)PE_parse_boot_argn("lvcamdelay", &delay_s, sizeof(delay_s));
	if (delay_s > 300) {
		delay_s = 300;
	}
	printf("SWEET CAM: F0 probe (lvcam=%u) in %u s\n", cam_step, delay_s);
	cam_wait(delay_s * 1000);

	cam_dt_facts();

	mmcc = ml_io_map(CAM_MMCC_PA, CAM_MMCC_SIZE);
	if (mmcc == 0) {
		printf("SWEET CAM: MMCC map failed\n");
		return;
	}
	cam_gdsc();
	cam_branches();
	cam_plls_rcg();

	if (cam_step >= 2) {
		powered = cam_domain_power_up() == 0;
		if (powered) {
			cam_ahb_power_up();
		}
	} else {
		cam_ahb_on = cam_domain_on && cam_ahb_running();
	}

	if (cam_ahb_on) {
		camss = ml_io_map(CAM_CAMSS_PA, CAM_CAMSS_SIZE);
		csiphy = ml_io_map(CAM_CSIPHY_PA, CAM_CSIPHY_SIZE);
		if (camss != 0) {
			cam_muxes();
		} else {
			printf("SWEET CAM: CAMSS map failed\n");
		}
		if (csiphy == 0) {
			printf("SWEET CAM: CSIPHY map failed\n");
		} else if (cam_rd(mmcc, cam_ahb[3].off) & CAM_CBCR_CLK_OFF) {
			printf("SWEET CAM: csiphy_ahb2crif is off; CSIPHY "
			    "reads skipped\n");
		} else {
			cam_csiphy();
		}
	} else {
		printf("SWEET CAM: the AHB path to the CAMSS blocks is off; "
		    "block reads need lvcam=2 or a domain left on\n");
	}

	if (powered) {
		cam_ahb_power_down();
		cam_domain_power_down();
	}

	tlmm = ml_io_map(CAM_TLMM_SOUTH_PA, CAM_TLMM_SOUTH_SIZE);
	if (tlmm != 0) {
		cam_gpios();
		if (cam_step >= 3) {
			cam_f1();
		}
	} else {
		printf("SWEET CAM: TLMM map failed\n");
	}

	smmu_mmss = ml_io_map(CAM_SMMU_PA, CAM_SMMU_SIZE);
	if (smmu_mmss != 0) {
		cam_smmu();
	} else {
		printf("SWEET CAM: MMSS SMMU map failed\n");
	}

	printf("SWEET CAM: probe done (lvcam=%u)\n", cam_step);
}

int
lavender_cam_probe(void)
{
	static int started;
	uint32_t step = 0, sel = 0, smoke = 0;
	thread_t thread;

	if (started ||
	    !PE_parse_boot_argn("lvcam", &step, sizeof(step)) || step == 0) {
		return 0;
	}
	started = 1;
	cam_step = step;
	if (!PE_parse_boot_argn("lvcamsel", &sel, sizeof(sel)) || sel > 2) {
		sel = 0;
	}
	cam_sel = sel;
	(void)PE_parse_boot_argn("lvcamsmoke", &smoke, sizeof(smoke));
	if (step >= 4 || smoke != 0) {
		cam_no_pmic = 1;
	}
	if (kernel_thread_start(lavender_cam_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_LAVENDER */

int lavender_cam_probe(void);
int
lavender_cam_probe(void)
{
	return 0;
}

#endif
