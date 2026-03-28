/*
 * Expunge.c  -- library expunge for pa6t_eth.device.
 *
 * Called when the OS wants to unload the device (open count == 0).
 * Tears down the interrupt, hardware rings, and all library handles.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct PA6TEthBase *base  = (struct PA6TEthBase *)Self->Data.LibBase;
    struct ExecIFace   *IExec = base->IExec;
    BPTR seglist = (BPTR)NULL;

    DPRINTF(IExec, "[pa6t_eth] Expunge: OpenCnt=%u\n",
            base->dev_Base.dd_Library.lib_OpenCnt);

    if (base->dev_Base.dd_Library.lib_OpenCnt != 0) {
        base->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
        return (BPTR)NULL;
    }

    seglist = base->dev_SegList;
    IExec->Remove((struct Node *)base);

    /* Remove interrupt handler first */
    PA6TEth_RemoveIRQ(base);

    /* Tear down DMA rings and free RX buffers */
    PA6TEth_HWCleanup(base);

    /* Free unit struct */
    if (base->unit) {
        IExec->FreeVec(base->unit);
        base->unit = NULL;
    }

    /* Release PCI device handles */
    if (base->iob_dev) {
        base->IPCI->FreeDevice(base->iob_dev);
        base->iob_dev = NULL;
    }
    if (base->dma_dev) {
        base->IPCI->FreeDevice(base->dma_dev);
        base->dma_dev = NULL;
    }
    if (base->mac_dev) {
        base->IPCI->FreeDevice(base->mac_dev);
        base->mac_dev = NULL;
    }

    if (base->IUtility)     IExec->DropInterface((struct Interface *)base->IUtility);
    if (base->UtilityBase)  IExec->CloseLibrary(base->UtilityBase);
    if (base->IPCI)         IExec->DropInterface((struct Interface *)base->IPCI);
    if (base->ExpansionBase) IExec->CloseLibrary(base->ExpansionBase);

    IExec->DeleteLibrary((struct Library *)base);
    return seglist;
}
