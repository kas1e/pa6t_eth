/*
 * hw/tx.c  -- TX ring operations for pa6t_eth.device.
 *
 * Fire-and-forget: submit descriptor pair (MACTX + PTR), reply
 * immediately, clean up previous TX buffer on next call.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>
#include <exec/errors.h>
#include <stddef.h>
#include <exec/exectags.h>

/* ---- PA6TEth_TransmitPacket ----------------------------------------------- */

static void tx_reply(struct ExecIFace *IExec, struct IOSana2Req *ioreq,
                     BYTE err, ULONG werr)
{
    ioreq->ios2_Req.io_Error = err;
    ioreq->ios2_WireError    = werr;
    if (ioreq->ios2_Req.io_Flags & IOF_QUICK)
        return;
    IExec->ReplyMsg((struct Message *)ioreq);
}

/* Wait for previous TX to finish and free its buffer. */
static void tx_cleanup_pending(struct PA6TEthBase *base, struct PA6TEthUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;

    if (!unit->tx_buf_pending)
        return;

    uint32 fill = unit->tx_fill_pending;
    uint32 tries = 0;

    while (tries < 50000) {
        if (!(TX_DESC(unit->tx_ring, fill) & XCT_MACTX_O))
            break;
        tries++;
    }

    if (TX_DESC(unit->tx_ring, fill) & XCT_MACTX_O) {
        IExec->Reschedule();
        if (TX_DESC(unit->tx_ring, fill) & XCT_MACTX_O) {
            uint32 sta = read_dma_reg(base,
                PAS_DMA_TXCHAN_TCMDSTA((uint32)base->tx_channel_num));
            IExec->DebugPrintF("[pa6t_eth] TX cleanup: hw stuck after %lu tries"
                               " TCMDSTA=0x%08lX  -- releasing anyway\n",
                               tries, sta);
        }
    }

    TX_DESC(unit->tx_ring, fill)     = 0;
    TX_DESC(unit->tx_ring, fill + 1) = 0;
    pasemi_wmb();

    IExec->EndDMA(unit->tx_buf_pending, unit->tx_buf_pending_len,
                  DMA_ReadFromRAM | DMAF_NoModify);
    IExec->FreeVec(unit->tx_buf_pending);
    unit->tx_buf_pending = NULL;
}

/* Transmit a frame.  Must be called from the network stack's task context. */
void PA6TEth_TransmitPacket(struct PA6TEthBase *base, struct PA6TEthUnit *unit,
                            struct IOSana2Req *ioreq)
{
    struct ExecIFace *IExec = base->IExec;

    if (!unit->tx_ring) {
        tx_reply(IExec, ioreq, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
        return;
    }

    uint32 pay_len    = ioreq->ios2_DataLength;
    uint32 frame_len  = ETH_HDR_LEN + pay_len;

    if (frame_len < 60)  frame_len = 60;   /* minimum Ethernet frame */
    if (frame_len > ETH_MAX_FRAME) {
        IExec->DebugPrintF("[pa6t_eth] TX: frame too large (%lu)\n", frame_len);
        tx_reply(IExec, ioreq, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
        return;
    }

    /* Allocate a temporary TX buffer in shared (DMA-able) memory */
    uint8 *tx_buf = (uint8 *)IExec->AllocVecTags(frame_len,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      64,
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!tx_buf) {
        IExec->DebugPrintF("[pa6t_eth] TX: alloc failed\n");
        tx_reply(IExec, ioreq, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
        return;
    }

    /* Build Ethernet header */
    byte_copy(tx_buf,                ioreq->ios2_DstAddr, ETH_ADDR_LEN);
    byte_copy(tx_buf + ETH_ADDR_LEN, base->mac_addr,      ETH_ADDR_LEN);
    uint16 etype = (uint16)(ioreq->ios2_PacketType & 0xFFFF);
    tx_buf[12] = (uint8)(etype >> 8);
    tx_buf[13] = (uint8)(etype & 0xFF);

    /* Copy payload via native PPC hook */
    {
        APTR cookie = ioreq->ios2_StatData;
        struct Opener *op = PA6TEth_FindOpener(unit, ioreq->ios2_BufferManagement);

        DPRINTFV(IExec, "[pa6t_eth] TX: type=0x%04X len=%lu dst=%02X:%02X:%02X:%02X:%02X:%02X\n",
                 (uint32)etype, pay_len,
                 ioreq->ios2_DstAddr[0], ioreq->ios2_DstAddr[1],
                 ioreq->ios2_DstAddr[2], ioreq->ios2_DstAddr[3],
                 ioreq->ios2_DstAddr[4], ioreq->ios2_DstAddr[5]);

        if (!cookie || !op || !op->s2hook) {
            IExec->DebugPrintF("[pa6t_eth] TX: missing cookie or s2hook\n");
            ioreq->ios2_Data = cookie;
            IExec->FreeVec(tx_buf);
            tx_reply(IExec, ioreq, S2ERR_BAD_ARGUMENT, S2WERR_GENERIC_ERROR);
            return;
        }

        struct SANA2CopyHookMsg msg;
        msg.schm_Method  = S2_CopyFromBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To      = tx_buf + ETH_HDR_LEN;
        msg.schm_From    = cookie;
        msg.schm_Size    = pay_len;
        BOOL ok = (BOOL)base->IUtility->CallHookPkt(
            &op->s2hook->s2h_Hook, ioreq, &msg);

        ioreq->ios2_Data = cookie;

        if (!ok) {
            IExec->DebugPrintF("[pa6t_eth] TX: CopyFromBuff failed\n");
            IExec->FreeVec(tx_buf);
            tx_reply(IExec, ioreq, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            return;
        }
    }

    /* DMA-map the TX buffer to get its physical address */
    uint32 tx_phys = 0;
    {
        uint32 nentries = IExec->StartDMA(tx_buf, frame_len, DMA_ReadFromRAM);
        if (nentries == 0) {
            IExec->DebugPrintF("[pa6t_eth] TX: StartDMA failed\n");
            IExec->FreeVec(tx_buf);
            tx_reply(IExec, ioreq, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
            return;
        }

        struct DMAEntry *dlist = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, nentries, TAG_DONE);
        if (!dlist) {
            IExec->EndDMA(tx_buf, frame_len, DMA_ReadFromRAM | DMAF_NoModify);
            IExec->FreeVec(tx_buf);
            tx_reply(IExec, ioreq, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
            return;
        }
        IExec->GetDMAList(tx_buf, frame_len, DMA_ReadFromRAM, dlist);
        tx_phys = (uint32)dlist[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, dlist);
    }

    /* Acquire ring lock, clean up previous TX, submit new one */
    IExec->ObtainSemaphore(&base->tx_lock);

    /* Wait for previous fire-and-forget TX to complete and free its buffer */
    tx_cleanup_pending(base, unit);

    uint32 fill = unit->tx_next_fill;

    /* Build descriptors */
    uint64 desc0 = XCT_MACTX_O | XCT_MACTX_ST | XCT_MACTX_CRC_PAD |
                   XCT_MACTX_LLEN(frame_len);
    uint64 desc1 = XCT_PTR_LEN(frame_len) | XCT_PTR_ADDR(tx_phys);

    TX_DESC(unit->tx_ring, fill)     = desc0;
    TX_DESC(unit->tx_ring, fill + 1) = desc1;

    pasemi_wmb();

    unit->tx_next_fill = (fill + 2) & (TX_RING_SIZE - 1);

    /* Clear DB (W1C) then ring doorbell */
    write_dma_reg(base, PAS_DMA_TXCHAN_TCMDSTA((uint32)base->tx_channel_num),
                  PAS_DMA_TXCHAN_TCMDSTA_EN | PAS_DMA_TXCHAN_TCMDSTA_DB);
    write_dma_reg(base, PAS_DMA_TXCHAN_INCR((uint32)base->tx_channel_num), 1);

    /* Store buffer info for cleanup on next TX call */
    unit->tx_buf_pending     = tx_buf;
    unit->tx_buf_pending_len = frame_len;
    unit->tx_fill_pending    = fill;

    IExec->ReleaseSemaphore(&base->tx_lock);

    /* Reply immediately -- frame is queued */
    unit->stat_tx++;
    tx_reply(IExec, ioreq, 0, 0);

    /* Signal unit task to run ProcessRX (reply may arrive shortly after TX) */
    if (unit->online && unit->irq_wait_task)
        IExec->Signal(unit->irq_wait_task, unit->irq_signal_mask);
}
