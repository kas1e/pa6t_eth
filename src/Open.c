/*
 * Open.c  -- OpenDevice() implementation for pa6t_eth.device.
 *
 * Allocates an Opener struct and starts the unit task on the first open.
 * Copy hook is installed later by S2_SANA2HOOK (BeginIO).
 */

#include "pa6t_eth.h"
#include <exec/errors.h>
#include <exec/exec.h>

struct PA6TEthBase *_manager_Open(struct DeviceManagerInterface *Self,
                                  struct IOSana2Req *ioreq,
                                  ULONG unitNum, ULONG flags)
{
    struct PA6TEthBase *base  = (struct PA6TEthBase *)Self->Data.LibBase;
    struct ExecIFace   *IExec = base->IExec;

    base->dev_Base.dd_Library.lib_OpenCnt++;

    DPRINTF(IExec, "[pa6t_eth] Open: unit %lu flags 0x%lX\n", unitNum, flags);

    if (unitNum != 0) {
        IExec->DebugPrintF("[pa6t_eth] Open: unit %lu requested but this device"
                           " only has unit 0 -- check network preferences\n",
                           unitNum);
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        goto fail;
    }

    struct PA6TEthUnit *unit = base->unit;
    if (!unit) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        goto fail;
    }

    /*
     * Allocate and register the opener.  The native copy hook is installed
     * later when Roadshow sends S2_SANA2HOOK via BeginIO.
     */
    struct Opener *opener = (struct Opener *)IExec->AllocVecTags(
        sizeof(struct Opener),
        AVT_Type,           MEMF_SHARED,   /* SHARED: unit task reads this too */
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!opener) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        goto fail;
    }

    opener->bm_cookie = ioreq->ios2_BufferManagement;
    /* s2hook is set later by S2_SANA2HOOK (BeginIO)  -- NULL until then */

    /* Start unit task on first open */
    if (unit->open_count == 0) {
        if (!UnitTask_Start(base, unit)) {
            DPRINTF(IExec, "[pa6t_eth] Open: UnitTask_Start failed\n");
            IExec->FreeVec(opener);
            ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            goto fail;
        }
    }

    /* Add opener to the unit's list */
    IExec->ObtainSemaphore(&unit->opener_lock);
    IExec->AddTail((struct List *)&unit->opener_list,
                   (struct Node *)&opener->node);
    IExec->ReleaseSemaphore(&unit->opener_lock);

    unit->open_count++;
    ioreq->ios2_Req.io_Unit                    = (struct Unit *)unit;
    ioreq->ios2_Req.io_Error                   = 0;
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    base->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;
    return base;

fail:
    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;
    base->dev_Base.dd_Library.lib_OpenCnt--;
    return NULL;
}
