/*
 * lavender: modem (MSS/QDSP6) bring-up, F1. Boot-arg gated, read-mostly.
 *
 *   sweetmodem=1       deassert the MSS restart, enable the MSS clocks and
 *                      read QDSP6/RMB (no firmware)
 *   sweetmodem=2       also load MBA.MBN from the modem FAT16 (eMMC), hand
 *                      the MBA region to MSS with SCM, reset the Q6 and wait
 *                      for the MBA to answer
 *   sweetmodem=3       also authenticate MODEM.MDT, load every segment blob
 *                      into the modem carve-out and wait for auth complete
 *   sweetmodemdelay=S  seconds to wait after boot before it (default 25)
 *   sweetmodemclk=N    clock mask (default 0x3f): bit0 mss_cfg_ahb,
 *                      bit1 bimc_mss_q6_axi, bit2 boot_rom_ahb,
 *                      bit3 gpll0_out_msscc, bit4 mss_snoc_axi,
 *                      bit5 mss_mnoc_bimc_axi. Use it to bisect one boot
 *                      per value if a clock write wedges the thread.
 *
 * F0b (`lavender_wlan.c`) showed the ABL left mss_cfg_ahb (0x8a000) and
 * mss_snoc_axi (0x8a03c) off, while mss_q6_bimc_axi (0x8a040) was on. The
 * 0244 boot enabled the full active-clock set and read a clean parked state:
 * Q6 reset 00000007 (held), PWR_CTL 00700000 (clamped, BHS/LDO off), RMB all
 * zero. This patch keeps that step and adds the MBA load.
 *
 * Order matters (pil-msa.c): power up, deassert the MSS restart (0x179000=0),
 * enable the clocks, program RMB, run the QDSP6 reset, wait for PBL/MBA.
 * The MBA image is copied as-is into pil_mba_region (0x94800000, 2 MiB) and
 * the first 1 MiB is assigned to MSS_MSA with SCM MEM_PROT_ASSIGN (the DTB's
 * qcom,mem-protect-id is 0xf). Step 3 authenticates MODEM.MDT, loads the 26
 * segment blobs into the modem carve-out and waits for AUTH_COMPLETE.
 *
 * Register meanings: Linux pil-msa.c, pil-q6v5.c, gcc-sdm660.c,
 * secure_buffer.c and the SDM660 DTB, read as behaviour references.
 */
#if defined(ARM64_BOARD_CONFIG_LAVENDER)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <arm/caches_internal.h>
#include <vm/memory_types.h>
#include <libkern/libkern.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern int santoni_emmc_read(uint64_t lba, uint32_t count, void *buf);
extern void lavender_wlan_after_modem(void);

int lavender_modem_fw_size(const char *name, uint32_t *size);
int lavender_modem_fw_read(const char *name, uint32_t off, void *dst,
    uint32_t len);
int lavender_modem_part_prepare(const char *name, uint32_t *size);
int lavender_modem_part_read(const char *name, uint32_t off, void *dst,
    uint32_t len);
int lavender_modem_part_write(const char *name, uint32_t off, const void *src,
    uint32_t len);
int lavender_modem_rmtfs_buf(uint64_t *pa, uint32_t *size);
int lavender_modem_rmtfs_copy(uint32_t off, void *buf, uint32_t len,
    boolean_t to_buf);
int lavender_modem_assign(uint32_t pa, uint32_t size, const uint32_t *src,
    int nsrc, const uint32_t *dst, const uint32_t *perm, int ndst);

#define IO_CHUNK        (2u << 20)      /* windows into the 118 MiB image */

#define GCC_PA          0x00100000u
#define GCC_SIZE        0x94000u
#define QDSP6_PA        0x04080000u
#define QDSP6_SIZE      0x00001000u
#define RMB_PA          0x04180000u
#define RMB_SIZE        0x00001000u
#define MSS_RESTART_PA  0x00179000u
#define MSS_RESTART_SIZE 0x00001000u

#define MBA_PA          0x94800000u
#define MBA_SIZE        0x00200000u
#define MBA_ASSIGN_SIZE 0x00100000u

/* modem_fw_region@8ac00000 (126 MiB) from the SDM660 DTB */
#define MODEM_FW_PA     0x8ac00000u
#define MODEM_FW_SIZE   0x07e00000u

/* GCC byte offsets from GCC_PA (gcc-sdm660.c) */
#define GCC_MSS_CFG_AHB         0x8a000u
#define GCC_MSS_MNOC_BIMC_AXI   0x8a004u
#define GCC_MSS_SNOC_AXI        0x8a03cu
#define GCC_MSS_Q6_BIMC_AXI     0x8a040u
#define GCC_BIMC_MSS_Q6_AXI     0x4401cu
#define GCC_BOOT_ROM_AHB_HALT   0x38004u
#define GCC_BOOT_ROM_AHB_VOTE   0x52004u
#define GCC_BOOT_ROM_AHB_BIT    (1u << 10)
#define GCC_GPLL0_OUT_MSSCC     0x5200cu
#define GCC_GPLL0_OUT_MSSCC_BIT (1u << 2)
#define CBCR_EN                 (1u << 0)
#define CBCR_OFF                (1u << 31)

/* QDSP6SS (pil-q6v5.c) */
#define Q6_RESET                0x014u
#define Q6_GFMUX_CTL            0x020u
#define Q6_PWR_CTL              0x030u
#define Q6V6_MEM_PWR_CTL        0x034u
#define Q6_BHS_STATUS           0x078u
#define Q6V62_BHS_STATUS        0x0c4u
#define Q6_XO_CBCR              0x038u
#define Q6_STRAP_ACC            0x110u

/* QDSP6SS_RESET / PWR_CTL / GFMUX bits */
#define Q6SS_STOP_CORE          (1u << 0)
#define Q6SS_CORE_ARES          (1u << 1)
#define Q6SS_BUS_ARES_ENA       (1u << 2)
#define Q6SS_CLK_ENA            (1u << 1)
#define Q6SS_L2DATA_STBY_N      (1u << 18)
#define Q6SS_SLP_RET_N          (1u << 19)
#define Q6SS_CLAMP_IO           (1u << 20)
#define QDSP6v55_CLAMP_WL       (1u << 21)
#define QDSP6v55_CLAMP_QMC_MEM  (1u << 22)
#define QDSP6v55_BHS_ON         (1u << 24)
#define QDSP6v55_LDO_BYP        (1u << 25)
#define QDSP6v55_BHS_EN_REST_ACK (1u << 0)

/* RMB (pil-msa.c) */
#define RMB_MBA_IMAGE           0x00u
#define RMB_PBL_STATUS          0x04u
#define RMB_MBA_COMMAND         0x08u
#define RMB_MBA_STATUS          0x0cu
#define RMB_PMI_META_DATA       0x10u
#define RMB_PMI_CODE_START      0x14u
#define RMB_PMI_CODE_LENGTH     0x18u
#define RMB_PROTOCOL_VERSION    0x1cu
#define RMB_DEBUG               0x20u
#define CMD_META_DATA_READY     0x1u
#define CMD_LOAD_READY          0x2u
#define STATUS_PBL_SUCCESS      0x1u
#define STATUS_XPU_UNLOCKED     0x1u
#define STATUS_XPU_UNLOCKED_SCRIBBLED 0x2u
#define STATUS_META_DATA_AUTH_SUCCESS 0x3u
#define STATUS_AUTH_COMPLETE    0x4u

/* SCM memory protection (secure_buffer.c, secure_buffer.h) */
#define SCM_FN(svc, cmd)        (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_MP              0x0cu
#define MEM_PROT_ASSIGN_ID      0x16u
#define VMID_HLOS               0x3u
#define VMID_MSS_MSA            0x0fu
#define VMID_WLAN               0x18u
#define VMID_WLAN_CE            0x19u
#define PERM_READ               0x4u
#define PERM_WRITE              0x2u
#define PERM_EXEC               0x1u
#define SCM_ARGINFO_7           0x1117u  /* SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL) */
#define SCM_CHUNK               (2u << 20) /* secure_buffer.c BATCH_MAX_SIZE */

/* modem FAT16 (GPT: modem at LBA 786432; BPB: 512/32/1/2/512/49) */
#define MODEM_LBA               786432ull
#define SECTOR_SIZE             512u
#define FAT_DIR_MAX             (192u << 10)
#define FAT_BOUNCE_SIZE         (1u << 20)
#define SCM_BUF_SIZE            0x1000u
#define FAT_MAX_CLUSTER         0xfff8u

static vm_offset_t gcc, rst, q6, rmb, mba;
static void *fat_buf, *bounce, *scm_buf, *mdt_buf;
static uint64_t scm_pa;
static int modem_chan;
static uint32_t fat_cluster_bytes, fat_spc, fat_data_start, fat_fat_off;
static uint32_t fat_root_start, fat_root_entries, fat_fat_size;
static uint8_t *fat_image;              /* IMAGE/ directory, cached */
static uint32_t fat_image_len;

static inline uint32_t
mrd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
mwr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/*
 * 0247: ml_io_map() returns Device-nGnRnE memory. XNU's memset/bzero store
 * engine emits STNP (non-temporal) pairs, which are not safe on Device
 * memory: the 0246 boot wedged the modem thread inside the 1 MiB MBA zeroing
 * with no error line. Zero/copy with explicit volatile stores.
 *
 * 0711: Device memory also requires naturally aligned accesses. MODEM.B05 has
 * an odd filesz, so its tail zero started at 0x8ac901d3 (offset 0x1d3) and
 * the first 64-bit store wedged the thread after the "blob 05 zero" line.
 * Touch single bytes until the destination is 8-byte aligned, then use
 * 64-bit stores, then finish with single bytes.
 */
static void
io_zero(vm_offset_t dst, uint32_t n)
{
	uint32_t i = 0;

	while (i < n && ((dst + i) & 7) != 0) {
		*(volatile uint8_t *)(dst + i) = 0;
		i++;
	}
	for (; i + 8 <= n; i += 8) {
		*(volatile uint64_t *)(dst + i) = 0;
	}
	for (; i < n; i++) {
		*(volatile uint8_t *)(dst + i) = 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
io_copy(vm_offset_t dst, const void *src, uint32_t n)
{
	const uint8_t *s = src;
	uint32_t i = 0;

	while (i < n && ((dst + i) & 7) != 0) {
		*(volatile uint8_t *)(dst + i) = s[i];
		i++;
	}
	for (; i + 8 <= n; i += 8) {
		uint64_t v;

		memcpy(&v, s + i, 8);
		*(volatile uint64_t *)(dst + i) = v;
	}
	for (; i < n; i++) {
		*(volatile uint8_t *)(dst + i) = s[i];
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* The reverse of io_copy: read from Device memory into a normal buffer. */
static void
io_read(void *dst, vm_offset_t src, uint32_t n)
{
	uint8_t *d = dst;
	uint32_t i = 0;

	while (i < n && ((src + i) & 7) != 0) {
		d[i] = *(volatile uint8_t *)(src + i);
		i++;
	}
	for (; i + 8 <= n; i += 8) {
		uint64_t v = *(volatile uint64_t *)(src + i);

		memcpy(d + i, &v, 8);
	}
	for (; i < n; i++) {
		d[i] = *(volatile uint8_t *)(src + i);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * 0709: io_submap is only 8 MiB (osfmk/arm/io_map.c IO_MAP_SIZE), so the
 * 118 MiB image cannot be mapped in one go: 0708 wedged inside
 * ml_io_map()/kmem_alloc(KMA_NOFAIL). Copy through 2 MiB windows instead.
 * The windows use ml_io_map_unmappable(), which allocates in kernel_map,
 * because that is the map ml_io_unmap() frees. ml_io_map() allocates from
 * io_submap, whose parent entry is permanent, so ml_io_unmap() cannot
 * release it. pmap_remove() wants page-aligned bounds: unmap from
 * trunc_page(va) with round_page(offset + length).
 *
 * 0710 keeps that mapping and adds one trace per window and per MiB: the
 * 0709 v2 boot stopped inside B05 without a line after blob 04.
 */
static int
io_zero_pa(uint32_t pa, uint32_t n)
{
	uint32_t off = 0;

	while (off < n) {
		uint32_t chunk = MIN(n - off, IO_CHUNK);
		vm_offset_t va;

		printf("SWEET MODEM: zero window %08x+%x\n", pa + off, chunk);
		va = ml_io_map_unmappable(pa + off, chunk, VM_WIMG_IO);
		if (va == 0) {
			return ENOMEM;
		}
		printf("SWEET MODEM: zero mapped va %08x\n", (uint32_t)va);
		io_zero(va, chunk);
		printf("SWEET MODEM: zero done\n");
		ml_io_unmap(trunc_page(va),
		    round_page(va - trunc_page(va) + chunk));
		off += chunk;
	}
	return 0;
}

static void
modem_wait(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&modem_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
modem_announce(const char *what)
{
	printf("SWEET MODEM: about to %s\n", what);
	modem_wait(2000);
}

/*
 * The eMMC is shared with the root filesystem and the console flusher. The
 * SDHCI wrapper serializes on its lock, but a transfer can still time out
 * while the card is busy; retry with a thread delay instead of hammering the
 * controller (0245 died on the first modem sector read at 45 s).
 */
static int
emmc_read(uint64_t lba, uint32_t count, void *buf)
{
	int err = EIO;

	for (int attempt = 0; attempt < 5; attempt++) {
		err = santoni_emmc_read(lba, count, buf);
		if (err == 0) {
			return 0;
		}
		printf("SWEET MODEM: eMMC read 0x%llx+%u failed %d "
		    "(attempt %d/5)\n", (unsigned long long)lba, count, err,
		    attempt + 1);
		modem_wait(100);
	}
	return err;
}

static uint32_t
modem_poll_off(uint32_t off, int n)
{
	uint32_t v = 0;

	for (int i = 0; i < n; i++) {
		v = mrd(gcc, off);
		if (!(v & CBCR_OFF)) {
			break;
		}
		modem_wait(10);
	}
	return v;
}

static void
gcc_branch(const char *name, uint32_t off)
{
	uint32_t v = mrd(gcc, off);

	printf("SWEET MODEM: gcc %s before %08x\n", name, v);
	if (!(v & CBCR_EN)) {
		mwr(gcc, off, v | CBCR_EN);
	}
	v = modem_poll_off(off, 100);
	printf("SWEET MODEM: gcc %s after %08x (%s)\n", name, v,
	    (v & CBCR_OFF) ? "STILL OFF" : "on");
}

static void
modem_restart_deassert(void)
{
	uint32_t v;

	modem_announce("deassert the MSS restart (write 0 to 0x179000)");
	v = mrd(rst, 0);
	printf("SWEET MODEM: mss restart before %08x\n", v);
	if (v != 0) {
		mwr(rst, 0, 0);
	}
	delay(100);
	v = mrd(rst, 0);
	printf("SWEET MODEM: mss restart after %08x (%s)\n", v,
	    v == 0 ? "deasserted" : "STILL ASSERTED");
}

static void
modem_clocks(uint32_t mask)
{
	uint32_t v;

	printf("SWEET MODEM: clock mask %02x\n", mask);
	if (mask & 0x01) {
		gcc_branch("mss_cfg_ahb", GCC_MSS_CFG_AHB);
	}
	if (mask & 0x02) {
		gcc_branch("bimc_mss_q6_axi", GCC_BIMC_MSS_Q6_AXI);
	}
	if (mask & 0x04) {
		v = mrd(gcc, GCC_BOOT_ROM_AHB_VOTE);
		printf("SWEET MODEM: gcc boot_rom vote before %08x (halt %08x)\n",
		    v, mrd(gcc, GCC_BOOT_ROM_AHB_HALT));
		mwr(gcc, GCC_BOOT_ROM_AHB_VOTE, v | GCC_BOOT_ROM_AHB_BIT);
		v = modem_poll_off(GCC_BOOT_ROM_AHB_HALT, 100);
		printf("SWEET MODEM: gcc boot_rom vote after %08x (halt %08x, %s)\n",
		    mrd(gcc, GCC_BOOT_ROM_AHB_VOTE), v,
		    (v & CBCR_OFF) ? "STILL OFF" : "on");
	}
	if (mask & 0x08) {
		v = mrd(gcc, GCC_GPLL0_OUT_MSSCC);
		printf("SWEET MODEM: gcc gpll0_out_msscc before %08x\n", v);
		mwr(gcc, GCC_GPLL0_OUT_MSSCC, v | GCC_GPLL0_OUT_MSSCC_BIT);
		printf("SWEET MODEM: gcc gpll0_out_msscc after %08x\n",
		    mrd(gcc, GCC_GPLL0_OUT_MSSCC));
	}
	if (mask & 0x10) {
		gcc_branch("mss_snoc_axi", GCC_MSS_SNOC_AXI);
	}
	if (mask & 0x20) {
		gcc_branch("mss_mnoc_bimc_axi", GCC_MSS_MNOC_BIMC_AXI);
	}
	printf("SWEET MODEM: gcc mss_q6_bimc_axi (reference) %08x\n",
	    mrd(gcc, GCC_MSS_Q6_BIMC_AXI));
}

static void
modem_dump_q6(void)
{
	modem_announce("read the QDSP6 and RMB blocks (clocks are on)");
	printf("SWEET MODEM: Q6 reset %08x gfmux %08x pwr %08x mem_pwr %08x\n",
	    mrd(q6, Q6_RESET), mrd(q6, Q6_GFMUX_CTL), mrd(q6, Q6_PWR_CTL),
	    mrd(q6, Q6V6_MEM_PWR_CTL));
	printf("SWEET MODEM: Q6 bhs %08x bhs62 %08x xo_cbcr %08x strap_acc "
	    "%08x\n", mrd(q6, Q6_BHS_STATUS), mrd(q6, Q6V62_BHS_STATUS),
	    mrd(q6, Q6_XO_CBCR), mrd(q6, Q6_STRAP_ACC));
	printf("SWEET MODEM: RMB image %08x pbl %08x cmd %08x status %08x\n",
	    mrd(rmb, RMB_MBA_IMAGE), mrd(rmb, RMB_PBL_STATUS),
	    mrd(rmb, RMB_MBA_COMMAND), mrd(rmb, RMB_MBA_STATUS));
}

/* --- SCM memory protection ------------------------------------------------ */

static uint64_t modem_scm7(uint32_t fn, uint32_t arginfo, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t extra_pa, uint64_t *res);

/*
 * One page holds the mem_prot_info entry, the source VMID list and the
 * dest_vm_and_perm_info list. The physical address goes in x2..x4; the
 * remaining four arguments go in the extended-argument page pointed to by
 * x5 (scm.c allocate_extra_arg_buffer, N_REGISTER_ARGS).
 */
static int
scm_mem_assign(uint32_t pa, uint32_t size, const uint32_t *src_vmids,
    int nsrc, const uint32_t *dst_vmids, const uint32_t *dst_perms, int ndst)
{
	struct mem_prot_info {
		uint64_t addr;
		uint64_t size;
	};
	struct dest_info {
		uint32_t vm;
		uint32_t perm;
		uint64_t ctx;
		uint32_t ctx_size;
		uint32_t pad;
	};
	uint8_t *buf = scm_buf;
	struct mem_prot_info *mp = (struct mem_prot_info *)(void *)buf;
	uint32_t *src = (uint32_t *)(void *)(buf + 0x40);
	struct dest_info *dst = (struct dest_info *)(void *)(buf + 0x80);
	uint64_t *extra = (uint64_t *)(void *)(buf + 0x200);
	uint64_t ret = 0;
	uint32_t fn = SCM_FN(SCM_SVC_MP, MEM_PROT_ASSIGN_ID);
	int err;

	if (nsrc < 1 || nsrc > 4 || ndst < 1 || ndst > 4) {
		return EINVAL;
	}
	memset(buf, 0, SCM_BUF_SIZE);
	mp->addr = pa;
	mp->size = size;
	for (int i = 0; i < nsrc; i++) {
		src[i] = src_vmids[i];
	}
	for (int i = 0; i < ndst; i++) {
		dst[i].vm = dst_vmids[i];
		dst[i].perm = dst_perms[i];
	}

	extra[0] = (uint64_t)nsrc * sizeof(uint32_t);   /* source list bytes */
	extra[1] = scm_pa + 0x80;                       /* dest list pa */
	extra[2] = (uint64_t)ndst * sizeof(*dst);       /* dest list bytes */
	extra[3] = 0;

	flush_dcache64((addr64_t)scm_pa, SCM_BUF_SIZE, TRUE);
	__asm__ volatile("dsb sy" ::: "memory");

	err = (int)modem_scm7(fn, SCM_ARGINFO_7, scm_pa, sizeof(*mp), scm_pa + 0x40,
	    scm_pa + 0x200, &ret);
	printf("SWEET MODEM: SCM assign %08x+%x src %u dst %u ret %d res %llx\n",
	    pa, size, nsrc, ndst, err, (unsigned long long)ret);
	return err ? err : (ret ? EIO : 0);
}

static int
scm_assign(uint32_t pa, uint32_t size, uint32_t src_vmid, uint32_t dst_vmid,
    uint32_t dst_perm)
{
	return scm_mem_assign(pa, size, &src_vmid, 1, &dst_vmid, &dst_perm, 1);
}

/* WLFW needs the MSA handed to MSS_MSA + WLAN + WLAN_CE in one call. */
int
lavender_modem_assign(uint32_t pa, uint32_t size, const uint32_t *src,
    int nsrc, const uint32_t *dst, const uint32_t *perm, int ndst)
{
	return scm_mem_assign(pa, size, src, nsrc, dst, perm, ndst);
}

/* --- modem FAT16 ---------------------------------------------------------- */

static inline uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t
fat_cluster_sector(uint32_t cluster)
{
	return fat_data_start + (cluster - 2) * fat_spc;
}

static uint32_t
fat_next(uint32_t cluster)
{
	/*
	 * 0248: fat_buf already starts at the FAT's first sector. Adding
	 * fat_fat_off (the partition-relative offset, 512) made every chain
	 * read the wrong entry and stop after two clusters.
	 */
	return le16((const uint8_t *)fat_buf + cluster * 2);
}

/* Build the 8.3 name of a directory entry, without the trailing dot. */
static void
fat_name(const uint8_t *e, char *out, size_t outlen)
{
	size_t n = 0;

	for (int i = 0; i < 8 && e[i] != ' '; i++) {
		if (n + 1 < outlen) {
			out[n++] = (char)e[i];
		}
	}
	if (e[8] != ' ') {
		if (n + 1 < outlen) {
			out[n++] = '.';
		}
		for (int i = 8; i < 11 && e[i] != ' '; i++) {
			if (n + 1 < outlen) {
				out[n++] = (char)e[i];
			}
		}
	}
	out[n] = '\0';
}

/*
 * Read the BPB, FAT, root directory and IMAGE/ directory once. The partition
 * is the phone's own modem image (FAT16, 512 B sectors, 16 KiB clusters);
 * anything else is refused, never written.
 */
static int
fat_open(void)
{
	uint8_t *fat, *root;
	uint32_t fat_bytes, root_bytes, cluster, n, off;
	int err;

	err = emmc_read(MODEM_LBA, 1, bounce);
	if (err) {
		printf("SWEET MODEM: modem sector 0 read failed %d\n", err);
		return err;
	}
	{
		const uint8_t *b = bounce;

		if (le16(b + 0x0b) != 512 || b[0x0d] != 32 ||
		    le16(b + 0x0e) != 1 || b[0x10] != 2 ||
		    le16(b + 0x11) != 512 || le16(b + 0x16) != 49) {
			printf("SWEET MODEM: unexpected modem BPB (%u/%u/%u/%u/"
			    "%u/%u)\n", le16(b + 0x0b), b[0x0d], le16(b + 0x0e),
			    b[0x10], le16(b + 0x11), le16(b + 0x16));
			return EINVAL;
		}
		fat_root_entries = le16(b + 0x11);
		fat_fat_size = le16(b + 0x16);
		fat_spc = b[0x0d];
		fat_root_start = le16(b + 0x0e) + b[0x10] * fat_fat_size;
	}
	fat_cluster_bytes = fat_spc * SECTOR_SIZE;
	fat_fat_off = le16((const uint8_t *)bounce + 0x0e) * SECTOR_SIZE;
	fat_data_start = fat_root_start + (fat_root_entries * 32 + SECTOR_SIZE - 1) /
	    SECTOR_SIZE;

	/* FAT: 49 sectors (25,088 bytes). */
	fat_bytes = fat_fat_size * SECTOR_SIZE;
	fat = (uint8_t *)fat_buf;
	err = emmc_read(MODEM_LBA + fat_fat_off / SECTOR_SIZE,
	    fat_fat_size, fat);
	if (err) {
		printf("SWEET MODEM: FAT read failed %d\n", err);
		return err;
	}

	/* Root directory: 512 entries. */
	root_bytes = fat_root_entries * 32;
	root = fat + fat_bytes;
	err = emmc_read(MODEM_LBA + fat_root_start, root_bytes / SECTOR_SIZE,
	    root);
	if (err) {
		printf("SWEET MODEM: root directory read failed %d\n", err);
		return err;
	}

	cluster = 0;
	for (uint32_t i = 0; i < fat_root_entries; i++) {
		const uint8_t *e = root + i * 32;
		char name[13];

		if (e[0] == 0x00) {
			break;
		}
		if (e[0] == 0xe5 || e[0x0b] == 0x0f) {
			continue;
		}
		fat_name(e, name, sizeof(name));
		if (strcasecmp(name, "IMAGE") == 0 && (e[0x0b] & 0x10)) {
			cluster = le16(e + 0x1a) | (le16(e + 0x14) << 16);
			break;
		}
	}
	if (cluster < 2) {
		printf("SWEET MODEM: IMAGE/ not found in the modem FAT\n");
		return ENOENT;
	}

	/* IMAGE/: follow the chain into the remaining cached space. */
	off = fat_bytes + root_bytes;
	n = 0;
	while (cluster >= 2 && cluster < FAT_MAX_CLUSTER) {
		if (off + fat_cluster_bytes > FAT_DIR_MAX) {
			printf("SWEET MODEM: IMAGE/ exceeds the cache\n");
			return ENOMEM;
		}
		err = emmc_read(MODEM_LBA + fat_cluster_sector(cluster),
		    fat_spc, (uint8_t *)fat_buf + off);
		if (err) {
			printf("SWEET MODEM: IMAGE/ read failed %d\n", err);
			return err;
		}
		off += fat_cluster_bytes;
		n++;
		cluster = fat_next(cluster);
	}
	fat_image = fat + fat_bytes + root_bytes;
	fat_image_len = off - (fat_bytes + root_bytes);
	printf("SWEET MODEM: modem FAT: %u image clusters, %u bytes cached\n", n,
	    fat_image_len);
	return 0;
}

static int
fat_lookup(const char *name, uint32_t *cluster, uint32_t *size)
{
	for (uint32_t i = 0; i < fat_image_len / 32; i++) {
		const uint8_t *e = fat_image + i * 32;
		char entry[13];

		if (e[0] == 0x00) {
			break;
		}
		if (e[0] == 0xe5 || e[0x0b] == 0x0f || (e[0x0b] & 0x10)) {
			continue;
		}
		fat_name(e, entry, sizeof(entry));
		if (strcasecmp(entry, name) == 0) {
			*cluster = le16(e + 0x1a) | (le16(e + 0x14) << 16);
			*size = le32(e + 0x1c);
			return 0;
		}
	}
	return ENOENT;
}

/* Stream one FAT16 file into an uncached mapping (the modem region). */
static int
fat_read_to_va(const char *name, vm_offset_t dst, uint32_t max)
{
	uint32_t cluster, size, off = 0;
	int err;

	err = fat_lookup(name, &cluster, &size);
	if (err) {
		printf("SWEET MODEM: %s not found\n", name);
		return err;
	}
	if (size > max) {
		printf("SWEET MODEM: %s is %u bytes, max %u\n", name, size, max);
		return EINVAL;
	}
	printf("SWEET MODEM: %s %u bytes at va %08x\n", name, size,
	    (uint32_t)dst);
	while (off < size) {
		uint32_t n = MIN(size - off, fat_cluster_bytes);

		err = emmc_read(MODEM_LBA + fat_cluster_sector(cluster),
		    fat_spc, bounce);
		if (err) {
			printf("SWEET MODEM: %s read failed %d at %u\n", name,
			    err, off);
			return err;
		}
		io_copy(dst + off, bounce, n);
		off += n;
		if (off < size) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				printf("SWEET MODEM: %s chain ends at %u/%u\n",
				    name, off, size);
				return EIO;
			}
		}
	}
	return 0;
}

/* Read one FAT16 file into a cached buffer (MDT and other small files). */
static int
fat_read_to_buf(const char *name, void *dst, uint32_t max, uint32_t *out)
{
	uint32_t cluster, size, off = 0;
	int err;

	err = fat_lookup(name, &cluster, &size);
	if (err) {
		printf("SWEET MODEM: %s not found\n", name);
		return err;
	}
	if (size > max) {
		printf("SWEET MODEM: %s is %u bytes, max %u\n", name, size, max);
		return EINVAL;
	}
	while (off < size) {
		uint32_t n = MIN(size - off, fat_cluster_bytes);

		err = emmc_read(MODEM_LBA + fat_cluster_sector(cluster),
		    fat_spc, bounce);
		if (err) {
			printf("SWEET MODEM: %s read failed %d at %u\n", name,
			    err, off);
			return err;
		}
		memcpy((uint8_t *)dst + off, bounce, n);
		off += n;
		if (off < size) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				printf("SWEET MODEM: %s chain ends at %u/%u\n",
				    name, off, size);
				return EIO;
			}
		}
	}
	*out = size;
	return 0;
}

/*
 * Firmware files for the AP's tftp host service (0721). The modem's elf
 * loader asks for /readonly/firmware/image/wlanmdsp.mbn by basename; the
 * modem FAT's IMAGE/ directory is already cached in fat_image.
 */
int
lavender_modem_fw_size(const char *name, uint32_t *size)
{
	uint32_t cluster;

	return fat_lookup(name, &cluster, size);
}

int
lavender_modem_fw_read(const char *name, uint32_t off, void *dst, uint32_t len)
{
	uint32_t cluster, size, pos = 0, done = 0;
	int err;

	err = fat_lookup(name, &cluster, &size);
	if (err) {
		return err;
	}
	if (off >= size) {
		return 0;
	}
	len = MIN(len, size - off);
	while (pos < off) {
		cluster = fat_next(cluster);
		if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
			return EIO;
		}
		pos += fat_cluster_bytes;
	}
	while (done < len) {
		uint32_t in = (off + done) - pos;
		uint32_t n = MIN(len - done, fat_cluster_bytes - in);

		err = emmc_read(MODEM_LBA + fat_cluster_sector(cluster), fat_spc,
		    bounce);
		if (err) {
			return err;
		}
		memcpy((uint8_t *)dst + done, (uint8_t *)bounce + in, n);
		done += n;
		if (done < len) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				return EIO;
			}
			pos += fat_cluster_bytes;
		}
	}
	return (int)done;
}

/* --- rmtfs: EFS partitions and shared buffer (0722) ---------------------- */

#define PART_MAX        4
#define PART_MAX_SIZE   (16u << 20)
#define PART_CHUNK      (512u << 10)
#define RMTFS_BUF_SIZE  0x300000u       /* 3 MiB, like qcom,rmtfs_sharedmem */

struct part_copy {
	boolean_t used;
	char name[16];
	uint64_t lba;
	uint32_t size;
	uint8_t *buf;
};

static struct part_copy g_parts[PART_MAX];
static vm_offset_t g_rmtfs_va;
static uint32_t g_rmtfs_pa;
static boolean_t g_rmtfs_ready;

static uint64_t
le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int
gpt_lookup(const char *name, uint64_t *lba, uint64_t *sectors)
{
	uint8_t *h = bounce;
	uint64_t entry_lba;
	uint32_t nent, esz, blocks;
	int err;

	err = emmc_read(1, 1, h);
	if (err) {
		return err;
	}
	if (memcmp(h, "EFI PART", 8) != 0) {
		printf("SWEET MODEM: eMMC has no GPT\n");
		return ENXIO;
	}
	entry_lba = le64(h + 72);
	nent = le32(h + 80);
	esz = le32(h + 84);
	if (esz != 128 || nent == 0 || nent > FAT_BOUNCE_SIZE / 128) {
		printf("SWEET MODEM: unsupported GPT (%u x %u)\n", nent, esz);
		return EINVAL;
	}
	blocks = (nent * 128 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	err = emmc_read(entry_lba, blocks, h);
	if (err) {
		return err;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = h + (uint64_t)i * 128;
		char n[37];
		unsigned c;

		for (c = 0; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] |
			    (e[57 + 2 * c] << 8));

			if (ch == 0 || ch > 0x7f) {
				break;
			}
			n[c] = (char)ch;
		}
		n[c] = 0;
		if (c == 0 || strcasecmp(n, name) != 0) {
			continue;
		}
		*lba = le64(e + 32);
		*sectors = le64(e + 40) - *lba + 1;
		return 0;
	}
	return ENOENT;
}

/*
 * Copy one EFS partition to RAM once. The modem writes its EFS through the
 * host, so the copy absorbs the writes and the eMMC is only read (0722).
 */
int
lavender_modem_part_prepare(const char *name, uint32_t *size)
{
	struct part_copy *p = NULL;
	uint64_t lba, sectors, bytes, off;
	int err;

	for (unsigned i = 0; i < PART_MAX; i++) {
		if (g_parts[i].used && strcasecmp(g_parts[i].name, name) == 0) {
			*size = g_parts[i].size;
			return 0;
		}
		if (!g_parts[i].used && p == NULL) {
			p = &g_parts[i];
		}
	}
	if (p == NULL) {
		return ENOSPC;
	}
	err = gpt_lookup(name, &lba, &sectors);
	if (err) {
		printf("SWEET MODEM: rmtfs partition %s not in the GPT (%d)\n",
		    name, err);
		return err;
	}
	bytes = sectors * SECTOR_SIZE;
	if (bytes == 0 || bytes > PART_MAX_SIZE) {
		printf("SWEET MODEM: rmtfs partition %s has %llu bytes\n", name,
		    (unsigned long long)bytes);
		return EFBIG;
	}
	if (kmem_alloc_contig(kernel_map, (vm_offset_t *)&p->buf,
	    (vm_size_t)bytes, PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MODEM: rmtfs partition %s: no %llu bytes\n", name,
		    (unsigned long long)bytes);
		p->buf = NULL;
		return ENOMEM;
	}
	for (off = 0; off < bytes; off += PART_CHUNK) {
		uint32_t n = (uint32_t)MIN((uint64_t)PART_CHUNK, bytes - off);

		err = emmc_read(lba + off / SECTOR_SIZE, n / SECTOR_SIZE,
		    p->buf + off);
		if (err) {
			printf("SWEET MODEM: rmtfs %s read failed %d at %llu\n",
			    name, err, (unsigned long long)off);
			kmem_free(kernel_map, (vm_offset_t)p->buf,
			    (vm_size_t)bytes);
			p->buf = NULL;
			return err;
		}
	}
	p->used = TRUE;
	p->lba = lba;
	p->size = (uint32_t)bytes;
	memset(p->name, 0, sizeof(p->name));
	for (unsigned i = 0; i + 1 < sizeof(p->name) && name[i] != 0; i++) {
		p->name[i] = name[i];
	}
	printf("SWEET MODEM: rmtfs %s: %llu sectors at %llx, %u bytes "
	    "copied to RAM\n", name, (unsigned long long)sectors,
	    (unsigned long long)lba, p->size);
	*size = p->size;
	return 0;
}

static struct part_copy *
part_find(const char *name)
{
	for (unsigned i = 0; i < PART_MAX; i++) {
		if (g_parts[i].used && strcasecmp(g_parts[i].name, name) == 0) {
			return &g_parts[i];
		}
	}
	return NULL;
}

int
lavender_modem_part_read(const char *name, uint32_t off, void *dst,
    uint32_t len)
{
	struct part_copy *p = part_find(name);

	if (p == NULL) {
		return ENXIO;
	}
	if (off >= p->size) {
		return 0;
	}
	len = MIN(len, p->size - off);
	memcpy(dst, p->buf + off, len);
	return (int)len;
}

int
lavender_modem_part_write(const char *name, uint32_t off, const void *src,
    uint32_t len)
{
	struct part_copy *p = part_find(name);

	if (p == NULL) {
		return ENXIO;
	}
	if (off >= p->size) {
		return 0;
	}
	len = MIN(len, p->size - off);
	memcpy(p->buf + off, src, len);
	return (int)len;
}

/*
 * The modem stages rmtfs transfers in this buffer. Like topaz, take it from
 * the kernel's contiguous pool and hand it to HLOS and the MSS: the modem
 * needs to read it and the AP has to keep writing it for RW_IOVEC.
 */
int
lavender_modem_rmtfs_buf(uint64_t *pa, uint32_t *size)
{
	if (!g_rmtfs_ready) {
		uint32_t src[1] = { VMID_HLOS };
		uint32_t dst[2] = { VMID_HLOS, VMID_MSS_MSA };
		uint32_t perm[2] = { PERM_READ | PERM_WRITE,
		    PERM_READ | PERM_WRITE };
		vm_offset_t va = 0;
		uint32_t pa32;
		int r;

		if (kmem_alloc_contig(kernel_map, &va, RMTFS_BUF_SIZE,
		    PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || va == 0) {
			printf("SWEET MODEM: rmtfs buf alloc failed\n");
			return ENOMEM;
		}
		pa32 = (uint32_t)pmap_find_phys(kernel_pmap,
		    (addr64_t)va) << PAGE_SHIFT;
		printf("SWEET MODEM: rmtfs buf va %08x pa %08x alloc ok\n",
		    (uint32_t)va, pa32);
		if (pa32 == 0 || pa32 + RMTFS_BUF_SIZE < pa32) {
			printf("SWEET MODEM: rmtfs buf pa out of range\n");
			kmem_free(kernel_map, va, RMTFS_BUF_SIZE);
			return EINVAL;
		}
		r = scm_mem_assign(pa32, RMTFS_BUF_SIZE, src, 1, dst, perm, 2);
		printf("SWEET MODEM: rmtfs buf %08x assign HLOS+MSS ret %d\n",
		    pa32, r);
		if (r != 0) {
			kmem_free(kernel_map, va, RMTFS_BUF_SIZE);
			return EIO;
		}
		g_rmtfs_pa = pa32;
		/*
		 * Map the same PA as Device (no cache): the modem reads and
		 * writes this buffer directly, and flush_dcache64() on these
		 * pages faults inside FlushPoC_DcacheRegion (0733 panic).
		 */
		g_rmtfs_va = ml_io_map_unmappable(pa32, RMTFS_BUF_SIZE,
		    VM_WIMG_IO);
		printf("SWEET MODEM: rmtfs buf %08x map %s va %08x\n", pa32,
		    g_rmtfs_va != 0 ? "ok" : "FAILED", (uint32_t)g_rmtfs_va);
		if (g_rmtfs_va == 0) {
			return ENOMEM;
		}
		g_rmtfs_ready = TRUE;
		printf("SWEET MODEM: rmtfs buffer %08x+%x ready\n", pa32,
		    RMTFS_BUF_SIZE);
	}
	*pa = g_rmtfs_pa;
	*size = RMTFS_BUF_SIZE;
	return 0;
}

int
lavender_modem_rmtfs_copy(uint32_t off, void *buf, uint32_t len,
    boolean_t to_buf)
{
	if (!g_rmtfs_ready) {
		return ENXIO;
	}
	if (off > RMTFS_BUF_SIZE || len > RMTFS_BUF_SIZE - off) {
		return EINVAL;
	}
	if (to_buf) {
		io_copy(g_rmtfs_va + off, buf, len);
	} else {
		io_read(buf, g_rmtfs_va + off, len);
	}
	return (int)len;
}

/*
 * Stream one segment blob; its directory size must match p_filesz. The blob
 * is copied into the modem carve-out through IO_CHUNK windows.
 */
static int
fat_read_blob(const char *name, uint32_t pa, uint32_t want)
{
	uint32_t cluster, size, off = 0, win = 0, used = 0;
	vm_offset_t va = 0;
	int err;

	printf("SWEET MODEM: %s lookup\n", name);
	err = fat_lookup(name, &cluster, &size);
	if (err) {
		printf("SWEET MODEM: %s not found\n", name);
		return err;
	}
	if (size != want) {
		printf("SWEET MODEM: %s is %u bytes, expected %u\n", name, size,
		    want);
		return EINVAL;
	}
	while (off < size) {
		uint32_t n;

		if (va == 0 || used == win) {
			if (va != 0) {
				ml_io_unmap(trunc_page(va),
				    round_page(va - trunc_page(va) + win));
			}
			win = MIN(size - off, IO_CHUNK);
			printf("SWEET MODEM: %s window %08x+%x\n", name,
			    pa + off, win);
			va = ml_io_map_unmappable(pa + off, win, VM_WIMG_IO);
			if (va == 0) {
				printf("SWEET MODEM: %s map failed at %u\n",
				    name, off);
				return ENOMEM;
			}
			printf("SWEET MODEM: %s mapped va %08x\n", name,
			    (uint32_t)va);
			used = 0;
		}
		n = MIN(size - off, fat_cluster_bytes);
		if (used == 0 || (used & 0xfffff) == 0) {
			printf("SWEET MODEM: %s read %u/%u\n", name, off, size);
		}
		err = emmc_read(MODEM_LBA + fat_cluster_sector(cluster),
		    fat_spc, bounce);
		if (err) {
			printf("SWEET MODEM: %s read failed %d at %u\n", name,
			    err, off);
			break;
		}
		io_copy(va + used, bounce, n);
		used += n;
		off += n;
		if (off < size) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				printf("SWEET MODEM: %s chain ends at %u/%u\n",
				    name, off, size);
				err = EIO;
				break;
			}
		}
	}
	if (va != 0) {
		ml_io_unmap(trunc_page(va),
		    round_page(va - trunc_page(va) + win));
	}
	return err;
}

/* --- MBA ------------------------------------------------------------------ */

static uint64_t
modem_scm7(uint32_t fn, uint32_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t extra_pa, uint64_t *res)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo,
	    x2 __asm__("x2") = a0, x3 __asm__("x3") = a1,
	    x4 __asm__("x4") = a2, x5 __asm__("x5") = extra_pa,
	    x6 __asm__("x6") = 0;

	for (int n = 0; n < 1000; n++) {
		__asm__ volatile ("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
		      "+r"(x5), "+r"(x6)
		    :
		    : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
		      "x15", "x16", "x17", "memory");
		if (x0 != 1) {
			break;
		}
		x0 = 1;
		x1 = arginfo;
		x2 = a0;
		x3 = a1;
		x4 = a2;
		x5 = extra_pa;
		x6 = 0;
	}
	*res = x1;
	return x0;
}

static int
modem_q6_reset(void)
{
	uint32_t v, mem;
	int i;

	modem_announce("run the QDSP6 power-up and reset sequence");
	v = mrd(q6, Q6_RESET);
	mwr(q6, Q6_RESET, v | Q6SS_STOP_CORE | Q6SS_CORE_ARES | Q6SS_BUS_ARES_ENA);

	/* BHS requires the XO branch clock. */
	v = mrd(q6, Q6_XO_CBCR);
	mwr(q6, Q6_XO_CBCR, v | 1u);
	for (i = 0; i < 200; i++) {
		if (!(mrd(q6, Q6_XO_CBCR) & CBCR_OFF)) {
			break;
		}
		delay(1);
	}
	if (i == 200) {
		printf("SWEET MODEM: xo_cbcr did not come on (%08x)\n",
		    mrd(q6, Q6_XO_CBCR));
		return ETIMEDOUT;
	}
	printf("SWEET MODEM: q6 xo_cbcr %08x\n", mrd(q6, Q6_XO_CBCR));

	v = mrd(q6, Q6_PWR_CTL);
	mwr(q6, Q6_PWR_CTL, v | QDSP6v55_BHS_ON);
	__asm__ volatile("dsb sy" ::: "memory");
	delay(1);
	for (i = 0; i < 200; i++) {
		if (mrd(q6, Q6V62_BHS_STATUS) & QDSP6v55_BHS_EN_REST_ACK) {
			break;
		}
		delay(1);
	}
	if (i == 200) {
		printf("SWEET MODEM: BHS_EN_REST_ACK not set (%08x)\n",
		    mrd(q6, Q6V62_BHS_STATUS));
		return ETIMEDOUT;
	}
	printf("SWEET MODEM: q6 bhs62 %08x\n", mrd(q6, Q6V62_BHS_STATUS));

	v = mrd(q6, Q6_PWR_CTL);
	v |= QDSP6v55_LDO_BYP;
	mwr(q6, Q6_PWR_CTL, v);
	v &= ~QDSP6v55_CLAMP_QMC_MEM;
	mwr(q6, Q6_PWR_CTL, v);
	v |= Q6SS_L2DATA_STBY_N | Q6SS_SLP_RET_N;
	mwr(q6, Q6_PWR_CTL, v);

	/* Turn on the memories one bit at a time (q6v62-1-5: bits 29..0). */
	mem = mrd(q6, Q6V6_MEM_PWR_CTL);
	for (i = 29; i >= 0; i--) {
		mem |= (1u << i);
		mwr(q6, Q6V6_MEM_PWR_CTL, mem);
		(void)mrd(q6, Q6V6_MEM_PWR_CTL);
		delay(1);
	}

	v = mrd(q6, Q6_PWR_CTL);
	v &= ~QDSP6v55_CLAMP_WL;
	mwr(q6, Q6_PWR_CTL, v);
	v &= ~Q6SS_CLAMP_IO;
	mwr(q6, Q6_PWR_CTL, v);

	v = mrd(q6, Q6_RESET);
	v &= ~(Q6SS_CORE_ARES | Q6SS_STOP_CORE);
	mwr(q6, Q6_RESET, v);

	v = mrd(q6, Q6_GFMUX_CTL);
	v |= Q6SS_CLK_ENA;
	mwr(q6, Q6_GFMUX_CTL, v);
	printf("SWEET MODEM: q6 reset %08x gfmux %08x pwr %08x mem_pwr %08x\n",
	    mrd(q6, Q6_RESET), mrd(q6, Q6_GFMUX_CTL), mrd(q6, Q6_PWR_CTL),
	    mrd(q6, Q6V6_MEM_PWR_CTL));
	return 0;
}

static int
modem_boot_mba(void)
{
	uint32_t status = 0;
	int err, i;

	modem_announce("read MBA.MBN from the modem partition");
	io_zero(mba, MBA_ASSIGN_SIZE);
	err = fat_read_to_va("MBA.MBN", mba, MBA_ASSIGN_SIZE);
	if (err) {
		return err;
	}
	if (mrd(mba, 0) != 0x464c457fu) {
		printf("SWEET MODEM: MBA.MBN has no ELF magic (%08x)\n",
		    mrd(mba, 0));
		return EINVAL;
	}

	modem_announce("assign the MBA region to MSS");
	err = scm_assign(MBA_PA, MBA_ASSIGN_SIZE, VMID_HLOS, VMID_MSS_MSA,
	    PERM_READ | PERM_WRITE);
	if (err) {
		return EIO;
	}

	modem_announce("program RMB and reset the Q6");
	mwr(rmb, RMB_MBA_IMAGE, MBA_PA);
	mwr(rmb, RMB_PMI_CODE_START, 0);
	mwr(rmb, RMB_PMI_CODE_LENGTH, 0);
	printf("SWEET MODEM: RMB image %08x code_start %08x code_len %08x\n",
	    mrd(rmb, RMB_MBA_IMAGE), mrd(rmb, RMB_PMI_CODE_START),
	    mrd(rmb, RMB_PMI_CODE_LENGTH));
	err = modem_q6_reset();
	if (err) {
		return err;
	}

	for (i = 0; i < 500; i++) {
		status = mrd(rmb, RMB_PBL_STATUS);
		if (status != 0) {
			break;
		}
		modem_wait(10);
	}
	printf("SWEET MODEM: pbl status %08x after %d ms\n", status, i * 10);
	if (status != STATUS_PBL_SUCCESS) {
		printf("SWEET MODEM: PBL did not report success\n");
		return EIO;
	}

	status = 0;
	for (i = 0; i < 500; i++) {
		status = mrd(rmb, RMB_MBA_STATUS);
		if (status != 0) {
			break;
		}
		modem_wait(10);
	}
	printf("SWEET MODEM: mba status %08x after %d ms\n", status, i * 10);
	if (status != STATUS_XPU_UNLOCKED &&
	    status != STATUS_XPU_UNLOCKED_SCRIBBLED) {
		printf("SWEET MODEM: MBA did not unlock\n");
		return EIO;
	}
	printf("SWEET MODEM: MBA alive (pbl %08x, mba %08x)\n",
	    mrd(rmb, RMB_PBL_STATUS), mrd(rmb, RMB_MBA_STATUS));
	return 0;
}

/* --- MDT, segments and final authentication (F1 step 3) ------------------- */

#define MDT_BUF_SIZE 16384u

static int
modem_load_mdt(void)
{
	const uint8_t *mdt = mdt_buf;
	uint32_t len = 0;
	int32_t status = 0;
	int err, i;

	modem_announce("read MODEM.MDT and authenticate the header");
	err = fat_read_to_buf("MODEM.MDT", mdt_buf, MDT_BUF_SIZE, &len);
	if (err) {
		return err;
	}
	if (len < 0x34 || memcmp(mdt, "\177ELF", 4) != 0) {
		printf("SWEET MODEM: MODEM.MDT is not the expected ELF\n");
		return EINVAL;
	}
	printf("SWEET MODEM: MODEM.MDT %u bytes, entry %08x phnum %u\n", len,
	    le32(mdt + 24), le16(mdt + 44));

	io_zero(mba + MBA_ASSIGN_SIZE, 0x3000);
	io_copy(mba + MBA_ASSIGN_SIZE, mdt_buf, len);
	err = scm_assign(MBA_PA + MBA_ASSIGN_SIZE, 0x3000, VMID_HLOS,
	    VMID_MSS_MSA, PERM_READ | PERM_WRITE);
	if (err) {
		return EIO;
	}

	mwr(rmb, RMB_PMI_CODE_LENGTH, 0);
	mwr(rmb, RMB_PMI_META_DATA, MBA_PA + MBA_ASSIGN_SIZE);
	mwr(rmb, RMB_MBA_COMMAND, CMD_META_DATA_READY);
	for (i = 0; i < 1000; i++) {
		status = (int32_t)mrd(rmb, RMB_MBA_STATUS);
		if (status == (int32_t)STATUS_META_DATA_AUTH_SUCCESS ||
		    status < 0) {
			break;
		}
		modem_wait(10);
	}
	printf("SWEET MODEM: mdt auth status %d after %d ms\n", status, i * 10);
	if (status != (int32_t)STATUS_META_DATA_AUTH_SUCCESS) {
		printf("SWEET MODEM: MBA rejected the MDT header\n");
		return EIO;
	}
	(void)scm_assign(MBA_PA + MBA_ASSIGN_SIZE, 0x3000, VMID_MSS_MSA,
	    VMID_HLOS, PERM_READ | PERM_WRITE | PERM_EXEC);
	return 0;
}

static int
modem_load_segments(void)
{
	const uint8_t *mdt = mdt_buf;
	const uint8_t *ph;
	uint32_t phoff, phnum, region_start = 0xffffffffu, region_end = 0;
	uint32_t i, len, paddr, filesz, memsz, status;
	uint32_t off, chunk, region_size;
	int err;

	phoff = le32(mdt + 28);
	phnum = le16(mdt + 44);
	if (phoff < 0x34 || phnum == 0 || phoff + phnum * 32 > MDT_BUF_SIZE) {
		printf("SWEET MODEM: MDT program headers are invalid\n");
		return EINVAL;
	}
	for (i = 0; i < phnum; i++) {
		ph = mdt + phoff + i * 32;
		if (le32(ph) != 1 ||
		    (le32(ph + 28) & (7u << 24)) == (2u << 24) ||
		    le32(ph + 20) == 0) {
			continue;
		}
		paddr = le32(ph + 12);
		memsz = le32(ph + 20);
		if (paddr < region_start) {
			region_start = paddr;
		}
		if (paddr + memsz > region_end) {
			region_end = paddr + memsz;
		}
	}
	region_start &= ~0xfffu;
	region_end = (region_end + 0xfffu) & ~0xfffu;
	if (region_start < MODEM_FW_PA ||
	    region_end > MODEM_FW_PA + MODEM_FW_SIZE ||
	    region_end <= region_start) {
		printf("SWEET MODEM: image limits %08x..%08x outside the "
		    "carve-out\n", region_start, region_end);
		return EINVAL;
	}
	region_size = region_end - region_start;
	printf("SWEET MODEM: modem image %08x..%08x (%u bytes)\n", region_start,
	    region_end, region_size);
	/* The image is written through io_zero_pa()/fat_read_blob() windows. */

	{
		uint32_t src[1] = { VMID_HLOS };
		uint32_t dst[2] = { VMID_HLOS, VMID_MSS_MSA };
		uint32_t perm[2] = { PERM_READ | PERM_WRITE,
		    PERM_READ | PERM_WRITE };

		for (off = 0; off < region_size; off += SCM_CHUNK) {
			chunk = MIN(SCM_CHUNK, region_size - off);
			printf("SWEET MODEM: region chunk %08x+%x\n",
			    region_start + off, chunk);
			err = scm_mem_assign(region_start + off, chunk, src, 1,
			    dst, perm, 2);
			if (err) {
				return EIO;
			}
		}
	}

	len = 0;
	mwr(rmb, RMB_PMI_CODE_LENGTH, 0);
	for (i = 0; i < phnum; i++) {
		char name[16];

		ph = mdt + phoff + i * 32;
		if (le32(ph) != 1 ||
		    (le32(ph + 28) & (7u << 24)) == (2u << 24) ||
		    le32(ph + 20) == 0) {
			continue;
		}
		paddr = le32(ph + 12);
		filesz = le32(ph + 16);
		memsz = le32(ph + 20);
		if (paddr < region_start || paddr + memsz > region_end) {
			printf("SWEET MODEM: blob %u is outside the image\n", i);
			return EINVAL;
		}
		if (filesz != 0) {
			snprintf(name, sizeof(name), "MODEM.B%02u", i);
			printf("SWEET MODEM: loading %s at %08x (%u)\n", name,
			    paddr, filesz);
			err = fat_read_blob(name, paddr, filesz);
			if (err) {
				return err;
			}
		}
		if (memsz > filesz) {
			printf("SWEET MODEM: blob %02u zero %08x+%x\n", i,
			    paddr + filesz, memsz - filesz);
			err = io_zero_pa(paddr + filesz, memsz - filesz);
			if (err) {
				return err;
			}
		}
		if (len == 0) {
			mwr(rmb, RMB_PMI_CODE_START, paddr);
			mwr(rmb, RMB_MBA_COMMAND, CMD_LOAD_READY);
		}
		len += memsz;
		mwr(rmb, RMB_PMI_CODE_LENGTH, len);
		status = mrd(rmb, RMB_MBA_STATUS);
		printf("SWEET MODEM: blob %02u %08x filesz %u memsz %u len %08x "
		    "status %08x\n", i, paddr, filesz, memsz, len, status);
		if ((int32_t)status < 0) {
			printf("SWEET MODEM: MBA rejected blob %u\n", i);
			return EIO;
		}
	}

	{
		uint32_t src[2] = { VMID_HLOS, VMID_MSS_MSA };
		uint32_t dst[1] = { VMID_MSS_MSA };
		uint32_t perm[1] = { PERM_READ | PERM_WRITE };

		for (off = 0; off < region_size; off += SCM_CHUNK) {
			chunk = MIN(SCM_CHUNK, region_size - off);
			printf("SWEET MODEM: reclaim chunk %08x+%x\n",
			    region_start + off, chunk);
			err = scm_mem_assign(region_start + off, chunk, src, 2,
			    dst, perm, 1);
			if (err) {
				return EIO;
			}
		}
	}
	return 0;
}

static int
modem_auth(void)
{
	int32_t status = 0;
	int i;

	modem_announce("wait for the MBA image authentication");
	for (i = 0; i < 1000; i++) {
		status = (int32_t)mrd(rmb, RMB_MBA_STATUS);
		if (status == (int32_t)STATUS_AUTH_COMPLETE || status < 0) {
			break;
		}
		modem_wait(10);
	}
	printf("SWEET MODEM: auth status %d after %d ms\n", status, i * 10);
	if (status != (int32_t)STATUS_AUTH_COMPLETE) {
		printf("SWEET MODEM: modem image authentication failed\n");
		return EIO;
	}
	(void)scm_assign(MBA_PA, MBA_ASSIGN_SIZE, VMID_MSS_MSA, VMID_HLOS,
	    PERM_READ | PERM_WRITE | PERM_EXEC);
	printf("SWEET MODEM: auth complete, modem is running\n");
	return 0;
}

/* --- thread --------------------------------------------------------------- */

static void
lavender_modem_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t step = 0, delay_s = 25, clk_mask = 0x3f;

	(void)PE_parse_boot_argn("sweetmodem", &step, sizeof(step));
	(void)PE_parse_boot_argn("sweetmodemdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("sweetmodemclk", &clk_mask, sizeof(clk_mask));
	if (delay_s > 300) {
		delay_s = 300;
	}
	printf("SWEET MODEM: F1 step %u in %u s, clock mask %02x\n", step,
	    delay_s, clk_mask);
	modem_wait(delay_s * 1000);

	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	rst = ml_io_map(MSS_RESTART_PA, MSS_RESTART_SIZE);
	if (gcc == 0 || rst == 0) {
		printf("SWEET MODEM: GCC/restart map failed (gcc %p rst %p)\n",
		    (void *)gcc, (void *)rst);
		return;
	}
	modem_restart_deassert();
	modem_clocks(clk_mask);

	if (step < 1) {
		printf("SWEET MODEM: step %u does nothing\n", step);
		return;
	}
	q6 = ml_io_map(QDSP6_PA, QDSP6_SIZE);
	rmb = ml_io_map(RMB_PA, RMB_SIZE);
	if (q6 == 0 || rmb == 0) {
		printf("SWEET MODEM: QDSP6/RMB map failed (q6 %p rmb %p)\n",
		    (void *)q6, (void *)rmb);
		return;
	}
	modem_dump_q6();

	if (step < 2) {
		printf("SWEET MODEM: step 1 done (no firmware loaded)\n");
		return;
	}

	mba = ml_io_map(MBA_PA, MBA_SIZE);
	if (mba == 0) {
		printf("SWEET MODEM: MBA map failed\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, (vm_offset_t *)&fat_buf, FAT_DIR_MAX,
	    PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MODEM: no contiguous memory for the FAT cache\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, (vm_offset_t *)&bounce,
	    FAT_BOUNCE_SIZE, PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc_contig(kernel_map, (vm_offset_t *)&scm_buf,
	    SCM_BUF_SIZE, PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MODEM: no contiguous bounce/SCM buffer\n");
		return;
	}
	{
		ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)scm_buf);

		if (pn == 0) {
			printf("SWEET MODEM: SCM buffer has no physical page\n");
			return;
		}
		scm_pa = ptoa_64(pn);
	}

	if (fat_open() == 0) {
		int err = modem_boot_mba();

		if (err) {
			printf("SWEET MODEM: step 2 failed %d\n", err);
		} else if (step >= 3) {
			if (kmem_alloc(kernel_map, (vm_offset_t *)&mdt_buf,
			    MDT_BUF_SIZE, KMA_DATA | KMA_ZERO,
			    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				printf("SWEET MODEM: no memory for the MDT\n");
			} else if (modem_load_mdt() == 0 &&
			    modem_load_segments() == 0) {
				if (modem_auth() == 0) {
					lavender_wlan_after_modem();
				}
			}
		}
	}
	printf("SWEET MODEM: step %u finished\n", step);
}

int lavender_modem_probe(void);

int
lavender_modem_probe(void)
{
	static int started;
	uint32_t step = 0;
	thread_t thread;

	if (started ||
	    !PE_parse_boot_argn("sweetmodem", &step, sizeof(step)) ||
	    step == 0) {
		return 0;
	}
	started = 1;
	if (kernel_thread_start(lavender_modem_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_LAVENDER */

int lavender_modem_probe(void);
int
lavender_modem_probe(void)
{
	return 0;
}

#endif
