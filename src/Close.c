/*
 * Close.c  -- CloseDevice() for pa6t_eth.device.
 *
 * Removes the matching Opener, decrements the open count, and shuts
 * down the unit task when the last opener closes.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self);

BPTR _manager_Close(struct DeviceManagerInterface *Self,
                    struct IOSana2Req *ioreq)
{
    struct PA6TEthBase *base  = (struct PA6TEthBase *)Self->Data.LibBase;
    struct ExecIFace   *IExec = base->IExec;
    struct PA6TEthUnit *unit  = (struct PA6TEthUnit *)ioreq->ios2_Req.io_Unit;
    BPTR seglist = (BPTR)NULL;

    DPRINTF(IExec, "[pa6t_eth] Close: unit ptr %p open_count %lu\n",
            unit, unit ? unit->open_count : 0UL);

    if (unit && unit != (struct PA6TEthUnit *)-1) {
        /*
         * Remove the opener whose bm_cookie matches ios2_BufferManagement.
         */
        APTR bm = ioreq->ios2_BufferManagement;
        IExec->ObtainSemaphore(&unit->opener_lock);
        struct Opener *op = (struct Opener *)unit->opener_list.lh_Head;
        while (op->node.mln_Succ) {
            struct Opener *next = (struct Opener *)op->node.mln_Succ;
            if (op->bm_cookie == bm) {
                IExec->Remove((struct Node *)&op->node);
                IExec->FreeVec(op);
                break;
            }
            op = next;
        }
        IExec->ReleaseSemaphore(&unit->opener_lock);

        if (unit->open_count > 0) {
            unit->open_count--;
            if (unit->open_count == 0) {
                UnitTask_Shutdown(base, unit);
            }
        }
    }

    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;

    base->dev_Base.dd_Library.lib_OpenCnt--;

    if (base->dev_Base.dd_Library.lib_OpenCnt == 0 &&
        (base->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP))
    {
        seglist = _manager_Expunge(Self);
    }

    return seglist;
}
