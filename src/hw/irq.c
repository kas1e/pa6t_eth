/*
 * hw/irq.c  -- PCI interrupt handler for pa6t_eth.
 *
 * Reads RXCH_STAT via iob_mmio (lwbrx), checks INTGEN, ACKs with
 * PCNTRST + PINTC (+ conditional TINTC/SINTC/DINTC from pasdma_status),
 * and signals the unit task to run ProcessRX.
 */

#include "pa6t_eth.h"

/* ---- PCI RX interrupt handler --------------------------------------------- */

static uint32 PA6TEth_InterruptHandler(struct ExceptionContext *ctx,
                                       struct ExecBase *SysBase,
                                       APTR is_Data)
{
    struct PA6TEthBase *base = (struct PA6TEthBase *)is_Data;
    (void)ctx;
    (void)SysBase;

    base->irq_enter++;

    /* Read RXCH_STAT via byte-swapped MMIO */
    uint32 rx_channel_num   = (uint32)base->rx_channel_num;
    uint32 rxch_stat = read_iob_mmio(base, PAS_IOB_DMA_RXCH_STAT(rx_channel_num));

    /* Not our channel */
    if (!(rxch_stat & PAS_IOB_DMA_RXCH_STAT_INTGEN)) {
        base->irq_miss++;
        return 0;
    }

    /* Read pasdma_status for cause bits (conditional TINTC/SINTC/DINTC) */
    uint64 ps = base->pasdma_status ? base->pasdma_status[rx_channel_num] : 0;

    /* ACK: PCNTRST atomically resets CNTDEL to 0, clear PINTC.
     * Avoids read-modify-write race that causes CNTDEL accumulation. */
    uint32 reset_val = PAS_IOB_DMA_RXCH_RESET_PCNTRST |
                       PAS_IOB_DMA_RXCH_RESET_PINTC;
    if (ps & PAS_STATUS_TIMER) reset_val |= PAS_IOB_DMA_RXCH_RESET_TINTC;
    if (ps & PAS_STATUS_SOFT)  reset_val |= PAS_IOB_DMA_RXCH_RESET_SINTC;
    if (ps & PAS_STATUS_ERROR) reset_val |= PAS_IOB_DMA_RXCH_RESET_DINTC;

    write_iob_mmio(base, PAS_IOB_DMA_RXCH_RESET(rx_channel_num), reset_val);

    /* Wake unit task to run ProcessRX */
    struct PA6TEthUnit *unit = base->unit;
    if (unit && unit->irq_wait_task)
        base->IExec->Signal(unit->irq_wait_task, unit->irq_signal_mask);

    base->irq_exit++;
    return 1;  /* claimed */
}

BOOL PA6TEth_InstallIRQ(struct PA6TEthBase *base)
{
    struct ExecIFace *IExec = base->IExec;

    DPRINTF(IExec, "[pa6t_eth] InstallIRQ: IOB=0x%08lX DMA=0x%08lX MAC=0x%08lX\n",
                   (ULONG)PA6T_IOB_ECAM, (ULONG)PA6T_DMA_ECAM, (ULONG)PA6T_MAC_ECAM);

    /* Per-channel DMA IRQ vector: MapInterrupt() + ntxch + rx_channel_num */
    ULONG ntxch = (base->dma_dev->ReadConfigLong(0x44) >> 16) & 0xff;
    base->irq_number = base->dma_dev->MapInterrupt() + ntxch + (ULONG)base->rx_channel_num;
    DPRINTF(IExec, "[pa6t_eth] InstallIRQ: DMA MapInterrupt=%lu ntxch=%lu"
                   " rx_channel_num=%ld => vector %lu\n",
                   base->dma_dev->MapInterrupt(), ntxch,
                   (long)base->rx_channel_num, base->irq_number);

    if (base->irq_number == 0) {
        IExec->DebugPrintF("[pa6t_eth] InstallIRQ: MapInterrupt failed\n");
        return FALSE;
    }

    base->irq_enter = 0;
    base->irq_exit  = 0;
    base->irq_miss  = 0;

    base->irq_handler.is_Node.ln_Type = NT_INTERRUPT;
    base->irq_handler.is_Node.ln_Pri  = 10;
    base->irq_handler.is_Node.ln_Name = DEVNAME;
    base->irq_handler.is_Data         = (APTR)base;
    base->irq_handler.is_Code         = (VOID (*)())PA6TEth_InterruptHandler;

    BOOL ok = IExec->AddIntServer(base->irq_number, &base->irq_handler);
    if (!ok) {
        IExec->DebugPrintF("[pa6t_eth] InstallIRQ: AddIntServer failed\n");
        return FALSE;
    }

    base->irq_installed = TRUE;
    DPRINTF(IExec, "[pa6t_eth] InstallIRQ: installed on vector %lu\n",
                   base->irq_number);

    return TRUE;
}

void PA6TEth_RemoveIRQ(struct PA6TEthBase *base)
{
    struct ExecIFace *IExec = base->IExec;

    if (base->irq_installed) {
        IExec->RemIntServer(base->irq_number, &base->irq_handler);
        base->irq_installed = FALSE;
        DPRINTF(IExec, "[pa6t_eth] RemoveIRQ: removed\n");
    }
}
