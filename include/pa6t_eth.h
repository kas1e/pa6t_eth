#ifndef PA6T_ETH_H
#define PA6T_ETH_H

/*
 * pa6t_eth.h  -- Driver internal definitions for pa6t_eth.device.
 *
 * AmigaOS 4 SANA-II network driver for the PASemi GMAC (1G Ethernet)
 * in the AmigaOne X1000 (PA6T-1682M SoC).
 */

#include <exec/devices.h>
#include <exec/emulation.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <interfaces/utility.h>
#include <utility/utility.h>
#include <devices/sana2.h>

#include "version.h"
#include "pa6t_eth_regs.h"

/* ---- Ethernet constants --------------------------------------------------- */

#define ETH_ADDR_LEN    6
#define ETH_HDR_LEN     14
#define ETH_MAX_FRAME   1514         /* max Ethernet payload (without FCS) */
#define ETH_MAX_FRAME_PADDED (ETH_MAX_FRAME + 4)  /* +4 for FCS headroom = 1518 */

/* RX DMA buffer size  -- must be a multiple of 16 (DMA engine requirement).
 * 2048 provides ample headroom for max-size Ethernet frames. */
#define RX_BUF_SIZE     2048

/* ---- Ring sizes (must be powers of two) ----------------------------------- */

#define TX_RING_SIZE        256      /* 256 x 8-byte u64 descriptors = 2 KB  */
#define RX_RING_SIZE        2048     /* 2048 x 8-byte u64 descriptors = 16 KB */
#define RX_BUF_RING_SIZE    RX_RING_SIZE  /* must equal RX_RING_SIZE (paired 1:1) */
#define NUM_RX_BUFS         512      /* pre-allocated receive data buffers */

/* Macros to index rings with wrap-around */
#define TX_DESC(ring, n) ((ring)[(n) & (TX_RING_SIZE - 1)])
#define RX_DESC(ring, n) ((ring)[(n) & (RX_RING_SIZE - 1)])
#define RX_BUFF(ring, n) ((ring)[(n) & (RX_BUF_RING_SIZE - 1)])

/* ---- byte_copy  -- volatile-safe loop (no libc memcpy) -------------------- */
static inline void byte_copy(void *dst, const void *src, uint32 len)
{
    volatile uint8 *d = (volatile uint8 *)dst;
    const volatile uint8 *s = (const volatile uint8 *)src;
    uint32 i;
    for (i = 0; i < len; i++)
        d[i] = s[i];
}

/* ---- Per-opener state ----------------------------------------------------- */

/* One Opener per OpenDevice().  s2hook is set later by S2_SANA2HOOK. */
struct Opener
{
    struct MinNode      node;
    APTR                bm_cookie;
    struct Sana2Hook   *s2hook;
};

/* Wrapper for a queued CMD_READ.  Uses a private node to avoid
 * conflicting with the caller's own list linkage of the ioreq. */
struct PendingRead
{
    struct MinNode     node;   /* driver-owned list node */
    struct IOSana2Req *ioreq;  /* the queued SANA-II request */
};

/* ---- Per-unit state ------------------------------------------------------- */

struct PA6TEthUnit
{
    struct Unit     unit_Base;
    uint32          open_count;
    BOOL            online;

    /* Unit task */
    struct Task    *task;
    struct MsgPort *io_port;
    uint32          io_port_mask;
    BOOL            task_shutdown;

    /* Signal fired by interrupt handler when RX completions arrive */
    struct Task    *irq_wait_task;
    uint32          irq_signal_mask;

    /* Pending CMD_READ lists  -- one per EtherType */
    struct List     read_list_ipv4;    /* EtherType 0x0800  -- IPv4 */
    struct List     read_list_arp;   /* EtherType 0x0806  -- ARP */
    struct List     read_list_other; /* all other EtherTypes */

    /* Pending S2_ONEVENT list (and S2_READORPHAN) */
    struct List     event_list;

    /* Opener list */
    struct List              opener_list;
    struct SignalSemaphore   opener_lock;

    /* TX ring  -- written by driver, DMA read by hardware */
    volatile uint64 *tx_ring;          /* virtual address */
    uint32           tx_ring_phys;     /* physical address */
    uint32           tx_ring_dma_entries;
    uint32           tx_next_fill;     /* next ring slot to write */
    uint32           tx_next_clean;    /* next ring slot to reclaim */

    /* RX descriptor ring  -- written by hardware when packet arrives */
    volatile uint64 *rx_ring;
    uint32           rx_ring_phys;
    uint32           rx_ring_dma_entries;
    uint32           rx_next_clean;    /* next ring slot to check */

    /* RX buffer pointer ring  -- written by driver to post buffers */
    volatile uint64 *rx_buf_ring;      /* XCT_RXB entries */
    uint32           rx_buf_ring_phys;
    uint32           rx_buf_ring_dma_entries;

    /* Next slot to post a buffer into (hardware reads sequentially) */
    uint32           rx_buf_write_ptr;

    /* Slot-to-buffer mapping: rx_slot_buf[slot] = index into rx_bufs[] */
    uint16           rx_slot_buf[RX_RING_SIZE];

    /* Pre-allocated RX data buffers */
    uint8           *rx_bufs[NUM_RX_BUFS];
    uint32           rx_bufs_phys[NUM_RX_BUFS];
    uint32           rx_bufs_dma_entries[NUM_RX_BUFS];

    /* Fire-and-forget TX: pending buffer is cleaned up on next TX call */
    uint8           *tx_buf_pending;
    uint32           tx_buf_pending_len;
    uint32           tx_fill_pending;   /* ring slot whose O bit we wait on */

    /* Statistics */
    uint32           stat_rx;
    uint32           stat_tx;
    uint32           stat_rx_dropped;
    uint32           stat_tx_errors;

    /* Diagnostic counter  -- incremented each unit task loop iteration */
    uint32           loop_count;
};

/* ---- Device library base -------------------------------------------------- */

struct PA6TEthBase
{
    struct Device   dev_Base;
    struct ExecIFace *IExec;

    /* Libraries */
    struct Library        *ExpansionBase;
    struct PCIIFace       *IPCI;
    struct Library        *UtilityBase;
    struct UtilityIFace   *IUtility;

    /* PCI devices */
    struct PCIDevice  *mac_dev;       /* GMAC: 0x1959:0xa005 */
    struct PCIDevice  *dma_dev;       /* DMA:  0x1959:0xa007 */
    struct PCIDevice  *iob_dev;       /* IOB:  0x1959:0xa001 */

    /* DMA interface number for this MAC (from mac_to_intf lookup) */
    int32             dma_if;         /* -1 = not found */

    /* Allocated DMA channel numbers */
    int32             tx_channel_num;        /* TX channel (0-based) */
    int32             rx_channel_num;        /* RX channel (0-based) */

    /* Hardware MAC address */
    uint8             mac_addr[ETH_ADDR_LEN];

    BPTR              dev_SegList;

    /* The one and only unit */
    struct PA6TEthUnit *unit;

    /* Per-channel DMA status area at phys 0xfd800000 (big-endian native).
     * Layout: rx_sta[64] + tx_sta[20], each 8 bytes. */
    volatile uint64         *pasdma_status;

    /* PCI interrupt (on DMA per-channel vector) */
    struct Interrupt  irq_handler;
    uint32            irq_number;
    BOOL              irq_installed;

    /* IRQ handler entry/exit counters (diagnostic) */
    volatile ULONG    irq_enter;     /* incremented at handler entry */
    volatile ULONG    irq_exit;      /* incremented at handler exit (claimed) */
    volatile ULONG    irq_miss;      /* incremented when not our interrupt */

    /* TX semaphore (protects ring fill pointer) */
    struct SignalSemaphore tx_lock;
};

/* ---- Register access  (ReadConfigLong / WriteConfigLong) ------------------ */
static inline uint32 read_mac_reg(struct PA6TEthBase *b, uint32 offset)
{
    return b->mac_dev->ReadConfigLong(offset);
}

static inline void write_mac_reg(struct PA6TEthBase *b, uint32 offset, uint32 val)
{
    b->mac_dev->WriteConfigLong(offset, val);
}

static inline uint32 read_dma_reg(struct PA6TEthBase *b, uint32 offset)
{
    return b->dma_dev->ReadConfigLong(offset);
}

static inline void write_dma_reg(struct PA6TEthBase *b, uint32 offset, uint32 val)
{
    b->dma_dev->WriteConfigLong(offset, val);
}

/*
 * IOB MMIO helpers  -- byte-swapped access (lwbrx/stwbrx).
 * PCI config space is little-endian; CPU is big-endian.
 */
static inline uint32 read_iob_mmio(struct PA6TEthBase *b, uint32 offset)
{
    uint32 val;
    volatile uint32 *addr = (volatile uint32 *)(PA6T_IOB_ECAM + offset);
    __asm__ volatile ("lwbrx %0, 0, %1" : "=r"(val) : "r"(addr) : "memory");
    (void)b;
    return val;
}

static inline void write_iob_mmio(struct PA6TEthBase *b, uint32 offset, uint32 val)
{
    volatile uint32 *addr = (volatile uint32 *)(PA6T_IOB_ECAM + offset);
    __asm__ volatile ("stwbrx %0, 0, %1; eieio" : : "r"(val), "r"(addr) : "memory");
    (void)b;
}

/* ---- Function prototypes -------------------------------------------------- */

struct Library *_manager_Init(struct Library *library, BPTR seglist,
                              struct Interface *exec);

struct PA6TEthBase *_manager_Open(struct DeviceManagerInterface *Self,
                                  struct IOSana2Req *ioreq,
                                  ULONG unitNum, ULONG flags);

BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
BPTR _manager_Close(struct DeviceManagerInterface *Self,
                    struct IOSana2Req *ioreq);

void _manager_BeginIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq);
LONG _manager_AbortIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq);

uint32 _manager_Obtain(struct DeviceManagerInterface *Self);
uint32 _manager_Release(struct DeviceManagerInterface *Self);

/* HW layer */
BOOL PA6TEth_FindDevices(struct PA6TEthBase *base);
BOOL PA6TEth_HWInit(struct PA6TEthBase *base);
void PA6TEth_HWCleanup(struct PA6TEthBase *base);

BOOL PA6TEth_InstallIRQ(struct PA6TEthBase *base);
void PA6TEth_RemoveIRQ(struct PA6TEthBase *base);

void PA6TEth_PostRxBuffers(struct PA6TEthBase *base, struct PA6TEthUnit *unit);
void PA6TEth_ProcessRX(struct PA6TEthBase *base, struct PA6TEthUnit *unit);
void PA6TEth_TransmitPacket(struct PA6TEthBase *base, struct PA6TEthUnit *unit,
                            struct IOSana2Req *ioreq);
void PA6TEth_FlushReadQueue(struct PA6TEthBase *base, struct PA6TEthUnit *unit,
                            BYTE error);
void PA6TEth_FlushAllPending(struct PA6TEthBase *base, struct PA6TEthUnit *unit);

/* Unit task */
BOOL UnitTask_Start(struct PA6TEthBase *base, struct PA6TEthUnit *unit);
void UnitTask_Shutdown(struct PA6TEthBase *base, struct PA6TEthUnit *unit);

/* Opener helpers */
struct Opener *PA6TEth_FindOpener(struct PA6TEthUnit *unit, APTR bm_cookie);

/* ---- Debug macros (DEBUG=1: init/config, DEBUG=2: per-packet verbose) ----- */
#if defined(DEBUG) && DEBUG >= 1
# define DPRINTF(iexec, ...)   ((iexec)->DebugPrintF(__VA_ARGS__))
#else
# define DPRINTF(iexec, ...)   ((void)0)
#endif

#if defined(DEBUG) && DEBUG >= 2
# define DPRINTFV(iexec, ...)  ((iexec)->DebugPrintF(__VA_ARGS__))
#else
# define DPRINTFV(iexec, ...)  ((void)0)
#endif

#endif /* PA6T_ETH_H */
