/*
 * hw/rx.c  -- RX ring operations and frame delivery for pa6t_eth.device.
 *
 * RX completion: 4 ring slots per packet (MACRX, eval, PTR, pad).
 * Advance RXCHAN_INCR by count*2 after processing.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>
#include <exec/errors.h>
#include <stddef.h>
#include <exec/exectags.h>

/* ---- PA6TEth_FindOpener --------------------------------------------------- */

struct Opener *PA6TEth_FindOpener(struct PA6TEthUnit *unit, APTR bm_cookie)
{
    struct Opener *op = (struct Opener *)unit->opener_list.lh_Head;
    while (op->node.mln_Succ) {
        if (op->bm_cookie == bm_cookie)
            return op;
        op = (struct Opener *)op->node.mln_Succ;
    }
    return NULL;
}

/* ---- PA6TEth_FlushReadQueue / PA6TEth_FlushAllPending -------------------- */

/* Drain one read list, freeing each PendingRead wrapper. */
static void flush_read_list(struct ExecIFace *IExec, struct List *list, BYTE error)
{
    struct Node *n = list->lh_Head;
    while (n->ln_Succ) {
        struct Node        *next = n->ln_Succ;
        struct PendingRead *pr   = (struct PendingRead *)n;
        struct IOSana2Req  *req  = pr->ioreq;
        IExec->Remove(n);
        IExec->FreeVec(pr);
        req->ios2_Req.io_Error = error;
        req->ios2_WireError    = S2WERR_GENERIC_ERROR;
        IExec->ReplyMsg((struct Message *)req);
        n = next;
    }
}

/* Abort all pending CMD_READ and S2_READORPHAN requests. */
void PA6TEth_FlushReadQueue(struct PA6TEthBase *base, struct PA6TEthUnit *unit,
                            BYTE error)
{
    struct ExecIFace *IExec = base->IExec;

    IExec->Forbid();

    flush_read_list(IExec, &unit->read_list_ipv4,    error);
    flush_read_list(IExec, &unit->read_list_arp,   error);
    flush_read_list(IExec, &unit->read_list_other, error);

    /* Flush S2_READORPHAN from event_list */
    struct Node *n = unit->event_list.lh_Head;
    while (n->ln_Succ) {
        struct Node       *next = n->ln_Succ;
        struct IOSana2Req *ev   = (struct IOSana2Req *)
            ((uint8 *)n - offsetof(struct IORequest, io_Message.mn_Node));
        if (ev->ios2_Req.io_Command == S2_READORPHAN) {
            IExec->Remove(n);
            ev->ios2_Req.io_Error = error;
            ev->ios2_WireError    = S2WERR_GENERIC_ERROR;
            IExec->ReplyMsg((struct Message *)ev);
        }
        n = next;
    }

    IExec->Permit();
}

/* Flush everything on unit task exit. */
void PA6TEth_FlushAllPending(struct PA6TEthBase *base, struct PA6TEthUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;

    /* First drain all read queues (also handles S2_READORPHAN) */
    PA6TEth_FlushReadQueue(base, unit, IOERR_ABORTED);

    /* Now flush any remaining event_list entries (S2_ONEVENT etc.) */
    IExec->Forbid();
    struct Node *n = unit->event_list.lh_Head;
    while (n->ln_Succ) {
        struct Node       *next = n->ln_Succ;
        struct IOSana2Req *ev   = (struct IOSana2Req *)
            ((uint8 *)n - offsetof(struct IORequest, io_Message.mn_Node));
        IExec->Remove(n);
        ev->ios2_Req.io_Error = IOERR_ABORTED;
        ev->ios2_WireError    = S2WERR_GENERIC_ERROR;
        IExec->ReplyMsg((struct Message *)ev);
        n = next;
    }
    IExec->Permit();
}

/* ---- PA6TEth_PostRxBuffers ------------------------------------------------ */

/* Post RX buffers and enable the RX interface + channel.  Called on online. */
void PA6TEth_PostRxBuffers(struct PA6TEthBase *base, struct PA6TEthUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;
    uint32 i;

    if (!unit->rx_buf_ring || !unit->rx_ring) {
        DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: rings not ready\n");
        return;
    }

    /* Allocate RX data buffers in user-mode context (not in HWInit). */
    {
        uint32 nalloced = 0;
        for (i = 0; i < NUM_RX_BUFS; i++) {
            if (unit->rx_bufs[i])
                continue;   /* already allocated (e.g. on second configure) */

            unit->rx_bufs[i] = (uint8 *)IExec->AllocVecTags(RX_BUF_SIZE,
                AVT_Type,           MEMF_SHARED,
                AVT_Contiguous,     TRUE,        /* required for reliable DMA mapping */
                AVT_Alignment,      64,
                AVT_ClearWithValue, 0,
                TAG_DONE);
            if (!unit->rx_bufs[i]) {
                IExec->DebugPrintF("[pa6t_eth] PostRxBuffers: rx_buf[%lu] alloc failed\n",
                                   (uint32)i);
                break;
            }

            /* DMA-map: device writes to RAM (flags=0) */
            uint32 nentries = IExec->StartDMA(unit->rx_bufs[i], RX_BUF_SIZE, 0);
            if (nentries == 0) {
                IExec->DebugPrintF("[pa6t_eth] PostRxBuffers: rx_buf[%lu] StartDMA failed\n",
                                   (uint32)i);
                IExec->FreeVec(unit->rx_bufs[i]);
                unit->rx_bufs[i] = NULL;
                break;
            }

            struct DMAEntry *dlist = (struct DMAEntry *)IExec->AllocSysObjectTags(
                ASOT_DMAENTRY, ASODMAE_NumEntries, nentries, TAG_DONE);
            if (!dlist) {
                IExec->EndDMA(unit->rx_bufs[i], RX_BUF_SIZE, DMAF_NoModify);
                IExec->FreeVec(unit->rx_bufs[i]);
                unit->rx_bufs[i] = NULL;
                break;
            }
            IExec->GetDMAList(unit->rx_bufs[i], RX_BUF_SIZE, 0, dlist);
            unit->rx_bufs_phys[i] = (uint32)dlist[0].PhysicalAddress;
            unit->rx_bufs_dma_entries[i] = nentries;
            IExec->FreeSysObject(ASOT_DMAENTRY, dlist);

            if (!unit->rx_bufs_phys[i]) {
                IExec->DebugPrintF("[pa6t_eth] PostRxBuffers: rx_buf[%lu] phys=0\n",
                                   (uint32)i);
                IExec->EndDMA(unit->rx_bufs[i], RX_BUF_SIZE, DMAF_NoModify);
                IExec->FreeVec(unit->rx_bufs[i]);
                unit->rx_bufs[i] = NULL;
                break;
            }

            DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: rx_buf[%lu] virt=%p phys=0x%08lX\n",
                           (uint32)i, unit->rx_bufs[i], unit->rx_bufs_phys[i]);
            nalloced++;
        }
        DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: %lu/%lu RX buffers allocated\n",
                       nalloced, (uint32)NUM_RX_BUFS);
        if (nalloced == 0) {
            IExec->DebugPrintF("[pa6t_eth] PostRxBuffers: no RX buffers  -- aborting\n");
            return;
        }
    }

    /* Clear descriptor ring */
    for (i = 0; i < RX_RING_SIZE; i++)
        unit->rx_ring[i] = 0;

    /* Clear buffer pointer ring */
    for (i = 0; i < RX_BUF_RING_SIZE; i++)
        unit->rx_buf_ring[i] = 0;

    pasemi_wmb();

    /* Configure RX interface (RBP enables buffer pointer ring) */
    {
        uint32 cfg = PAS_DMA_RXINT_CFG_RBP      |
                     PAS_DMA_RXINT_CFG_DHL(2)    |
                     PAS_DMA_RXINT_CFG_LW         |
                     PAS_DMA_RXINT_CFG_L2         |
                     PAS_DMA_RXINT_CFG_HEN;
        write_dma_reg(base, PAS_DMA_RXINT_CFG((uint32)base->dma_if), cfg);
        DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: RXINT_CFG=0x%08lX\n", cfg);
    }

    /* Enable RX interface (W1C bits clear stale status from firmware) */
    write_dma_reg(base, PAS_DMA_RXINT_RCMDSTA((uint32)base->dma_if),
                  PAS_DMA_RXINT_RCMDSTA_EN      |
                  PAS_DMA_RXINT_RCMDSTA_DROPS_M |
                  PAS_DMA_RXINT_RCMDSTA_BP       |
                  PAS_DMA_RXINT_RCMDSTA_OO       |
                  PAS_DMA_RXINT_RCMDSTA_DR       |
                  PAS_DMA_RXINT_RCMDSTA_BT);
    /* Enable RX channel */
    write_dma_reg(base, PAS_DMA_RXCHAN_CCMDSTA((uint32)base->rx_channel_num),
                  PAS_DMA_RXCHAN_CCMDSTA_EN |
                  PAS_DMA_RXCHAN_CCMDSTA_DU |
                  PAS_DMA_RXCHAN_CCMDSTA_OD |
                  PAS_DMA_RXCHAN_CCMDSTA_FD |
                  PAS_DMA_RXCHAN_CCMDSTA_DT);
    /* Post buffers after enabling the interface (INCR while disabled is ignored) */
    for (i = 0; i < NUM_RX_BUFS; i++) {
        if (!unit->rx_bufs_phys[i]) continue;
        unit->rx_buf_ring[i] = XCT_RXB_LEN(RX_BUF_SIZE) |
                                XCT_RXB_ADDR(unit->rx_bufs_phys[i]);
        unit->rx_slot_buf[i] = (uint16)i;   /* slot i holds buffer i initially */
    }
    unit->rx_buf_write_ptr = NUM_RX_BUFS; /* next post goes after initial batch */
    pasemi_wmb();

    /* Tell the DMA engine we posted N buffers */
    write_dma_reg(base, PAS_DMA_RXINT_INCR((uint32)base->dma_if),
                  (uint32)NUM_RX_BUFS);

    /* Credit the full empty descriptor ring (each completion = 2 units) */
    write_dma_reg(base, PAS_DMA_RXCHAN_INCR((uint32)base->rx_channel_num),
                  RX_RING_SIZE >> 1);

    /* Ensure TX DMA section is enabled */
    write_dma_reg(base, PAS_DMA_COM_TXCMD, PAS_DMA_COM_TXCMD_EN);

    /* Enable TX channel (deferred from HWInit to avoid spurious interrupts) */
    write_dma_reg(base, PAS_DMA_TXCHAN_TCMDSTA((uint32)base->tx_channel_num),
                  PAS_DMA_TXCHAN_TCMDSTA_EN |
                  PAS_DMA_TXCHAN_TCMDSTA_SZ |
                  PAS_DMA_TXCHAN_TCMDSTA_DB |
                  PAS_DMA_TXCHAN_TCMDSTA_DE |
                  PAS_DMA_TXCHAN_TCMDSTA_DA);
    /* Enable Memory + BusMaster on the MAC device */
    {
        uint16 cmd = base->mac_dev->ReadConfigWord(PCI_COMMAND);
        if ((cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
                   (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) {
            base->mac_dev->WriteConfigWord(PCI_COMMAND,
                cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
            DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: MAC Memory+BusMaster enabled"
                    " (was 0x%04lX)\n", (uint32)cmd);
        }
    }

    /* Enable BusMaster on the DMA device */
    {
        uint16 cmd = base->dma_dev->ReadConfigWord(PCI_COMMAND);
        if ((cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
                   (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) {
            base->dma_dev->WriteConfigWord(PCI_COMMAND,
                cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
            IExec->DebugPrintF("[pa6t_eth] PostRxBuffers: DMA Memory+BusMaster enabled"
                               " (was 0x%04lX)\n", (uint32)cmd);
        } else {
            DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: DMA PCI_COMMAND=0x%04lX"
                           " (already has Mem+Master)\n", (uint32)cmd);
        }
    }

    /* Enable MAC at 1 Gbps full-duplex. */
    {
        uint32 pcfg = PAS_MAC_CFG_PCFG_S1       |
                      PAS_MAC_CFG_PCFG_PR        |
                      PAS_MAC_CFG_PCFG_CE        |
                      PAS_MAC_CFG_PCFG_TSR_1G    |
                      PAS_MAC_CFG_PCFG_SPD_1G    |
                      PAS_MAC_CFG_PCFG_PE;
        write_mac_reg(base, PAS_MAC_CFG_PCFG, pcfg);
        DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: PCFG=0x%08lX (1 Gbps)\n", pcfg);
    }

    unit->rx_next_clean = 0;

    /* ---- Enable IOB packet-count interrupts -------------------------------- */
    {
        /* Global timeout: ~31us */
        write_iob_mmio(base, PAS_IOB_DMA_COM_TIMEOUTCFG,
                       PAS_IOB_DMA_COM_TIMEOUTCFG_TCNT(0x3ff));

        /* RX: fire after every packet (CNTTH=1) */
        write_iob_mmio(base, PAS_IOB_DMA_RXCH_CFG((uint32)base->rx_channel_num),
                       PAS_IOB_DMA_RXCH_CFG_CNTTH(1));

        /* TX: CNTTH=0 (no TX interrupt handler) */
        write_iob_mmio(base, PAS_IOB_DMA_TXCH_CFG((uint32)base->tx_channel_num),
                       PAS_IOB_DMA_TXCH_CFG_CNTTH(0));

        /* TX: clear stale packet-count interrupt state */
        {
            uint32 txch_stat = read_iob_mmio(base,
                                   PAS_IOB_DMA_TXCH_STAT((uint32)base->tx_channel_num));
            uint32 tx_cntdel = txch_stat & PAS_IOB_DMA_TXCH_STAT_CNTDEL_M;
            write_iob_mmio(base, PAS_IOB_DMA_TXCH_RESET((uint32)base->tx_channel_num),
                           PAS_IOB_DMA_TXCH_RESET_PCNT(tx_cntdel) |
                           PAS_IOB_DMA_TXCH_RESET_PINTC);
        }

        /* Clear stale RX interrupt state */
        uint32 rxch_stat_init = read_iob_mmio(base,
                                    PAS_IOB_DMA_RXCH_STAT((uint32)base->rx_channel_num));
        uint32 cntdel_init    = rxch_stat_init & PAS_IOB_DMA_RXCH_STAT_CNTDEL_M;

        uint32 clear_all = PAS_IOB_DMA_RXCH_RESET_PCNT(cntdel_init) |
                           PAS_IOB_DMA_RXCH_RESET_DCNTRST |
                           PAS_IOB_DMA_RXCH_RESET_TINTC   |
                           PAS_IOB_DMA_RXCH_RESET_DINTC   |
                           PAS_IOB_DMA_RXCH_RESET_SINTC   |
                           PAS_IOB_DMA_RXCH_RESET_PINTC;
        write_iob_mmio(base, PAS_IOB_DMA_RXCH_RESET((uint32)base->rx_channel_num),
                       clear_all);

    }

    DPRINTF(IExec, "[pa6t_eth] PostRxBuffers: %lu buffers posted, MAC enabled\n",
            (uint32)NUM_RX_BUFS);
}

/* ---- PA6TEth_ProcessRX ---------------------------------------------------- */

/* Process all completed RX descriptors and deliver frames. */
void PA6TEth_ProcessRX(struct PA6TEthBase *base, struct PA6TEthUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;
    uint32 n;
    uint32 count = 0;      /* packets processed in this call */
    uint32 replenish = 0;  /* buffers re-posted */

    if (!unit->rx_ring || !unit->rx_buf_ring) return;

    DPRINTFV(IExec, "[pa6t_eth] ProcessRX: called (rx_next_clean=%lu)\n",
             unit->rx_next_clean);

    n = unit->rx_next_clean;

    for (;;) {
        /* Sentinel values so goto next_desc is always safe */
        uint32 ring_slot  = RX_RING_SIZE;  /* sentinel = "no slot" (>= RX_RING_SIZE) */
        uint32 actual_buf = NUM_RX_BUFS;   /* sentinel = "no buffer" */

        /* Ring is cache-inhibited -- reads go directly to RAM */
        uint64 macrx = RX_DESC(unit->rx_ring, n);

        if (!(macrx & XCT_MACRX_O))
            break;

        /* Always extract ring slot (even for errors) to avoid buffer leak */
        uint64 eval_word = RX_DESC(unit->rx_ring, n + 1);
        uint64 ptr_word  = RX_DESC(unit->rx_ring, n + 2);
        uint32 eval = (uint32)((eval_word & XCT_RXRES_8B_EVAL_M) >>
                                XCT_RXRES_8B_EVAL_S);
        ring_slot = (eval > 0) ? (eval - 1) : RX_RING_SIZE;
        if (ring_slot < RX_RING_SIZE)
            actual_buf = unit->rx_slot_buf[ring_slot];

        /* E bit = diagnostic only (not fatal) */
        if (macrx & XCT_MACRX_E) {
            DPRINTFV(IExec, "[pa6t_eth] ProcessRX: E-bit set (diagnostic only)"
                            " flags=0x%08lX%08lX len=%lu  -- continuing\n",
                            (uint32)(macrx >> 32), (uint32)macrx,
                            (uint32)((macrx & XCT_MACRX_LLEN_M) >> XCT_MACRX_LLEN_S));
        }

        /* CRC error  -- frame is corrupt, discard it */
        if (macrx & XCT_MACRX_CRC) {
            IExec->DebugPrintF("[pa6t_eth] ProcessRX: CRC error"
                               " flags=0x%08lX%08lX  -- dropping\n",
                               (uint32)(macrx >> 32), (uint32)macrx);
            unit->stat_rx_dropped++;
            goto next_desc;
        }

        DPRINTFV(IExec, "[pa6t_eth] ProcessRX: n=%lu eval_word=0x%08lX%08lX"
                 " ptr=0x%08lX%08lX eval=%lu ring_slot=%lu\n",
                 n,
                 (uint32)(eval_word >> 32), (uint32)eval_word,
                 (uint32)(ptr_word  >> 32), (uint32)ptr_word,
                 eval, ring_slot);

        if (ring_slot >= RX_RING_SIZE) {
            IExec->DebugPrintF("[pa6t_eth] ProcessRX: bad ring_slot=%lu (eval=%lu)\n",
                               ring_slot, eval);
            unit->stat_rx_dropped++;
            goto next_desc;
        }

        if (actual_buf >= NUM_RX_BUFS || !unit->rx_bufs[actual_buf]) {
            IExec->DebugPrintF("[pa6t_eth] ProcessRX: bad actual_buf=%lu"
                               " (ring_slot=%lu eval=%lu)\n",
                               actual_buf, ring_slot, eval);
            unit->stat_rx_dropped++;
            goto next_desc;
        }

        /* Frame length  -- includes Ethernet header and FCS */
        uint32 frame_len = (uint32)((macrx & XCT_MACRX_LLEN_M) >>
                                     XCT_MACRX_LLEN_S);

        /* Strip 4-byte FCS */
        if (frame_len < 4) {
            IExec->DebugPrintF("[pa6t_eth] ProcessRX: frame_len=%lu too short\n",
                               frame_len);
            unit->stat_rx_dropped++;
            goto next_desc;
        }
        frame_len -= 4;

        if (frame_len < ETH_HDR_LEN) {
            IExec->DebugPrintF("[pa6t_eth] ProcessRX: truncated frame (%lu bytes)\n",
                               frame_len);
            unit->stat_rx_dropped++;
            goto next_desc;
        }

        uint8  *eth_frame = unit->rx_bufs[actual_buf];

        /* Validate PTR matches our buffer (detect split-frame errors) */
        {
            uint32 ptr_phys = (uint32)(ptr_word & XCT_PTR_ADDR_M);
            if (ptr_phys != unit->rx_bufs_phys[actual_buf]) {
                IExec->DebugPrintF("[pa6t_eth] ProcessRX: WARNING split frame!"
                                   " ptr_phys=0x%08lX expected=0x%08lX"
                                   " (eval=%lu ring_slot=%lu actual_buf=%lu"
                                   " frame_len=%lu)  -- increase RX_BUF_SIZE\n",
                                   ptr_phys, unit->rx_bufs_phys[actual_buf],
                                   eval, ring_slot, actual_buf, frame_len);
                unit->stat_rx_dropped++;
                goto next_desc;
            }
        }

        DPRINTFV(IExec, "[pa6t_eth] ProcessRX: actual_buf=%lu eth_frame=%p"
                 " phys=0x%08lX frame_len=%lu\n",
                 actual_buf, eth_frame,
                 unit->rx_bufs_phys[actual_buf], frame_len);
        uint16  ether_type = ((uint16)eth_frame[12] << 8) | eth_frame[13];
        uint8  *payload    = eth_frame + ETH_HDR_LEN;
        uint32  pay_len    = frame_len - ETH_HDR_LEN;

        DPRINTFV(IExec, "[pa6t_eth] RX: len=%lu type=0x%04X"
                 " src=%02X:%02X:%02X:%02X:%02X:%02X\n",
                 frame_len, (uint32)ether_type,
                 eth_frame[6], eth_frame[7], eth_frame[8],
                 eth_frame[9], eth_frame[10], eth_frame[11]);

        unit->stat_rx++;

        /* Deliver to a waiting CMD_READ from the per-type list */
        struct List *rlist;
        switch (ether_type) {
        case 0x0800: rlist = &unit->read_list_ipv4;    break;
        case 0x0806: rlist = &unit->read_list_arp;   break;
        default:     rlist = &unit->read_list_other; break;
        }

        IExec->Forbid();

        BOOL matched = FALSE;
        struct Node *rhead = rlist->lh_Head;
        if (rhead->ln_Succ) {
            /* Remove PendingRead wrapper and deliver */
            struct PendingRead *pr    = (struct PendingRead *)rhead;
            struct IOSana2Req  *ioreq = pr->ioreq;
            IExec->Remove(rhead);
            IExec->Permit();
            IExec->FreeVec(pr);

            ioreq->ios2_DataLength = pay_len;
            byte_copy(ioreq->ios2_SrcAddr, eth_frame + 6, ETH_ADDR_LEN);
            byte_copy(ioreq->ios2_DstAddr, eth_frame,     ETH_ADDR_LEN);

            ioreq->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
            if ((macrx & XCT_MACRX_CAST_M) == XCT_MACRX_CAST_BROAD)
                ioreq->ios2_Req.io_Flags |= SANA2IOF_BCAST;
            else if ((macrx & XCT_MACRX_CAST_M) == XCT_MACRX_CAST_MULTI)
                ioreq->ios2_Req.io_Flags |= SANA2IOF_MCAST;

            /* Call CopyToBuff via native PPC hook */
            {
                APTR cookie = ioreq->ios2_StatData;
                struct Opener *op = PA6TEth_FindOpener(unit, ioreq->ios2_BufferManagement);
                if (cookie && op && op->s2hook) {
                    struct SANA2CopyHookMsg msg;
                    msg.schm_Method  = S2_CopyToBuff;
                    msg.schm_MsgSize = sizeof(msg);
                    msg.schm_To      = cookie;
                    msg.schm_From    = payload;
                    msg.schm_Size    = pay_len;
                    BOOL ok = (BOOL)base->IUtility->CallHookPkt(
                        &op->s2hook->s2h_Hook, ioreq, &msg);
                    if (!ok) {
                        IExec->DebugPrintF("[pa6t_eth] ProcessRX: CopyToBuff failed\n");
                        ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                        ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
                    }
                } else if (cookie && op && !op->s2hook) {
                    IExec->DebugPrintF("[pa6t_eth] ProcessRX: no s2hook"
                                       "  -- frame dropped\n");
                    ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                    ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
                }
                ioreq->ios2_Data = cookie;
            }

            if (ioreq->ios2_Req.io_Error == 0)
                ioreq->ios2_WireError = 0;
            IExec->ReplyMsg((struct Message *)ioreq);

            matched = TRUE;
            goto next_desc;
        }

        /* ---- Try S2_READORPHAN (still in Forbid if no read_list match) ---- */
        struct Node *ehead = unit->event_list.lh_Head;
        while (ehead->ln_Succ) {
            struct Node       *next = ehead->ln_Succ;
            struct IOSana2Req *ev   = (struct IOSana2Req *)
                ((uint8 *)ehead - offsetof(struct IORequest, io_Message.mn_Node));

            if (ev->ios2_Req.io_Command == S2_READORPHAN) {
                IExec->Remove(ehead);
                IExec->Permit();

                ev->ios2_DataLength = pay_len;
                ev->ios2_PacketType = (ULONG)ether_type;
                byte_copy(ev->ios2_SrcAddr, eth_frame + 6, ETH_ADDR_LEN);
                byte_copy(ev->ios2_DstAddr, eth_frame,     ETH_ADDR_LEN);

                ev->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
                if ((macrx & XCT_MACRX_CAST_M) == XCT_MACRX_CAST_BROAD)
                    ev->ios2_Req.io_Flags |= SANA2IOF_BCAST;
                else if ((macrx & XCT_MACRX_CAST_M) == XCT_MACRX_CAST_MULTI)
                    ev->ios2_Req.io_Flags |= SANA2IOF_MCAST;

                {
                    APTR cookie = ev->ios2_StatData;
                    struct Opener *op = PA6TEth_FindOpener(unit, ev->ios2_BufferManagement);
                    if (cookie && op && op->s2hook) {
                        struct SANA2CopyHookMsg msg;
                        msg.schm_Method  = S2_CopyToBuff;
                        msg.schm_MsgSize = sizeof(msg);
                        msg.schm_To      = cookie;
                        msg.schm_From    = payload;
                        msg.schm_Size    = pay_len;
                        BOOL ok = (BOOL)base->IUtility->CallHookPkt(
                            &op->s2hook->s2h_Hook, ev, &msg);
                        if (!ok) {
                            ev->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                            ev->ios2_WireError    = S2WERR_BUFF_ERROR;
                        }
                    }
                    ev->ios2_Data = cookie;
                }

                if (ev->ios2_Req.io_Error == 0)
                    ev->ios2_WireError = 0;
                IExec->ReplyMsg((struct Message *)ev);

                matched = TRUE;
                goto next_desc;
            }
            ehead = next;
        }

        IExec->Permit();

        if (!matched) {
            unit->stat_rx_dropped++;
            if (ether_type == 0x0800 || ether_type == 0x0806)
                DPRINTF(IExec, "[pa6t_eth] SW DROP: type=0x%04X"
                               " total_sw_drops=%lu\n",
                               (uint32)ether_type, unit->stat_rx_dropped);
            else
                DPRINTF(IExec, "[pa6t_eth] ProcessRX: DROPPED type=0x%04X (no reader)"
                               " stat_rx_dropped=%lu\n",
                               (uint32)ether_type, unit->stat_rx_dropped);
        }

next_desc:
        /* Clear all 4 descriptor slots */
        RX_DESC(unit->rx_ring, n)   = 0;
        RX_DESC(unit->rx_ring, n+1) = 0;
        RX_DESC(unit->rx_ring, n+2) = 0;
        RX_DESC(unit->rx_ring, n+3) = 0;

        /* Zero the consumed buffer-ring slot */
        if (ring_slot < RX_RING_SIZE)
            RX_BUFF(unit->rx_buf_ring, ring_slot) = 0;

        /* Repost buffer at next sequential position (hardware reads in order) */
        if (actual_buf < NUM_RX_BUFS) {
            uint32 wp = unit->rx_buf_write_ptr;
            RX_BUFF(unit->rx_buf_ring, wp) =
                XCT_RXB_LEN(RX_BUF_SIZE) |
                XCT_RXB_ADDR(unit->rx_bufs_phys[actual_buf]);
            unit->rx_slot_buf[wp] = (uint16)actual_buf;
            unit->rx_buf_write_ptr = (wp + 1) & (RX_BUF_RING_SIZE - 1);
            replenish++;
        }

        n += 4;   /* do NOT mask here -- we need the raw value for Errata 5971 */
        count++;
    }

    /* Errata 5971: reset PKTHDRCNT on every RX batch to prevent
     * stale counters from misdirecting DMA. */
    if (count > 0)
        write_iob_mmio(base, PAS_IOB_COM_PKTHDRCNT, 0);

    if (n >= RX_RING_SIZE)
        n &= (RX_RING_SIZE - 1);

    unit->rx_next_clean = n;

    if (count > 0) {
        pasemi_wmb();
        write_dma_reg(base, PAS_DMA_RXCHAN_INCR((uint32)base->rx_channel_num),
                      count * 2);
        write_dma_reg(base, PAS_DMA_RXINT_INCR((uint32)base->dma_if),
                      replenish);
    }

    DPRINTFV(IExec, "[pa6t_eth] ProcessRX: %lu packets, %lu reposted\n",
             count, replenish);
}
