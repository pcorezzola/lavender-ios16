/*
 * lavender: Wi-Fi (WCN3990) platform probe, F0b/F2. Boot-arg gated.
 *
 *   sweetwlan=1       run the F0b platform probe (read only)
 *   sweetwlan=2       run the F2 SMEM/GLINK probe after the modem auths
 *   sweetwlan=3       also create the SMP2P out item, answer IPA and wait
 *                     for the modem READY
 *   sweetwlandelay=S  seconds to wait after boot before the F0b probe
 *                     (default 25), so the console ring flusher and the USB
 *                     live log are up
 *
 * The WCN3990 is an icnss child of the ANOC2 SMMU (SIDs 0x1a00/0x1a01), not
 * of the apps SMMU: in the DTB the icnss iommus phandle resolves to
 * /soc/arm,smmu-anoc2@16c0000. The modem DSP runs wlanmdsp.mbn and talks to
 * the chip over the CE/HTC transport. Before writing any of that, this probe
 * answers the questions that decide the port:
 *
 *   - did the bootloader leave the MSS GCC branches on (0x8a000 cfg_ahb,
 *     0x8a004 mnoc_bimc_axi, 0x8a03c snoc_axi, 0x8a040 q6_bimc_axi)?
 *   - what does the ANOC2 SMMU have for stream IDs 0x1a00/0x1a01, and is it
 *     translating, in bypass or faulting?
 *   - is the SMEM header at 0x86000000 intact?
 *   - is the WLAN MSA carve-out at 0x85700000 still zero, or did the
 *     bootloader leave something there?
 *
 * F2 (`sweetwlan=2`) is called from the modem thread after AUTH_COMPLETE and
 * reads the SMEM v12 $TOC/$PRT, the APPS/MODEM partition items, SMP2P
 * 428/435, the GLINK descriptor 478/FIFOs 479-480 and the APCS IPC register.
 * It also READS ONLY: it does not allocate SMEM items, take the hwlock or
 * ring a doorbell. Those come after this probe says what the modem created.
 *
 * The QDSP6 (0x04080000) and RMB (0x04180000) blocks are not touched by the
 * F0b probe: the modem is still in reset, and a read there can hang the bus
 * or reset the SoC. Every step is announced before the access and spaced 2 s
 * so the log leaves the machine even if the access kills it.
 *
 * Register meanings: Linux gcc-sdm660.c, arm-smmu.c and glink_smem_native
 * xprt.c, read as a behaviour reference (values only, no code copied); DTB
 * values from
 * work/briefs/wifi-wcn3990-brief.md section 1.
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
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern int lavender_modem_fw_size(const char *name, uint32_t *size);
extern int lavender_modem_fw_read(const char *name, uint32_t off, void *dst,
    uint32_t len);
extern int lavender_modem_part_prepare(const char *name, uint32_t *size);
extern int lavender_modem_part_read(const char *name, uint32_t off, void *dst,
    uint32_t len);
extern int lavender_modem_part_write(const char *name, uint32_t off,
    const void *src, uint32_t len);
extern int lavender_modem_rmtfs_buf(uint64_t *pa, uint32_t *size);
extern int lavender_modem_rmtfs_copy(uint32_t off, void *buf, uint32_t len,
    boolean_t to_buf);
extern int lavender_modem_assign(uint32_t pa, uint32_t size,
    const uint32_t *src, int nsrc, const uint32_t *dst, const uint32_t *perm,
    int ndst);

#define GCC_PA          0x00100000u
#define GCC_SIZE        0x94000u
#define ANOC2_PA        0x016c0000u
#define ANOC2_SIZE      0x40000u
#define SMEM_PA         0x86000000u
#define SMEM_SIZE       0x00001000u
#define MSA_PA          0x85700000u
#define MSA_SIZE        0x00001000u

/* F2: the whole SMEM v12 window, the APCS IPC register and the item IDs */
#define SMEM_F2_SIZE     0x00200000u
#define SMEM_TOC_OFF     (SMEM_F2_SIZE - 0x1000u)
#define APCS_PA          0x17911000u
#define APCS_SIZE        0x00001000u
#define APCS_IPC_OFF     0x8u
#define SMEM_HOST_APPS   0x0000u
#define SMEM_HOST_MODEM  0x0001u
#define ITEM_SMP2P_OUT   428u
#define ITEM_SMP2P_IN    435u
#define ITEM_GLINK_DESC  478u
#define ITEM_GLINK_TX    479u
#define ITEM_GLINK_RX    480u
#define SMP2P_MAGIC      0x504d5324u     /* "$SMP" */
#define SMP2P_MAX_ENTRY  16
#define SMP2P_IRQ_BIT    14              /* qcom,irq-bitmask 0x4000 */
#define SMP2P_SSR_ACK    1

/* SMEM allocation lock: qcom,ipc-spinlock-sfpb @0x01f40000, lock 3 */
#define TCSR_PA          0x01f43000u
#define TCSR_SIZE        0x00001000u
#define SMEM_HWLOCK_OWNER 1u             /* APPS */

/* SMEM v12 (qcom_smem): TOC at the end of the window, partitions in it. */
struct smem_pt_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, rsvd[7];
} __attribute__((packed));
struct smem_pt {
	uint8_t magic[4];
	uint32_t version, num_entries, rsvd[5];
} __attribute__((packed));
struct smem_part_hdr {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, rsvd[3];
} __attribute__((packed));
struct smem_priv_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t pad_data, pad_hdr;
	uint32_t rsvd;
} __attribute__((packed));
struct smp2p_hdr {
	uint32_t magic;
	uint8_t version, features[3];
	uint16_t local_pid, remote_pid, total, valid;
	uint32_t flags;
} __attribute__((packed));
struct smp2p_entry {
	char name[16];
	uint32_t value;
} __attribute__((packed));

/* GCC byte offsets from GCC_PA (gcc-sdm660.c) */
#define GCC_MSS_CFG_AHB         0x8a000u
#define GCC_MSS_MNOC_BIMC_AXI   0x8a004u
#define GCC_MSS_SNOC_AXI        0x8a03cu
#define GCC_MSS_Q6_BIMC_AXI     0x8a040u
#define CBCR_EN                 (1u << 0)
#define CBCR_OFF                (1u << 31)

/* ANOC2 SMMU GR0 (arm-smmu.c, qcom,smmu-v2) */
#define SMMU_SCR0               0x0000u
#define SMMU_SACR               0x0010u
#define SMMU_IDR0               0x0020u
#define SMMU_IDR1               0x0024u
#define SMMU_IDR7               0x003cu
#define SMMU_SGFAR              0x0040u
#define SMMU_SGFSR              0x0048u
#define SMMU_SMR(n)             (0x0800u + ((n) << 2))
#define SMMU_S2CR(n)            (0x0c00u + ((n) << 2))
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_SMR_MASK_SHIFT     16
#define SMMU_SMR_MASK           0x7fffu
#define SMMU_SMR_ID             0x7fffu
#define SMMU_S2CR_TYPE_SHIFT    16
#define SMMU_S2CR_CBNDX         0xffu
#define SMMU_MAX_SMRS           128
#define WLAN_SID0               0x1a00u
#define WLAN_SID1               0x1a01u

static vm_offset_t gcc, smmu, smem, msa, apcs, tcsr;
static int wlan_chan;
static uint32_t wlan_mode;
static uint32_t g_cacheline = 0x20;

void lavender_wlan_after_modem(void);

static inline uint32_t
wlan_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static void
wlan_wait(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&wlan_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
wlan_announce(const char *what)
{
	printf("SWEET WLAN: about to %s\n", what);
	wlan_wait(2000);
}

static void
wlan_gcc(void)
{
	static const struct {
		const char *name;
		uint32_t off;
	} branches[] = {
		{ "mss_cfg_ahb",      GCC_MSS_CFG_AHB },
		{ "mss_mnoc_bimc_axi", GCC_MSS_MNOC_BIMC_AXI },
		{ "mss_snoc_axi",     GCC_MSS_SNOC_AXI },
		{ "mss_q6_bimc_axi",  GCC_MSS_Q6_BIMC_AXI },
	};
	unsigned on = 0;

	wlan_announce("read the GCC MSS branches (read only)");
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		uint32_t v = wlan_rd(gcc, branches[i].off);

		if (!(v & CBCR_OFF)) {
			on++;
		}
		printf("SWEET WLAN: gcc %s %08x (en %u, %s)\n", branches[i].name,
		    v, v & CBCR_EN, (v & CBCR_OFF) ? "OFF" : "on");
	}
	printf("SWEET WLAN: %u of 4 MSS branches are on\n", on);
}

static const char *
wlan_smmu_type(uint32_t s2cr)
{
	switch ((s2cr >> SMMU_S2CR_TYPE_SHIFT) & 3u) {
	case 0:
		return "translate";
	case 1:
		return "bypass";
	case 2:
		return "fault";
	default:
		return "reserved";
	}
}

static void
wlan_smmu(void)
{
	uint32_t idr0, idr1, n, valid = 0, wlan = 0;

	wlan_announce("read the ANOC2 SMMU global registers");
	idr0 = wlan_rd(smmu, SMMU_IDR0);
	idr1 = wlan_rd(smmu, SMMU_IDR1);
	printf("SWEET WLAN: ANOC2 sCR0 %08x sACR %08x IDR0 %08x IDR1 %08x "
	    "IDR7 %08x\n", wlan_rd(smmu, SMMU_SCR0), wlan_rd(smmu, SMMU_SACR),
	    idr0, idr1, wlan_rd(smmu, SMMU_IDR7));
	printf("SWEET WLAN: ANOC2 sGFAR %08x sGFSR %08x\n",
	    wlan_rd(smmu, SMMU_SGFAR), wlan_rd(smmu, SMMU_SGFSR));

	/*
	 * IDR0[7:0] is NUMSMRG. The architecture maximum is 128; a zero or a
	 * bogus count falls back to the full table so a WLAN SMR cannot hide.
	 */
	n = idr0 & 0xffu;
	if (n == 0 || n > SMMU_MAX_SMRS) {
		n = SMMU_MAX_SMRS;
	}
	wlan_announce("scan the SMMU stream-mapping table (SMR/S2CR)");
	for (uint32_t i = 0; i < n; i++) {
		uint32_t smr = wlan_rd(smmu, SMMU_SMR(i));
		uint32_t s2cr, id, mask;

		if (!(smr & SMMU_SMR_VALID)) {
			continue;
		}
		valid++;
		id = smr & SMMU_SMR_ID;
		mask = (smr >> SMMU_SMR_MASK_SHIFT) & SMMU_SMR_MASK;
		s2cr = wlan_rd(smmu, SMMU_S2CR(i));
		printf("SWEET WLAN: SMR[%u] %08x sid %04x mask %04x -> "
		    "S2CR %08x %s cb %u\n", i, smr, id, mask, s2cr,
		    wlan_smmu_type(s2cr), s2cr & SMMU_S2CR_CBNDX);
		if (((WLAN_SID0 & ~mask) == (id & ~mask)) ||
		    ((WLAN_SID1 & ~mask) == (id & ~mask))) {
			wlan++;
			printf("SWEET WLAN: SMR[%u] covers WLAN sid %04x\n", i,
			    id);
		}
	}
	printf("SWEET WLAN: SMMU %u SMRs scanned, %u valid, %u cover the "
	    "WLAN SIDs\n", n, valid, wlan);
}

/* Count nonzero words over the whole mapping, then dump the first 0x200. */
static void
wlan_region(const char *name, vm_offset_t base, uint32_t size)
{
	uint32_t off, nonzero = 0;

	for (off = 0; off < size; off += 4) {
		if (wlan_rd(base, off) != 0) {
			nonzero++;
		}
	}
	printf("SWEET WLAN: %s: %u of %u words are nonzero\n", name, nonzero,
	    size / 4);
	for (off = 0; off < 0x200 && off < size; off += 32) {
		printf("SWEET WLAN: %s %03x: %08x %08x %08x %08x %08x %08x "
		    "%08x %08x\n", name, off, wlan_rd(base, off),
		    wlan_rd(base, off + 4), wlan_rd(base, off + 8),
		    wlan_rd(base, off + 12), wlan_rd(base, off + 16),
		    wlan_rd(base, off + 20), wlan_rd(base, off + 24),
		    wlan_rd(base, off + 28));
	}
}

/* --- F2: SMEM / GLINK / SMP2P read-only probe ----------------------------- */

static void
wlan_magic4(const uint8_t *m, char out[5])
{
	for (int i = 0; i < 4; i++) {
		out[i] = (m[i] >= 0x20 && m[i] < 0x7f) ? (char)m[i] : '.';
	}
	out[4] = 0;
}

static struct smem_part_hdr *
wlan_f2_partition(uint8_t *base, uint16_t a, uint16_t b)
{
	struct smem_pt *pt = (struct smem_pt *)(base + SMEM_TOC_OFF);
	struct smem_pt_entry *e = (struct smem_pt_entry *)(pt + 1);
	uint32_t i, n;

	if (memcmp(pt->magic, "$TOC", 4) != 0) {
		return NULL;
	}
	n = pt->num_entries;
	if (n > 64) {
		n = 64;
	}
	for (i = 0; i < n; i++) {
		if (e[i].offset == 0 || e[i].size == 0 ||
		    e[i].offset >= SMEM_F2_SIZE) {
			continue;
		}
		if ((e[i].host0 == a && e[i].host1 == b) ||
		    (e[i].host0 == b && e[i].host1 == a)) {
			g_cacheline = e[i].cacheline != 0 ? e[i].cacheline :
			    0x20;
			return (struct smem_part_hdr *)(base + e[i].offset);
		}
	}
	return NULL;
}

static void
wlan_f2_toc(uint8_t *base)
{
	struct smem_pt *pt = (struct smem_pt *)(base + SMEM_TOC_OFF);
	struct smem_pt_entry *e = (struct smem_pt_entry *)(pt + 1);
	char magic[5];
	uint32_t i, n;

	if (memcmp(pt->magic, "$TOC", 4) != 0) {
		wlan_magic4((const uint8_t *)pt->magic, magic);
		printf("SWEET WLAN: no $TOC at %08x (got %s)\n",
		    SMEM_PA + SMEM_TOC_OFF, magic);
		return;
	}
	n = pt->num_entries;
	printf("SWEET WLAN: $TOC ver %u entries %u\n", pt->version, n);
	if (n > 64) {
		n = 64;
	}
	for (i = 0; i < n; i++) {
		struct smem_part_hdr *p;
		char pm[5];

		if (e[i].offset == 0 || e[i].size == 0) {
			continue;
		}
		printf("SWEET WLAN: toc[%u] off %05x size %05x host %04x/%04x "
		    "flags %x cl %x\n", i, e[i].offset, e[i].size, e[i].host0,
		    e[i].host1, e[i].flags, e[i].cacheline);
		/*
		 * 0713: only read partitions with an APPS host. The 0712 boot
		 * hung on the first modem<->adsp partition (0x1/0x2) header;
		 * cross-subsystem SMEM is not readable from the AP.
		 */
		if (e[i].host0 != SMEM_HOST_APPS &&
		    e[i].host1 != SMEM_HOST_APPS) {
			printf("SWEET WLAN:   (no APPS host, not read)\n");
			continue;
		}
		if (e[i].offset >= SMEM_F2_SIZE - sizeof(*p)) {
			continue;
		}
		p = (struct smem_part_hdr *)(base + e[i].offset);
		wlan_magic4((const uint8_t *)p->magic, pm);
		printf("SWEET WLAN:   part %s host %04x/%04x size %x free %x/%x\n",
		    pm, p->host0, p->host1, p->size, p->free_uncached,
		    p->free_cached);
	}
}

static void *
wlan_f2_item(struct smem_part_hdr *p, uint16_t item, uint32_t *size)
{
	uint8_t *e, *end;

	if (p == NULL || memcmp(p->magic, "$PRT", 4) != 0 ||
	    p->free_uncached > p->size) {
		return NULL;
	}
	e = (uint8_t *)p + sizeof(*p);
	end = (uint8_t *)p + p->free_uncached;
	while (e + sizeof(struct smem_priv_entry) <= end) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)e;

		if (h->canary != 0xa5a5) {
			return NULL;
		}
		if (h->item == item) {
			if (size != NULL) {
				*size = h->size - h->pad_data;
			}
			return e + sizeof(*h) + h->pad_hdr;
		}
		e += sizeof(*h) + h->pad_hdr + h->size;
	}
	return NULL;
}

static void
wlan_f2_items(struct smem_part_hdr *p)
{
	uint8_t *e, *end;
	unsigned n = 0;

	if (memcmp(p->magic, "$PRT", 4) != 0 || p->free_uncached > p->size) {
		char pm[5];

		wlan_magic4((const uint8_t *)p->magic, pm);
		printf("SWEET WLAN: partition is not $PRT (%s, free %x/%x)\n",
		    pm, p->free_uncached, p->size);
		return;
	}
	e = (uint8_t *)p + sizeof(*p);
	end = (uint8_t *)p + p->free_uncached;
	while (e + sizeof(struct smem_priv_entry) <= end && n < 96) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)e;

		if (h->canary != 0xa5a5) {
			printf("SWEET WLAN: item walk stops at %u (canary %04x)\n",
			    n, h->canary);
			return;
		}
		printf("SWEET WLAN: item %u size %u pad %u/%u\n", h->item,
		    h->size, h->pad_data, h->pad_hdr);
		if (h->item == ITEM_GLINK_DESC && h->size >= 32) {
			uint32_t *d = (uint32_t *)(void *)(e + sizeof(*h) +
			    h->pad_hdr);

			printf("SWEET WLAN: glink desc %08x %08x %08x %08x "
			    "%08x %08x %08x %08x\n", d[0], d[1], d[2], d[3],
			    d[4], d[5], d[6], d[7]);
		}
		e += sizeof(*h) + h->pad_hdr + h->size;
		n++;
	}
	printf("SWEET WLAN: %u items, free_uncached %x free_cached %x\n", n,
	    p->free_uncached, p->free_cached);
}

/*
 * 0716: cached SMEM entries grow DOWN from the end and the header comes
 * AFTER the data: downstream qcom_smem has cached_entry_to_item(e) = e - size
 * and cached_entry_next(e) = e - size - ALIGN(sizeof(e), cacheline). The
 * modem's GLINK TX FIFO (item 480, 0x4000 bytes) is one of these, so the
 * uncached walk never saw it. With size 0x94000 and free_cached 0x8ffe0 the
 * header sits at 0x93fe0 and the data at 0x8ffe0.
 */
static uint32_t
wlan_cached_hdr(void)
{
	uint32_t cl = g_cacheline != 0 ? g_cacheline : 0x20;

	return (sizeof(struct smem_priv_entry) + cl - 1) & ~(cl - 1);
}

static void *
wlan_f2_item_cached(struct smem_part_hdr *p, uint16_t item, uint32_t *size)
{
	uint8_t *base = (uint8_t *)p;
	uint32_t hdr = wlan_cached_hdr();
	uint32_t pos, last;

	if (p == NULL || memcmp(p->magic, "$PRT", 4) != 0 ||
	    p->free_cached > p->size || p->size < hdr) {
		return NULL;
	}
	last = p->free_cached;
	pos = p->size - hdr;
	while (pos >= last && pos + sizeof(struct smem_priv_entry) <= p->size) {
		struct smem_priv_entry *e =
		    (struct smem_priv_entry *)(base + pos);

		if (e->canary != 0xa5a5) {
			return NULL;
		}
		if (e->item == item) {
			if (e->size > pos) {
				return NULL;
			}
			if (size != NULL) {
				*size = e->size - e->pad_data;
			}
			return base + pos - e->size;
		}
		if (e->size + hdr > pos - last) {
			return NULL;
		}
		pos -= e->size + hdr;
	}
	return NULL;
}

static void
wlan_f2_items_cached(struct smem_part_hdr *p)
{
	uint8_t *base = (uint8_t *)p;
	uint32_t hdr = wlan_cached_hdr();
	uint32_t pos, last;
	unsigned n = 0;

	if (p->free_cached > p->size || p->size < hdr) {
		printf("SWEET WLAN: cached area %x/%x (not walked)\n",
		    p->free_cached, p->size);
		return;
	}
	last = p->free_cached;
	pos = p->size - hdr;
	while (pos >= last && pos + sizeof(struct smem_priv_entry) <= p->size &&
	    n < 64) {
		struct smem_priv_entry *h =
		    (struct smem_priv_entry *)(base + pos);

		if (h->canary != 0xa5a5) {
			printf("SWEET WLAN: cached walk stops at %u (canary "
			    "%04x)\n", n, h->canary);
			break;
		}
		printf("SWEET WLAN: cached item %u size %u pad %u/%u at %x\n",
		    h->item, h->size, h->pad_data, h->pad_hdr, pos);
		if (h->size + hdr > pos - last) {
			break;
		}
		pos -= h->size + hdr;
		n++;
	}
	printf("SWEET WLAN: %u cached items, free_cached %x size %x cl %x\n",
	    n, p->free_cached, p->size, g_cacheline);
}

/* --- F2 step 2: SMP2P out (writes SMEM under the APPS hwlock) ------------ */

static boolean_t
wlan_smem_lock(void)
{
	volatile uint32_t *reg = (volatile uint32_t *)tcsr;

	if (tcsr == 0) {
		return FALSE;
	}
	for (int i = 0; i < 1000; i++) {
		*reg = SMEM_HWLOCK_OWNER;
		__asm__ volatile("dsb sy" ::: "memory");
		if (*reg == SMEM_HWLOCK_OWNER) {
			return TRUE;
		}
		wlan_wait(1);
	}
	printf("SWEET WLAN: SMEM hwlock busy (owner %u)\n", *reg);
	return FALSE;
}

static void
wlan_smem_unlock(void)
{
	if (tcsr != 0) {
		__asm__ volatile("dsb sy" ::: "memory");
		*(volatile uint32_t *)tcsr = 0;
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

static void *
wlan_smem_alloc(struct smem_part_hdr *p, uint16_t item, uint32_t size)
{
	uint32_t aligned = (size + 7) & ~7u;
	uint32_t need = sizeof(struct smem_priv_entry) + aligned;
	void *v;

	v = wlan_f2_item(p, item, NULL);
	if (v != NULL) {
		return v;
	}
	if (tcsr == 0 || !wlan_smem_lock()) {
		return NULL;
	}
	v = wlan_f2_item(p, item, NULL);
	if (v == NULL && p->free_uncached + need <= p->free_cached &&
	    p->free_uncached + need <= p->size) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)
		    ((uint8_t *)p + p->free_uncached);
		uint8_t *d = (uint8_t *)(h + 1);

		h->canary = 0xa5a5;
		h->item = item;
		h->size = aligned;
		h->pad_data = (uint16_t)(aligned - size);
		h->pad_hdr = 0;
		h->rsvd = 0;
		for (uint32_t i = 0; i < aligned; i++) {
			d[i] = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		p->free_uncached += need;
		__asm__ volatile("dsb sy" ::: "memory");
		v = d;
	} else if (v == NULL) {
		printf("SWEET WLAN: SMEM partition full (%x..%x need %x)\n",
		    p->free_uncached, p->free_cached, need);
	}
	wlan_smem_unlock();
	return v;
}

static void
wlan_smp2p_kick(void)
{
	if (apcs != 0) {
		__asm__ volatile("dsb sy" ::: "memory");
		*(volatile uint32_t *)(apcs + APCS_IPC_OFF) =
		    1u << SMP2P_IRQ_BIT;
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

static boolean_t
wlan_name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++) {
		if (a[i] != b[i]) {
			return FALSE;
		}
		if (a[i] == 0) {
			return TRUE;
		}
	}
	return TRUE;
}

static volatile uint32_t *
wlan_smp2p_find(struct smp2p_hdr *h, const char *name)
{
	struct smp2p_entry *e = (struct smp2p_entry *)(h + 1);
	uint32_t i;

	if (h->magic != SMP2P_MAGIC) {
		return NULL;
	}
	for (i = 0; i < h->valid && i < SMP2P_MAX_ENTRY; i++) {
		if (wlan_name_eq(e[i].name, name)) {
			return &e[i].value;
		}
	}
	return NULL;
}

static volatile uint32_t *
wlan_smp2p_add(struct smp2p_hdr *h, const char *name, uint32_t value)
{
	struct smp2p_entry *e = (struct smp2p_entry *)(h + 1);
	volatile uint32_t *v = wlan_smp2p_find(h, name);
	uint32_t i;

	if (v != NULL) {
		return v;
	}
	if (h->valid >= h->total || h->valid >= SMP2P_MAX_ENTRY) {
		return NULL;
	}
	for (i = 0; i < 16; i++) {
		e[h->valid].name[i] = 0;
	}
	for (i = 0; i < 15 && name[i] != 0; i++) {
		e[h->valid].name[i] = name[i];
	}
	e[h->valid].value = value;
	__asm__ volatile("dsb sy" ::: "memory");
	h->valid++;
	wlan_smp2p_kick();
	return &e[h->valid - 1].value;
}

static void
wlan_f2_smp2p(struct smem_part_hdr *p, uint16_t item, const char *label)
{
	uint32_t size = 0, total, avail, i;
	struct smp2p_hdr *h;
	struct smp2p_entry *ent;

	h = (struct smp2p_hdr *)wlan_f2_item(p, item, &size);
	if (h == NULL) {
		printf("SWEET WLAN: smp2p %s item %u absent\n", label, item);
		return;
	}
	if (size < sizeof(*h)) {
		printf("SWEET WLAN: smp2p %s item %u too small (%u)\n", label,
		    item, size);
		return;
	}
	printf("SWEET WLAN: smp2p %s item %u size %u magic %08x ver %u "
	    "pid %u/%u total %u valid %u flags %x\n", label, item, size,
	    h->magic, h->version, h->local_pid, h->remote_pid, h->total,
	    h->valid, h->flags);
	if (h->magic != SMP2P_MAGIC) {
		return;
	}
	total = h->total;
	avail = (size - sizeof(*h)) / sizeof(*ent);
	if (total > avail) {
		total = avail;
	}
	if (total > 16) {
		total = 16;
	}
	ent = (struct smp2p_entry *)(h + 1);
	for (i = 0; i < total; i++) {
		char name[17];

		memcpy(name, ent[i].name, 16);
		name[16] = 0;
		printf("SWEET WLAN: smp2p %s[%u] %s = %08x\n", label, i, name,
		    ent[i].value);
	}
}

/* --- F2 step 3: GLINK over SMEM + QRTR ----------------------------------- */

#define GLINK_TX_SIZE     0x4000u
#define GLINK_IRQ_BIT     15
#define GLINK_FEATURES    1             /* INTENT_REUSE */
#define GLINK_CHAN_NAME   "IPCRTR"
#define GLINK_LCID        1
#define GLINK_OUR_INTENTS 8
#define GLINK_INTENT_SIZE 0x4400u
#define GLINK_MAX_INTENTS 128

#define CMD_VERSION           0
#define CMD_VERSION_ACK       1
#define CMD_OPEN              2
#define CMD_CLOSE             3
#define CMD_OPEN_ACK          4
#define CMD_INTENT            5
#define CMD_RX_DONE           6
#define CMD_RX_INTENT_REQ     7
#define CMD_RX_INTENT_REQ_ACK 8
#define CMD_TX_DATA           9
#define CMD_CLOSE_ACK         11
#define CMD_TX_DATA_CONT      12
#define CMD_READ_NOTIF        13
#define CMD_RX_DONE_W_REUSE   14
#define CMD_SIGNALS           15

#define QRTR_NODE_APPS    1
#define QRTR_PORT_CTRL    0xfffffffeu
#define QRTR_NODE_BCAST   0xffffffffu
#define QRTR_TYPE_DATA    1
#define QRTR_TYPE_HELLO   2
#define QRTR_TYPE_NEW_LOOKUP 3
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_RESUME_TX 7
#define QMI_SVC_WLFW      0x45
#define QMI_REQ           0
#define QMI_RESP          2

/*
 * Host services (topaz layout): the modem's elf loader fetches
 * /readonly/firmware/image/<file> through the AP tftp server announced on
 * QRTR service 4096. Sessions live on 0x4100 + slot.
 */
#define TFTP_SVC          4096
#define PORT_TFTP         0x4002
#define PORT_SESS_BASE    0x4100
#define TFTP_MAX_SESS     16
#define TFTP_BLK          16384

#define TFTP_RRQ          1
#define TFTP_WRQ          2
#define TFTP_DATA         3
#define TFTP_ACK          4
#define TFTP_ERROR        5
#define TFTP_OACK         6

#define RMTFS_SVC         14
#define PORT_RMTFS        0x4000
#define RMTFS_SECTOR      512

struct glink_msg {
	uint16_t cmd, p1;
	uint32_t p2;
} __attribute__((packed));
struct glink_data_hdr {
	struct glink_msg m;
	uint32_t chunk, left;
} __attribute__((packed));
struct qrtr_hdr_v1 {
	uint32_t version, type, src_node, src_port, confirm_rx, size;
	uint32_t dst_node, dst_port;
} __attribute__((packed));
struct qrtr_hdr_v2 {
	uint8_t version, type, flags, optlen;
	uint32_t size;
	uint16_t src_node, src_port, dst_node, dst_port;
} __attribute__((packed));
struct qrtr_ctrl {
	uint32_t cmd, a, b, c, d;
} __attribute__((packed));

static struct smem_part_hdr *g_part;
static volatile uint32_t *g_desc;
static uint8_t *g_tx, *g_rx;
static uint32_t g_rx_len, g_features = GLINK_FEATURES, g_rcid;
static uint32_t g_next_intent = 1;
static boolean_t g_remote_open, g_open_sent, g_open_acked, g_intents_sent;
static boolean_t g_intent_req_sent;
static struct {
	uint32_t id, size;
	boolean_t used;
} g_ri[GLINK_MAX_INTENTS];
static unsigned g_ri_count;
static uint8_t g_asm[GLINK_INTENT_SIZE];
static uint32_t g_asm_len;
static uint32_t q_ver = 1, q_modem_node = QRTR_NODE_BCAST;
static boolean_t q_hello_sent, q_hello_rx, q_wlfw_seen;
static uint8_t q_tx[4 + TFTP_BLK + 80];
static uint8_t q_pending[4 + TFTP_BLK + 80];
static uint32_t q_pending_len;
static boolean_t g_qrtr_ctl;
static uint32_t q_pending_dport, q_last_dport;
#define Q_CTRL_SLOTS 4
#define Q_CTRL_SIZE  80
static uint8_t q_ctrl[Q_CTRL_SLOTS][Q_CTRL_SIZE];
static uint32_t q_ctrl_len[Q_CTRL_SLOTS];
static unsigned q_ctrl_head, q_ctrl_tail;
static unsigned g_ms, g_tx_msgs, g_rx_msgs;
static uint32_t g_last_desc[4];
static uint32_t g_last_intent_size, g_last_miss_len, g_max_intent_size;

static void
wlan_mb(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t
wlan_rx_avail(void)
{
	uint32_t h, t;

	if (g_desc == NULL || g_rx == NULL) {
		return 0;
	}
	h = g_desc[3];
	t = g_desc[2];
	return h >= t ? h - t : g_rx_len - t + h;
}

static void
wlan_rx_peek(uint32_t off, void *dst, uint32_t n)
{
	uint32_t t = g_desc[2] + off;
	uint32_t n1;

	if (t >= g_rx_len) {
		t -= g_rx_len;
	}
	n1 = MIN(n, g_rx_len - t);
	memcpy(dst, g_rx + t, n1);
	if (n > n1) {
		memcpy((uint8_t *)dst + n1, g_rx, n - n1);
	}
}

static void
wlan_rx_advance(uint32_t n)
{
	uint32_t t = g_desc[2] + n;

	if (t >= g_rx_len) {
		t -= g_rx_len;
	}
	wlan_mb();
	g_desc[2] = t;
}

static uint32_t
wlan_tx_put(uint32_t head, const void *src, uint32_t n)
{
	uint32_t n1 = MIN(n, GLINK_TX_SIZE - head);

	memcpy(g_tx + head, src, n1);
	if (n > n1) {
		memcpy(g_tx, (const uint8_t *)src + n1, n - n1);
	}
	head += n;
	return head >= GLINK_TX_SIZE ? head - GLINK_TX_SIZE : head;
}

static boolean_t
wlan_tx_write(const void *hdr, uint32_t hlen, const void *data, uint32_t dlen)
{
	uint32_t head = 0, tail = 0, avail = 0, need;
	static uint32_t last_full;

	if (g_desc == NULL || g_tx == NULL) {
		return FALSE;
	}
	need = (hlen + dlen + 7) & ~7u;
	/*
	 * Never wait for the modem here: this runs inside the RX handler,
	 * so blocking stops the very processing that frees the FIFO and
	 * deadlocks the transfer (0747 stalled at block 800). The caller
	 * queues the packet and the poll loop retries.
	 */
	head = g_desc[1];
	tail = g_desc[0];
	avail = tail <= head ? GLINK_TX_SIZE - head + tail : tail - head;
	avail = avail < 8 ? 0 : avail - 8;
	if (avail < need) {
		if (last_full != need) {
			last_full = need;
			printf("SWEET WLAN: glink TX busy (%u need, %u free)\n",
			    need, avail);
		}
		return FALSE;
	}
	last_full = 0;
	head = wlan_tx_put(head, hdr, hlen);
	if (dlen != 0) {
		head = wlan_tx_put(head, data, dlen);
	}
	head = (head + 7) & ~7u;
	if (head >= GLINK_TX_SIZE) {
		head -= GLINK_TX_SIZE;
	}
	wlan_mb();
	g_desc[1] = head;
	wlan_mb();
	if (apcs != 0) {
		*(volatile uint32_t *)(apcs + APCS_IPC_OFF) =
		    1u << GLINK_IRQ_BIT;
	}
	wlan_mb();
	g_tx_msgs++;
	return TRUE;
}

static void
wlan_glink_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
	struct glink_msg m = { cmd, p1, p2 };

	(void)wlan_tx_write(&m, sizeof(m), NULL, 0);
}

static void
wlan_glink_open(void)
{
	struct {
		struct glink_msg m;
		char name[8];
	} __attribute__((packed)) req = { { CMD_OPEN, GLINK_LCID,
		sizeof(GLINK_CHAN_NAME) }, GLINK_CHAN_NAME };

	(void)wlan_tx_write(&req, sizeof(req), NULL, 0);
	g_open_sent = TRUE;
}

static void
wlan_glink_intents(uint32_t count, uint32_t size)
{
	struct {
		struct glink_msg m;
		uint32_t pair[2 * GLINK_OUR_INTENTS];
	} __attribute__((packed)) m = { { CMD_INTENT, GLINK_LCID, count } };

	for (uint32_t i = 0; i < count && i < GLINK_OUR_INTENTS; i++) {
		m.pair[2 * i] = size;
		m.pair[2 * i + 1] = g_next_intent++;
	}
	(void)wlan_tx_write(&m, sizeof(struct glink_msg) + 8 * count, NULL, 0);
}

static boolean_t
wlan_chan_up(void)
{
	return g_remote_open && g_open_acked;
}

static const char *
wlan_svc_name(uint32_t svc)
{
	switch (svc) {
	case 0x45: return "WLFW";
	case 0x0e: return "rmtfs";
	case 0x40: return "servreg-loc";
	case 0x42: return "servreg-notif";
	case 0x2b: return "ssctl";
	case 0x31: return "ipa";
	case 0x01: return "wds";
	case 0x02: return "dms";
	case 0x03: return "nas";
	case 0x0b: return "uim";
	case 0x1a: return "wda";
	default:   return "";
	}
}

/* Queue one QRTR packet into a modem RX intent; one pending packet at a time. */
static boolean_t
wlan_glink_send_now(const void *data, uint32_t len)
{
	unsigned i;

	for (i = 0; i < g_ri_count; i++) {
		if (!g_ri[i].used && g_ri[i].size >= len) {
			break;
		}
	}
	if (i == g_ri_count) {
		return FALSE;
	}
	{
		struct glink_data_hdr h = { { CMD_TX_DATA, GLINK_LCID,
		    g_ri[i].id }, len, 0 };

		g_ri[i].used = TRUE;
		if (!wlan_tx_write(&h, sizeof(h), data, len)) {
			g_ri[i].used = FALSE;
			return FALSE;
		}
	}
	return TRUE;
}

static boolean_t
wlan_glink_send(const void *data, uint32_t len)
{
	unsigned i;
	uint32_t maxs = 0, all = 0;

	if (q_pending_len != 0) {
		return FALSE;
	}
	if (wlan_glink_send_now(data, len)) {
		return TRUE;
	}
	for (i = 0; i < g_ri_count; i++) {
		if (g_ri[i].size > all) {
			all = g_ri[i].size;
		}
		if (g_ri[i].size > maxs && !g_ri[i].used) {
			maxs = g_ri[i].size;
		}
	}
	if (len != g_last_miss_len) {
		g_last_miss_len = len;
		printf("SWEET WLAN: glink no intent for %u (free max "
		    "%x, all max %x, n %u, q %u)\n", len, maxs, all,
		    g_ri_count, q_pending_len);
	}
	if (len <= sizeof(q_pending)) {
		memcpy(q_pending, data, len);
		q_pending_len = len;
		q_pending_dport = q_last_dport;
	}
	if (!g_intent_req_sent) {
		wlan_glink_cmd(CMD_RX_INTENT_REQ, GLINK_LCID, len);
		g_intent_req_sent = TRUE;
	}
	return FALSE;
}

/*
 * Control traffic (QRTR RESUME_TX credits) must never be dropped: if it
 * is, the peer's flow-control window fills, its TFTP thread blocks in
 * qrtr_sendmsg and the transfer stalls silently (0750 died at ~block
 * 780). Small packets, separate ring, flushed every poll.
 */
static boolean_t
wlan_glink_send_ctl(const void *data, uint32_t len)
{
	if (wlan_glink_send_now(data, len)) {
		return TRUE;
	}
	if (len <= Q_CTRL_SIZE) {
		memcpy(q_ctrl[q_ctrl_head], data, len);
		q_ctrl_len[q_ctrl_head] = len;
		q_ctrl_head = (q_ctrl_head + 1) % Q_CTRL_SLOTS;
		if (q_ctrl_head == q_ctrl_tail) {
			q_ctrl_tail = (q_ctrl_tail + 1) % Q_CTRL_SLOTS;
		}
	}
	if (!g_intent_req_sent) {
		wlan_glink_cmd(CMD_RX_INTENT_REQ, GLINK_LCID, len);
		g_intent_req_sent = TRUE;
	}
	return FALSE;
}

static void
wlan_glink_ctl_flush(void)
{
	while (q_ctrl_tail != q_ctrl_head) {
		if (!wlan_glink_send_now(q_ctrl[q_ctrl_tail],
		    q_ctrl_len[q_ctrl_tail])) {
			break;
		}
		q_ctrl_tail = (q_ctrl_tail + 1) % Q_CTRL_SLOTS;
	}
}

static boolean_t
wlan_qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node,
    uint32_t dst_port, const void *payload, uint32_t len)
{
	uint32_t hlen, total;

	if (len + 40 > sizeof(q_tx)) {
		return FALSE;
	}
	memset(q_tx, 0, sizeof(q_tx));
	if (q_ver == 2) {
		struct qrtr_hdr_v2 *h = (struct qrtr_hdr_v2 *)(void *)q_tx;

		h->version = 2;
		h->type = (uint8_t)type;
		h->flags = type == QRTR_TYPE_DATA ? 1 : 0;
		h->size = len;
		h->src_node = QRTR_NODE_APPS;
		h->src_port = (uint16_t)src_port;
		h->dst_node = (uint16_t)dst_node;
		h->dst_port = (uint16_t)dst_port;
		hlen = sizeof(*h);
	} else {
		struct qrtr_hdr_v1 *h = (struct qrtr_hdr_v1 *)(void *)q_tx;

		h->version = 1;
		h->type = type;
		h->src_node = QRTR_NODE_APPS;
		h->src_port = src_port;
		h->confirm_rx = type == QRTR_TYPE_DATA ? 1 : 0;
		h->size = len;
		h->dst_node = dst_node;
		h->dst_port = dst_port;
		hlen = sizeof(*h);
	}
	memcpy(q_tx + hlen, payload, len);
	total = (hlen + len + 3) & ~3u;
	q_last_dport = dst_port;
	if (g_qrtr_ctl) {
		g_qrtr_ctl = FALSE;
		return wlan_glink_send_ctl(q_tx, total);
	}
	return wlan_glink_send(q_tx, total);
}

/* Same packet, but the control ring keeps it when no intent is free. */
static boolean_t
wlan_qrtr_send_ctl(uint32_t type, uint32_t src_port, uint32_t dst_node,
    uint32_t dst_port, const void *payload, uint32_t len)
{
	boolean_t ok;

	g_qrtr_ctl = TRUE;
	ok = wlan_qrtr_send(type, src_port, dst_node, dst_port, payload, len);
	g_qrtr_ctl = FALSE;
	return ok;
}

/* --- F2 step 5: tftp host service (firmware files) ----------------------- */

struct tftp_sess {
	boolean_t used, last, write, wr_src;
	boolean_t has_wsize, has_timeout, has_tsize, has_rsize, has_seek;
	uint32_t node, port, rport, size, blk, sent, wlen;
	uint32_t wsize, timeout, rsize, seek;
	uint32_t seq, last_ms, last_ack;
	char name[13];
	char path[64];
};

static struct tftp_sess t_sess[TFTP_MAX_SESS];
static uint32_t t_seq;
static uint8_t t_pkt[4 + TFTP_BLK];
static uint8_t t_wr[64 * 1024];
static uint32_t t_wr_len;
static char t_wr_name[64];

static void wlan_tftp_sess_rx(uint32_t port, uint32_t src_port,
    const uint8_t *d, uint32_t len);

static void
wlan_tftp_reply(struct tftp_sess *s, uint32_t n)
{
	wlan_qrtr_send(QRTR_TYPE_DATA, s->port, s->node, s->rport, t_pkt, n);
}

static void
wlan_tftp_error(struct tftp_sess *s, uint16_t code, const char *msg)
{
	uint32_t n = 4;

	t_pkt[0] = 0;
	t_pkt[1] = TFTP_ERROR;
	t_pkt[2] = (uint8_t)(code >> 8);
	t_pkt[3] = (uint8_t)code;
	for (uint32_t i = 0; msg[i] != 0 && n + 1 < sizeof(t_pkt); i++) {
		t_pkt[n++] = (uint8_t)msg[i];
	}
	t_pkt[n++] = 0;
	wlan_tftp_reply(s, n);
}

static void
wlan_tftp_data(struct tftp_sess *s, uint32_t block)
{
	uint32_t off = s->seek + (block - 1) * s->blk;
	uint32_t n = 0;
	int r;

	if (s->wr_src && off < s->size) {
		n = MIN(s->blk, s->size - off);
		memcpy(t_pkt + 4, t_wr + off, n);
	} else if (!s->wr_src && off < s->size) {
		r = lavender_modem_fw_read(s->name, off, t_pkt + 4, s->blk);
		if (r < 0) {
			printf("SWEET WLAN: tftp %s read %u failed %d\n", s->name,
			    off, r);
			wlan_tftp_error(s, 2, "read error");
			s->used = FALSE;
			return;
		}
		n = (uint32_t)r;
	}
	t_pkt[0] = 0;
	t_pkt[1] = TFTP_DATA;
	t_pkt[2] = (uint8_t)(block >> 8);
	t_pkt[3] = (uint8_t)block;
	s->sent = block;
	s->last = n < s->blk;
	if (block <= 3 || (block % 100) == 0) {
		printf("SWEET WLAN: tftp %s block %u/%u\n", s->name, block,
		    (s->size + s->blk - 1) / s->blk);
	}
	wlan_tftp_reply(s, 4 + n);
	s->last_ms = g_ms;
	if (block <= 3) {
		printf("SWEET WLAN: tftp %s block %u -> q %u tx %u\n",
		    s->name, block, q_pending_len, g_tx_msgs);
	}
}

/* Send up to wsize DATA blocks, stopping at the real end of the file. */
static void
wlan_tftp_window(struct tftp_sess *s, uint32_t from)
{
	uint32_t n = s->has_wsize ? s->wsize : 1;

	if (n < 1) {
		n = 1;
	}
	if (n > 16) {
		n = 16;
	}
	for (uint32_t k = 0; k < n; k++) {
		uint32_t block = from + k;
		uint32_t off = s->seek + (block - 1) * s->blk;

		if (off >= s->size) {
			break;          /* past the last real block */
		}
		/*
		 * There is only one pending slot: a second block sent while
		 * it is busy is dropped silently (the modem grants RX
		 * intents at its own pace). Stop and let the ACKs drive.
		 */
		if (q_pending_len != 0) {
			break;
		}
		wlan_tftp_data(s, block);
		if (s->last) {
			break;
		}
	}
}

/* /readonly/firmware/image/wlanmdsp.mbn -> WLANMDSP.MBN (8.3, uppercase). */
static boolean_t
wlan_tftp_fname(const char *path, char *out, size_t max)
{
	const char *base = path;
	size_t k = 0;

	for (const char *p = path; *p != 0; p++) {
		if (*p == '/') {
			base = p + 1;
		}
	}
	if (*base == 0) {
		return FALSE;
	}
	for (; *base != 0 && k + 1 < max; base++) {
		char c = *base;

		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		out[k++] = c;
	}
	out[k] = 0;
	return TRUE;
}

static void
wlan_tftp_u32(uint8_t *p, uint32_t *n, uint32_t v)
{
	char tmp[12];
	int l = 0;

	do {
		tmp[l++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	while (l > 0) {
		p[(*n)++] = (uint8_t)tmp[--l];
	}
}

/* Echo one option name (NUL-terminated) into the OACK packet. */
static void
wlan_tftp_key(uint8_t *p, uint32_t *n, const char *k)
{
	while (*k != 0) {
		p[(*n)++] = (uint8_t)*k++;
	}
	p[(*n)++] = 0;
}

/*
 * The modem's TFTP client demands the tqftpserv layout, with timeoutms and
 * wsize always present: blksize, timeoutms, tsize, wsize, rsize, seek.
 */
static void
wlan_tftp_oack(struct tftp_sess *s)
{
	uint32_t n = 2;

	t_pkt[0] = 0;
	t_pkt[1] = TFTP_OACK;
	wlan_tftp_key(t_pkt, &n, "blksize");
	wlan_tftp_u32(t_pkt, &n, s->blk);
	t_pkt[n++] = 0;
	wlan_tftp_key(t_pkt, &n, "timeoutms");
	wlan_tftp_u32(t_pkt, &n, s->has_timeout ? s->timeout : 1000);
	t_pkt[n++] = 0;
	if (s->has_tsize) {
		wlan_tftp_key(t_pkt, &n, "tsize");
		wlan_tftp_u32(t_pkt, &n, s->write ? 0 : s->size);
		t_pkt[n++] = 0;
	}
	wlan_tftp_key(t_pkt, &n, "wsize");
	wlan_tftp_u32(t_pkt, &n, s->has_wsize ? s->wsize : 1);
	t_pkt[n++] = 0;
	if (s->has_rsize && s->rsize != 0) {
		wlan_tftp_key(t_pkt, &n, "rsize");
		wlan_tftp_u32(t_pkt, &n, s->rsize);
		t_pkt[n++] = 0;
	}
	if (s->has_seek) {
		wlan_tftp_key(t_pkt, &n, "seek");
		wlan_tftp_u32(t_pkt, &n, s->seek);
		t_pkt[n++] = 0;
	}
	if (t_pkt[2] != 'b') {
		/* An offset-2 garbage byte broke every OACK before 0742. */
		printf("SWEET WLAN: tftp oack bad first byte %x\n", t_pkt[2]);
	}
	wlan_tftp_reply(s, n);
}

static void
wlan_tftp_req(uint32_t node, uint32_t port, const uint8_t *d, uint32_t len)
{
	struct tftp_sess *s = NULL;
	char path[96], name[13];
	uint32_t o = 2;
	boolean_t opts = FALSE;
	uint16_t op;

	if (len < 4) {
		return;
	}
	op = (uint16_t)((d[0] << 8) | d[1]);
	if (op != TFTP_RRQ && op != TFTP_WRQ) {
		/*
		 * Some clients ACK the OACK to the well-known port instead
		 * of the transfer port; route it to the matching session.
		 */
		for (unsigned i = 0; i < TFTP_MAX_SESS; i++) {
			if (t_sess[i].used && t_sess[i].node == node &&
			    t_sess[i].rport == port) {
				printf("SWEET WLAN: tftp op %u block %u at "
				    "server port, routed to %x\n", op,
				    len >= 4 ? (d[2] << 8) | d[3] : 0,
				    t_sess[i].port);
				wlan_tftp_sess_rx(t_sess[i].port, port, d, len);
				return;
			}
		}
		if (len >= 4) {
			printf("SWEET WLAN: tftp op %u block %u at server port "
			    "from %x\n", op, (d[2] << 8) | d[3], port);
		}
		return;
	}
	{
		uint32_t i = 0;

		while (o < len && d[o] != 0 && i + 1 < sizeof(path)) {
			path[i++] = (char)d[o++];
		}
		path[i] = 0;
		o++;                            /* NUL */
		while (o < len && d[o] != 0) {
			o++;                    /* transfer mode */
		}
		o++;
	}
	if (!wlan_tftp_fname(path, name, sizeof(name))) {
		return;
	}
	{
		struct tftp_sess *oldest = NULL;

		for (unsigned i = 0; i < TFTP_MAX_SESS; i++) {
			/*
			 * A retrying client reuses its own slot, then the
			 * first free one; when every slot is busy the oldest
			 * transfer is evicted (as tqftpserv does).
			 */
			if (t_sess[i].used && t_sess[i].node == node &&
			    t_sess[i].rport == port) {
				s = &t_sess[i];
				break;
			}
			if (!t_sess[i].used && s == NULL) {
				s = &t_sess[i];
			}
			if (t_sess[i].used && (oldest == NULL ||
			    t_sess[i].seq < oldest->seq)) {
				oldest = &t_sess[i];
			}
		}
		if (s == NULL) {
			s = oldest;
		}
	}
	if (s == NULL) {
		printf("SWEET WLAN: tftp no free session for %s\n", name);
		return;
	}
	if (s->used) {
		printf("SWEET WLAN: tftp evict %s port %x\n", s->name, s->port);
	}
	memset(s, 0, sizeof(*s));
	s->seq = ++t_seq;
	s->last_ms = g_ms;
	s->node = node;
	s->rport = port;
	s->port = PORT_SESS_BASE + (uint32_t)(s - t_sess);
	memcpy(s->name, name, sizeof(s->name));
	{
		unsigned k = 0;

		while (path[k] != 0 && k + 1 < sizeof(s->path)) {
			s->path[k] = path[k];
			k++;
		}
		s->path[k] = 0;
	}
	s->blk = 512;
	while (o + 1 < len) {
		char key[24];
		uint32_t i = 0, v = 0;

		while (o < len && d[o] != 0 && i + 1 < sizeof(key)) {
			key[i++] = (char)d[o++];
		}
		key[i] = 0;
		o++;
		while (o < len && d[o] != 0) {
			if (d[o] >= '0' && d[o] <= '9') {
				v = v * 10 + (uint32_t)(d[o] - '0');
			}
			o++;
		}
		o++;
		if (key[0] == 0) {
			break;
		}
		opts = TRUE;
		if (strcasecmp(key, "blksize") == 0 && v >= 8 && v <= TFTP_BLK) {
			s->blk = v;		} else if (strcasecmp(key, "wsize") == 0 && v >= 1 && v <= 64) {
			s->wsize = v;
			s->has_wsize = TRUE;
		} else if (strcasecmp(key, "timeoutms") == 0 && v >= 1) {
			s->timeout = v;
			s->has_timeout = TRUE;
		} else if (strcasecmp(key, "tsize") == 0) {
			s->has_tsize = TRUE;
		} else if (strcasecmp(key, "rsize") == 0 && v >= 1) {
			s->rsize = v;
			s->has_rsize = TRUE;
		} else if (strcasecmp(key, "seek") == 0) {
			s->seek = v;
			s->has_seek = TRUE;
		}
	}
	printf("SWEET WLAN: tftp asked blk %u wsize %u rsize %u\n", s->blk,
	    s->wsize, s->rsize);
	/*
	 * 0750: back to 448 (packet ~484, known to reach the client) while
	 * testing the one-window-per-closing-ACK flow; 7680 DATA never
	 * reached the client and 4096/1024 were only tried with the old
	 * ACK-multiplying window.
	 */
	if (s->blk > 448) {
		s->blk = 448;
	}
	/*
	 * Each DATA also carries a GLINK data header (8 B), a QRTR header
	 * (~24 B) and the 4-byte TFTP header, and the modem only accepts a
	 * packet that fits an RX intent it granted. If those intents are
	 * smaller than the requested block, negotiate a block that fits
	 * (RFC 2347 allows lowering it) instead of dropping every DATA.
	 */
	if (g_max_intent_size > 512) {
		uint32_t fit = (g_max_intent_size - 128) & ~7u;

		if (fit < 512) {
			fit = 512;
		}
		if (s->blk > fit) {
			s->blk = fit;
		}
	}
	if (op == TFTP_WRQ) {
		unsigned k = 0;

		/*
		 * The modem's server check writes /readwrite/server_check.txt
		 * and expects it to stick; keep those writes in RAM and never
		 * touch the phone's storage.
		 */
		if (strncmp(path, "/readwrite/", 11) != 0) {
			printf("SWEET WLAN: tftp WRQ %s refused\n", name);
			s->used = TRUE;
			wlan_tftp_error(s, 2, "read only");
			s->used = FALSE;
			return;
		}
		t_wr_len = 0;
		memset(t_wr, 0, sizeof(t_wr));
		while (path[k] != 0 && k + 1 < sizeof(t_wr_name)) {
			t_wr_name[k] = path[k];
			k++;
		}
		t_wr_name[k] = 0;
		s->write = TRUE;
		s->used = TRUE;
		printf("SWEET WLAN: tftp WRQ %s accepted\n", path);
		if (opts) {
			wlan_tftp_oack(s);
		} else {
			t_pkt[0] = 0;
			t_pkt[1] = TFTP_ACK;
			t_pkt[2] = 0;
			t_pkt[3] = 0;
			wlan_tftp_reply(s, 4);
		}
		return;
	}
	{
		if (strcmp(path, t_wr_name) == 0 && t_wr_len != 0) {
			s->wr_src = TRUE;
			s->size = t_wr_len;
		} else {
			int r = lavender_modem_fw_size(name, &s->size);

			if (r != 0) {
				printf("SWEET WLAN: tftp RRQ %s not found (%d)\n",
				    name, r);
				s->used = TRUE;
				wlan_tftp_error(s, 1, "not found");
				s->used = FALSE;
				return;
			}
		}
	}
	s->used = TRUE;
	printf("SWEET WLAN: tftp RRQ %s (%u bytes, blk %u%s) port %x\n",
	    s->name, s->size, s->blk, opts ? ", oack" : "", s->port);
	if (opts) {
		printf("SWEET WLAN: tftp opts blk %u rsize %u wsize %u tmo "
		    "%u tsize %u seek %u\n", s->blk, s->rsize, s->wsize,
		    s->timeout, s->has_tsize, s->seek);
		wlan_tftp_oack(s);
		/*
		 * Wait for ACK 0 before the first block (as tqftpserv does).
		 * A window sent before the ACK left a queued block that made
		 * the post-ACK retransmission miss the client's 1 s timeout
		 * by a few ms, so it never accepted block 1 (0746 boot).
		 */
	} else {
		wlan_tftp_data(s, 1);
	}
}

static void
wlan_tftp_sess_rx(uint32_t port, uint32_t src_port, const uint8_t *d,
    uint32_t len)
{
	struct tftp_sess *s = NULL;
	uint16_t op, block;

	for (unsigned i = 0; i < TFTP_MAX_SESS; i++) {
		if (t_sess[i].used && t_sess[i].port == port) {
			s = &t_sess[i];
			break;
		}
	}
	if (s == NULL || len < 4) {
		return;
	}
	/*
	 * TFTP ties the reply to the client's TID; if the eloader moved to
	 * a new socket after the OACK, follow it instead of answering the
	 * RRQ source port forever.
	 */
	if (src_port != s->rport) {
		printf("SWEET WLAN: tftp %s client port %x -> %x\n",
		    s->name, s->rport, src_port);
		s->rport = src_port;
	}
	s->last_ms = g_ms;
	op = (uint16_t)((d[0] << 8) | d[1]);
	block = (uint16_t)((d[2] << 8) | d[3]);
	switch (op) {
	case TFTP_ACK:
		if (s->write) {
			break;
		}
		if (block <= 20 || (block % 100) == 0) {
			printf("SWEET WLAN: tftp %s ack %u (sent %u)\n",
			    s->name, block, s->sent);
		}
		if (block >= s->last_ack) {
			s->last_ack = block;    /* never move backwards */
		}
		if (block == 0) {
			wlan_tftp_window(s, 1);
		} else if (block >= s->sent) {
			/*
			 * 0750: only the ACK that closes the window starts
			 * the next one. Sending a fresh 10-block window for
			 * every intermediate ACK multiplied the traffic by
			 * wsize, filled the intents and stalled the link
			 * (0748 died at ~block 800); the timer covers holes.
			 */
			if (q_pending_len != 0 && q_pending_dport == s->port) {
				q_pending_len = 0;
			}
			if (s->last) {
				printf("SWEET WLAN: tftp %s done (%u bytes)\n",
				    s->name, s->size);
				s->used = FALSE;
			} else {
				wlan_tftp_window(s, s->sent + 1);
			}
		}
		break;
	case TFTP_DATA:
		if (!s->write) {
			break;
		}
		{
			uint32_t n = len - 4;

			if (t_wr_len + n <= sizeof(t_wr)) {
				memcpy(t_wr + t_wr_len, d + 4, n);
				t_wr_len += n;
			}
			s->sent = block;
			t_pkt[0] = 0;
			t_pkt[1] = TFTP_ACK;
			t_pkt[2] = (uint8_t)(block >> 8);
			t_pkt[3] = (uint8_t)block;
			wlan_tftp_reply(s, 4);
			if (n < s->blk) {
				printf("SWEET WLAN: tftp %s got %u bytes\n",
				    s->path, t_wr_len);
				s->used = FALSE;
			}
		}
		break;
	case TFTP_ERROR:
		printf("SWEET WLAN: tftp %s peer error %u\n", s->name, block);
		s->used = FALSE;
		break;
	default:
		break;
	}
}

/*
 * TFTP retransmission timer: if a transfer has been silent for 1.5 s,
 * resend the window from the last acknowledged block. Without this a
 * lost ACK or a transient FIFO stall parks the download forever (0747
 * stalled at block 800 and never asked again).
 */
static void
wlan_tftp_tick(uint32_t ms)
{
	for (unsigned i = 0; i < TFTP_MAX_SESS; i++) {
		struct tftp_sess *s = &t_sess[i];

		if (!s->used || s->write || s->sent == 0) {
			continue;       /* waiting for ACK 0: do not send early */
		}
		if (ms - s->last_ms < 100) {
			continue;
		}
		s->last_ms = ms;
		printf("SWEET WLAN: tftp %s stall at %u (ack %u), resend\n",
		    s->name, s->sent, s->last_ack);
		wlan_tftp_window(s, s->last_ack + 1);
	}
}

/* --- F2 step 5b: rmtfs (EFS partitions over QRTR, writes to RAM) --------- */

static const struct {
	const char *path, *part;
} rmtfs_map[4] = {
	{ "/boot/modem_fs1", "modemst1" },
	{ "/boot/modem_fs2", "modemst2" },
	{ "/boot/modem_fsc", "fsc" },
	{ "/boot/modem_fsg", "fsg" },
};

static struct {
	boolean_t open;
	char part[16];
} rmtfs_caller[4];
static uint64_t rmtfs_stage_pa;
static uint32_t rmtfs_stage_size;

static const uint8_t *wlan_qm_find(const uint8_t *d, uint32_t len,
    uint8_t type, uint16_t *tlen);

static void
wlan_rmtfs_send(uint32_t node, uint32_t port, uint16_t txn, uint16_t msg,
    boolean_t ok, uint8_t type, const void *opt, uint16_t optlen)
{
	uint8_t r[64];
	uint16_t n = 7, zero = 0, one = 1, four = 4;

	r[0] = QMI_RESP;
	memcpy(r + 1, &txn, 2);
	memcpy(r + 3, &msg, 2);
	r[n] = 2;
	memcpy(r + n + 1, &four, 2);
	memcpy(r + n + 3, ok ? &zero : &one, 2);
	memcpy(r + n + 5, ok ? &zero : &one, 2);
	n += 7;
	if (ok && opt != NULL && optlen <= 16) {
		uint16_t l = optlen;

		r[n] = type;
		memcpy(r + n + 1, &l, 2);
		memcpy(r + n + 3, opt, optlen);
		n += 3 + optlen;
	}
	{
		uint16_t body = (uint16_t)(n - 7);

		memcpy(r + 5, &body, 2);
	}
	wlan_qrtr_send(QRTR_TYPE_DATA, PORT_RMTFS, node, port, r, n);
}

static uint32_t
wlan_qm_u32(const uint8_t *d, uint32_t len, uint8_t type, boolean_t *ok)
{
	uint16_t l = 0;
	const uint8_t *v = wlan_qm_find(d, len, type, &l);
	uint32_t x = 0;

	if (v == NULL || l < 4) {
		*ok = FALSE;
		return 0;
	}
	memcpy(&x, v, 4);
	return x;
}

static void
wlan_rmtfs_rx(uint32_t node, uint32_t port, const uint8_t *d, uint32_t len)
{
	uint16_t txn, msg, mlen;
	boolean_t ok = TRUE;

	if (len < 7 || d[0] != QMI_REQ) {
		return;
	}
	memcpy(&txn, d + 1, 2);
	memcpy(&msg, d + 3, 2);
	memcpy(&mlen, d + 5, 2);
	(void)mlen;
	switch (msg) {
	case 1: {                       /* OPEN: string path -> caller */
		uint16_t l = 0;
		const uint8_t *v = wlan_qm_find(d, len, 1, &l);
		char path[64];
		uint32_t caller = 4;

		memset(path, 0, sizeof(path));
		if (v != NULL) {
			memcpy(path, v, MIN((uint32_t)l,
			    (uint32_t)sizeof(path) - 1));
		}
		for (uint32_t k = 0; k < 4; k++) {
			if (strcmp(path, rmtfs_map[k].path) == 0) {
				caller = k;
			}
		}
		if (caller < 4) {
			uint32_t sz = 0;

			ok = lavender_modem_part_prepare(rmtfs_map[caller].part,
			    &sz) == 0;
			if (ok) {
				rmtfs_caller[caller].open = TRUE;
				memset(rmtfs_caller[caller].part, 0,
				    sizeof(rmtfs_caller[caller].part));
				memcpy(rmtfs_caller[caller].part,
				    rmtfs_map[caller].part,
				    strlen(rmtfs_map[caller].part));
			}
		}
		printf("SWEET WLAN: rmtfs open \"%s\" -> %s caller %u\n", path,
		    caller < 4 ? rmtfs_map[caller].part : "REJECTED", caller);
		wlan_rmtfs_send(node, port, txn, msg, ok && caller < 4, 0x10,
		    &caller, 4);
		break;
	}
	case 2: {                       /* CLOSE */
		uint32_t caller = wlan_qm_u32(d, len, 1, &ok);

		if (caller < 4) {
			rmtfs_caller[caller].open = FALSE;
		}
		printf("SWEET WLAN: rmtfs close caller %u\n", caller);
		wlan_rmtfs_send(node, port, txn, msg, ok, 0, NULL, 0);
		break;
	}
	case 3: {                       /* RW_IOVEC */
		uint32_t caller = wlan_qm_u32(d, len, 1, &ok);
		uint16_t l2 = 0, l3 = 0;
		const uint8_t *dir = wlan_qm_find(d, len, 2, &l2);
		const uint8_t *iov = wlan_qm_find(d, len, 3, &l3);
		uint32_t n = 0, total = 0, first = 0;

		ok = ok && dir != NULL && l2 >= 1 && iov != NULL && l3 >= 1 &&
		    l3 >= 1 + 12u * iov[0] && caller < 4 &&
		    rmtfs_caller[caller].open;
		n = ok ? iov[0] : 0;
		for (uint32_t k = 0; ok && k < n; k++) {
			uint32_t e[3], off, bytes, done = 0, buf_off;

			memcpy(e, iov + 1 + 12 * k, sizeof(e));
			off = e[0] * RMTFS_SECTOR;
			bytes = e[2] * RMTFS_SECTOR;
			/*
			 * e[1] is the absolute physical address inside the
			 * staging buffer the modem got from ALLOC_BUFF; the
			 * copy helper takes an offset into it.
			 */
			if (e[1] < rmtfs_stage_pa ||
			    (uint64_t)e[1] + bytes >
			    rmtfs_stage_pa + rmtfs_stage_size) {
				ok = FALSE;
				break;
			}
			buf_off = (uint32_t)((uint64_t)e[1] - rmtfs_stage_pa);
			if (k == 0) {
				first = e[0];
			}
			total += e[2];
			while (ok && done < bytes) {
				uint32_t n2 = MIN(bytes - done,
				    (uint32_t)sizeof(t_pkt));
				int r;

				if (dir[0] != 0) {
					r = lavender_modem_rmtfs_copy(
					    buf_off + done, t_pkt, n2, FALSE);
					if (r == (int)n2) {
						r = lavender_modem_part_write(
						    rmtfs_caller[caller].part,
						    off + done, t_pkt, n2);
					}
				} else {
					r = lavender_modem_part_read(
					    rmtfs_caller[caller].part,
					    off + done, t_pkt, n2);
					if (r == (int)n2) {
						r = lavender_modem_rmtfs_copy(
						    buf_off + done, t_pkt, n2,
						    TRUE);
					}
				}
				if (r != (int)n2) {
					ok = FALSE;
				}
				done += n2;
			}
		}
		printf("SWEET WLAN: rmtfs %s caller %u: %u entries, %u sectors "
		    "from %u: %s\n", dir != NULL && dir[0] ? "write" : "read",
		    caller, n, total, first, ok ? "ok" : "FAILED");
		wlan_rmtfs_send(node, port, txn, msg, ok, 0, NULL, 0);
		break;
	}
	case 4: {                       /* ALLOC_BUFF */
		uint32_t caller = wlan_qm_u32(d, len, 1, &ok);
		uint32_t size = wlan_qm_u32(d, len, 2, &ok);
		uint64_t pa = 0;
		uint32_t bsz = 0;

		ok = ok && lavender_modem_rmtfs_buf(&pa, &bsz) == 0 &&
		    size <= bsz;
		printf("SWEET WLAN: rmtfs alloc caller %u %u bytes -> %llx%s\n",
		    caller, size, (unsigned long long)pa, ok ? "" : " REFUSED");
		wlan_rmtfs_send(node, port, txn, msg, ok, 0x10, &pa, 8);
		break;
	}
	case 5: {                       /* GET_DEV_ERROR */
		uint32_t caller = wlan_qm_u32(d, len, 1, &ok);
		uint8_t status = 0;

		printf("SWEET WLAN: rmtfs dev-error caller %u\n", caller);
		wlan_rmtfs_send(node, port, txn, msg, ok, 0x10, &status, 1);
		break;
	}
	default:
		printf("SWEET WLAN: rmtfs unknown request %u (%u bytes)\n", msg,
		    len);
		break;
	}
}

/* --- F2 step 6: WLFW (QMI 0x45, host client) ---------------------------- */

#define WLFW_SVC          0x45
#define PORT_WLFW         0x4003
#define WLFW_CLIENT_ID    0x4b4e454cu
#define WLFW_MAX_DATA     6144u
#define WLFW_MSA_PA       0x85700000u
#define WLFW_MSA_SIZE     0x100000u
#define WLFW_PERM_RW      (0x4u | 0x2u)

#define WF_IND_REGISTER   0x20
#define WF_FW_READY_IND   0x21
#define WF_CAP            0x24
#define WF_BDF_DOWNLOAD   0x25
#define WF_CAL_REPORT     0x26
#define WF_MSA_READY_IND  0x2b
#define WF_MSA_INFO       0x2d
#define WF_MSA_READY      0x2e

enum {
	WS_OFF, WS_IND, WS_MSA_INFO, WS_MSA_READY, WS_CAP,
	WS_WAIT_MSA_IND, WS_BDF, WS_CAL, WS_WAIT_FW_READY, WS_READY, WS_FAILED
};

struct wlfw_reg {
	uint64_t addr;
	uint32_t size;
	boolean_t secure;
};

static struct {
	uint32_t node, port, txn, pending, state, board_id;
	boolean_t fw_ready, msa_ind;
	uint8_t buf[7 + WLFW_MAX_DATA + 256];
	uint8_t bdf[0x40000];
	uint32_t bdf_len, bdf_off, bdf_seg;
	struct wlfw_reg reg[2];
	unsigned nreg;
} wf;

static const char *
wlfw_state(uint32_t st)
{
	static const char *names[] = {
		"off", "ind", "msa-info", "msa-ready", "cap", "wait-msa-ind",
		"bdf", "cal", "wait-fw-ready", "ready", "failed"
	};

	return st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}

static void
wlfw_tlv(uint8_t *b, uint32_t *n, uint8_t type, const void *v, uint16_t len)
{
	if (*n + 3 + len > 7 + WLFW_MAX_DATA + 256) {
		return;
	}
	b[*n] = type;
	memcpy(b + *n + 1, &len, 2);
	memcpy(b + *n + 3, v, len);
	*n += 3 + len;
}

static void
wlfw_request(uint16_t msg, uint32_t n, uint32_t next)
{
	uint16_t body = (uint16_t)(n - 7);

	wf.txn++;
	if (wf.txn == 0) {
		wf.txn = 1;
	}
	wf.buf[0] = QMI_REQ;
	memcpy(wf.buf + 1, &wf.txn, 2);
	memcpy(wf.buf + 3, &msg, 2);
	memcpy(wf.buf + 5, &body, 2);
	wf.pending = msg;
	wf.state = next;
	wlan_qrtr_send(QRTR_TYPE_DATA, PORT_WLFW, wf.node, wf.port, wf.buf, n);
}

static void
wlfw_fail(const char *why)
{
	printf("SWEET WLAN: wlfw FAIL %s in %s\n", why, wlfw_state(wf.state));
	wf.state = WS_FAILED;
	wf.pending = 0;
}

static void
wlfw_ind_register(void)
{
	uint8_t one = 1;
	uint32_t n = 7, id = WLFW_CLIENT_ID;

	wlfw_tlv(wf.buf, &n, 0x10, &one, 1);    /* fw_ready_enable */
	wlfw_tlv(wf.buf, &n, 0x13, &one, 1);    /* msa_ready_enable */
	wlfw_tlv(wf.buf, &n, 0x15, &id, 4);     /* client_id */
	printf("SWEET WLAN: wlfw ind-register to %u:%x\n", wf.node, wf.port);
	wlfw_request(WF_IND_REGISTER, n, WS_IND);
}

static boolean_t
wlfw_bdf_load(void)
{
	char name[16];
	uint32_t size = 0;

	if (wf.board_id < 0xff) {
		name[0] = 'B'; name[1] = 'D'; name[2] = 'W'; name[3] = 'L';
		name[4] = 'A'; name[5] = 'N'; name[6] = '.'; name[7] = 'B';
		name[8] = "0123456789ABCDEF"[(wf.board_id >> 4) & 0xf];
		name[9] = "0123456789ABCDEF"[wf.board_id & 0xf];
		name[10] = 0;
	} else if (wf.board_id > 0xff && wf.board_id <= 0xfff) {
		name[0] = 'B'; name[1] = 'D'; name[2] = 'W'; name[3] = 'L';
		name[4] = 'A'; name[5] = 'N'; name[6] = '.';
		name[7] = "0123456789ABCDEF"[(wf.board_id >> 8) & 0xf];
		name[8] = "0123456789ABCDEF"[(wf.board_id >> 4) & 0xf];
		name[9] = "0123456789ABCDEF"[wf.board_id & 0xf];
		name[10] = 0;
	} else {
		const char *d = "BDWLAN.BIN";
		unsigned i = 0;

		while (d[i] != 0) {
			name[i] = d[i];
			i++;
		}
		name[i] = 0;
	}
	if (lavender_modem_fw_size(name, &size) != 0) {
		const char *d = "BDWLAN.BIN";
		unsigned i = 0;

		printf("SWEET WLAN: wlfw %s missing, trying BDWLAN.BIN\n", name);
		while (d[i] != 0) {
			name[i] = d[i];
			i++;
		}
		name[i] = 0;
		if (lavender_modem_fw_size(name, &size) != 0) {
			return FALSE;
		}
	}
	if (size == 0 || size > sizeof(wf.bdf)) {
		printf("SWEET WLAN: wlfw %s has %u bytes\n", name, size);
		return FALSE;
	}
	if (lavender_modem_fw_read(name, 0, wf.bdf, size) != (int)size) {
		return FALSE;
	}
	wf.bdf_len = size;
	printf("SWEET WLAN: wlfw board id %x -> %s (%u bytes)\n",
	    wf.board_id, name, size);
	return TRUE;
}

static void
wlfw_bdf_next(void)
{
	uint8_t one = 1, zero = 0;
	uint32_t n = 7, zero32 = 0;
	uint32_t left = wf.bdf_len - wf.bdf_off;
	uint16_t len = (uint16_t)MIN(left, WLFW_MAX_DATA);
	uint8_t end = left <= WLFW_MAX_DATA;
	uint16_t tl = (uint16_t)(2 + len);

	wlfw_tlv(wf.buf, &n, 0x01, &one, 1);
	wlfw_tlv(wf.buf, &n, 0x10, &zero32, 4);         /* file_id */
	wlfw_tlv(wf.buf, &n, 0x11, &wf.bdf_len, 4);
	wlfw_tlv(wf.buf, &n, 0x12, &wf.bdf_seg, 4);
	wf.buf[n] = 0x13;
	memcpy(wf.buf + n + 1, &tl, 2);
	memcpy(wf.buf + n + 3, &len, 2);
	memcpy(wf.buf + n + 5, wf.bdf + wf.bdf_off, len);
	n += 5u + len;
	wlfw_tlv(wf.buf, &n, 0x14, &end, 1);
	wlfw_request(WF_BDF_DOWNLOAD, n, WS_BDF);
	(void)zero;
}

static void
wlfw_bdf_start(void)
{
	if (!wlfw_bdf_load()) {
		wlfw_fail("no board data");
		return;
	}
	wf.bdf_off = 0;
	wf.bdf_seg = 0;
	printf("SWEET WLAN: wlfw sending board data (%u bytes)\n", wf.bdf_len);
	wlfw_bdf_next();
}

static boolean_t
wlfw_msa_assign(unsigned k, boolean_t to_wlan)
{
	uint32_t hlos = 0x3, rw = WLFW_PERM_RW;
	uint32_t vm[3] = { 0x0f, 0x18, 0x19 };
	uint32_t perm[3] = { rw, rw, rw };
	unsigned cnt = wf.reg[k].secure ? 2 : 3;
	int r;

	if (to_wlan) {
		r = lavender_modem_assign((uint32_t)wf.reg[k].addr,
		    wf.reg[k].size, &hlos, 1, vm, perm, cnt);
	} else {
		r = lavender_modem_assign((uint32_t)wf.reg[k].addr,
		    wf.reg[k].size, vm, cnt, &hlos, &rw, 1);
	}
	printf("SWEET WLAN: wlfw msa %llx+%x %s: ret %d\n",
	    (unsigned long long)wf.reg[k].addr, wf.reg[k].size,
	    to_wlan ? "to WLAN" : "back to HLOS", r);
	return r == 0;
}

static void
wlfw_rx(uint32_t node, uint32_t port, const uint8_t *d, uint32_t len)
{
	uint16_t txn, msg, mlen;
	uint16_t l = 0, res = 1, err = 0;
	const uint8_t *t, *v;

	if (len < 7 || node != wf.node || port != wf.port ||
	    wf.state == WS_OFF || wf.state == WS_FAILED) {
		return;
	}
	memcpy(&txn, d + 1, 2);
	memcpy(&msg, d + 3, 2);
	memcpy(&mlen, d + 5, 2);
	t = d + 7;
	(void)mlen;

	if (d[0] == 4) {                        /* indication */
		if (msg == WF_MSA_READY_IND) {
			wf.msa_ind = TRUE;
			printf("SWEET WLAN: wlfw MSA ready indication\n");
			if (wf.state == WS_WAIT_MSA_IND) {
				wlfw_bdf_start();
			}
		} else if (msg == WF_FW_READY_IND) {
			wf.fw_ready = TRUE;
			printf("SWEET WLAN: *** wlfw FW READY ***\n");
			if (wf.pending == 0) {
				wf.state = WS_READY;
			}
		} else {
			printf("SWEET WLAN: wlfw indication %x (%u bytes)\n",
			    msg, len);
		}
		return;
	}
	if (msg != wf.pending || txn != wf.txn) {
		printf("SWEET WLAN: wlfw unexpected %x txn %u (want %x/%u)\n",
		    msg, txn, wf.pending, wf.txn);
		return;
	}
	wf.pending = 0;
	v = wlan_qm_find(d, len, 2, &l);
	if (v != NULL && l >= 4) {
		memcpy(&res, v, 2);
		memcpy(&err, v + 2, 2);
	}
	switch (msg) {
	case WF_IND_REGISTER: {
		uint64_t st = 0;

		v = wlan_qm_find(d, len, 0x10, &l);
		if (v != NULL && l >= 8) {
			memcpy(&st, v, 8);
		}
		printf("SWEET WLAN: wlfw ind-register result %u err %u "
		    "fw_status %llx\n", res, err, (unsigned long long)st);
		if (res != 0) {
			wlfw_fail("ind-register rejected");
		} else if (st & 2) {
			wf.state = WS_READY;
			wf.fw_ready = TRUE;
			printf("SWEET WLAN: *** wlfw already up ***\n");
		} else {
			uint32_t n = 7, size = WLFW_MSA_SIZE;
			uint64_t pa = WLFW_MSA_PA;

			wlfw_tlv(wf.buf, &n, 0x01, &pa, 8);
			wlfw_tlv(wf.buf, &n, 0x02, &size, 4);
			wlfw_request(WF_MSA_INFO, n, WS_MSA_INFO);
		}
		break;
	}
	case WF_MSA_INFO: {
		unsigned cnt;
		unsigned k;

		v = wlan_qm_find(d, len, 0x03, &l);
		cnt = v != NULL && l >= 1 ? v[0] : 0;
		if (res != 0 || v == NULL || cnt > 2 || l < 1 + 13 * cnt) {
			wlfw_fail("msa-info rejected");
			break;
		}
		wf.nreg = cnt;
		for (k = 0; k < cnt; k++) {
			memcpy(&wf.reg[k].addr, v + 1 + 13 * k, 8);
			memcpy(&wf.reg[k].size, v + 9 + 13 * k, 4);
			wf.reg[k].secure = v[13 + 13 * k] != 0;
			printf("SWEET WLAN: wlfw msa %u: %llx+%x secure %u\n",
			    k, (unsigned long long)wf.reg[k].addr,
			    wf.reg[k].size, wf.reg[k].secure);
			if (wf.reg[k].size == 0 ||
			    wf.reg[k].size > WLFW_MSA_SIZE ||
			    wf.reg[k].addr < WLFW_MSA_PA ||
			    wf.reg[k].addr - WLFW_MSA_PA >
			    WLFW_MSA_SIZE - wf.reg[k].size) {
				wlfw_fail("msa region out of range");
				return;
			}
		}
		for (k = 0; k < cnt; k++) {
			if (!wlfw_msa_assign(k, TRUE)) {
				wlfw_fail("msa assign failed");
				return;
			}
		}
		wlfw_request(WF_MSA_READY, 7, WS_MSA_READY);
		break;
	}
	case WF_MSA_READY:
		printf("SWEET WLAN: wlfw msa-ready result %u err %u\n", res, err);
		if (res != 0) {
			wlfw_fail("msa-ready rejected");
		} else {
			wlfw_request(WF_CAP, 7, WS_CAP);
		}
		break;
	case WF_CAP:
		if (res != 0) {
			wlfw_fail("cap rejected");
			break;
		}
		wf.board_id = 0xff;
		v = wlan_qm_find(d, len, 0x11, &l);
		if (v != NULL && l >= 4) {
			memcpy(&wf.board_id, v, 4);
		}
		printf("SWEET WLAN: wlfw cap board_id %x\n", wf.board_id);
		wf.state = WS_WAIT_MSA_IND;
		if (wf.msa_ind) {
			wlfw_bdf_start();
		}
		break;
	case WF_BDF_DOWNLOAD: {
		uint32_t left = wf.bdf_len - wf.bdf_off;
		uint32_t seg = MIN(left, WLFW_MAX_DATA);
		boolean_t end = wf.bdf_off + seg >= wf.bdf_len;

		if (res != 0 && !end) {
			wlfw_fail("bdf segment rejected");
			break;
		}
		wf.bdf_off += seg;
		wf.bdf_seg++;
		if (!end) {
			wlfw_bdf_next();
			break;
		}
		printf("SWEET WLAN: wlfw board data sent in %u segments\n",
		    wf.bdf_seg);
		{
			uint8_t zero = 0;
			uint32_t n = 7;

			wlfw_tlv(wf.buf, &n, 0x01, &zero, 1);
			wlfw_request(WF_CAL_REPORT, n, WS_CAL);
		}
		break;
	}
	case WF_CAL_REPORT:
		printf("SWEET WLAN: wlfw cal-report result %u err %u\n", res,
		    err);
		if (res != 0) {
			wlfw_fail("cal-report rejected");
		} else {
			wf.state = wf.fw_ready ? WS_READY : WS_WAIT_FW_READY;
		}
		break;
	default:
		printf("SWEET WLAN: wlfw response %x (%u bytes)\n", msg, len);
		break;
	}
}

static void
wlfw_start(uint32_t node, uint32_t port)
{
	wf.node = node;
	wf.port = port;
	wf.txn = 0;
	wf.pending = 0;
	wf.msa_ind = FALSE;
	wf.fw_ready = FALSE;
	wf.state = WS_OFF;
	printf("SWEET WLAN: *** WLFW service at %u:%x ***\n", node, port);
	wlfw_ind_register();
}

/* --- F2 step 4: QMI servreg-locator (pd-mapper) ------------------------- */

#define PORT_PDM  0x4001

struct qmsg {
	uint8_t b[512];
	uint32_t n;
};

static boolean_t q_announced;
static unsigned pd_reqs;

static void
wlan_qm_init(struct qmsg *m, uint16_t txn, uint16_t msg)
{
	m->b[0] = QMI_RESP;
	memcpy(m->b + 1, &txn, 2);
	memcpy(m->b + 3, &msg, 2);
	m->n = 7;
}

static void
wlan_qm_tlv(struct qmsg *m, uint8_t type, const void *v, uint16_t len)
{
	if (m->n + 3 + len > sizeof(m->b)) {
		return;
	}
	m->b[m->n] = type;
	memcpy(m->b + m->n + 1, &len, 2);
	memcpy(m->b + m->n + 3, v, len);
	m->n += 3 + len;
}

static void
wlan_qm_result(struct qmsg *m, boolean_t ok)
{
	uint16_t r[2] = { (uint16_t)(ok ? 0 : 1), (uint16_t)(ok ? 0 : 1) };

	wlan_qm_tlv(m, 2, r, 4);
}

static const uint8_t *
wlan_qm_find(const uint8_t *d, uint32_t len, uint8_t type, uint16_t *tlen)
{
	uint32_t o = 7;

	while (o + 3 <= len) {
		uint16_t l;

		memcpy(&l, d + o + 1, 2);
		if (o + 3 + l > len) {
			break;
		}
		if (d[o] == type) {
			*tlen = l;
			return d + o + 3;
		}
		o += 3 + l;
	}
	return NULL;
}

static void
wlan_qm_string(const uint8_t *d, uint32_t len, uint8_t type, char *out,
    size_t max)
{
	uint16_t l = 0;
	const uint8_t *v = wlan_qm_find(d, len, type, &l);

	out[0] = 0;
	if (v != NULL) {
		l = (uint16_t)MIN((size_t)l, max - 1);
		memcpy(out, v, l);
		out[l] = 0;
	}
}

static void
wlan_qm_send(struct qmsg *m, uint32_t src_port, uint32_t node, uint32_t port)
{
	uint16_t l = (uint16_t)(m->n - 7);

	memcpy(m->b + 5, &l, 2);
	wlan_qrtr_send(QRTR_TYPE_DATA, src_port, node, port, m->b, m->n);
}

static boolean_t
wlan_streq(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void
wlan_pdm_rx(uint32_t node, uint32_t port, uint16_t txn, uint16_t msg,
    const uint8_t *d, uint32_t len)
{
	static const struct {
		const char *svc, *domain;
		uint32_t inst;
	} map[] = {
		/*
		 * Same table as bergamota boot 43: pddump_disabled and
		 * pdr_enabled must answer 0 domains, or the modem's PDR
		 * never asks for kernel/elf_loader (and never starts the
		 * WLAN PD).
		 */
		{ "tms/servreg",       "msm/modem/root_pd", 180 },
		{ "kernel/elf_loader", "msm/modem/wlan_pd", 180 },
		{ "tms/servreg",       "msm/modem/wlan_pd", 180 },
		{ "wlan/fw",           "msm/modem/wlan_pd", 180 },
		{ "avs/audio",         "msm/adsp/audio_pd", 74 },
	};
	struct qmsg m;
	char name[80];
	uint8_t list[256];
	uint32_t n = 1, cnt = 0;

	wlan_qm_init(&m, txn, msg);
	if (msg != 0x21) {
		printf("SWEET WLAN: pd-mapper msg %x (ack)\n", msg);
		wlan_qm_result(&m, TRUE);
		wlan_qm_send(&m, PORT_PDM, node, port);
		return;
	}
	printf("SWEET WLAN: pd-mapper raw %02x %02x %02x %02x %02x %02x "
	    "%02x %02x %02x %02x %02x %02x %02x %02x len %u\n", d[0], d[1],
	    d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11],
	    d[12], d[13], len);
	wlan_qm_string(d, len, 1, name, sizeof(name));
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (wlan_streq(map[i].svc, name)) {
			uint32_t dl = 0, inst = map[i].inst, zero = 0;

			while (map[i].domain[dl] != 0) {
				dl++;
			}
			if (n + dl + 10 > sizeof(list)) {
				break;
			}
			list[n++] = (uint8_t)dl;
			memcpy(list + n, map[i].domain, dl);
			n += dl;
			memcpy(list + n, &inst, 4);
			n += 4;
			list[n++] = 0;          /* service_data_valid */
			memcpy(list + n, &zero, 4);
			n += 4;
			cnt++;
		}
	}
	/*
	 * servreg_loc_get_domain_list_resp_ei as bergamota 1115 sends it:
	 * TLV 0x10 u16 total_domains, TLV 0x11 u16 db_revision, TLV 0x12
	 * [u8 count][entries]. No QMI_OPT_FLAG prefixes: the 0720 "validity
	 * byte" was added while rmtfs was still missing and broke the parser.
	 */
	list[0] = (uint8_t)cnt;
	wlan_qm_result(&m, TRUE);
	{
		uint8_t t16[2] = { (uint8_t)(cnt & 0xff),
		    (uint8_t)(cnt >> 8) };
		uint8_t t17[2] = { 1, 0 };

		wlan_qm_tlv(&m, 0x10, t16, sizeof(t16));
		wlan_qm_tlv(&m, 0x11, t17, sizeof(t17));
	}
	if (cnt != 0) {
		wlan_qm_tlv(&m, 0x12, list, (uint16_t)n);
	}
	wlan_qm_send(&m, PORT_PDM, node, port);
	pd_reqs++;
	printf("SWEET WLAN: pd-mapper \"%s\" -> %u domain(s)\n", name, cnt);
}

static void
wlan_svc_announce(void)
{
	struct qrtr_ctrl c = { QRTR_TYPE_NEW_SERVER, 0x40, 0x101,
	    QRTR_NODE_APPS, PORT_PDM };
	struct qrtr_ctrl t = { QRTR_TYPE_NEW_SERVER, TFTP_SVC, 1,
	    QRTR_NODE_APPS, PORT_TFTP };
	struct qrtr_ctrl r = { QRTR_TYPE_NEW_SERVER, RMTFS_SVC, 1,
	    QRTR_NODE_APPS, PORT_RMTFS };
	uint64_t pa = 0;
	uint32_t bsz = 0;

	wlan_qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, q_modem_node,
	    QRTR_PORT_CTRL, &c, sizeof(c));
	printf("SWEET WLAN: announced pd-mapper 0x40:0x101 port %x\n",
	    PORT_PDM);
	wlan_qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, q_modem_node,
	    QRTR_PORT_CTRL, &t, sizeof(t));
	printf("SWEET WLAN: announced tftp %u:1 port %x\n", TFTP_SVC,
	    PORT_TFTP);
	if (lavender_modem_rmtfs_buf(&pa, &bsz) == 0) {
		rmtfs_stage_pa = pa;
		rmtfs_stage_size = bsz;
		printf("SWEET WLAN: rmtfs buffer ready at %08llx\n",
		    (unsigned long long)pa);
	} else {
		rmtfs_stage_pa = 0;
		rmtfs_stage_size = 0;
		printf("SWEET WLAN: rmtfs buffer unavailable\n");
	}
	wlan_qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, q_modem_node,
	    QRTR_PORT_CTRL, &r, sizeof(r));
	printf("SWEET WLAN: announced rmtfs %u:1 port %x\n", RMTFS_SVC,
	    PORT_RMTFS);
}

static boolean_t
wlan_svc_rx(uint32_t src_node, uint32_t src_port, uint32_t dst_port,
    const uint8_t *d, uint32_t len)
{
	uint16_t txn, msg;

	if (dst_port == PORT_RMTFS) {
		wlan_rmtfs_rx(src_node, src_port, d, len);
		return TRUE;
	}
	if (dst_port == PORT_WLFW) {
		wlfw_rx(src_node, src_port, d, len);
		return TRUE;
	}
	if (dst_port == PORT_TFTP) {
		wlan_tftp_req(src_node, src_port, d, len);
		return TRUE;
	}
	if (dst_port >= PORT_SESS_BASE &&
	    dst_port < PORT_SESS_BASE + TFTP_MAX_SESS) {
		wlan_tftp_sess_rx(dst_port, src_port, d, len);
		return TRUE;
	}
	if (dst_port != PORT_PDM || len < 7 || d[0] != QMI_REQ) {
		return FALSE;
	}
	memcpy(&txn, d + 1, 2);
	memcpy(&msg, d + 3, 2);
	wlan_pdm_rx(src_node, src_port, txn, msg, d, len);
	return TRUE;
}

static void
wlan_qrtr_rx(const uint8_t *p, uint32_t len)
{
	uint32_t type, src_node, src_port, dst_node, dst_port, size, hlen, confirm;
	const struct qrtr_ctrl *c;

	if (len >= sizeof(struct qrtr_hdr_v1) && p[0] == 1 && p[1] == 0 &&
	    p[2] == 0 && p[3] == 0) {
		const struct qrtr_hdr_v1 *h =
		    (const struct qrtr_hdr_v1 *)(const void *)p;

		type = h->type;
		src_node = h->src_node;
		src_port = h->src_port;
		dst_node = h->dst_node;
		dst_port = h->dst_port;
		size = h->size;
		hlen = sizeof(*h);
		confirm = h->confirm_rx;
		q_ver = 1;
	} else if (len >= sizeof(struct qrtr_hdr_v2) && p[0] == 2) {
		const struct qrtr_hdr_v2 *h =
		    (const struct qrtr_hdr_v2 *)(const void *)p;

		type = h->type;
		src_node = h->src_node;
		src_port = h->src_port == 0xfffe ? QRTR_PORT_CTRL : h->src_port;
		dst_node = h->dst_node;
		dst_port = h->dst_port == 0xfffe ? QRTR_PORT_CTRL : h->dst_port;
		size = h->size;
		hlen = sizeof(*h) + h->optlen * 4;
		confirm = h->flags & 1;
		q_ver = 2;
	} else {
		printf("SWEET WLAN: qrtr bad packet, %u bytes\n", len);
		return;
	}
	if (hlen + size > len) {
		return;
	}
	c = (const struct qrtr_ctrl *)(const void *)(p + hlen);

	switch (type) {
	case QRTR_TYPE_HELLO:
		q_modem_node = src_node;
		q_hello_rx = TRUE;
		q_hello_sent = FALSE;
		printf("SWEET WLAN: qrtr v%u HELLO from node %u\n", q_ver,
		    src_node);
		break;
	case QRTR_TYPE_NEW_LOOKUP:
		printf("SWEET WLAN: qrtr lookup from node %u, re-announcing\n",
		    src_node);
		wlan_svc_announce();
		break;
	case QRTR_TYPE_NEW_SERVER:
		if (size < sizeof(*c)) {
			break;
		}
		printf("SWEET WLAN: service %x:%x node %u port %x %s\n",
		    c->a, c->b, c->c, c->d, wlan_svc_name(c->a));
		if (c->a == QMI_SVC_WLFW) {
			printf("SWEET WLAN: *** WLFW 0x45 up: node %u port "
			    "%x ***\n", c->c, c->d);
			q_wlfw_seen = TRUE;
			wlfw_start(c->c, c->d);
		}
		break;
	case QRTR_TYPE_DATA:
		if (confirm) {
			struct qrtr_ctrl r = { QRTR_TYPE_RESUME_TX,
				dst_node, dst_port, 0, 0 };

			/* Never dropped: the peer blocks after 10 misses. */
			(void)wlan_qrtr_send_ctl(QRTR_TYPE_RESUME_TX,
			    dst_port, src_node, src_port, &r, sizeof(r));
		}
		(void)wlan_svc_rx(src_node, src_port, dst_port, p + hlen,
		    size);
		break;
	case QRTR_TYPE_RESUME_TX:
		/*
		 * The modem confirms our DATA with RESUME_TX; for the TFTP
		 * session ports that proves it consumed the packet.
		 */
		if (dst_port >= PORT_SESS_BASE &&
		    dst_port < PORT_SESS_BASE + TFTP_MAX_SESS) {
			printf("SWEET WLAN: qrtr RESUME_TX node %u port %x "
			    "to %x\n", src_node, src_port, dst_port);
		}
		break;
	default:
		printf("SWEET WLAN: qrtr type %u node %u port %x to %x "
		    "size %u\n", type, src_node, src_port, dst_port, size);
		break;
	}
}

static boolean_t
wlan_rx_one(void)
{
	struct glink_msg m;
	uint32_t avail = wlan_rx_avail(), n;

	if (avail < sizeof(m)) {
		return FALSE;
	}
	wlan_rx_peek(0, &m, sizeof(m));
	g_rx_msgs++;
	switch (m.cmd) {
	case CMD_VERSION:
		wlan_rx_advance(8);
		printf("SWEET WLAN: glink VERSION %u features %x\n", m.p1,
		    m.p2);
		if (m.p1 != 0) {
			g_features &= m.p2;
			wlan_glink_cmd(CMD_VERSION_ACK, 1, g_features);
		}
		break;
	case CMD_VERSION_ACK:
		wlan_rx_advance(8);
		printf("SWEET WLAN: glink VERSION_ACK %u features %x\n", m.p1,
		    m.p2);
		if (m.p1 != 0 && m.p2 != g_features) {
			g_features &= m.p2;
			wlan_glink_cmd(CMD_VERSION, 1, g_features);
		}
		break;
	case CMD_OPEN: {
		char name[33] = {};

		n = (8 + m.p2 + 7) & ~7u;
		if (avail < n) {
			return FALSE;
		}
		wlan_rx_peek(8, name, MIN(m.p2, 32u));
		wlan_rx_advance(n);
		printf("SWEET WLAN: glink modem OPEN \"%s\" rcid %u\n", name,
		    m.p1);
		if (wlan_name_eq(name, GLINK_CHAN_NAME)) {
			g_rcid = m.p1;
			g_remote_open = TRUE;
			wlan_glink_cmd(CMD_OPEN_ACK, (uint16_t)g_rcid, 0);
			if (!g_open_sent) {
				wlan_glink_open();
			}
		}
		break;
	}
	case CMD_OPEN_ACK:
		wlan_rx_advance(8);
		printf("SWEET WLAN: glink OPEN_ACK lcid %u\n", m.p1);
		if (m.p1 == GLINK_LCID) {
			g_open_acked = TRUE;
		}
		break;
	case CMD_CLOSE:
		wlan_rx_advance(8);
		printf("SWEET WLAN: glink modem CLOSE rcid %u\n", m.p1);
		wlan_glink_cmd(CMD_CLOSE_ACK, m.p1, 0);
		if (m.p1 == g_rcid) {
			g_remote_open = FALSE;
		}
		break;
	case CMD_INTENT:
		n = (8 + 8 * m.p2 + 7) & ~7u;
		if (avail < n) {
			return FALSE;
		}
		for (uint32_t i = 0; i < m.p2; i++) {
			uint32_t pair[2];
			unsigned k;

			wlan_rx_peek(8 + 8 * i, pair, 8);
			if (m.p1 != g_rcid) {
				continue;
			}
			/*
			 * The modem re-announces an id when it reuses a
			 * buffer; update that slot instead of appending a
			 * duplicate, or the table fills with stale ids.
			 */
			for (k = 0; k < g_ri_count; k++) {
				if (g_ri[k].id == pair[1]) {
					g_ri[k].size = pair[0];
					g_ri[k].used = FALSE;
					break;
				}
			}
			if (k == g_ri_count && g_ri_count < GLINK_MAX_INTENTS) {
				g_ri[g_ri_count].size = pair[0];
				g_ri[g_ri_count].id = pair[1];
				g_ri[g_ri_count].used = FALSE;
				g_ri_count++;
			}
			if (pair[0] != g_last_intent_size) {
				g_last_intent_size = pair[0];
				printf("SWEET WLAN: glink intent %x bytes id %x\n",
				    pair[0], pair[1]);
			}
			if (pair[0] > g_max_intent_size) {
				g_max_intent_size = pair[0];
			}
		}
		wlan_rx_advance(n);
		g_intent_req_sent = FALSE;
		break;
	case CMD_RX_DONE:
	case CMD_RX_DONE_W_REUSE:
		wlan_rx_advance(8);
		for (unsigned i = 0; i < g_ri_count; i++) {
			if (g_ri[i].id == m.p2) {
				if (m.cmd == CMD_RX_DONE_W_REUSE) {
					g_ri[i].used = FALSE;
				} else {
					g_ri[i] = g_ri[--g_ri_count];
				}
				break;
			}
		}
		break;
	case CMD_RX_INTENT_REQ:
		wlan_rx_advance(8);
		wlan_glink_intents(1, MAX(m.p2, GLINK_INTENT_SIZE));
		wlan_glink_cmd(CMD_RX_INTENT_REQ_ACK, GLINK_LCID, 1);
		break;
	case CMD_RX_INTENT_REQ_ACK:
		wlan_rx_advance(8);
		if (m.p2 == 0) {
			g_intent_req_sent = FALSE;
		}
		break;
	case CMD_CLOSE_ACK:
	case CMD_SIGNALS:
		wlan_rx_advance(8);
		break;
	case CMD_READ_NOTIF:
		wlan_rx_advance(8);
		if (apcs != 0) {
			*(volatile uint32_t *)(apcs + APCS_IPC_OFF) =
			    1u << GLINK_IRQ_BIT;
		}
		wlan_mb();
		break;
	case CMD_TX_DATA:
	case CMD_TX_DATA_CONT: {
		uint32_t hdr[2];

		if (avail < 16) {
			return FALSE;
		}
		wlan_rx_peek(8, hdr, 8);
		n = (16 + hdr[0] + 7) & ~7u;
		if (avail < n) {
			return FALSE;
		}
		if (m.cmd == CMD_TX_DATA) {
			g_asm_len = 0;
		}
		if (g_asm_len + hdr[0] <= sizeof(g_asm)) {
			wlan_rx_peek(16, g_asm + g_asm_len, hdr[0]);
			g_asm_len += hdr[0];
		}
		wlan_rx_advance(n);
		if (hdr[1] == 0) {
			if (m.p1 == g_rcid) {
				wlan_qrtr_rx(g_asm, g_asm_len);
			}
			if (g_features & 1) {
				wlan_glink_cmd(CMD_RX_DONE_W_REUSE,
				    GLINK_LCID, m.p2);
			} else {
				wlan_glink_cmd(CMD_RX_DONE, GLINK_LCID, m.p2);
				wlan_glink_intents(1, GLINK_INTENT_SIZE);
			}
		}
		break;
	}
	default:
		printf("SWEET WLAN: glink unknown cmd %u (%x %x)\n", m.cmd,
		    m.p1, m.p2);
		g_rx = NULL;
		return FALSE;
	}
	return TRUE;
}

static uint8_t *
wlan_glink_rx_fifo(struct smem_part_hdr *part, uint32_t *sz)
{
	uint8_t *v = (uint8_t *)wlan_f2_item_cached(part, ITEM_GLINK_RX, sz);

	if (v != NULL) {
		return v;
	}
	return (uint8_t *)wlan_f2_item(part, ITEM_GLINK_RX, sz);
}

static void
wlan_glink_poll(unsigned ms)
{
	g_ms = ms;
	wlan_tftp_tick(ms);
	wlan_glink_ctl_flush();
	if (g_desc != NULL) {
		uint32_t d0 = g_desc[0], d1 = g_desc[1], d2 = g_desc[2];
		uint32_t d3 = g_desc[3];

		if (d0 != g_last_desc[0] || d1 != g_last_desc[1] ||
		    d2 != g_last_desc[2] || d3 != g_last_desc[3]) {
			g_last_desc[0] = d0;
			g_last_desc[1] = d1;
			g_last_desc[2] = d2;
			g_last_desc[3] = d3;
			printf("SWEET WLAN: t=%u desc tx %x/%x rx %x/%x\n",
			    ms, d0, d1, d2, d3);
		}
	}
	/* Re-ring while the remote has not answered: it may have missed the
	 * first doorbell if its transport was still initializing. */
	if (!g_remote_open && g_tx != NULL && ms != 0 && (ms % 500) == 0 &&
	    apcs != 0) {
		*(volatile uint32_t *)(apcs + APCS_IPC_OFF) =
		    1u << GLINK_IRQ_BIT;
		wlan_mb();
	}
	if (g_rx == NULL && g_part != NULL) {
		uint32_t sz = 0;

		g_rx = wlan_glink_rx_fifo(g_part, &sz);
		if (g_rx != NULL) {
			g_rx_len = sz;
			printf("SWEET WLAN: glink modem RX FIFO %u bytes\n",
			    sz);
		}
	}
	for (int i = 0; i < 64 && g_rx != NULL && wlan_rx_one(); i++) {
	}
	/* A RESUME_TX generated while handling RX should leave right away. */
	wlan_glink_ctl_flush();
	if (q_pending_len != 0) {
		unsigned i;

		for (i = 0; i < g_ri_count; i++) {
			if (!g_ri[i].used && g_ri[i].size >= q_pending_len) {
				break;
			}
		}
		if (i < g_ri_count) {
			struct glink_data_hdr h = { { CMD_TX_DATA,
			    GLINK_LCID, g_ri[i].id }, q_pending_len, 0 };

			g_ri[i].used = TRUE;
			if (wlan_tx_write(&h, sizeof(h), q_pending,
			    q_pending_len)) {
				q_pending_len = 0;
			} else {
				g_ri[i].used = FALSE;
			}
		} else if (!g_intent_req_sent) {
			wlan_glink_cmd(CMD_RX_INTENT_REQ, GLINK_LCID,
			    q_pending_len);
			g_intent_req_sent = TRUE;
		}
	}
	if (wlan_chan_up() && !g_intents_sent) {
		wlan_glink_intents(GLINK_OUR_INTENTS, GLINK_INTENT_SIZE);
		g_intents_sent = TRUE;
		printf("SWEET WLAN: t=%u ms IPCRTR open, %u intents sent\n", ms,
		    GLINK_OUR_INTENTS);
	}
	if (wlan_chan_up() && !q_hello_sent) {
		struct qrtr_ctrl c = { QRTR_TYPE_HELLO, 0, 0, 0, 0 };

		wlan_qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, q_modem_node,
		    QRTR_PORT_CTRL, &c, sizeof(c));
		q_hello_sent = TRUE;
		printf("SWEET WLAN: t=%u ms sent qrtr HELLO to node %x\n", ms,
		    q_modem_node);
	}
	if (wlan_chan_up() && q_hello_rx && !q_announced) {
		wlan_svc_announce();
		q_announced = TRUE;
	}
	if (ms >= 2000 && !g_open_sent && g_rx != NULL) {
		printf("SWEET WLAN: t=%u ms modem did not open IPCRTR, "
		    "opening it\n", ms);
		wlan_glink_open();
	}
}

static boolean_t
wlan_glink_start(struct smem_part_hdr *part)
{
	uint32_t *desc;
	uint32_t sz = 0;

	g_part = part;
	desc = (uint32_t *)wlan_smem_alloc(part, ITEM_GLINK_DESC, 32);
	if (desc == NULL) {
		printf("SWEET WLAN: glink descriptor allocation failed\n");
		return FALSE;
	}
	g_desc = desc;
	g_tx = (uint8_t *)wlan_smem_alloc(part, ITEM_GLINK_TX, GLINK_TX_SIZE);
	if (g_tx == NULL) {
		printf("SWEET WLAN: glink TX FIFO allocation failed\n");
		return FALSE;
	}
	g_rx = wlan_glink_rx_fifo(part, &sz);
	if (g_rx != NULL) {
		g_rx_len = sz;
	}
	printf("SWEET WLAN: glink desc before %08x %08x %08x %08x\n", desc[0],
	    desc[1], desc[2], desc[3]);
	desc[1] = 0;                            /* tx head */
	desc[2] = 0;                            /* rx tail */
	wlan_mb();
	printf("SWEET WLAN: glink up: tx %08x, rx %s (%u bytes)\n",
	    (uint32_t)(vm_offset_t)g_tx, g_rx != NULL ? "ready" : "wait", sz);
	wlan_glink_cmd(CMD_VERSION, 1, g_features);
	return TRUE;
}

/* --- F2 step 5c: RPM GLINK and the WCN3990 rails ------------------------ */
/*
 * The four icnss supplies are RPM-SMD resources driven through the RPM's
 * own GLINK edge: a 28 KiB msgram at 0x778000 with a TOC in its last 256
 * bytes, two FIFOs ("ap2r" TX, "r2ap" RX) and IPC bit 0 in the APCS
 * register. Bergamota votes these rails at the start of its WLAN stage;
 * without them the modem's loader fills its buffer and blocks when it
 * starts writing the firmware to the (dead) WCN3990 (0747/0750 stall at
 * ~380 KB).
 */
#define RPM_MSGRAM_PA     0x778000u
#define RPM_MSGRAM_SIZE   0x7000u
#define RPM_TOC_OFF       (RPM_MSGRAM_SIZE - 256u)
#define RPM_TOC_MAGIC     0x67727430u   /* "grt0" */
#define RPM_FIFO_AP2R     0x61703272u   /* "ap2r" */
#define RPM_FIFO_R2AP     0x72326170u   /* "r2ap" */
#define RPM_IPC_BIT       0u
#define RPM_LCID          1u
#define RPM_CHAN          "rpm_requests"
#define RPM_RSC_LDOA      0x616f646cu   /* "ldoa" */
#define RPM_KEY_SWEN      0x6e657773u   /* "swen" */
#define RPM_KEY_PCEN      0x6e656370u   /* "pcen" */
#define RPM_KEY_PCMD      0x646d6370u   /* "pcmd" */
#define RPM_KEY_UV        0x7675u       /* "uv" */
#define RPM_SET_ACTIVE    0u
#define RPM_SET_SLEEP     1u

static vm_offset_t rpm_va;
static uint32_t rpm_tx_off, rpm_tx_len, rpm_rx_off, rpm_rx_len;
static uint32_t rpm_msg_id;
static boolean_t rpm_open, rpm_ver;
static boolean_t rpm_rails_done;
static boolean_t rpm_kick = TRUE;

static uint32_t
rpm_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(rpm_va + off);
}

static void
rpm_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(rpm_va + off) = val;
	wlan_mb();
}

static uint8_t
rpm_rd8(uint32_t off)
{
	return *(volatile uint8_t *)(rpm_va + off);
}

static void
rpm_wr8(uint32_t off, uint8_t val)
{
	*(volatile uint8_t *)(rpm_va + off) = val;
}

static uint32_t
rpm_tx_avail(void)
{
	uint32_t head = rpm_rd32(rpm_tx_off + 4);
	uint32_t tail = rpm_rd32(rpm_tx_off);

	return tail <= head ? rpm_tx_len - head + tail : tail - head;
}

static void
rpm_tx_put(uint32_t *head, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		rpm_wr8(rpm_tx_off + 8 + *head, p[i]);
		if (++*head >= rpm_tx_len) {
			*head = 0;
		}
	}
}

/* Write one GLINK message into the RPM TX FIFO and kick IPC bit 0. */
static boolean_t
rpm_tx(const uint8_t *hdr, uint32_t hlen, const uint8_t *data, uint32_t dlen)
{
	uint8_t zero = 0;
	uint32_t total = hlen + dlen;
	uint32_t pad = (total + 7) & ~7u;
	uint32_t head;
	unsigned n;

	head = rpm_rd32(rpm_tx_off + 4);
	if (rpm_tx_avail() < pad) {
		for (n = 0; n < 100 && rpm_tx_avail() < pad; n++) {
			wlan_wait(1);
		}
		head = rpm_rd32(rpm_tx_off + 4);
		if (rpm_tx_avail() < pad) {
			printf("SWEET RPM: TX FIFO full (%u need, %u free)\n",
			    pad, rpm_tx_avail());
			return FALSE;
		}
	}
	rpm_tx_put(&head, hdr, hlen);
	if (dlen != 0) {
		rpm_tx_put(&head, data, dlen);
	}
	for (uint32_t i = total; i < pad; i++) {
		rpm_tx_put(&head, &zero, 1);
	}
	rpm_wr32(rpm_tx_off + 4, head);
	if (apcs != 0 && rpm_kick) {
		*(volatile uint32_t *)(apcs + APCS_IPC_OFF) = 1u << RPM_IPC_BIT;
		wlan_mb();
	}
	return TRUE;
}

static void
rpm_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
	uint8_t h[8];

	h[0] = (uint8_t)cmd;
	h[1] = (uint8_t)(cmd >> 8);
	h[2] = (uint8_t)p1;
	h[3] = (uint8_t)(p1 >> 8);
	h[4] = (uint8_t)p2;
	h[5] = (uint8_t)(p2 >> 8);
	h[6] = (uint8_t)(p2 >> 16);
	h[7] = (uint8_t)(p2 >> 24);
	(void)rpm_tx(h, sizeof(h), NULL, 0);
}

static void
rpm_data(const uint8_t *data, uint32_t len)
{
	uint8_t h[16];

	h[0] = 9;                   /* CMD_TX_DATA */
	h[1] = 0;
	h[2] = (uint8_t)RPM_LCID;
	h[3] = 0;
	memset(h + 4, 0, 4);        /* intent id: this edge is intentless */
	h[8] = (uint8_t)len;
	h[9] = (uint8_t)(len >> 8);
	h[10] = (uint8_t)(len >> 16);
	h[11] = (uint8_t)(len >> 24);
	memset(h + 12, 0, 4);       /* left */
	(void)rpm_tx(h, sizeof(h), data, len);
}

static uint32_t
rpm_rx_avail(void)
{
	uint32_t head = rpm_rd32(rpm_rx_off + 4);
	uint32_t tail = rpm_rd32(rpm_rx_off);

	return head >= tail ? head - tail : rpm_rx_len - tail + head;
}

static void
rpm_rx_peek(uint8_t *dst, uint32_t off, uint32_t n)
{
	uint32_t t = rpm_rd32(rpm_rx_off) + off;

	if (t >= rpm_rx_len) {
		t -= rpm_rx_len;
	}
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = rpm_rd8(rpm_rx_off + 8 + t);
		if (++t >= rpm_rx_len) {
			t = 0;
		}
	}
}

static void
rpm_rx_advance(uint32_t n)
{
	uint32_t t = rpm_rd32(rpm_rx_off) + n;

	/* A garbage length must never leave the tail past the ring. */
	while (t >= rpm_rx_len) {
		t -= rpm_rx_len;
	}
	rpm_wr32(rpm_rx_off, t);
}

static void
rpm_poll(void)
{
	uint8_t h[16];

	/* Bounded: a malformed FIFO must not spin the WLAN thread forever. */
	for (unsigned parsed = 0; parsed < 16 && rpm_rx_avail() >= 8; parsed++) {
		uint16_t cmd;
		uint32_t p1, p2;

		rpm_rx_peek(h, 0, 8);
		cmd = (uint16_t)(h[0] | (h[1] << 8));
		p1 = (uint32_t)h[2] | ((uint32_t)h[3] << 8);
		p2 = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
		    ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
		if (cmd == 9 || cmd == 12) {
			uint32_t chunk, left, n;
			uint8_t buf[32];
			uint32_t c;

			if (rpm_rx_avail() < 16) {
				break;
			}
			rpm_rx_peek(h, 0, 16);
			chunk = (uint32_t)h[8] | ((uint32_t)h[9] << 8) |
			    ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
			if (chunk > rpm_rx_len) {
				printf("SWEET RPM: bad chunk %u, drop fifo\n",
				    chunk);
				rpm_rx_advance(rpm_rx_avail());
				break;
			}
			left = (uint32_t)h[12] | ((uint32_t)h[13] << 8) |
			    ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
			n = (16 + chunk + 7) & ~7u;
			if (rpm_rx_avail() < n) {
				break;
			}
			c = MIN(chunk, (uint32_t)sizeof(buf));
			rpm_rx_peek(buf, 16, c);
			printf("SWEET RPM: ack %u bytes (left %u): %02x %02x "
			    "%02x %02x\n", chunk, left, buf[0], buf[1],
			    buf[2], buf[3]);
			rpm_rx_advance(n);
			continue;
		}
		switch (cmd) {
		case 0:         /* VERSION from the RPM */
			rpm_cmd(1, (uint16_t)p1, p2);   /* VERSION_ACK */
			rpm_ver = TRUE;
			printf("SWEET RPM: version %u features %x\n", p1, p2);
			break;
		case 1:         /* VERSION_ACK */
			rpm_ver = TRUE;
			break;
		case 4:         /* OPEN_ACK */
			rpm_open = TRUE;
			printf("SWEET RPM: channel " RPM_CHAN
			    " open (rcid %x lcid %x)\n", p1, p2);
			break;
		case 5: {       /* INTENT (not expected on this edge) */
			uint32_t n = (8 + 8 * p2 + 7) & ~7u;

			if (rpm_rx_avail() < n) {
				return;
			}
			rpm_rx_advance(n);
			continue;
		}
		case 3:
		case 6:
		case 11:
			break;
		default:
			printf("SWEET RPM: cmd %u p1 %x p2 %x\n", cmd, p1, p2);
			break;
		}
		rpm_rx_advance(8);
	}
}

/* One RPM v1 request with u32 KVPs; active or sleep set. */
static boolean_t
rpm_request(uint32_t rsc_id, uint32_t set, const uint32_t *keys,
    const uint32_t *vals, unsigned n)
{
	uint8_t b[16 + 6 * 12];
	uint32_t o = 16, data_len, req_len, hdr, details, id;

	for (unsigned i = 0; i < n; i++) {
		uint32_t four = 4;

		memcpy(b + o, &keys[i], 4);
		memcpy(b + o + 4, &four, 4);           /* nbytes, 4-aligned */
		memcpy(b + o + 8, &vals[i], 4);
		o += 12;
	}
	data_len = o - 16;
	req_len = data_len + 12;
	hdr = (req_len & 0xffffu) | (1u << 24);        /* version 1 */
	details = (data_len & 0xffffu) |
	    ((rsc_id & 0xfffu) << 16) | ((set & 0xfu) << 28);
	id = ++rpm_msg_id;
	memcpy(b, &hdr, 4);
	memcpy(b + 4, &id, 4);
	{
		uint32_t type = RPM_RSC_LDOA;

		memcpy(b + 8, &type, 4);
	}
	memcpy(b + 12, &details, 4);
	printf("SWEET RPM: ldoa%u set %u: %u kvp(s)\n", rsc_id, set, n);
	return rpm_data(b, o), TRUE;
}

static void
wlan_rails_on(void)
{
	uint32_t magic, count, i;

	if (rpm_rails_done) {
		return;
	}
	rpm_rails_done = TRUE;
	rpm_va = ml_io_map_wcomb(RPM_MSGRAM_PA, RPM_MSGRAM_SIZE);
	if (rpm_va == 0) {
		printf("SWEET RPM: msgram map failed\n");
		return;
	}
	magic = rpm_rd32(RPM_TOC_OFF);
	count = rpm_rd32(RPM_TOC_OFF + 4);
	printf("SWEET RPM: toc magic %x count %u\n", magic, count);
	if (magic != RPM_TOC_MAGIC || count > 20) {
		printf("SWEET RPM: bad TOC\n");
		return;
	}
	for (i = 0; i < count; i++) {
		uint32_t id = rpm_rd32(RPM_TOC_OFF + 8 + i * 12);
		uint32_t off = rpm_rd32(RPM_TOC_OFF + 12 + i * 12);
		uint32_t sz = rpm_rd32(RPM_TOC_OFF + 16 + i * 12);

		if (off + 8 + sz > RPM_MSGRAM_SIZE) {
			continue;
		}
		if (id == RPM_FIFO_AP2R) {
			rpm_tx_off = off;
			rpm_tx_len = sz;
		} else if (id == RPM_FIFO_R2AP) {
			rpm_rx_off = off;
			rpm_rx_len = sz;
		}
	}
	printf("SWEET RPM: tx %x+%x rx %x+%x\n", rpm_tx_off, rpm_tx_len,
	    rpm_rx_off, rpm_rx_len);
	if (rpm_tx_len == 0 || rpm_rx_len == 0) {
		printf("SWEET RPM: FIFOs not found\n");
		return;
	}
	/*
	 * 0760: the whole RPM sequence now runs after the modem auth (the
	 * pre-auth kick panicked, 0759). VERSION, then a short wait so any
	 * asynchronous platform error from the kick lands before we touch the
	 * RX descriptor, then poll for the RPM's VERSION/ACK, then OPEN and
	 * wait for OPEN_ACK.
	 */
	printf("SWEET RPM: send version\n");
	rpm_cmd(0, 0, 0);              /* VERSION with the IPC kick */
	wlan_wait(50);
	printf("SWEET RPM: version sent, rx %x %x\n",
	    rpm_rd32(rpm_rx_off), rpm_rd32(rpm_rx_off + 4));
	for (i = 0; i < 200 && !rpm_ver; i++) {
		rpm_poll();
		if ((i % 40) == 0) {
			printf("SWEET RPM: ver wait %u avail %u\n", i,
			    rpm_rx_avail());
		}
		wlan_wait(1);
	}
	printf("SWEET RPM: ver done ver %u open %u avail %u\n", rpm_ver,
	    rpm_open, rpm_rx_avail());
	if (!rpm_ver) {
		printf("SWEET RPM: no VERSION from the RPM\n");
		return;
	}
	{
		uint8_t open[32];
		uint32_t name_len = sizeof(RPM_CHAN);
		uint32_t olen = (8 + name_len + 7) & ~7u;

		memset(open, 0, sizeof(open));
		open[0] = 2;           /* CMD_OPEN */
		open[2] = (uint8_t)RPM_LCID;
		open[4] = (uint8_t)name_len;
		memcpy(open + 8, RPM_CHAN, name_len);
		printf("SWEET RPM: open %s (%u bytes)\n", RPM_CHAN, olen);
		(void)rpm_tx(open, olen, NULL, 0);
	}
	wlan_wait(20);
	for (i = 0; i < 300 && !rpm_open; i++) {
		rpm_poll();
		if ((i % 50) == 0) {
			printf("SWEET RPM: open wait %u avail %u\n", i,
			    rpm_rx_avail());
		}
		wlan_wait(1);
	}
	if (!rpm_open) {
		printf("SWEET RPM: no OPEN_ACK (rx head %x tail %x avail %u)\n",
		    rpm_rd32(rpm_rx_off + 4), rpm_rd32(rpm_rx_off),
		    rpm_rx_avail());
		return;
	}
	/*
	 * ath10k's vreg order for this node: 0.8 (swen), 1.8 (pcen), 1.3
	 * (pcen), 3.3 (pcen). Values are the min of vdd-*-config from the
	 * icnss node; pcmd 2 is qcom,init-pin-ctrl-mode.
	 */
	{
		uint32_t k2[3] = { RPM_KEY_PCEN, RPM_KEY_PCMD, RPM_KEY_UV };
		uint32_t k1[2] = { RPM_KEY_SWEN, RPM_KEY_UV };

		{
			uint32_t v[2] = { 1, 848000 };

			(void)rpm_request(5, RPM_SET_ACTIVE, k1, v, 2);
		}
		{
			uint32_t v[3] = { 2, 2, 1750000 };

			(void)rpm_request(9, RPM_SET_ACTIVE, k2, v, 3);
		}
		{
			uint32_t v[3] = { 2, 2, 1200000 };

			(void)rpm_request(6, RPM_SET_ACTIVE, k2, v, 3);
		}
		{
			uint32_t v[3] = { 2, 2, 3200000 };

			(void)rpm_request(19, RPM_SET_ACTIVE, k2, v, 3);
		}
	}
	for (i = 0; i < 100; i++) {
		rpm_poll();
		wlan_wait(1);
	}
	printf("SWEET RPM: rails voted\n");
}

/* Called by the modem thread after AUTH_COMPLETE, only for sweetwlan>=2. */
void
lavender_wlan_after_modem(void)
{
	struct smem_part_hdr *part = NULL;
	uint32_t i, size = 0;

	if (wlan_mode < 2) {
		return;
	}
	printf("SWEET WLAN: F2 SMEM/GLINK probe after modem auth\n");

	smem = ml_io_map_wcomb(SMEM_PA, SMEM_F2_SIZE);
	if (smem == 0) {
		printf("SWEET WLAN: SMEM 2 MiB map failed\n");
		return;
	}
	apcs = ml_io_map(APCS_PA, APCS_SIZE);
	if (apcs == 0) {
		printf("SWEET WLAN: APCS map failed\n");
	}

	/* Power the WCN3990 before the modem's loader needs it. */
	wlan_rails_on();

	wlan_f2_toc((uint8_t *)smem);

	/*
	 * The modem SMEM items (SMP2P 428/435, GLINK 478-480) live in the
	 * APPS<->MODEM partition, not in a GLOBAL one (0712 showed only host
	 * pairs 0/1, 0/2, 0/5, 1/2 and ffff/ffff in this TOC).
	 */
	for (i = 0; i < 20; i++) {
		part = wlan_f2_partition((uint8_t *)smem, SMEM_HOST_APPS,
		    SMEM_HOST_MODEM);
		if (part != NULL && wlan_f2_item(part, ITEM_SMP2P_IN, &size)) {
			break;
		}
		wlan_wait(500);
	}
	if (part == NULL) {
		printf("SWEET WLAN: no APPS/MODEM SMEM partition\n");
		return;
	}
	printf("SWEET WLAN: APPS/MODEM partition off %05x after %u ms\n",
	    (uint32_t)((uint8_t *)part - (uint8_t *)smem), i * 500);
	wlan_f2_items(part);
	wlan_f2_items_cached(part);
	wlan_f2_smp2p(part, ITEM_SMP2P_OUT, "out");
	wlan_f2_smp2p(part, ITEM_SMP2P_IN, "in");
	if (apcs != 0) {
		uint32_t v = wlan_rd(apcs, APCS_IPC_OFF);

		printf("SWEET WLAN: APCS IPC %08x (bit15 modem %u, bit9 adsp "
		    "%u)\n", v, (v >> 15) & 1, (v >> 9) & 1);
	}
	if (wlan_mode < 3) {
		printf("SWEET WLAN: F2 probe done (read only)\n");
		return;
	}

	/*
	 * Android's AP creates the SMP2P out item (428) and answers the modem's
	 * IPA power query; only then does the modem publish slave-kernel READY
	 * (bit 1) in item 435.
	 */
	tcsr = ml_io_map(TCSR_PA, TCSR_SIZE);
	if (tcsr == 0) {
		printf("SWEET WLAN: TCSR hwlock map failed\n");
		return;
	}
	{
		struct smp2p_hdr *in = NULL;
		struct smp2p_hdr *out;
		volatile uint32_t *sk = NULL, *ipa_in = NULL, *ipa_out = NULL;
		uint32_t sk_last = ~0u, ipa_last = ~0u;
		uint32_t i2, osz = sizeof(struct smp2p_hdr) +
		    SMP2P_MAX_ENTRY * sizeof(struct smp2p_entry);
		boolean_t ready = FALSE;

		printf("SWEET WLAN: creating SMP2P out item %u\n",
		    ITEM_SMP2P_OUT);
		out = (struct smp2p_hdr *)wlan_smem_alloc(part, ITEM_SMP2P_OUT,
		    osz);
		if (out == NULL) {
			printf("SWEET WLAN: SMP2P out allocation failed\n");
			return;
		}
		if (out->version == 0) {
			out->magic = SMP2P_MAGIC;
			out->local_pid = 0;
			out->remote_pid = 1;
			out->total = SMP2P_MAX_ENTRY;
			out->valid = 0;
			out->features[0] = SMP2P_SSR_ACK;
			__asm__ volatile("dsb sy" ::: "memory");
			out->version = 1;
			wlan_smp2p_kick();
			printf("SWEET WLAN: SMP2P out created\n");
		} else {
			printf("SWEET WLAN: SMP2P out exists v%u valid %u\n",
			    out->version, out->valid);
		}
		ipa_out = wlan_smp2p_add(out, "ipa", 0);
		printf("SWEET WLAN: SMP2P out ipa entry %s\n", ipa_out != NULL ?
		    "ready" : "missing");

		for (i2 = 0; i2 < 60000; i2++) {
			if (in == NULL) {
				uint32_t sz = 0;

				in = (struct smp2p_hdr *)wlan_f2_item(part,
				    ITEM_SMP2P_IN, &sz);
				if (in != NULL && in->magic != SMP2P_MAGIC) {
					in = NULL;
				}
				if (in != NULL) {
					printf("SWEET WLAN: t=%u ms SMP2P in "
					    "v%u valid %u\n", i2, in->version,
					    in->valid);
				}
			}
			if (in != NULL && sk == NULL) {
				sk = wlan_smp2p_find(in, "slave-kernel");
			}
			if (in != NULL && ipa_in == NULL) {
				ipa_in = wlan_smp2p_find(in, "ipa");
			}
			if (sk != NULL && *sk != sk_last) {
				sk_last = *sk;
				printf("SWEET WLAN: t=%u ms slave-kernel %08x"
				    "%s%s%s\n", i2, sk_last,
				    (sk_last & 1) ? " FATAL" : "",
				    (sk_last & 2) ? " READY" : "",
				    (sk_last & 4) ? " HANDOVER" : "");
				if ((sk_last & 2) && !ready) {
					ready = TRUE;
					printf("SWEET WLAN: modem READY\n");
				}
				if (sk_last & 1) {
					break;
				}
			}
			if (ipa_in != NULL && ipa_out != NULL &&
			    *ipa_in != ipa_last) {
				uint32_t v = *ipa_in;

				if ((v & 1) && (ipa_last == ~0u ||
				    !(ipa_last & 1))) {
					*ipa_out = 1;
					wlan_smp2p_kick();
					printf("SWEET WLAN: t=%u ms answered IPA "
					    "query %08x\n", i2, v);
				}
				ipa_last = v;
			}
			if (ready) {
				break;
			}
			wlan_wait(1);
		}
		printf("SWEET WLAN: SMP2P watch done, slave-kernel %08x %s\n",
		    sk_last, ready ? "READY" : "not ready");
		if (ready) {
			if (wlan_glink_start(part)) {
				for (i2 = 0; i2 < 240000; i2++) {
					wlan_glink_poll(i2);
					if (q_wlfw_seen) {
						break;
					}
					wlan_wait(1);
				}
				printf("SWEET WLAN: glink summary rx %u tx %u "
				    "chan %s intents %u, qrtr node %x hello %u "
				    "wlfw %u\n", g_rx_msgs, g_tx_msgs,
				    wlan_chan_up() ? "UP" : "down", g_ri_count,
				    q_modem_node, q_hello_rx, q_wlfw_seen);
			}
		}
	}
	printf("SWEET WLAN: F2 probe done\n");
}

static void
lavender_wlan_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t delay_s = 25;

	(void)PE_parse_boot_argn("sweetwlandelay", &delay_s, sizeof(delay_s));
	if (delay_s > 300) {
		delay_s = 300;
	}
	if (wlan_mode >= 2) {
		/*
		 * 0760: the RPM/AOSS does not tolerate the IPC kick before the
		 * modem authenticates (0759 panicked at t=75 with an
		 * implementation-specific platform error; 0758 kicked at t=133,
		 * after auth, and was fine). All RPM work now happens in
		 * lavender_wlan_after_modem(), right after AUTH_COMPLETE.
		 */
		printf("SWEET WLAN: F2 probe waits for the modem auth\n");
		return;
	}
	printf("SWEET WLAN: F0b platform probe in %u s\n", delay_s);
	wlan_wait(delay_s * 1000);

	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		printf("SWEET WLAN: GCC map failed\n");
		return;
	}
	wlan_gcc();

	smmu = ml_io_map(ANOC2_PA, ANOC2_SIZE);
	if (smmu == 0) {
		printf("SWEET WLAN: ANOC2 SMMU map failed\n");
	} else {
		wlan_smmu();
	}

	smem = ml_io_map(SMEM_PA, SMEM_SIZE);
	if (smem == 0) {
		printf("SWEET WLAN: SMEM map failed\n");
	} else {
		wlan_announce("dump the SMEM header");
		wlan_region("SMEM", smem, SMEM_SIZE);
	}

	msa = ml_io_map(MSA_PA, MSA_SIZE);
	if (msa == 0) {
		printf("SWEET WLAN: MSA map failed\n");
	} else {
		wlan_announce("dump the WLAN MSA carve-out");
		wlan_region("MSA", msa, MSA_SIZE);
	}
	printf("SWEET WLAN: F0b probe done\n");
}

int
lavender_wlan_probe(void)
{
	static int started;
	uint32_t on = 0;
	thread_t thread;

	if (started ||
	    !PE_parse_boot_argn("sweetwlan", &on, sizeof(on)) || on == 0) {
		return 0;
	}
	started = 1;
	wlan_mode = on;
	if (kernel_thread_start(lavender_wlan_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_LAVENDER */

int lavender_wlan_probe(void);
void lavender_wlan_after_modem(void);
int
lavender_wlan_probe(void)
{
	return 0;
}

void
lavender_wlan_after_modem(void)
{
}

#endif
