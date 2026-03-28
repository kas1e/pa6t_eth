/*
 * hw/hw_init.c  -- PASemi GMAC + DMA engine initialisation for pa6t_eth.device.
 *
 * Register access via ReadConfigLong / WriteConfigLong on the on-chip
 * PCI devices (MAC, DMA).  IOB is accessed via direct MMIO (lwbrx/stwbrx)
 * in irq.c and rx.c -- safe for interrupt context.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>
#include <exec/memory.h>
#include <interfaces/exec.h>

/* ---- Helpers -------------------------------------------------------------- */

/* Scan the DMA interface table to find our MAC's DMA interface number. */
static int32 find_dma_interface_of_mac(struct PA6TEthBase *base)
{
    struct ExecIFace *IExec = base->IExec;
    uint32 tmp;
    int32  nintf, off, i, j;
    uint8  bus, dev, fn;
    uint8  devfn;

    base->mac_dev->GetAddress(&bus, &dev, &fn);
    devfn = (uint8)((dev << 3) | fn);

    tmp   = read_dma_reg(base, PAS_DMA_CAP_IFI);
    nintf = (int32)((tmp & PAS_DMA_CAP_IFI_NIN_M) >> PAS_DMA_CAP_IFI_NIN_S);
    off   = (int32)((tmp & PAS_DMA_CAP_IFI_IOFF_M) >> PAS_DMA_CAP_IFI_IOFF_S);

    DPRINTF(IExec, "[pa6t_eth] find_dma_interface_of_mac: nintf=%ld off=0x%04lX devfn=0x%02X\n",
            (long)nintf, (long)off, (uint32)devfn);

    for (i = 0; i < (nintf + 3) / 4; i++) {
        tmp = read_dma_reg(base, (uint32)off + (uint32)(4 * i));
        for (j = 0; j < 4; j++) {
            if (((tmp >> (8 * j)) & 0xff) == devfn) {
                int32 intf = i * 4 + j;
                DPRINTF(IExec, "[pa6t_eth] find_dma_interface_of_mac: found at intf=%ld\n", (long)intf);
                return intf;
            }
        }
    }

    IExec->DebugPrintF("[pa6t_eth] find_dma_interface_of_mac: devfn 0x%02X not found in table!\n",
                       (uint32)devfn);
    return -1;
}

/* Extract the hardware MAC address from MAC ADR0/ADR1 registers. */
static void read_mac_addr(struct PA6TEthBase *base)
{
    struct ExecIFace *IExec = base->IExec;
    (void)IExec;
    uint32 adr0 = read_mac_reg(base, PAS_MAC_CFG_ADR0);
    uint32 adr1 = read_mac_reg(base, PAS_MAC_CFG_ADR1);

    base->mac_addr[0] = (uint8)((adr1 >> 8) & 0xff);
    base->mac_addr[1] = (uint8)(adr1 & 0xff);
    base->mac_addr[2] = (uint8)((adr0 >> 24) & 0xff);
    base->mac_addr[3] = (uint8)((adr0 >> 16) & 0xff);
    base->mac_addr[4] = (uint8)((adr0 >> 8) & 0xff);
    base->mac_addr[5] = (uint8)(adr0 & 0xff);

    DPRINTF(IExec, "[pa6t_eth] MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   (uint32)base->mac_addr[0], (uint32)base->mac_addr[1],
                   (uint32)base->mac_addr[2], (uint32)base->mac_addr[3],
                   (uint32)base->mac_addr[4], (uint32)base->mac_addr[5]);
}

/* Write MAC address into the hardware registers. */
static void program_mac_addr(struct PA6TEthBase *base)
{
    uint8  *addr = base->mac_addr;
    uint32  adr0, adr1;

    adr0 = ((uint32)addr[2] << 24) | ((uint32)addr[3] << 16) |
           ((uint32)addr[4] << 8)  |  (uint32)addr[5];

    adr1 = read_mac_reg(base, PAS_MAC_CFG_ADR1);
    adr1 &= ~0x0000fffful;
    adr1 |= ((uint32)addr[0] << 8) | (uint32)addr[1];

    write_mac_reg(base, PAS_MAC_CFG_ADR0, adr0);
    write_mac_reg(base, PAS_MAC_CFG_ADR1, adr1);
}

/* Stop a DMA channel with timeout.  Returns TRUE if stopped, FALSE on timeout. */
static BOOL stop_tx_channel(struct PA6TEthBase *base, uint32 chno)
{
    uint32 i;
    write_dma_reg(base, PAS_DMA_TXCHAN_TCMDSTA(chno), PAS_DMA_TXCHAN_TCMDSTA_ST);
    for (i = 0; i < 50000; i++) {
        if (!(read_dma_reg(base, PAS_DMA_TXCHAN_TCMDSTA(chno)) &
              PAS_DMA_TXCHAN_TCMDSTA_ACT)) {
            write_dma_reg(base, PAS_DMA_TXCHAN_TCMDSTA(chno), 0);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL stop_rx_channel(struct PA6TEthBase *base, uint32 chno)
{
    uint32 i;
    write_dma_reg(base, PAS_DMA_RXCHAN_CCMDSTA(chno), PAS_DMA_RXCHAN_CCMDSTA_ST);
    for (i = 0; i < 50000; i++) {
        if (!(read_dma_reg(base, PAS_DMA_RXCHAN_CCMDSTA(chno)) &
              PAS_DMA_RXCHAN_CCMDSTA_ACT)) {
            write_dma_reg(base, PAS_DMA_RXCHAN_CCMDSTA(chno), 0);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL stop_rx_interface(struct PA6TEthBase *base, uint32 intf)
{
    uint32 i;
    write_dma_reg(base, PAS_DMA_RXINT_RCMDSTA(intf),
                  PAS_DMA_RXINT_RCMDSTA_ST);
    for (i = 0; i < 50000; i++) {
        if (!(read_dma_reg(base, PAS_DMA_RXINT_RCMDSTA(intf)) &
              PAS_DMA_RXINT_RCMDSTA_ACT)) {
            write_dma_reg(base, PAS_DMA_RXINT_RCMDSTA(intf), 0);
            return TRUE;
        }
    }
    return FALSE;
}

/* DMA-map a buffer and return its physical address. */
static uint32 get_phys(struct ExecIFace *IExec, void *virt, uint32 size,
                        uint32 flags, uint32 *out_entries)
{
    uint32 nentries = IExec->StartDMA(virt, size, flags);
    if (nentries == 0) {
        *out_entries = 0;
        return 0;
    }

    struct DMAEntry *dlist = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, nentries, TAG_DONE);
    if (!dlist) {
        IExec->EndDMA(virt, size, flags | DMAF_NoModify);
        *out_entries = 0;
        return 0;
    }

    IExec->GetDMAList(virt, size, flags, dlist);
    uint32 phys = (uint32)dlist[0].PhysicalAddress;

    if (nentries > 1) {
        IExec->DebugPrintF("[pa6t_eth] get_phys: WARNING non-contiguous!"
                           " virt=%p size=%lu nentries=%lu\n",
                           virt, size, nentries);
    }

    IExec->FreeSysObject(ASOT_DMAENTRY, dlist);

    *out_entries = nentries;
    return phys;
}

/* ---- PA6TEth_HWInit ------------------------------------------------------- */

/* Initialise the GMAC + DMA engine.  MAC is enabled later in PostRxBuffers. */
BOOL PA6TEth_HWInit(struct PA6TEthBase *base)
{
    struct ExecIFace  *IExec = base->IExec;
    struct PA6TEthUnit *unit = base->unit;
    uint32 i;

    DPRINTF(IExec, "[pa6t_eth] HWInit: starting\n");


    /* ---- 1. DMA interface number + MAC function number ---- */
    base->dma_if = find_dma_interface_of_mac(base);
    if (base->dma_if < 0) {
        IExec->DebugPrintF("[pa6t_eth] HWInit: find_dma_interface_of_mac failed\n");
        goto fail;
    }
    DPRINTF(IExec, "[pa6t_eth] HWInit: dma_if=%ld\n", (long)base->dma_if);

    /* ---- 2. MAC address ---- */
    read_mac_addr(base);

    /* Validate  -- all zeros or all 0xFF are bad */
    {
        int valid = 0;
        for (i = 0; i < ETH_ADDR_LEN; i++) {
            if (base->mac_addr[i] != 0x00 && base->mac_addr[i] != 0xFF)
                valid = 1;
        }
        if (!valid) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: MAC address invalid  -- using fallback\n");
            /* Locally-administered fallback address */
            base->mac_addr[0] = 0x02;
            base->mac_addr[1] = 0x00;
            base->mac_addr[2] = 0xDE;
            base->mac_addr[3] = 0xAD;
            base->mac_addr[4] = 0xBE;
            base->mac_addr[5] = 0xEF;
        }
    }

    
    /* TX: channel 1, RX: channel 1.
     * Check that our chosen channels are not already active (e.g. channel 0
     * is used by pasemi_dma.resource). */
    base->tx_channel_num = 1;
    base->rx_channel_num = 1;

    {
        uint32 sta = read_dma_reg(base, PAS_DMA_TXCHAN_TCMDSTA((uint32)base->tx_channel_num));
        if (sta & PAS_DMA_TXCHAN_TCMDSTA_ACT) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: TX channel %ld is already active (sta=0x%08lX)!\n",
                               (long)base->tx_channel_num, sta);
            goto fail;
        }
    }
    {
        uint32 sta = read_dma_reg(base, PAS_DMA_RXCHAN_CCMDSTA((uint32)base->rx_channel_num));
        if (sta & PAS_DMA_RXCHAN_CCMDSTA_ACT) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: RX channel %ld is already active (sta=0x%08lX)!\n",
                               (long)base->rx_channel_num, sta);
            goto fail;
        }
    }

    DPRINTF(IExec, "[pa6t_eth] HWInit: tx_channel_num=%ld rx_channel_num=%ld\n",
            (long)base->tx_channel_num, (long)base->rx_channel_num);

    /* ---- 3-5. Allocate DMA rings -----------------------------------------------
     * TX needs one ring: descriptors telling the DMA engine what to send.
     * RX needs two rings: a descriptor ring (completed packet descriptors
     * written by the DMA engine) and a buffer pointer ring (empty buffers
     * we provide for the DMA engine to fill with incoming packets). */

    /* ---- 3. Allocate TX descriptor ring ---- */
    {
        uint32 tx_ring_bytes = TX_RING_SIZE * sizeof(uint64);

        unit->tx_ring = (volatile uint64 *)IExec->AllocVecTags(tx_ring_bytes,
            AVT_Type,           MEMF_SHARED,
            AVT_Contiguous,     TRUE,        /* must not span page boundary */
            AVT_Alignment,      64,          /* 64-byte (cache line) aligned */
            AVT_ClearWithValue, 0,
            TAG_DONE);
        if (!unit->tx_ring) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: TX ring alloc failed\n");
            goto fail;
        }

        unit->tx_ring_phys = get_phys(IExec, (void *)unit->tx_ring,
                                       tx_ring_bytes, DMA_ReadFromRAM,
                                       &unit->tx_ring_dma_entries);
        if (!unit->tx_ring_phys) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: TX ring DMA-map failed\n");
            goto fail;
        }
        DPRINTF(IExec, "[pa6t_eth] HWInit: TX ring phys=0x%08lX size=%lu\n",
                unit->tx_ring_phys, (uint32)tx_ring_bytes);
    }

    /* ---- 4. Allocate RX descriptor ring ---- */
    {
        uint32 rx_ring_bytes = RX_RING_SIZE * sizeof(uint64);

        unit->rx_ring = (volatile uint64 *)IExec->AllocVecTags(rx_ring_bytes,
            AVT_Type,           MEMF_SHARED,
            AVT_Contiguous,     TRUE,        /* must not span page boundary */
            AVT_Alignment,      64,
            AVT_ClearWithValue, 0,
            TAG_DONE);
        if (!unit->rx_ring) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: RX desc ring alloc failed\n");
            goto fail;
        }

        unit->rx_ring_phys = get_phys(IExec, (void *)unit->rx_ring,
                                       rx_ring_bytes, 0,   /* device->RAM */
                                       &unit->rx_ring_dma_entries);
        if (!unit->rx_ring_phys) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: RX desc ring DMA-map failed\n");
            goto fail;
        }
        DPRINTF(IExec, "[pa6t_eth] HWInit: RX desc ring phys=0x%08lX\n",
                unit->rx_ring_phys);
    }

    /* ---- 5. Allocate RX buffer pointer ring ---- */
    {
        uint32 rxbuf_ring_bytes = RX_BUF_RING_SIZE * sizeof(uint64);

        unit->rx_buf_ring = (volatile uint64 *)IExec->AllocVecTags(rxbuf_ring_bytes,
            AVT_Type,           MEMF_SHARED,
            AVT_Contiguous,     TRUE,        /* must not span page boundary */
            AVT_Alignment,      64,
            AVT_ClearWithValue, 0,
            TAG_DONE);
        if (!unit->rx_buf_ring) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: RX buf ring alloc failed\n");
            goto fail;
        }

        unit->rx_buf_ring_phys = get_phys(IExec, (void *)unit->rx_buf_ring,
                                           rxbuf_ring_bytes, 0,
                                           &unit->rx_buf_ring_dma_entries);
        if (!unit->rx_buf_ring_phys) {
            IExec->DebugPrintF("[pa6t_eth] HWInit: RX buf ring DMA-map failed\n");
            goto fail;
        }
        DPRINTF(IExec, "[pa6t_eth] HWInit: RX buf ring phys=0x%08lX\n",
                unit->rx_buf_ring_phys);
    }

    /* ---- 5b. Mark rings cache-inhibited (like Linux dma_alloc_coherent) ---- */
    {
        struct MMUIFace *IMMU = (struct MMUIFace *)
            IExec->GetInterface(IExec->Data.LibBase, "mmu", 1, NULL);
        if (IMMU) {
            uint32 ci = MEMATTRF_CACHEINHIBIT | MEMATTRF_SUPER_RW_USER_RW;
            IMMU->SetMemoryAttrs((void *)unit->tx_ring,
                                 TX_RING_SIZE * sizeof(uint64), ci);
            IMMU->SetMemoryAttrs((void *)unit->rx_ring,
                                 RX_RING_SIZE * sizeof(uint64), ci);
            IMMU->SetMemoryAttrs((void *)unit->rx_buf_ring,
                                 RX_BUF_RING_SIZE * sizeof(uint64), ci);
            IExec->DropInterface((struct Interface *)IMMU);
            DPRINTF(IExec, "[pa6t_eth] HWInit: rings marked cache-inhibited\n");
        } else {
            IExec->DebugPrintF("[pa6t_eth] HWInit: WARNING: IMMU unavailable"
                               "  -- rings NOT cache-inhibited, O-bit stall likely!\n");
        }
    }

    /* ---- 6. Configure TX DMA channel ---- */
    {
        uint32 siz = TX_RING_SIZE >> 3;
        uint32 cfg;

        stop_tx_channel(base, (uint32)base->tx_channel_num);

        write_dma_reg(base, PAS_DMA_TXCHAN_BASEL((uint32)base->tx_channel_num),
                      PAS_DMA_TXCHAN_BASEL_BRBL(unit->tx_ring_phys));
        write_dma_reg(base, PAS_DMA_TXCHAN_BASEU((uint32)base->tx_channel_num),
                      PAS_DMA_TXCHAN_BASEU_BRBH(0) | PAS_DMA_TXCHAN_BASEU_SIZ(siz));

        /* TATTR = DMA interface index for this MAC */
        cfg = PAS_DMA_TXCHAN_CFG_TY_IFACE |
              PAS_DMA_TXCHAN_CFG_TATTR((uint32)base->dma_if) |
              PAS_DMA_TXCHAN_CFG_UP |
              PAS_DMA_TXCHAN_CFG_WT(4);
        write_dma_reg(base, PAS_DMA_TXCHAN_CFG((uint32)base->tx_channel_num), cfg);

        DPRINTF(IExec, "[pa6t_eth] HWInit: TX chan%ld base=0x%08lX siz=%lu"
                       " CFG=0x%08lX (TATTR=%lu)\n",
                       (long)base->tx_channel_num, unit->tx_ring_phys, siz,
                       cfg, (cfg >> 2) & 0xf);
    }

    /* ---- 7. Configure RX DMA channel and interface ---- */
    {
        uint32 siz = RX_RING_SIZE >> 3;
        uint32 cfg;

        /* Stop the RX section first -- stopping individual channels while
         * the section is running causes DMA writes to firmware's old ring
         * addresses, which can corrupt hardware state. */
        {
            uint32 i;
            write_dma_reg(base, PAS_DMA_COM_RXCMD, 0);  /* disable RX section */
            for (i = 0; i < 100000; i++) {
                if (!(read_dma_reg(base, PAS_DMA_COM_RXSTA) & PAS_DMA_COM_RXSTA_ACT))
                    break;
            }
            DPRINTF(IExec, "[pa6t_eth] HWInit: RX section stopped"
                           " (RXSTA=0x%08lX after %lu polls)\n",
                           read_dma_reg(base, PAS_DMA_COM_RXSTA), (uint32)i);
        }

        /* Hard-reset channel and interface (safe now -- section is stopped) */
        write_dma_reg(base, PAS_DMA_RXCHAN_CCMDSTA((uint32)base->rx_channel_num), 0);
        write_dma_reg(base, PAS_DMA_RXINT_RCMDSTA((uint32)base->dma_if),   0);

        /* RX channel: point at our descriptor ring */
        write_dma_reg(base, PAS_DMA_RXCHAN_BASEL((uint32)base->rx_channel_num),
                      PAS_DMA_RXCHAN_BASEL_BRBL(unit->rx_ring_phys));
        write_dma_reg(base, PAS_DMA_RXCHAN_BASEU((uint32)base->rx_channel_num),
                      PAS_DMA_RXCHAN_BASEU_BRBH(0) | PAS_DMA_RXCHAN_BASEU_SIZ(siz));

        cfg = PAS_DMA_RXCHAN_CFG_HBU(2);
        write_dma_reg(base, PAS_DMA_RXCHAN_CFG((uint32)base->rx_channel_num), cfg);

        /* RX interface: point at our buffer-pointer ring */
        write_dma_reg(base, PAS_DMA_RXINT_BASEL((uint32)base->dma_if),
                      PAS_DMA_RXINT_BASEL_BRBL(unit->rx_buf_ring_phys));
        write_dma_reg(base, PAS_DMA_RXINT_BASEU((uint32)base->dma_if),
                      PAS_DMA_RXINT_BASEU_BRBH(0) | PAS_DMA_RXINT_BASEU_SIZ(siz));

        cfg = PAS_DMA_RXINT_CFG_RBP   |
              PAS_DMA_RXINT_CFG_DHL(2) |
              PAS_DMA_RXINT_CFG_LW     |
              PAS_DMA_RXINT_CFG_L2     |
              PAS_DMA_RXINT_CFG_HEN;
        write_dma_reg(base, PAS_DMA_RXINT_CFG((uint32)base->dma_if), cfg);

        /* Re-enable the RX section with our rings programmed */
        write_dma_reg(base, PAS_DMA_COM_RXCMD, PAS_DMA_COM_RXCMD_EN);
        DPRINTF(IExec, "[pa6t_eth] HWInit: RX section re-enabled"
                       " (RXSTA=0x%08lX)\n",
                       read_dma_reg(base, PAS_DMA_COM_RXSTA));

        DPRINTF(IExec, "[pa6t_eth] HWInit: RX chan%ld/intf%ld configured (siz=%lu base=0x%08lX)\n",
                (long)base->rx_channel_num, (long)base->dma_if, siz, unit->rx_ring_phys);
    }

    /* ---- 8. Configure MAC ---- */

    /* Disable MAC while configuring */
    {
        uint32 flags = read_mac_reg(base, PAS_MAC_CFG_PCFG);
        flags &= ~PAS_MAC_CFG_PCFG_PE;
        write_mac_reg(base, PAS_MAC_CFG_PCFG, flags);
    }

    /* Set max frame size in MACCFG */
    {
        uint32 maccfg = read_mac_reg(base, PAS_MAC_CFG_MACCFG);
        maccfg &= ~PAS_MAC_CFG_MACCFG_MAXF_M;
        maccfg |= PAS_MAC_CFG_MACCFG_MAXF(ETH_MAX_FRAME + ETH_HDR_LEN + 4);
        write_mac_reg(base, PAS_MAC_CFG_MACCFG, maccfg);
    }

    /* TX parameters */
    {
        uint32 txp = PAS_MAC_CFG_TXP_FCE     |
                     PAS_MAC_CFG_TXP_FPC(3)  |
                     PAS_MAC_CFG_TXP_SL(3)   |
                     PAS_MAC_CFG_TXP_COB(0xf)|
                     PAS_MAC_CFG_TXP_TIFT(8) |
                     PAS_MAC_CFG_TXP_TIFG(12);
        write_mac_reg(base, PAS_MAC_CFG_TXP, txp);
    }

    /* Zero RMON counters */
    for (i = 0; i < 32; i++)
        write_mac_reg(base, PAS_MAC_CFG_RMON(i), 0);

    /* Program MAC address into hardware */
    program_mac_addr(base);

    /* Assign DMA channels to MAC (DCHNO + BCH both = rx_channel_num) */
    write_mac_reg(base, PAS_MAC_IPC_CHNL,
                  PAS_MAC_IPC_CHNL_DCHNO((uint32)base->rx_channel_num) |
                  PAS_MAC_IPC_CHNL_BCH((uint32)base->rx_channel_num));
    DPRINTF(IExec, "[pa6t_eth] HWInit: IPC_CHNL DCHNO=%ld BCH=%ld\n",
                   (long)base->rx_channel_num, (long)base->rx_channel_num);

    DPRINTF(IExec, "[pa6t_eth] HWInit: done\n");
    return TRUE;

fail:
    PA6TEth_HWCleanup(base);
    return FALSE;
}

/* ---- PA6TEth_HWCleanup ---------------------------------------------------- */

void PA6TEth_HWCleanup(struct PA6TEthBase *base)
{
    struct ExecIFace  *IExec = base->IExec;
    struct PA6TEthUnit *unit = base->unit;
    uint32 i;

    DPRINTF(IExec, "[pa6t_eth] HWCleanup\n");

    /* Disable MAC */
    if (base->mac_dev) {
        uint32 flags = read_mac_reg(base, PAS_MAC_CFG_PCFG);
        flags &= ~PAS_MAC_CFG_PCFG_PE;
        write_mac_reg(base, PAS_MAC_CFG_PCFG, flags);
    }

    /* Stop DMA channels */
    if (base->dma_dev) {
        if (base->tx_channel_num >= 0) stop_tx_channel(base, (uint32)base->tx_channel_num);
        if (base->rx_channel_num >= 0) stop_rx_channel(base, (uint32)base->rx_channel_num);
        if (base->dma_if  >= 0) stop_rx_interface(base, (uint32)base->dma_if);
    }

    if (!unit) return;

    /* Free RX buffers */
    for (i = 0; i < NUM_RX_BUFS; i++) {
        if (unit->rx_bufs[i]) {
            if (unit->rx_bufs_dma_entries[i])
                IExec->EndDMA(unit->rx_bufs[i], RX_BUF_SIZE, DMAF_NoModify);
            IExec->FreeVec(unit->rx_bufs[i]);
            unit->rx_bufs[i] = NULL;
        }
    }

    if (unit->rx_buf_ring) {
        if (unit->rx_buf_ring_dma_entries)
            IExec->EndDMA((void *)unit->rx_buf_ring,
                          RX_BUF_RING_SIZE * sizeof(uint64), DMAF_NoModify);
        IExec->FreeVec((void *)unit->rx_buf_ring);
        unit->rx_buf_ring = NULL;
    }

    if (unit->rx_ring) {
        if (unit->rx_ring_dma_entries)
            IExec->EndDMA((void *)unit->rx_ring,
                          RX_RING_SIZE * sizeof(uint64), DMAF_NoModify);
        IExec->FreeVec((void *)unit->rx_ring);
        unit->rx_ring = NULL;
    }

    if (unit->tx_ring) {
        if (unit->tx_ring_dma_entries)
            IExec->EndDMA((void *)unit->tx_ring,
                          TX_RING_SIZE * sizeof(uint64),
                          DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec((void *)unit->tx_ring);
        unit->tx_ring = NULL;
    }
}
