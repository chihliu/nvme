/*******************************************************************************
 * Portions Copyright (c) 2013-2015  VMware, Inc. All rights reserved.
 *
 * Portions Copyright (c) 2012-2014, Micron Technology, Inc.
 *
 * Portions Copyright (c) 2012-2013  Integrated Device Technology, Inc.
 * All rights reserved.
 *******************************************************************************/

/*
 * @file: nvme_scsi_io.c --
 *
 *    Low level IO.
 */
#include "oslib.h"
//#include "../../common/kernel/nvme_private.h"
//#include "nvme_debug.h"
#include "../../common/kernel/nvme_scsi_cmds.h"

/**
 * @brief This function prepares a prp list.
 *
 * @param[in] qinfo pointer to queue information block
 * @param[in] cmdInfo pointer to command information blcck
 *
 * @return This function returns int 0 if successful, otherwise Error Code
 *
 * @note It is assumed that queue lock is held by caller.
 */
vmk_ByteCount
NvmeIo_ProcessPrps(struct NvmeQueueInfo *qinfo, struct NvmeCmdInfo *cmdInfo)
{
   vmk_int64           dmaLen, offset;
   vmk_int64           length, processed;
   vmk_IOA             dmaAddr;
   int                 elemIndex;
   int                 thisPrpLen;
   vmk_SgArray        *sgArray = cmdInfo->sgPosition.sg;
   struct nvme_prp    *prps;
   struct NvmeCmdInfo *cmdBase = cmdInfo->cmdBase;
   struct NvmeNsInfo  *ns = cmdInfo->ns;
   struct NvmeCtrlr   *ctrlr = qinfo->ctrlr;
   vmk_ScsiCommand    *vmkCmd;
   vmk_uint64          lba;
   int                 stripeSize;
   int                 stripeRest;

   VMK_ASSERT(cmdInfo->sgPosition.type == VMK_SG_POSITION_TYPE_ELEMENT);
   VMK_ASSERT(cmdInfo->sgPosition.sg != NULL);
   /**
    * We should always split at offset 0 of an SG element unless this is an Intel stripe
    * limit device
    */
   VMK_ASSERT(NvmeCtrlr_IsIntelStripeLimit(ctrlr) ||
              cmdInfo->sgPosition.element.offset == 0);
   VMK_ASSERT(cmdInfo->sgPosition.sg->numElems > cmdInfo->sgPosition.element.element);

   elemIndex = cmdInfo->sgPosition.element.element;
   prps      = cmdInfo->nvmeCmd.header.prp;
   processed = 0;
   /*
    * maximum possible bytes to be transferred in this command
    * (e.g. all remaining SG elements are logically virtual contiguous)
    */
   length    = cmdBase->requiredLength - cmdBase->requestedLength;

   dmaAddr   = sgArray->elem[elemIndex].ioAddr + cmdInfo->sgPosition.element.offset;
   dmaLen    = sgArray->elem[elemIndex].length - cmdInfo->sgPosition.element.offset;
   offset    = dmaAddr & VMK_PAGE_MASK;

   DPRINT_CMD("length %ld, dma_addr 0x%0lx, offset 0x%0lx, dma_len %ld",
              length,  dmaAddr, offset, dmaLen);

   prps->addr = dmaAddr;
   prps ++;

   thisPrpLen  = min_t(vmk_uint32, dmaLen, (VMK_PAGE_SIZE - offset));

   /**
    * For Intel stripe limit device, we need to split IOs accross the stripe boundry
    * in driver to avoid the significant performance overhead in firmware.
    *
    * e.g. if the stripe size is 128KB, the command whose lba = 127, lbc = 8 needs
    * to be splited as two commands, see below.
    *
    *                               cmd1   cmd2
    *                                .      .
    *                               /|\    /|\
    *                                |      |
    *                               1K     3K
    * +----------------------------------------------------------------+
    * |                            |***|*********|                     |
    * +----------------------------------------------------------------+
    * 0                          127  128       131                   256KB
    *
    * We only do this check and split for regular read and write IO.
    * For admin command, we don't split because data buffer is already page size and
    * page aligned.
    *
    */
   if (NvmeCtrlr_IsIntelStripeLimit(ctrlr) && cmdInfo->type == BIO_CONTEXT) {
      GET_VMK_SCSI_CMD(cmdBase->cmdPtr, vmkCmd);
      lba = vmkCmd->lba;
      stripeSize = ctrlr->stripeSize;
      stripeRest = stripeSize - (((lba << ns->lbaShift) + cmdBase->requestedLength)
                                & (stripeSize-1));
      /**
       * Limit the xfered length of current command in one stripe.
       */
      length = min_t(vmk_uint32, stripeRest,
                     (cmdBase->requiredLength - cmdBase->requestedLength));
      dmaLen = min_t(vmk_uint32, dmaLen, length);
      DPRINT_CMD("stripeRest: %d, requiredLength: %ld, requestedLength:%d", stripeRest,
                 cmdBase->requiredLength, cmdBase->requestedLength);
      thisPrpLen  = min_t(vmk_uint32, thisPrpLen, length);
   }


   length     -= thisPrpLen;
   processed  += thisPrpLen;
   dmaAddr    += thisPrpLen;
   dmaLen     -= thisPrpLen;

   /*
    * Fast track for small payloads
    */
   if (length <= 0) {
      /* Only 1 prp entry (prp0) is sufficient */
      return processed;
   }

   /*
    * More than a single entry, should use prp list.
    *
    * Note: it is also possible that prp1 is sufficient instead of using prp
    *       entry list. We will check that condition after we have completed
    *       processing the list by reaching the end or finding a split point.
    */
   prps->addr = cmdInfo->prpPhy;
   prps = cmdInfo->prps;
   DPRINT_CMD("List PRP1 %016lx, PRP2 %016lx, length %ld",
      cmdInfo->nvmeCmd.header.prp[0].addr,
      cmdInfo->nvmeCmd.header.prp[1].addr,
      length);

   /*
    * Process the rest of sglist.
    *
    * Note: We will stop if we reach the end of the SG array (in case of length
    *       <= 0), or the SG element being processed is not virtually contiguous
    *       (in case of (dmaAddr & VMK_PAGE_MASK) is non-zero. In the latter case,
    *       we need to break out and split into another NVMe command to process
    *       the rest of the IO since NVMe can only process virtually contiguous
    *       SG PRPs in a single command.
    */
   while (length > 0 && ((dmaAddr & VMK_PAGE_MASK) == 0)) {
      if (dmaLen > 0) {
         prps->addr = dmaAddr;
         prps++;
         thisPrpLen = min_t(vmk_uint32, dmaLen, VMK_PAGE_SIZE);
         length    -= thisPrpLen;
         processed += thisPrpLen;
         dmaAddr   += thisPrpLen;
         dmaLen    -= thisPrpLen;
      } else {
         elemIndex ++;
         dmaAddr = sgArray->elem[elemIndex].ioAddr;
         dmaLen = sgArray->elem[elemIndex].length;
      }
   }

   DPRINT_CMD("processed: %ld, length: %ld, elemIndex: %d, numElems: %d",
              processed, length, elemIndex, sgArray->numElems);
   /*
    * When we completed processing the list, we should either have length > 0
    * with valid elemIndex, or length == 0 and elemIndex points to the last
    * element in the array unless this is an Intel stripe limit device
    */
   VMK_ASSERT(NvmeCtrlr_IsIntelStripeLimit(ctrlr) ||
              (length > 0 && elemIndex < sgArray->numElems) ||
              (length == 0 && elemIndex == sgArray->numElems - 1));

   /*
    * If we break when prps points to the second entry in prp list, that
    * means two PRP entries are sufficient for carrying this request, and we
    * should move cmdInfo->prps[0] to nvmeCmd->header.prp[1] for such case.
    */
   if (prps == &(cmdInfo->prps[1])) {
      vmk_ScsiCommand *vmkCmd;

      GET_VMK_SCSI_CMD(cmdInfo->cmdPtr, vmkCmd);

      cmdInfo->nvmeCmd.header.prp[1].addr = cmdInfo->prps[0].addr;
      DPRINT_CMD("using prp1 for io: vmkCmd %p base %p info %p prp0 0x%lx prp1 0x%lx",
         vmkCmd, cmdBase, cmdInfo, cmdInfo->nvmeCmd.header.prp[0].addr,
         cmdInfo->nvmeCmd.header.prp[1].addr);
   }

   /**
    * Done processing
    *
    * Total number of processed bytes plus remaining length should match up with the
    * total number of bytes left for the base request unless this is an Intel stripe
    * limit device.
    */
   VMK_ASSERT(NvmeCtrlr_IsIntelStripeLimit(ctrlr) || (processed + length) ==
      (cmdBase->requiredLength - cmdBase->requestedLength));

   return (processed);
}


/**
 * @brief This function called to generate scatter gather list
 *
 * Note: It is assumed that queue lock is held by caller.
 *
 * @param[in] qinfo pointer to queue information block
 * @param[in] cmdInfo pointer to command information block
 * @param[in] vmkCmd pointer to block io request
 * @param[in] dmaDir Data direction
 *
 * @return This function returns int length, number of bytes transferred.
 *
 */
vmk_ByteCount
NvmeIo_ProcessSgArray(struct NvmeQueueInfo *qinfo, struct NvmeCmdInfo *cmdInfo,
   vmk_ScsiCommand *vmkCmd, vmk_DMADirection dmaDir)
{
   VMK_ReturnStatus vmkStatus;
   int nsegs;
   vmk_ByteCount length;
   struct NvmeCmdInfo *cmdBase = cmdInfo->cmdBase;
   vmk_SgArray *sgArray;

   if (cmdInfo->bounceBufferType & DATA_BOUNCE_BUFFER) {
      sgArray = cmdInfo->dataDmaEntry.sgOut;
   } else {
      sgArray = vmkCmd->sgIOArray;
   }

   /*
    * TODO: this should never happen. Maybe just igore this check
    */
   nsegs = sgArray->numElems;
   if (nsegs > max_prp_list) {
      EPRINT("nsegs %d max_prp_list %d out of range.", nsegs, max_prp_list);
      /*
       * Ideally PSA should prevent such thing from happening. If it does
       * happen, a DATA UNDERRUN should be generated.
       */
      VMK_ASSERT(0);
      return 0;
   }

   /*
    * Figure out where we should start processing the SG array.
    */
   vmkStatus = vmk_SgFindPosition(sgArray, cmdBase->requestedLength,
      &cmdInfo->sgPosition);
   if (vmkStatus != VMK_OK) {
      /* out of range, should never happen. */
      EPRINT("Invalid position, vmkCmd %p, lba 0x%lx, lbc %d, cmdInfo %p, cmdBase %p, reqeustedLen %d.",
         vmkCmd, vmkCmd->lba, vmkCmd->lbc, cmdInfo, cmdBase, cmdBase->requestedLength);
      VMK_ASSERT(0);
      return 0;
   }

   /* Convert SG array starting at this position to prp lists.
    *
    * Nvme_ProcessPrps returns the length that has been processed.
    */
   length = NvmeIo_ProcessPrps(qinfo, cmdInfo);

   cmdInfo->count = length;

#if NVME_DEBUG
   if (nvme_dbg & NVME_DEBUG_DUMP_PRP) {
      NvmeDebug_DumpPrps(cmdInfo);
   }
#endif
   return length;
}

static VMK_ReturnStatus
copySgData(vmk_SgArray *dst, vmk_SgArray *src, vmk_ByteCount length)
{
   vmk_SgPosition srcPos, dstPos;
   vmk_ByteCount copied = 0;
   VMK_ReturnStatus status;

   VMK_ASSERT(src != NULL);
   VMK_ASSERT(dst != NULL);

   srcPos.type = VMK_SG_POSITION_TYPE_ELEMENT;
   srcPos.sg = src;
   srcPos.element.element = 0;
   srcPos.element.offset = 0;
   dstPos.type = VMK_SG_POSITION_TYPE_ELEMENT;
   dstPos.sg = dst;
   dstPos.element.element = 0;
   dstPos.element.offset = 0;

   status = vmk_SgCopyData(&dstPos, &srcPos, length, &copied);
   VMK_ASSERT(length == copied);
   return status;
}

/**
 * scsiIoCompleteCommand - completion callback for I/O commands
 */
static void
scsiIoCompleteCommand(struct NvmeQueueInfo *qinfo, struct NvmeCmdInfo *cmdInfo)
{
   vmk_ScsiCommand    *vmkCmd;
   struct NvmeCmdInfo *baseInfo;
   Nvme_Status         nvmeStatus;
   struct nvme_cmd         *cmd;

#if NVME_DEBUG
   DPRINT_CMD("compl q %p[%d] cmdInfo %p[%d].", qinfo, qinfo->id, cmdInfo, cmdInfo->cmdId);
#endif

   vmk_AtomicWrite32(&cmdInfo->atomicStatus, NVME_CMD_STATUS_DONE);

   if (cmdInfo->cmdStatus) {
      /**
       * TODO:
       *
       * 1. If this is a sub-command:
       *    Propagate the error code to base command later while holding
       *    the queue lock
       * 2. If this is a base command:
       *    1. set Scsi return code based on cmdInfo->cmdStatus;
       *    2. complete vmkCmd;
       */
      nvmeStatus = cmdInfo->cmdStatus;
   } else {
      nvmeStatus = NVME_STATUS_SUCCESS;
   }

   /**
    * TODO:
    *
    * Check the status of the controller and queue
    *
    * This is mainly for detecting updated controller/queue status, i.e. if the
    * controller has been hot removed.
    */


   cmd = &cmdInfo->nvmeCmd;
#if USE_TIMER
   /**
    * Only check Read/Write timeout
    */
   if((cmd->header.opCode == NVM_CMD_READ) ||
      (cmd->header.opCode == NVM_CMD_WRITE)) {
      vmk_AtomicInc32(&qinfo->timeoutComplCount[cmdInfo->timeoutId]);
   }
#endif

   /**
    * Proceed to I/O handling
    */

   if (cmdInfo->cmdBase) {
      /**
       * We either have a split command here, or an IO completed before
       * the submission code actually returned. The submission path holds
       * the queue lock, and we need to synchronize with that lock here.
       *
       * We don't need to hold qinfo lock when this function is called from
       * FlushQueue since the lock is already grabbed there, in which case,
       * cmdInfo->type is set as ABORT_CONTEXT. See PR #1964335.
       */

      if (cmdInfo->type == BIO_CONTEXT) {
         LOCK_FUNC(qinfo);
      }

      /**
       * Test again, under the lock, that cmdBase is still not NULL
       */
      if (cmdInfo->cmdBase) {
         baseInfo = cmdInfo->cmdBase;

         if (cmdInfo->cmdStatus) {
            /**
             *  Propagate the error code, if any to the cmdBase, while
             *  holding the lock
             */
            if ((cmdInfo->cmdStatus) &&
                (cmdInfo->cmdBase->cmdStatus == NVME_STATUS_SUCCESS)) {
               cmdInfo->cmdBase->cmdStatus = cmdInfo->cmdStatus;
            }
         }

         if (baseInfo != cmdInfo) {
            /**
             * This is a splitted command
             */
            qinfo->nrSplit --;
            DPRINT_SPLIT("cmd %p [%d] qid %d base %p [%d] count %d, status 0x%x, nrSplit %d",
               cmdInfo, cmdInfo->cmdId, qinfo->id, baseInfo,
               baseInfo->cmdId, baseInfo->cmdCount, cmdInfo->cmdStatus,
               qinfo->nrSplit);
            NvmeCore_PutCmdInfo(qinfo, cmdInfo);
         }

         if (baseInfo->cmdCount -- > 1) {
            /**
             * This is not the last splitted command, continue processing.
             */
            if (cmdInfo->type == BIO_CONTEXT) {
               UNLOCK_FUNC(qinfo);
            }
            return;
         }
         cmdInfo = baseInfo;
         DPRINT_SPLIT("Complete baseCmd %p [%d], vmkCmd %p, status 0x%x, qid %d, nrSplit %d",
            baseInfo, baseInfo->cmdId, baseInfo->cmdPtr, baseInfo->cmdStatus, qinfo->id,
            qinfo->nrSplit);
      }

      if (cmdInfo->type == BIO_CONTEXT) {
         UNLOCK_FUNC(qinfo);
      }
   }
   GET_VMK_SCSI_CMD(cmdInfo->cmdPtr, vmkCmd);

   VMK_ASSERT(vmkCmd);

   cmd = &cmdInfo->nvmeCmd;
   if((cmd->header.opCode == NVM_CMD_READ) ||
      (cmd->header.opCode == NVM_CMD_WRITE)) {
      /* Check OVERRUN/UNDERRUN for READ and WRITE commands.
       * Other commands dont need this because no bytesXferred reported by hw
       */
      vmkCmd->bytesXferred = cmdInfo->requestedLength;
      if ( vmkCmd->bytesXferred != (vmkCmd->lbc << cmdInfo->ns->lbaShift)) {
         if (vmkCmd->bytesXferred < (vmkCmd->lbc << cmdInfo->ns->lbaShift)) {
            /**
             * UNDERRUN condition
             */
            nvmeStatus = NVME_STATUS_UNDERRUN;
         } else {
            nvmeStatus = NVME_STATUS_OVERRUN;
         }

         WPRINT("vmkCmd %p[%Xh I:%p SN:0x%lx] %s %d/%d.",
                vmkCmd, vmkCmd->cdb[0], vmkCmd->cmdId.initiator,
                vmkCmd->cmdId.serialNumber,
                nvmeStatus == NVME_STATUS_UNDERRUN ?
                  "UNDERRUN" : "OVERRUN",
                vmkCmd->bytesXferred,
                (vmkCmd->lbc << cmdInfo->ns->lbaShift));
      }
      /* Check whether using bounce buffer for READ and WRITE commands.*/
      if (cmdInfo->bounceBufferType) {
#if ((NVME_PROTECTION) && (VMKAPIDDK_VERSION >= 600))
         if (cmdInfo->bounceBufferType & PROT_BOUNCE_BUFFER) {
            if (cmd->header.opCode == NVM_CMD_READ) {
               copySgData(vmk_ScsiCmdGetProtSgArray(vmkCmd),
                          cmdInfo->protDmaEntry.sgOut,
                          cmdInfo->protDmaEntry.size);
            }
            OsLib_DmaFree(&qinfo->ctrlr->ctrlOsResources, &cmdInfo->protDmaEntry);
         }
#endif
         if (cmdInfo->bounceBufferType & DATA_BOUNCE_BUFFER) {
            if (cmd->header.opCode == NVM_CMD_READ) {
               copySgData(vmkCmd->sgIOArray,
                          cmdInfo->dataDmaEntry.sgOut,
                          cmdInfo->dataDmaEntry.size);
            }
            OsLib_DmaFree(&qinfo->ctrlr->ctrlOsResources, &cmdInfo->dataDmaEntry);
         }
         cmdInfo->bounceBufferType = 0;
      }
   }

   NvmeScsiCmd_SetReturnStatus(cmdInfo->cmdPtr, nvmeStatus);

   /**
    * If in coredump context, complete the command by calling vmkCmd->done,
    * this callback is updated in function vmk_ScsiIssueSyncDumpCommand.
    */
   if (cmdInfo->isDumpCmd) {
      vmkCmd->done(vmkCmd);
   } else {
#if NVME_MUL_COMPL_WORLD
      OsLib_IOCompletionEnQueue(qinfo->ctrlr, vmkCmd);
#else
      SCSI_CMD_INVOKE_COMPLETION_CB(cmdInfo->cmdPtr);
#endif
   }

   NvmeCore_PutCmdInfo(qinfo, cmdInfo);
}


/**
 * NvmeIo_SubmitIoRequest - submit an I/O command to an I/O queue.
 *
 * @param [IN]  qinfo     pointer to the completion queue
 * @param [IN]  ns        pointer to namespace info
 * @param [IN]  vmkCmd    poniter to SCSI command to be processed
 * @param [IN]  retries   retry counts
 *
 * @return      NVME_STATUS_WOULD_BLOCK  if the command is submitted to hardware
 *                                       for processing sucessfully, the command
 *                                       will be returned in completion context.
 *              others                   failed to submit command to hardware,
 *                                       the command needs to be terminated and
 *                                       returned to storage stack immediately.
 *
 */
static Nvme_Status
NvmeIo_SubmitIoRequest(struct NvmeQueueInfo *qinfo, struct NvmeNsInfo *ns,
                        void *cmdPtr, int retries)
{
   Nvme_Status              nvmeStatus;
   struct NvmeCtrlr        *ctrlr  = qinfo->ctrlr;
   struct NvmeCmdInfo      *cmdInfo, *baseInfo;
   struct nvme_cmd         *cmd;
   vmk_ByteCount            length;
   vmk_DMADirection         dmaDir;
   vmk_ScsiCommand         *vmkCmd;

   vmk_Bool                 protPass = 0;
   vmk_uint32               bounceBufferType = 0;
   vmk_uint8                prChk = 0;
   vmk_SgArray             *protSgArray = NULL;
   VMK_ReturnStatus         vmkStatus;
   vmk_uint64               protLen = 0;
#if ((NVME_PROTECTION) && (VMKAPIDDK_VERSION >= 600))
   vmk_ScsiCommandProtOps   protOps;
   vmk_ScsiTargetProtTypes  protType;
#endif

   GET_VMK_SCSI_CMD(cmdPtr, vmkCmd);

   /**
    * TODO:
    *
    * 1. Check ctrlr status here
    * 2. Check NS status here
    * 3. Check congestion queue states here
    * 4. Check protection info here
    */

   if (!(ns->flags & NS_ONLINE)) {
      DPRINT_NS("*** ERROR *** Received request while Offlined. ns_id %d",
                    ns->id);
      return NVME_STATUS_QUIESCED;
   }

   if (NvmeState_GetCtrlrState(ctrlr) == NVME_CTRLR_STATE_INRESET) {
      DPRINT_CTRLR("****** Error Completion Command %p, dev state %d",
                    vmkCmd, NvmeState_GetCtrlrState(ctrlr));
      return NVME_STATUS_IN_RESET;
   }

#if ((NVME_PROTECTION) && (VMKAPIDDK_VERSION >= 600))
   vmk_ScsiCmdGetTargetProtType(vmkCmd, &protType);
   vmk_ScsiCmdGetProtOps(vmkCmd, &protOps);

   if (END2END_DPS_TYPE(ns->dataProtSet) == 0) {
      if (protOps != VMK_SCSI_COMMAND_PROT_NORMAL) {
         DPRINT_CMD("*** ERROR *** Received DIFDIX capable command while ns is not in PI enabled format");
         return NVME_STATUS_INVALID_PI;
      }
   } else {
      DPRINT_CMD("Cmd %p[0x%x], protType %d, protOps %d", vmkCmd, vmkCmd->cdb[0], protType, protOps);
      if (protOps == VMK_SCSI_COMMAND_PROT_READ_INSERT || protOps == VMK_SCSI_COMMAND_PROT_WRITE_STRIP) {
         DPRINT_CMD("*** ERROR *** Unsupported protection operation 0x%x", protOps);
         return NVME_STATUS_INVALID_PI;
      }

      if (protType > 0 && protType != END2END_DPS_TYPE(ns->dataProtSet)) {
         DPRINT_CMD("*** ERROR *** Unmatched protection type, prot set %d", END2END_DPS_TYPE(ns->dataProtSet));
         return NVME_STATUS_INVALID_PI;
      }

      if (protOps == VMK_SCSI_COMMAND_PROT_READ_PASS || protOps == VMK_SCSI_COMMAND_PROT_WRITE_PASS) {
         protPass = 1;
         protSgArray = vmk_ScsiCmdGetProtSgArray(vmkCmd);
         protLen = vmk_SgGetDataLen(protSgArray);
         if (protSgArray->numElems > 1 || (protSgArray->elem[0].ioAddr & 0x3) != 0) {
            bounceBufferType |= PROT_BOUNCE_BUFFER;
         }
      }

      switch((vmkCmd->cdb[1] >> 5) & 0x7) {
         case 0:
         case 1:
         case 5:
            prChk = 0x7;
            break;
         case 2:
            prChk = 0x3;
            break;
         case 3:
            prChk = 0x0;
            break;
         case 4:
            prChk = 0x4;
            break;
         default:
            DPRINT_CMD("*** ERROR *** Invalid code in RDPROTECT field 0x%x", (vmkCmd->cdb[1] >> 5) & 0x7);
            return NVME_STATUS_INVALID_FIELD_IN_CDB;
            break;
      }
      /**
       * Filter the checked fields according to PI types.
       * This should be consistent with Extended INQUIRY Data VPD page.
       */
      protType = END2END_DPS_TYPE(ns->dataProtSet);
      prChk = protType == 3 ? (prChk & 0x4) : (prChk & 0x5);
   }
#endif

   if (1 << ns->lbaShift != VMK_SECTOR_SIZE) {
      int i;
      for (i = 0; i < vmkCmd->sgIOArray->numElems; i++) {
         if ((vmkCmd->sgIOArray->elem[i].length & ((1 << ns->lbaShift) - 1)) !=0) {
            DPRINT_CMD("Illegal SG elem size. Cmd %p, sgELem: %d,"
                       "length: %d, blocksize: %d", vmkCmd, i,
                       vmkCmd->sgIOArray->elem[i].length, (1 << ns->lbaShift));
            bounceBufferType |= DATA_BOUNCE_BUFFER;
            break;
         }
      }
   }

   /**
    * Initialize the status to WOULD_BLOCK
    */
   baseInfo = NULL;

   do {
      cmdInfo = NvmeCore_GetCmdInfo(qinfo);
      if (!cmdInfo) {
         /**
          * We ran out of command slot now. break here and let error handling
          * process this error.
          */
         VPRINT("qinfo %p [%d] failing request.",
                qinfo, qinfo->id);
         nvmeStatus = NVME_STATUS_QFULL;

      #if (NVME_ENABLE_IO_STATS == 1)
            STATS_Increment(ctrlr->statsData.QFULLNoFreeCmdSlots);
      #endif

         break;
      }

      if (!baseInfo) {
         /**
          * This is the base command
          */
         baseInfo                   = cmdInfo;
         baseInfo->cmdPtr           = cmdPtr;
         baseInfo->cmdStatus        = 0;
         baseInfo->requestedLength  = 0;
         baseInfo->requiredLength   = vmk_SgGetDataLen(vmkCmd->sgIOArray);
         baseInfo->bounceBufferType = bounceBufferType;
         if (baseInfo->bounceBufferType & PROT_BOUNCE_BUFFER) {
            vmkStatus = OsLib_DmaAlloc(&ctrlr->ctrlOsResources, protLen,
                                       &baseInfo->protDmaEntry,
                                       VMK_TIMEOUT_NONBLOCKING);
            if (vmkStatus != VMK_OK) {
               baseInfo->bounceBufferType = 0;
               NvmeCore_PutCmdInfo(qinfo, baseInfo);
               nvmeStatus = NVME_STATUS_FAILURE;
               baseInfo = NULL;
               break;
            }
            if (vmk_ScsiIsWriteCdb(vmkCmd->cdb[0])) {
               copySgData(baseInfo->protDmaEntry.sgOut, protSgArray, protLen);
            }
         }
        if (baseInfo->bounceBufferType & DATA_BOUNCE_BUFFER) {
            vmkStatus = OsLib_DmaAlloc(&ctrlr->ctrlOsResources,
                                       baseInfo->requiredLength,
                                       &baseInfo->dataDmaEntry,
                                       VMK_TIMEOUT_NONBLOCKING);
            if (vmkStatus != VMK_OK) {
               if (baseInfo->bounceBufferType & PROT_BOUNCE_BUFFER) {
                  OsLib_DmaFree(&qinfo->ctrlr->ctrlOsResources, &cmdInfo->protDmaEntry);
               }
               baseInfo->bounceBufferType = 0;
               NvmeCore_PutCmdInfo(qinfo, baseInfo);
               nvmeStatus = NVME_STATUS_FAILURE;
               baseInfo = NULL;
               break;
            }
            if (vmk_ScsiIsWriteCdb(vmkCmd->cdb[0])) {
               copySgData(baseInfo->dataDmaEntry.sgOut,
                          vmkCmd->sgIOArray,
                          baseInfo->requiredLength);
            }
         }
      } else {
         cmdInfo->cmdPtr      = NULL;
         qinfo->nrSplit ++;
         DPRINT_SPLIT("splitCmd %p [%d], base %p [%d], qid %d, count %d, len %d/%ld, nrAct %d, nrSplit %d",
            cmdInfo, cmdInfo->cmdId, baseInfo, baseInfo->cmdId, baseInfo->cmdCount,
            qinfo->id, baseInfo->requestedLength, baseInfo->requiredLength,
            qinfo->nrAct - qinfo->pendingCmdFree.freeListLength, qinfo->nrSplit);
      }

      cmdInfo->cmdCount = 0;
      cmdInfo->cmdRetries = retries;
      cmdInfo->type   = BIO_CONTEXT;

      /**
       * The cmdBase assignment is done before publishing the cmdInfo to global
       * visibility.
       */
      cmdInfo->cmdBase  = baseInfo;
      cmdInfo->ns       = ns;

      cmd = &cmdInfo->nvmeCmd;
      Nvme_Memset64(cmd, 0LL, sizeof(*cmd)/sizeof(vmk_uint64));

      /*
       * process bio sglist and setup prp list.
       */
      if (vmk_ScsiIsReadCdb(vmkCmd->cdb[0])) {
         cmd->header.opCode = NVM_CMD_READ;
         dmaDir = VMK_DMA_DIRECTION_TO_MEMORY;
      } else {
         VMK_ASSERT(vmk_ScsiIsWriteCdb(vmkCmd->cdb[0]));
         cmd->header.opCode = NVM_CMD_WRITE;
         dmaDir = VMK_DMA_DIRECTION_FROM_MEMORY;
      }

      cmd->header.namespaceID = ns->id;
      length = NvmeIo_ProcessSgArray(qinfo, cmdInfo, vmkCmd, dmaDir);

      /*
       * Length should be a multiply of sector size (1 << ns->lbaShift).
       */
      VMK_ASSERT((length & ((1 << ns->lbaShift) - 1)) == 0);

      cmd->cmd.read.numLBA   = (length >> ns->lbaShift) - 1;
      cmd->cmd.read.startLBA = vmkCmd->lba +
                               (baseInfo->requestedLength >> ns->lbaShift);

      cmd->header.cmdID      = cmdInfo->cmdId;
#if USE_TIMER
      cmdInfo->timeoutId     = ctrlr->timeoutId;
      qinfo->timeoutCount[cmdInfo->timeoutId] ++;
#endif

      if (END2END_DPS_TYPE(ns->dataProtSet) != 0) {
         cmd->cmd.read.protInfo = prChk & 0x7;
         cmd->cmd.read.expInitLogBlkRefTag = cmd->cmd.read.startLBA & 0xffffffff;
         if (protPass) {
            if (baseInfo->bounceBufferType & PROT_BOUNCE_BUFFER) {
               cmd->header.metadataPtr = baseInfo->protDmaEntry.ioa +
                                         ((baseInfo->requestedLength >> ns->lbaShift) << 3);
            } else {
               cmd->header.metadataPtr = protSgArray->elem[0].ioAddr +
                                         ((baseInfo->requestedLength >> ns->lbaShift) << 3);
            }
         } else {
            cmd->cmd.read.protInfo = cmd->cmd.read.protInfo | 0x8; /* set PRACT=1 */
         }
      }

      if (vmkCmd->cdb[0] != VMK_SCSI_CMD_READ6 && vmkCmd->cdb[0] != VMK_SCSI_CMD_WRITE6) {
         cmd->cmd.read.forceUnitAccess = vmkCmd->cdb[1] & 0x8;
      }

   #if (NVME_ENABLE_IO_STATS == 1)
         STATS_Increment(ctrlr->statsData.TotalRequests);
         if (cmd->header.opCode == NVM_CMD_READ) {
            STATS_Increment(ctrlr->statsData.TotalReads);
         #if (NVME_ENABLE_IO_STATS_ADDITIONAL == 1)
               if ((cmd->cmd.read.numLBA & 0x07) || cmd->cmd.read.startLBA & 0x07) {
                  STATS_Increment(ctrlr->statsData.UnalignedReads);
               }
         #endif
         } else {
            STATS_Increment(ctrlr->statsData.TotalWrites);
         #if (NVME_ENABLE_IO_STATS_ADDITIONAL == 1)
               if ((cmd->cmd.read.numLBA & 0x07) || cmd->cmd.read.startLBA & 0x07) {
                  STATS_Increment(ctrlr->statsData.UnalignedWrites);
               }
         #endif
         }
   #endif

#if DO_IO_STAT
      cmdInfo->startTime = OsLib_GetTimerUs();
#endif

      nvmeStatus = NvmeCore_SubmitCommandAsync(qinfo, cmdInfo,
                                               scsiIoCompleteCommand);
      if (nvmeStatus != NVME_STATUS_SUCCESS) {
         /**
          * Failed to submit command to the hardware.
          */
         if (cmdInfo->bounceBufferType & PROT_BOUNCE_BUFFER) {
            OsLib_DmaFree(&qinfo->ctrlr->ctrlOsResources, &cmdInfo->protDmaEntry);
            cmdInfo->bounceBufferType &= ~PROT_BOUNCE_BUFFER;
         }
         if (cmdInfo->bounceBufferType & DATA_BOUNCE_BUFFER) {
            OsLib_DmaFree(&qinfo->ctrlr->ctrlOsResources, &cmdInfo->dataDmaEntry);
            cmdInfo->bounceBufferType &= ~DATA_BOUNCE_BUFFER;
         }
         if (baseInfo == cmdInfo) {
            baseInfo = NULL;
         } else {
            qinfo->nrSplit --;
         }
         NvmeCore_PutCmdInfo(qinfo, cmdInfo);
#if USE_TIMER
         qinfo->timeoutCount[cmdInfo->timeoutId] --;
#endif
         break;
      }

      /*
       * Update commands sent for request.
       * after the async submission above, completion can happen at any moment
       * however the cmdBase field will contain a valid pointer to baseInfo
       * which will require the complete path to acquire the queue lock
       * before acccessing the fields. This will wait for this function to
       * complete before releaseing the command.
       */
      baseInfo->cmdCount ++;
      baseInfo->requestedLength += cmdInfo->count;

   } while (baseInfo->requestedLength < baseInfo->requiredLength);

   if (nvmeStatus != NVME_STATUS_SUCCESS) {
      /**
       * We ran into some errors during command submission.
       */
      if (baseInfo) {
         /**
          * Check whether the command has been partially submitted to hardware
          */
         if (baseInfo->requestedLength == 0) {
            /**
             * The command has never been submitted to hardware.
             *
             * Since the SCSI command never reached the hardware, it is safe to
             * just return QFULL condition here.
             */

            nvmeStatus = NVME_STATUS_QFULL;
         } else {
            /**
             * The SCSI command has been split and some of the sub commands has
             * been submitted to hardware. We have two options here:
             *
             * 1. put the request into a congestion queue for later retry; or
             * 2. just complete the command here and raise an UNDERRUN condition
             *
             * We go with option two here for simplicity.
             */
            DPRINT_SPLIT("UNDERRUN: vmkCmd %p[%Xh I:%p SN:0x%lx] %d/%ld",
                    vmkCmd, vmkCmd->cdb[0], vmkCmd->cmdId.initiator,
                    vmkCmd->cmdId.serialNumber, baseInfo->requestedLength,
                    baseInfo->requiredLength);

            /**
             * Return WOULD_BLOCK to indicate the command will be completed
             * in completion context.
             */
            nvmeStatus = NVME_STATUS_WOULD_BLOCK;
         }
      } else {
         /**
          * Can't get the first cmd info out of the queue, must be in QFULL
          * condition or FAILURE condition(if using bounce buffer).
          */
         VMK_ASSERT(nvmeStatus == NVME_STATUS_QFULL || nvmeStatus == NVME_STATUS_FAILURE);
      }
   }

   if (baseInfo) {
      if (baseInfo->cmdCount == 1) {
         /**
          *  If this is not really a split command, while still holding the
          * queue lock, clear the field cmdBase. If by any chance the completion
          * happened meanwhile it will acquire the lock and check under the lock
          * if this is still not a split command. Otherwise clearing this field
          * will eliminate the need of acquiring locks for most common case
          * of non-split commands.
          */
         baseInfo->cmdBase = NULL;
      } else {
         DPRINT_SPLIT("vmkCmd %p[%Xh I:%p SN:0x%lx] %d/%ld base %p [%d] count %d status 0x%x",
            vmkCmd, vmkCmd->cdb[0], vmkCmd->cmdId.initiator,
            vmkCmd->cmdId.serialNumber, baseInfo->requestedLength,
            baseInfo->requiredLength, baseInfo, baseInfo->cmdId,
            baseInfo->cmdCount, nvmeStatus);
      }
   }

   /**
    * If all the command submissions are successful, return WOULD_BLOCK to
    * indicate the commands will be completed in completion context.
    */
   if (nvmeStatus == NVME_STATUS_SUCCESS) {
      nvmeStatus = NVME_STATUS_WOULD_BLOCK;
   }

   return nvmeStatus;
}

/**
 * NvmeIo_SubmitIo - submit a SCSI command to a namespace
 */
Nvme_Status
NvmeIo_SubmitIo(struct NvmeNsInfo *ns, void *cmdPtr)
{
   Nvme_Status           nvmeStatus;
   struct NvmeCtrlr     *ctrlr = ns->ctrlr;
   struct NvmeQueueInfo *qinfo;
   int                   qid;
   vmk_ScsiCommand      *vmkCmd;

   GET_VMK_SCSI_CMD(cmdPtr, vmkCmd);

   /* Get the queue for submitting I/O.
    * Note: we should prevent the mismatch between no. of SCSI completion queues
    * and no. of sq/cqs on the hardware.
    */
   qid = OsLib_GetQueue(ctrlr, vmkCmd);
   if (qid >= ctrlr->numIoQueues) {
      /**
       * This could only happen if our driver has been quiesced before PSA
       * quiesce completes.
       */
      EPRINT("invalid compleiton queue: %d numIoQueues: %d.",
             qid, ctrlr->numIoQueues);
      return NVME_STATUS_QUIESCED;
   }

   qinfo = &ctrlr->ioq[qid];
   DPRINT_CMD("vmkCmd %p [0x%x], ns: %d, Qinfo %p [%d], lba 0x%lx, lbc %d",
            vmkCmd, vmkCmd->cdb[0], ns->id, qinfo, qinfo->id, vmkCmd->lba,
            vmkCmd->lbc);

#if NVME_DEBUG
   if (nvme_dbg & NVME_DEBUG_DUMP_SG) {
      NvmeDebug_DumpSgArray(vmkCmd->sgArray);
   #if ((NVME_PROTECTION) && (VMKAPIDDK_VERSION >= 600))
      if (vmk_ScsiCmdGetProtSgArray(vmkCmd) != NULL) {
         DPRINT("pass protection SG Array");
         NvmeDebug_DumpSgArray(vmk_ScsiCmdGetProtSgArray(vmkCmd));
      }
   #endif
   }
#endif

   LOCK_FUNC(qinfo);
   nvmeStatus = NvmeIo_SubmitIoRequest(qinfo, ns, cmdPtr, MAX_RETRY);
   UNLOCK_FUNC(qinfo);

   return nvmeStatus;
}


/**
 * NvmeIo_SubmitDsm - submit a Dataset Management command to a namespace
 */
Nvme_Status
NvmeIo_SubmitDsm(struct NvmeNsInfo *ns, void *cmdPtr,
                 struct nvme_dataset_mgmt_data *dsmData, int count)
{
   Nvme_Status              nvmeStatus;
   struct NvmeCtrlr        *ctrlr;
   struct nvme_cmd         *cmd;
   struct NvmeCmdInfo      *cmdInfo;
   struct NvmeQueueInfo    *qinfo;
   struct NvmeSubQueueInfo *sqInfo;
   int                      qid;
   vmk_ScsiCommand         *vmkCmd;

   GET_VMK_SCSI_CMD(cmdPtr, vmkCmd);

   ctrlr  = ns->ctrlr;
   qid = OsLib_GetQueue(ctrlr, vmkCmd);
   if (qid >= ctrlr->numIoQueues) {
      qid = 0;
   }
   qinfo  = &ctrlr->ioq[qid];
   sqInfo = qinfo->subQueue;

   LOCK_FUNC(qinfo);
   cmdInfo = NvmeCore_GetCmdInfo(qinfo);
   UNLOCK_FUNC(qinfo);


   if (!cmdInfo) {
      return NVME_STATUS_QFULL;
   }

   cmdInfo->cmdCount        = 1;
   cmdInfo->cmdPtr          = cmdPtr;
   cmdInfo->cmdStatus       = 0;
   cmdInfo->requiredLength  = count * sizeof(struct nvme_dataset_mgmt_data);
   cmdInfo->requestedLength = cmdInfo->requiredLength;
   cmdInfo->cmdRetries      = 0;
   cmdInfo->ns              = ns;
   cmdInfo->cmdBase         = NULL;  /* no split command */

   cmd = &cmdInfo->nvmeCmd;
   Nvme_Memset64(cmd, 0LL, sizeof(*cmd)/sizeof(vmk_uint64));

   /*
    * We use cmdInfo's prp pool to save the DSM data
    */
   vmk_Memcpy(cmdInfo->prps, dsmData,
      count * sizeof(struct nvme_dataset_mgmt_data));

   /*
    * Populate DSM command data
    */
   cmd->header.opCode = NVM_CMD_DATASET_MGMNT;
   cmd->header.namespaceID = ns->id;
   cmd->header.prp[0].addr = cmdInfo->prpPhy;
   cmd->header.prp[1].addr = 0;
   cmd->cmd.dataset.numRanges = count - 1; /* 0 based value */
   cmd->cmd.dataset.attribute = (1 << 2); /* Deallocate */

   cmd->header.cmdID = cmdInfo->cmdId;

   cmdInfo->type = BIO_CONTEXT;

   LOCK_FUNC(qinfo);
   nvmeStatus = NvmeCore_SubmitCommandAsync(qinfo, cmdInfo,
                                            scsiIoCompleteCommand);
   if (nvmeStatus != NVME_STATUS_SUCCESS) {
      /**
       * Failed to submit the command to the hardware.
       */
      NvmeCore_PutCmdInfo(qinfo, cmdInfo);
   }

   if (SUCCEEDED(nvmeStatus)) {
      /**
       * Return WOULD_BLOCK indicating the command will be completed in
       * completion context.
       */
      nvmeStatus = NVME_STATUS_WOULD_BLOCK;
   }

   UNLOCK_FUNC(qinfo);
   return nvmeStatus;
}


/*
 * submit the flush command to I/O queue
 *
 * @param [IN]  ns        pointer to namespace info
 * @param [IN]  vmkPtr    poniter to SCSI command to be processed
 * @param [IN]  qinfo     pointer to the completion queue
 *
 * @return NVME_STATUS_WOULD_BLOCK if async command submitted to hardware successfully
 *         otherwise error code.
 *
 * Note that this command is supposed to be async.
 */
Nvme_Status
NvmeIo_SubmitFlush(struct NvmeNsInfo *ns, void *cmdPtr,
                   struct NvmeQueueInfo *qinfo)
{
   Nvme_Status          nvmeStatus;
   struct NvmeCmdInfo   *cmdInfo;
   struct nvme_cmd      *cmd;
   vmk_ScsiCommand      *vmkCmd;
   struct NvmeCtrlr     *ctrlr = ns->ctrlr;

   GET_VMK_SCSI_CMD(cmdPtr, vmkCmd);

   if (!(ns->flags & NS_ONLINE)) {
      DPRINT_NS("*** ERROR *** Received request while Offlined. ns_id %d",
                    ns->id);
      return NVME_STATUS_QUIESCED;
   }

   if (NvmeState_GetCtrlrState(ctrlr) != NVME_CTRLR_STATE_OPERATIONAL) {
      DPRINT_CTRLR("****** Error Completion Command %p, dev state %d",
                    vmkCmd, NvmeState_GetCtrlrState(ctrlr));
      return NVME_STATUS_IN_RESET;
   }

   LOCK_FUNC(qinfo);
   cmdInfo = NvmeCore_GetCmdInfo(qinfo);
   UNLOCK_FUNC(qinfo);

   if(!cmdInfo) {
      EPRINT("failing request, qfull.");
      return NVME_STATUS_QFULL;
   }
   cmdInfo->cmdCount = 0;
   cmdInfo->ns = ns;
   cmdInfo->cmdPtr = cmdPtr;
   cmdInfo->cmdStatus = NVME_CMD_STATUS_ACTIVE;
   cmdInfo->type = BIO_CONTEXT;


   cmd = &(cmdInfo->nvmeCmd);
   Nvme_Memset64(cmd, 0LL, sizeof(*cmd)/sizeof(vmk_uint64));

   cmd->header.opCode = NVM_CMD_FLUSH;
   cmd->header.namespaceID = ns->id;
   cmd->header.cmdID = cmdInfo->cmdId;

   LOCK_FUNC(qinfo);
   nvmeStatus = NvmeCore_SubmitCommandAsync(qinfo, cmdInfo, scsiIoCompleteCommand);
   UNLOCK_FUNC(qinfo);

   if(nvmeStatus != NVME_STATUS_SUCCESS) {
      /*Fail to submit FLUSH command to firmware*/
      EPRINT("failed to submit FLUSH command, 0x%x, %s.",
             nvmeStatus, NvmeCore_StatusToString(nvmeStatus));
      NvmeCore_PutCmdInfo(qinfo, cmdInfo);
      nvmeStatus = NVME_STATUS_FAILURE;
   }
   else {
      nvmeStatus = NVME_STATUS_WOULD_BLOCK;
   }

   return nvmeStatus;
}
