#ifndef PA6T_ETH_REGS_H
#define PA6T_ETH_REGS_H

/*
 * pa6t_eth_regs.h  -- Hardware register definitions for the PASemi GMAC
 * (PA6T-1682M SoC) used in the AmigaOne X1000.
 *
 * Register access inline functions (read_mac_reg, write_mac_reg, etc.) are
 * in pa6t_eth.h (they reference PA6TEthBase fields).
 */

#include <exec/types.h>

/* ---- PCI IDs -------------------------------------------------------------- */

#define PASEMI_VENDOR_ID    0x1959
#define PASEMI_DEV_GMAC     0xa005   /* 1G GMAC -- used in X1000 */
#define PASEMI_DEV_XAUI     0xa006   /* 10G XAUI */
#define PASEMI_DEV_DMA      0xa007   /* DMA controller */
#define PASEMI_DEV_IOB      0xa001   /* I/O Bridge */

/* ---- Descriptor format bit fields (from pasemi_dma.h) --------------------- */

/* TX descriptor (MAC transmit command) */
#define XCT_MACTX_O        0x0400000000000000ULL  /* owns descriptor */
#define XCT_MACTX_ST       0x4000000000000000ULL  /* start of frame */
#define XCT_MACTX_CRC_PAD  0x0040000000000000ULL  /* CRC insert + pad */
#define XCT_MACTX_LLEN_S   32ULL
#define XCT_MACTX_LLEN_M   0x00007fff00000000ULL
#define XCT_MACTX_LLEN(x)  ((((uint64)(x)) << XCT_MACTX_LLEN_S) & XCT_MACTX_LLEN_M)

/* TX PTR descriptor */
#define XCT_PTR_T           0x8000000000000000ULL  /* last pointer */
#define XCT_PTR_LEN_S       44ULL
#define XCT_PTR_LEN_M       0x7ffff00000000000ULL
#define XCT_PTR_LEN(x)      ((((uint64)(x)) << XCT_PTR_LEN_S) & XCT_PTR_LEN_M)
#define XCT_PTR_ADDR_M      0x00000fffffffffffULL
#define XCT_PTR_ADDR_S      0ULL
#define XCT_PTR_ADDR(x)     ((((uint64)(x)) << XCT_PTR_ADDR_S) & XCT_PTR_ADDR_M)

/* RX descriptor (MAC receive result)  -- written by hardware */
#define XCT_MACRX_ST        0x4000000000000000ULL  /* start-of-frame (always set) */
#define XCT_MACRX_RR_8BRES  0x1000000000000000ULL  /* 8-byte result word present */
#define XCT_MACRX_O         0x0400000000000000ULL  /* owned by driver (completion ready) */
#define XCT_MACRX_E         0x0200000000000000ULL  /* generic error (diagnostic only, not fatal) */
#define XCT_MACRX_FF        0x0100000000000000ULL  /* flow/FIFO-full or pause frame */
#define XCT_MACRX_NB_M      0x000fc00000000000ULL  /* number of buffers consumed */
#define XCT_MACRX_NB_S      46ULL
#define XCT_MACRX_LLEN_M    0x00003fff00000000ULL  /* frame length (incl. FCS) */
#define XCT_MACRX_LLEN_S    32ULL
#define XCT_MACRX_CRC       0x0000000080000000ULL  /* CRC error  -- drop the frame */
#define XCT_MACRX_LEN_M     0x0000000060000000ULL  /* length error type */
#define XCT_MACRX_CAST_M    0x0000000018000000ULL
#define XCT_MACRX_CAST_UNI  0x0000000000000000ULL
#define XCT_MACRX_CAST_MULTI 0x0000000008000000ULL
#define XCT_MACRX_CAST_BROAD 0x0000000010000000ULL

/* RX eval result (second descriptor word  -- 8-byte result) */
#define XCT_RXRES_8B_EVAL_M  0x000000ffff000000ULL
#define XCT_RXRES_8B_EVAL_S  24ULL

/* RX buffer descriptor (posted by driver to hardware) */
#define XCT_RXB_LEN_S       44ULL
#define XCT_RXB_LEN_M       0x0ffff00000000000ULL
#define XCT_RXB_LEN(x)      ((((uint64)(x)) << XCT_RXB_LEN_S) & XCT_RXB_LEN_M)
#define XCT_RXB_ADDR_M      0x00000fffffffffffULL
#define XCT_RXB_ADDR_S      0ULL
#define XCT_RXB_ADDR(x)     ((((uint64)(x)) << XCT_RXB_ADDR_S) & XCT_RXB_ADDR_M)

/* ---- MAC config-space register offsets (pasemi_mac.h) -------------------- */

#define PAS_MAC_CFG_PCFG        0x80            /* Port config */
#define PAS_MAC_CFG_MACCFG      0x84            /* MAC config  */
#define PAS_MAC_CFG_ADR0        0x8c            /* MAC address bytes 2-5 */
#define PAS_MAC_CFG_ADR1        0x90            /* MAC address bytes 0-1 (bits 15:0) */
#define PAS_MAC_CFG_TXP         0x98            /* TX parameters */
#define PAS_MAC_CFG_RMON(r)     (0x100 + (r)*4) /* RMON counters */
#define PAS_MAC_IPC_CHNL        0x208           /* DMA channel assignment */

/* PCFG bits */
#define PAS_MAC_CFG_PCFG_PE       0x80000000  /* port enable */
#define PAS_MAC_CFG_PCFG_CE       0x40000000  /* checksum enable */
#define PAS_MAC_CFG_PCFG_TSR_M    0x0c000000  /* TSR speed mask */
#define PAS_MAC_CFG_PCFG_TSR_100M 0x04000000  /* TSR = 100 Mbps */
#define PAS_MAC_CFG_PCFG_TSR_1G   0x08000000  /* TSR = 1 Gbps */
#define PAS_MAC_CFG_PCFG_PR       0x01000000  /* promiscuous */
#define PAS_MAC_CFG_PCFG_S1       0x00000080  /* start */
#define PAS_MAC_CFG_PCFG_HD       0x00000004  /* half duplex */
#define PAS_MAC_CFG_PCFG_SPD_M    0x00000003  /* speed field mask */
#define PAS_MAC_CFG_PCFG_SPD_100M 0x00000001  /* speed = 100 Mbps */
#define PAS_MAC_CFG_PCFG_SPD_1G   0x00000002  /* speed = 1 Gbps */

/* MACCFG bits */
#define PAS_MAC_CFG_MACCFG_MAXF_M   0x00ffff00
#define PAS_MAC_CFG_MACCFG_MAXF_S   8
#define PAS_MAC_CFG_MACCFG_MAXF(x)  (((x) << PAS_MAC_CFG_MACCFG_MAXF_S) & PAS_MAC_CFG_MACCFG_MAXF_M)

/* TXP bits */
#define PAS_MAC_CFG_TXP_FCE      0x00800000
#define PAS_MAC_CFG_TXP_FPC_S    20
#define PAS_MAC_CFG_TXP_FPC(x)   (((x) << PAS_MAC_CFG_TXP_FPC_S) & 0x00300000)
#define PAS_MAC_CFG_TXP_SL_S     16
#define PAS_MAC_CFG_TXP_SL(x)    (((x) << PAS_MAC_CFG_TXP_SL_S)  & 0x00030000)
#define PAS_MAC_CFG_TXP_COB_S    12
#define PAS_MAC_CFG_TXP_COB(x)   (((x) << PAS_MAC_CFG_TXP_COB_S) & 0x0000f000)
#define PAS_MAC_CFG_TXP_TIFT_S   8
#define PAS_MAC_CFG_TXP_TIFT(x)  (((x) << PAS_MAC_CFG_TXP_TIFT_S) & 0x00000f00)
#define PAS_MAC_CFG_TXP_TIFG_S   0
#define PAS_MAC_CFG_TXP_TIFG(x)  (((x) << PAS_MAC_CFG_TXP_TIFG_S) & 0x000000ff)

/* IPC_CHNL fields */
#define PAS_MAC_IPC_CHNL_DCHNO_S  16
#define PAS_MAC_IPC_CHNL_DCHNO(x) (((x) << PAS_MAC_IPC_CHNL_DCHNO_S) & 0x003f0000)
#define PAS_MAC_IPC_CHNL_BCH_S    0
#define PAS_MAC_IPC_CHNL_BCH(x)   (((x) << PAS_MAC_IPC_CHNL_BCH_S)  & 0x0000003f)

/* ---- DMA controller config-space register offsets (pasemi_dma.h) ---------- */

#define PAS_DMA_CAP_TXCH       0x44   /* TX channel count */
#define PAS_DMA_CAP_TXCH_TCHN_M  0x00ff0000
#define PAS_DMA_CAP_TXCH_TCHN_S  16
#define PAS_DMA_CAP_RXCH       0x48   /* RX channel count */
#define PAS_DMA_CAP_RXCH_RCHN_M  0x00ff0000
#define PAS_DMA_CAP_RXCH_RCHN_S  16
#define PAS_DMA_CAP_IFI   0x4c   /* Interface info   */

#define PAS_DMA_COM_TXCMD 0x100  /* TX command  */
#define PAS_DMA_COM_TXSTA 0x104  /* TX status   */
#define PAS_DMA_COM_RXCMD 0x108  /* RX command  */
#define PAS_DMA_COM_RXSTA 0x10c  /* RX status   */
#define PAS_DMA_COM_CFG   0x114  /* Common cfg  */
#define PAS_DMA_TXF_SFLG0 0x140  /* TX set flags 0   */
#define PAS_DMA_TXF_SFLG1 0x144  /* TX set flags 1   */
#define PAS_DMA_TXF_CFLG0 0x148  /* TX clear flags 0 */
#define PAS_DMA_TXF_CFLG1 0x14c  /* TX clear flags 1 */

#define PAS_DMA_COM_TXCMD_EN  0x00000001
#define PAS_DMA_COM_TXSTA_ACT 0x00000001
#define PAS_DMA_COM_RXCMD_EN  0x00000001
#define PAS_DMA_COM_RXSTA_ACT 0x00000001

#define PAS_DMA_CAP_IFI_NIN_M  0x00ff0000
#define PAS_DMA_CAP_IFI_NIN_S  16
#define PAS_DMA_CAP_IFI_IOFF_M 0xff000000
#define PAS_DMA_CAP_IFI_IOFF_S 24

/* RX interface registers */
#define _PAS_DMA_RXINT_STRIDE      0x20
#define PAS_DMA_RXINT_RCMDSTA(i)  (0x200 + (i)*_PAS_DMA_RXINT_STRIDE)
#define PAS_DMA_RXINT_CFG(i)      (0x204 + (i)*_PAS_DMA_RXINT_STRIDE)
#define PAS_DMA_RXINT_INCR(i)     (0x210 + (i)*_PAS_DMA_RXINT_STRIDE)
#define PAS_DMA_RXINT_BASEL(i)    (0x218 + (i)*_PAS_DMA_RXINT_STRIDE)
#define PAS_DMA_RXINT_BASEU(i)    (0x21c + (i)*_PAS_DMA_RXINT_STRIDE)

#define PAS_DMA_RXINT_RCMDSTA_EN      0x00000001
#define PAS_DMA_RXINT_RCMDSTA_ST      0x00000002
#define PAS_DMA_RXINT_RCMDSTA_MBT     0x00000008  /* interrupt enable: missing buf tag */
#define PAS_DMA_RXINT_RCMDSTA_MDR     0x00000010
#define PAS_DMA_RXINT_RCMDSTA_MOO     0x00000020  /* interrupt enable: missing ordering */
#define PAS_DMA_RXINT_RCMDSTA_MBP     0x00000040  /* interrupt enable: missing buf ptr */
#define PAS_DMA_RXINT_RCMDSTA_DROPS_M 0xfffe0000  /* drop counter (W1C) */
#define PAS_DMA_RXINT_RCMDSTA_ACT     0x00010000
#define PAS_DMA_RXINT_RCMDSTA_BP      0x00004000  /* status: buffer pointer needed (W1C) */
#define PAS_DMA_RXINT_RCMDSTA_OO      0x00002000  /* status: ordering outstanding (W1C) */
#define PAS_DMA_RXINT_RCMDSTA_DR      0x00001000  /* status: descriptor ring missing (W1C) */
#define PAS_DMA_RXINT_RCMDSTA_BT      0x00000800  /* status: buffer tag needed (W1C) */

#define PAS_DMA_RXINT_CFG_RBP      0x80000000
#define PAS_DMA_RXINT_CFG_DHL_S    24
#define PAS_DMA_RXINT_CFG_DHL_M    0x07000000
#define PAS_DMA_RXINT_CFG_DHL(x)   (((x) << PAS_DMA_RXINT_CFG_DHL_S) & PAS_DMA_RXINT_CFG_DHL_M)
#define PAS_DMA_RXINT_CFG_LW       0x00200000
#define PAS_DMA_RXINT_CFG_L2       0x00100000
#define PAS_DMA_RXINT_CFG_HEN      0x00080000

#define PAS_DMA_RXINT_BASEL_BRBL(x) ((x) & ~0x3f)
#define PAS_DMA_RXINT_BASEU_BRBH(x) ((x) & 0xfff)
#define PAS_DMA_RXINT_BASEU_SIZ_S   16
#define PAS_DMA_RXINT_BASEU_SIZ(x)  (((x) << PAS_DMA_RXINT_BASEU_SIZ_S) & 0x3fff0000)

/* TX channel registers */
#define _PAS_DMA_TXCHAN_STRIDE     0x20
#define PAS_DMA_TXCHAN_TCMDSTA(c) (0x300 + (c)*_PAS_DMA_TXCHAN_STRIDE)
#define PAS_DMA_TXCHAN_CFG(c)     (0x304 + (c)*_PAS_DMA_TXCHAN_STRIDE)
#define PAS_DMA_TXCHAN_INCR(c)    (0x310 + (c)*_PAS_DMA_TXCHAN_STRIDE)
#define PAS_DMA_TXCHAN_BASEL(c)   (0x318 + (c)*_PAS_DMA_TXCHAN_STRIDE)
#define PAS_DMA_TXCHAN_BASEU(c)   (0x31c + (c)*_PAS_DMA_TXCHAN_STRIDE)

#define PAS_DMA_TXCHAN_TCMDSTA_EN  0x00000001
#define PAS_DMA_TXCHAN_TCMDSTA_ST  0x00000002
#define PAS_DMA_TXCHAN_TCMDSTA_ACT 0x00010000
#define PAS_DMA_TXCHAN_TCMDSTA_SZ  0x00000800
#define PAS_DMA_TXCHAN_TCMDSTA_DB  0x00000400
#define PAS_DMA_TXCHAN_TCMDSTA_DE  0x00000200
#define PAS_DMA_TXCHAN_TCMDSTA_DA  0x00000100

#define PAS_DMA_TXCHAN_CFG_TY_IFACE 0x00000000
#define PAS_DMA_TXCHAN_CFG_TATTR_S  2
#define PAS_DMA_TXCHAN_CFG_TATTR_M  0x0000003c
#define PAS_DMA_TXCHAN_CFG_TATTR(x) (((x) << PAS_DMA_TXCHAN_CFG_TATTR_S) & PAS_DMA_TXCHAN_CFG_TATTR_M)
#define PAS_DMA_TXCHAN_CFG_UP       0x00004000
#define PAS_DMA_TXCHAN_CFG_WT_S     6
#define PAS_DMA_TXCHAN_CFG_WT(x)    (((x) << PAS_DMA_TXCHAN_CFG_WT_S) & 0x000003c0)

#define PAS_DMA_TXCHAN_BASEL_BRBL(x) ((x) & ~0x3f)
#define PAS_DMA_TXCHAN_BASEU_BRBH(x) ((x) & 0xfff)
#define PAS_DMA_TXCHAN_BASEU_SIZ_S   16
#define PAS_DMA_TXCHAN_BASEU_SIZ(x)  (((x) << PAS_DMA_TXCHAN_BASEU_SIZ_S) & 0x3fff0000)

/* RX channel registers */
#define _PAS_DMA_RXCHAN_STRIDE     0x20
#define PAS_DMA_RXCHAN_CCMDSTA(c) (0x800 + (c)*_PAS_DMA_RXCHAN_STRIDE)
#define PAS_DMA_RXCHAN_CFG(c)     (0x804 + (c)*_PAS_DMA_RXCHAN_STRIDE)
#define PAS_DMA_RXCHAN_INCR(c)    (0x810 + (c)*_PAS_DMA_RXCHAN_STRIDE)
#define PAS_DMA_RXCHAN_BASEL(c)   (0x818 + (c)*_PAS_DMA_RXCHAN_STRIDE)
#define PAS_DMA_RXCHAN_BASEU(c)   (0x81c + (c)*_PAS_DMA_RXCHAN_STRIDE)

#define PAS_DMA_RXCHAN_CCMDSTA_EN  0x00000001
#define PAS_DMA_RXCHAN_CCMDSTA_ST  0x00000002
#define PAS_DMA_RXCHAN_CCMDSTA_ACT 0x00010000
#define PAS_DMA_RXCHAN_CCMDSTA_DU  0x00020000
#define PAS_DMA_RXCHAN_CCMDSTA_OD  0x00002000
#define PAS_DMA_RXCHAN_CCMDSTA_FD  0x00001000
#define PAS_DMA_RXCHAN_CCMDSTA_DT  0x00000800

#define PAS_DMA_RXCHAN_CFG_HBU_S   7
#define PAS_DMA_RXCHAN_CFG_HBU_M   0x00000380
#define PAS_DMA_RXCHAN_CFG_HBU(x)  (((x) << PAS_DMA_RXCHAN_CFG_HBU_S) & PAS_DMA_RXCHAN_CFG_HBU_M)

#define PAS_DMA_RXCHAN_BASEL_BRBL(x) ((x) & ~0x3f)
#define PAS_DMA_RXCHAN_BASEU_BRBH(x) ((x) & 0xfff)
#define PAS_DMA_RXCHAN_BASEU_SIZ_S   16
#define PAS_DMA_RXCHAN_BASEU_SIZ(x)  (((x) << PAS_DMA_RXCHAN_BASEU_SIZ_S) & 0x3fff0000)

/* ---- IOB physical address constants --------------------------------------- */

/*
 * PA6T-1682M ECAM base and per-device fixed addresses (bus 0).
 * Formula: phys = 0xE0000000 | (dev<<15) | (fn<<12)
 * PCI config space is little-endian; access requires lwbrx/stwbrx.
 */
#define PA6T_ECAM_BASE       0xE0000000UL
#define PA6T_IOB_ECAM        0xE0000000UL  /* IOB: PCI 00:00.0 */
#define PA6T_DMA_ECAM        0xE00D0000UL  /* DMA: PCI 00:1a.0 */
#define PA6T_MAC_ECAM        0xE00A3000UL  /* MAC: PCI 00:14.3 */

/* ---- IOB registers (via MMIO  -- offsets >= 0x1100) ----------------------- */

/* IOB packet-header counter  -- reset for Errata 5971 workaround */
#define PAS_IOB_COM_PKTHDRCNT              0x0120

#define PAS_IOB_DMA_COM_TIMEOUTCFG         0x1700
#define PAS_IOB_DMA_COM_TIMEOUTCFG_TCNT(x) ((x) & 0x00ffffff)

#define PAS_IOB_DMA_RXCH_CFG(i)   (0x1100 + (i)*4)
#define PAS_IOB_DMA_TXCH_CFG(i)   (0x1200 + (i)*4)
#define PAS_IOB_DMA_RXCH_STAT(i)  (0x1300 + (i)*4)
#define PAS_IOB_DMA_TXCH_STAT(i)  (0x1400 + (i)*4)
#define PAS_IOB_DMA_RXCH_RESET(i) (0x1500 + (i)*4)
#define PAS_IOB_DMA_TXCH_RESET(i) (0x1600 + (i)*4)

#define PAS_IOB_DMA_RXCH_CFG_CNTTH(x)      ((x) & 0xfff)
#define PAS_IOB_DMA_TXCH_CFG_CNTTH(x)      ((x) & 0xfff)

/* RXCH_STAT / TXCH_STAT register bits */
#define PAS_IOB_DMA_RXCH_STAT_INTGEN      0x00001000  /* interrupt generated */
#define PAS_IOB_DMA_RXCH_STAT_CNTDEL_M   0x00000fff  /* pending packet count */
#define PAS_IOB_DMA_TXCH_STAT_INTGEN      0x00001000  /* interrupt generated */
#define PAS_IOB_DMA_TXCH_STAT_CNTDEL_M   0x00000fff  /* delivered packet count */

/* RXCH_RESET / TXCH_RESET fields (identical layout for RX and TX) */
#define PAS_IOB_DMA_RXCH_RESET_PCNT(x)     (((x) & 0xffff) << 16)
#define PAS_IOB_DMA_RXCH_RESET_PCNTRST     0x00000020  /* reset PCNT to 0 */
#define PAS_IOB_DMA_RXCH_RESET_DCNTRST     0x00000010  /* reset DCNT to 0 */
#define PAS_IOB_DMA_RXCH_RESET_TINTC       0x00000008  /* clear timer interrupt */
#define PAS_IOB_DMA_RXCH_RESET_DINTC       0x00000004  /* clear descriptor interrupt */
#define PAS_IOB_DMA_RXCH_RESET_SINTC       0x00000002  /* clear soft interrupt */
#define PAS_IOB_DMA_RXCH_RESET_PINTC       0x00000001  /* clear packet-count interrupt */
#define PAS_IOB_DMA_TXCH_RESET_PCNT(x)     (((x) & 0xffff) << 16)
#define PAS_IOB_DMA_TXCH_RESET_PCNTRST     0x00000020  /* reset PCNT to 0 */
#define PAS_IOB_DMA_TXCH_RESET_PINTC       0x00000001  /* clear packet-count interrupt */

/* ---- pasdma_status bit definitions ---------------------------------------- */

/* Per-channel 64-bit status word written by the IOB (big-endian native). */
#define PAS_STATUS_PCNT_M   0x000000000000ffffULL  /* pending packet count */
#define PAS_STATUS_CAUSE_M  0xf000000000000000ULL  /* any interrupt cause pending */
#define PAS_STATUS_TIMER    0x1000000000000000ULL  /* timer interrupt pending */
#define PAS_STATUS_ERROR    0x2000000000000000ULL  /* descriptor error interrupt */
#define PAS_STATUS_SOFT     0x4000000000000000ULL  /* soft interrupt */
#define PAS_STATUS_INT      0x8000000000000000ULL  /* packet-count interrupt pending */

/* ---- Memory barriers for ring descriptor access --------------------------- */

static inline void pasemi_wmb(void)
{
    __asm__ volatile("sync" : : : "memory");
}

#endif /* PA6T_ETH_REGS_H */
