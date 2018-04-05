/*
 * Copyright 2018, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

/*
 *
 * Copyright 2016, 2017 Hesham Almatary, Data61/CSIRO <hesham.almatary@data61.csiro.au>
 * Copyright 2015, 2016 Hesham Almatary <heshamelmatary@gmail.com>
 */

#include <types.h>
#include <benchmark/benchmark.h>
#include <api/failures.h>
#include <api/syscall.h>
#include <kernel/boot.h>
#include <kernel/cspace.h>
#include <kernel/thread.h>
#include <object/tcb.h>
#include <machine/io.h>
#include <model/preemption.h>
#include <model/statedata.h>
#include <object/cnode.h>
#include <object/untyped.h>
#include <arch/api/invocation.h>
#include <arch/kernel/vspace.h>
#include <linker.h>
#include <plat/machine/devices.h>
#include <plat/machine/hardware.h>
#include <kernel/stack.h>

struct resolve_ret {
    paddr_t frameBase;
    vm_page_size_t frameSize;
    bool_t valid;
};
typedef struct resolve_ret resolve_ret_t;

static exception_t performPageGetAddress(void *vbase_ptr);

static word_t CONST
RISCVGetWriteFromVMRights(vm_rights_t vm_rights)
{
    return (vm_rights != VMNoAccess) && (vm_rights != VMReadOnly);
}

static word_t RISCVGetUserFromVMRights(vm_rights_t vm_rights)
{
    return vm_rights != VMKernelOnly;
}

static inline word_t CONST
RISCVGetReadFromVMRights(vm_rights_t vm_rights)
{
    return (vm_rights != VMNoAccess) && (vm_rights != VMWriteOnly);
}

/* ==================== BOOT CODE STARTS HERE ==================== */

BOOT_CODE void
map_kernel_frame(paddr_t paddr, pptr_t vaddr, vm_rights_t vm_rights)
{
    /* This maps a 4KiB page on the corresponding PT level (depending on the
     * configured PT level
     */
    uint32_t idx = RISCV_GET_PT_INDEX(vaddr, RISCVpageAtPTLevel(RISCV_4K_Page));

    /* vaddr lies in the region the global PT covers */
    assert(vaddr >= PPTR_TOP);

    /* array starts at index 0, so (level - 1) to index */
    kernel_pageTables[RISCVpageAtPTLevel(RISCV_4K_Page) - 1][idx] =    pte_new(
                                                                           paddr >> RISCV_4K_PageBits,
                                                                           0,  /* sw */
                                                                           1,  /* dirty */
                                                                           1,  /* accessed */
                                                                           1,  /* global */
                                                                           0,  /* user */
                                                                           1,  /* execute */
                                                                           1,  /* write */
                                                                           1,  /* read */
                                                                           1   /* valid */
                                                                       );
}

BOOT_CODE VISIBLE void
map_kernel_window(void)
{
    /* mapping of kernelBase (virtual address) to kernel's physBase  */
    assert(CONFIG_PT_LEVELS > 1 && CONFIG_PT_LEVELS <= 4);


    /* Calculate the number of PTEs to map the kernel in the first level PT */
    // RVTODO: window size should be well defined multiple of the page size and should not need
    // any rounding
    int num_lvl1_entries = ROUND_UP((BIT(CONFIG_KERNEL_WINDOW_SIZE_BIT) / RISCV_GET_LVL_PGSIZE(1)), 1);

    for (int i = 0; i < num_lvl1_entries; i++) {
        kernel_pageTables[0][RISCV_GET_PT_INDEX(kernelBase, 1) + i] =  pte_new(
                                                                           /* physical address has to be strictly aligned to the correponding page size */
                                                                           ((physBase + RISCV_GET_LVL_PGSIZE(1) * i) >> RISCV_4K_PageBits),
                                                                           0,  /* sw */
                                                                           1,  /* dirty */
                                                                           1,  /* accessed */
                                                                           1,  /* global */
                                                                           0,  /* user */
                                                                           1,  /* execute */
                                                                           1,  /* write */
                                                                           1,  /* read */
                                                                           1   /* valid */
                                                                       );
    }
}

BOOT_CODE void
map_it_pt_cap(cap_t vspace_cap, cap_t pt_cap, uint32_t ptLevel)
{
    lookupPTSlot_ret_t pt_ret;
    pte_t* targetSlot;
    vptr_t vptr = cap_page_table_cap_get_capPTMappedAddress(pt_cap);
    pte_t* lvl1pt = PTE_PTR(pptr_of_cap(vspace_cap));

    /* lvl[ptLevel]pt to be mapped */
    pte_t* pt   = PTE_PTR(pptr_of_cap(pt_cap));

    /* Get PT[ptLevel - 1] slot to install the address of ptLevel in */
    pt_ret = lookupPTSlot(lvl1pt, vptr, ptLevel - 1);

    targetSlot = pt_ret.ptSlot;

    *targetSlot = pte_new(
                      (addrFromPPtr(pt) >> RISCV_4K_PageBits),
                      0, /* sw */
                      1, /* dirty */
                      1, /* accessed */
                      0,  /* global */
                      0,  /* user */
                      0,  /* execute */
                      0,  /* write */
                      0,  /* read */
                      1 /* valid */
                  );
    // RVTODO: make this a mcahine helper. probably needs a compiler barrier to ensure
    // *targetSlot happened before
    asm volatile ("sfence.vma");
}

BOOT_CODE void
map_it_frame_cap(cap_t vspace_cap, cap_t frame_cap)
{
    pte_t* lvl1pt   = PTE_PTR(pptr_of_cap(vspace_cap));
    pte_t* frame_pptr   = PTE_PTR(pptr_of_cap(frame_cap));
    vptr_t frame_vptr = cap_frame_cap_get_capFMappedAddress(frame_cap);

    /* We deal with a frame as 4KiB */
    lookupPTSlot_ret_t lu_ret = lookupPTSlot(lvl1pt, frame_vptr, RISCVpageAtPTLevel(RISCV_4K_Page));

    pte_t* targetSlot = lu_ret.ptSlot;

    *targetSlot = pte_new(
                      (pptr_to_paddr(frame_pptr) >> RISCV_4K_PageBits),
                      0, /* sw */
                      1, /* dirty */
                      1, /* accessed */
                      0,  /* global */
                      1,  /* user */
                      1,  /* execute */
                      1,  /* write */
                      1,  /* read */
                      1   /* valid */
                  );
    asm volatile ("sfence.vma");
}

BOOT_CODE cap_t
create_unmapped_it_frame_cap(pptr_t pptr, bool_t use_large)
{
    cap_t cap = cap_frame_cap_new(
                    asidInvalid,                     /* capFMappedASID       */
                    pptr,                            /* capFBasePtr          */
                    0,                               /* capFSize             */
                    0,                               /* capFVMRights         */
                    0,
                    0                                /* capFMappedAddress    */
                );

    return cap;
}

/* Create a page table for the initial thread */
static BOOT_CODE cap_t
create_it_pt_cap(cap_t vspace_cap, pptr_t pptr, vptr_t vptr, asid_t asid, uint32_t ptLevel)
{
    cap_t cap;
    cap = cap_page_table_cap_new(
              asid,   /* capPTMappedASID      */
              pptr,   /* capPTBasePtr         */
              1,      /* capPTIsMapped        */
              vptr    /* capPTMappedAddress   */
          );

    map_it_pt_cap(vspace_cap, cap, ptLevel);
    return cap;
}

/* Create an address space for the initial thread.
 * This includes page directory and page tables */
BOOT_CODE cap_t
create_it_address_space(cap_t root_cnode_cap, v_region_t it_v_reg)
{
    cap_t      lvl1pt_cap;
    vptr_t     pt_vptr;
    pptr_t     pt_pptr;
    pptr_t lvl1pt_pptr;

    /* create 1st level page table obj and cap */
    lvl1pt_pptr = alloc_region(PT_SIZE_BITS);

    if (!lvl1pt_pptr) {
        return cap_null_cap_new();
    }
    memzero(PTE_PTR(lvl1pt_pptr), 1 << PT_SIZE_BITS);

    copyGlobalMappings(PTE_PTR(lvl1pt_pptr));

    lvl1pt_cap =
        cap_page_table_cap_new(
            IT_ASID,               /* capPTMappedASID    */
            (word_t) lvl1pt_pptr,  /* capPTBasePtr       */
            1,                     /* capPTIsMapped      */
            (word_t) lvl1pt_pptr   /* capPTMappedAddress */
        );

    write_slot(SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadVSpace), lvl1pt_cap);

    /* create all n level PT objs and caps necessary to cover userland image in 4KiB pages */

    for (int i = 2; i <= CONFIG_PT_LEVELS; i++) {

        for (pt_vptr = ROUND_DOWN(it_v_reg.start, RISCV_GET_LVL_PGSIZE_BITS(i - 1));
                pt_vptr < it_v_reg.end;
                pt_vptr += RISCV_GET_LVL_PGSIZE(i - 1)) {
            pt_pptr = alloc_region(PT_SIZE_BITS);

            if (!pt_pptr) {
                return cap_null_cap_new();
            }

            memzero(PTE_PTR(pt_pptr), 1 << PT_SIZE_BITS);
            if (!provide_cap(root_cnode_cap,
                             create_it_pt_cap(lvl1pt_cap, pt_pptr, pt_vptr, IT_ASID, i))
               ) {
                return cap_null_cap_new();
            }
        }

    }
    // RVTODO: does not seem to populate ndks_boot.bi_frame->userImagePaging

    return lvl1pt_cap;
}

BOOT_CODE void
activate_kernel_vspace(void)
{
    setVSpaceRoot(addrFromPPtr(&kernel_pageTables[0]), 0);
}

BOOT_CODE void
write_it_asid_pool(cap_t it_ap_cap, cap_t it_lvl1pt_cap)
{
    asid_pool_t* ap = ASID_POOL_PTR(pptr_of_cap(it_ap_cap));
    ap->array[IT_ASID] = PTE_PTR(pptr_of_cap(it_lvl1pt_cap));
    riscvKSASIDTable[IT_ASID >> asidLowBits] = ap;
}

/* ==================== BOOT CODE FINISHES HERE ==================== */

static findVSpaceForASID_ret_t findVSpaceForASID(asid_t asid)
{
    findVSpaceForASID_ret_t ret;
    asid_pool_t*        poolPtr;
    pte_t*     vspace_root;

    poolPtr = riscvKSASIDTable[asid >> asidLowBits];
    if (!poolPtr) {
        current_lookup_fault = lookup_fault_invalid_root_new();

        ret.vspace_root = NULL;
        ret.status = EXCEPTION_LOOKUP_FAULT;
        return ret;
    }

    vspace_root = poolPtr->array[asid & MASK(asidLowBits)];
    if (!vspace_root) {
        //current_lookup_fault = lookup_fault_invalid_root_new();
        current_lookup_fault = lookup_fault_missing_capability_new(RISCV_GET_LVL_PGSIZE_BITS(1));

        ret.vspace_root = NULL;
        ret.status = EXCEPTION_LOOKUP_FAULT;
        return ret;
    }

    ret.vspace_root = vspace_root;
    ret.status = EXCEPTION_NONE;
    return ret;
}

bool_t CONST isVTableRoot(cap_t cap)
{
    findVSpaceForASID_ret_t ret = findVSpaceForASID(cap_page_table_cap_get_capPTMappedASID(cap));
    bool_t isRoot = false;

    if (cap_get_capType(cap) == cap_page_table_cap) {
        if (ret.status == EXCEPTION_NONE) {
            if (cap_page_table_cap_get_capPTBasePtr(cap) == (word_t) ret.vspace_root) {
                isRoot = true;
            }
        }
    }

    return isRoot;
}

// RVTODO: is this indirection needed? seems to only be one root type at the moment
static bool_t CONST isValidNativeRoot(cap_t cap)
{
    return isVTableRoot(cap) &&
           cap_page_table_cap_get_capPTIsMapped(cap);
}

void
copyGlobalMappings(pte_t *newLvl1pt)
{
    unsigned int i;
    pte_t *global_kernel_vspace = kernel_pageTables[0];

    for (i = RISCV_GET_PT_INDEX(kernelBase, 1); i < BIT(PT_INDEX_BITS); i++) {
        newLvl1pt[i] = global_kernel_vspace[i];
    }
}

word_t * PURE
lookupIPCBuffer(bool_t isReceiver, tcb_t *thread)
{
    word_t w_bufferPtr;
    cap_t bufferCap;
    vm_rights_t vm_rights;

    w_bufferPtr = thread->tcbIPCBuffer;
    bufferCap = TCB_PTR_CTE_PTR(thread, tcbBuffer)->cap;

    if (unlikely(cap_get_capType(bufferCap) != cap_frame_cap &&
                 cap_get_capType(bufferCap) != cap_frame_cap)) {
        return NULL;
    }
    // RVTODO: check if cap is device cap

    vm_rights = cap_frame_cap_get_capFVMRights(bufferCap);
    if (likely(vm_rights == VMReadWrite ||
               (!isReceiver && vm_rights == VMReadOnly))) {
        word_t basePtr;
        unsigned int pageBits;

        basePtr = cap_frame_cap_get_capFBasePtr(bufferCap);
        pageBits = pageBitsForSize(cap_frame_cap_get_capFSize(bufferCap));
        return (word_t *)(basePtr + (w_bufferPtr & MASK(pageBits)));
    } else {
        return NULL;
    }
}

// RVTODO: this function has a confusing name as it is doing two things
// checking if the entry is for another page table *AND* if the entry is valid
static inline pptr_t isValidHWPageTable(pte_t *pte)
{
    // RVTODO: read the R W X bits and valid bits explicitly
    return (pte->words[0] & 0xf) == 1;
}

static inline pte_t *getPPtrFromHWPTE(pte_t *pte)
{
    // RVTODO: use bitfield accessors
    assert(isValidHWPageTable(pte));
    return (pte_t*)ptrFromPAddr((pte->words[0] >> PTE_PPN_SHIFT) << (seL4_PageTableBits));
}

static lookupPTSlot_ret_t lookupPageTableLevelSlot(asid_t asid, vptr_t vptr, pte_t *target_pt)
{
    findVSpaceForASID_ret_t find_ret;
    lookupPTSlot_ret_t ret;
    pte_t* pt;

    find_ret = findVSpaceForASID(asid);
    if (unlikely(find_ret.status != EXCEPTION_NONE)) {
        userError("Couldn't find a root vspace for asid");
        ret.status = EXCEPTION_LOOKUP_FAULT;
        return ret;
    } else {
        ret.ptSlot = find_ret.vspace_root + RISCV_GET_PT_INDEX(vptr, 1);
        pt = find_ret.vspace_root;
    }

    for (int i = 2; i <= CONFIG_PT_LEVELS; i++) {
        if (unlikely(!isValidHWPageTable(ret.ptSlot))) {
            userError("Page table walk terminated, failed to find a PT");
            ret.status = EXCEPTION_LOOKUP_FAULT;
            return ret;
        }
        pt = getPPtrFromHWPTE(ret.ptSlot);
        if (pt == target_pt) {
            /* Found the PT Slot */
            ret.ptSlot = pt + RISCV_GET_PT_INDEX(vptr, i - 1);
            ret.status = EXCEPTION_NONE;
            return ret;
        }
        ret.ptSlot = pt + RISCV_GET_PT_INDEX(vptr, i);
    }

    /* Failed to find a corredponding PT  */
    userError("Couldn't find a corresponding PT in HW to delete");
    ret.status = EXCEPTION_LOOKUP_FAULT;
    return ret;
}

lookupPTSlot_ret_t
lookupPTSlot(pte_t *lvl1pt, vptr_t vptr, uint32_t ptLevel)
{
    lookupPTSlot_ret_t ret;
    pte_t* pt;

    assert(ptLevel <= CONFIG_PT_LEVELS);

    /* Is lvl1pt valid? */
    if (lvl1pt == 0) {
        userError("lvl1pt is invalid");
        ret.status = EXCEPTION_LOOKUP_FAULT;
        return ret;
    } else {
        ret.ptSlot = lvl1pt + RISCV_GET_PT_INDEX(vptr, 1);
    }

    for (int i = 2; i <= ptLevel; i++) {
        if (ret.ptSlot->words[0] == 0) {
            current_lookup_fault = lookup_fault_missing_capability_new(RISCV_GET_LVL_PGSIZE_BITS(i - 1));
            ret.missingPTLevel = i;
            ret.status = EXCEPTION_LOOKUP_FAULT;
            return ret;
        } else {
            pt = getPPtrFromHWPTE(ret.ptSlot);
            ret.ptSlot = pt + RISCV_GET_PT_INDEX(vptr, i);
        }
    }

    ret.status = EXCEPTION_NONE;
    return ret;
}

exception_t
handleVMFault(tcb_t *thread, vm_fault_type_t vm_faultType)
{
    uint64_t addr;

    addr = read_csr(sbadaddr);

    switch (vm_faultType) {
    case RISCVLoadPageFault:
    case RISCVLoadAccessFault:
        current_fault = seL4_Fault_VMFault_new(addr, RISCVLoadAccessFault, false);
        return EXCEPTION_FAULT;
    case RISCVStorePageFault:
    case RISCVStoreAccessFault:
        current_fault = seL4_Fault_VMFault_new(addr, RISCVStoreAccessFault, false);
        return EXCEPTION_FAULT;
    case RISCVInstructionPageFault:
    case RISCVInstructionAccessFault:
        setRegister(thread, NEXTPC, getRegister(thread, SEPC));
        current_fault = seL4_Fault_VMFault_new(addr, RISCVInstructionAccessFault, true);
        return EXCEPTION_FAULT;

    default:
        fail("Invalid VM fault type");
    }
}

void deleteASIDPool(asid_t asid_base, asid_pool_t* pool)
{
    /* Haskell error: "ASID pool's base must be aligned" */
    assert(IS_ALIGNED(asid_base, asidLowBits));

    if (riscvKSASIDTable[asid_base >> asidLowBits] == pool) {
        riscvKSASIDTable[asid_base >> asidLowBits] = NULL;
        setVMRoot(NODE_STATE(ksCurThread));
    }
}

static exception_t performASIDControlInvocation(void* frame, cte_t* slot, cte_t* parent, asid_t asid_base)
{
    cap_untyped_cap_ptr_set_capFreeIndex(&(parent->cap),
                                         MAX_FREE_INDEX(cap_untyped_cap_get_capBlockSize(parent->cap)));

    memzero(frame, 1 << pageBitsForSize(RISCV_4K_Page));
    cteInsert(
        cap_asid_pool_cap_new(
            asid_base,          /* capASIDBase  */
            WORD_REF(frame)     /* capASIDPool  */
        ),
        parent,
        slot
    );
    /* Haskell error: "ASID pool's base must be aligned" */
    assert((asid_base & MASK(asidLowBits)) == 0);
    riscvKSASIDTable[asid_base >> asidLowBits] = (asid_pool_t*)frame;

    return EXCEPTION_NONE;
}

static exception_t performASIDPoolInvocation(asid_t asid, asid_pool_t* poolPtr, cte_t* vspaceCapSlot)
{
    pte_t *regionBase = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(vspaceCapSlot->cap));
    cap_t cap = vspaceCapSlot->cap;
    cap = cap_page_table_cap_set_capPTMappedASID(cap, asid);
    cap = cap_page_table_cap_set_capPTIsMapped(cap, 1);
    vspaceCapSlot->cap = cap;

    copyGlobalMappings(regionBase);

    poolPtr->array[asid & MASK(asidLowBits)] = regionBase;

    return EXCEPTION_NONE;
}

static void hwASIDFlush(asid_t asid)
{
    // RVTODO: define abstraction operation for this in machine.h
    asm volatile ("sfence.vma x0, %0" :: "r" (asid): "memory");
}

void deleteASID(asid_t asid, pte_t *vspace)
{
    asid_pool_t* poolPtr;

    poolPtr = riscvKSASIDTable[asid >> asidLowBits];
    if (poolPtr != NULL && poolPtr->array[asid & MASK(asidLowBits)] == vspace) {
        hwASIDFlush(asid);
        poolPtr->array[asid & MASK(asidLowBits)] = NULL;
        setVMRoot(NODE_STATE(ksCurThread));
    }
}

void
unmapPageTable(asid_t asid, vptr_t vaddr, pte_t* pt)
{
    lookupPTSlot_ret_t pt_ret = lookupPageTableLevelSlot(asid, vaddr, pt);

    if (pt_ret.status != EXCEPTION_NONE) {
        *pt_ret.ptSlot = pte_new(
                             0,  /* phy_address */
                             0,  /* sw */
                             0,  /* dirty */
                             0,  /* accessed */
                             0,  /* global */
                             0,  /* user */
                             0,  /* execute */
                             0,  /* write */
                             0,  /* read */
                             0  /* valid */
                         );
        asm volatile ("sfence.vma");
    }
}

static pte_t pte_pte_invalid_new(void)
{
    return (pte_t) {
        0
    };
}

void
unmapPage(vm_page_size_t page_size, asid_t asid, vptr_t vptr, pptr_t pptr)
{
    findVSpaceForASID_ret_t find_ret;
    lookupPTSlot_ret_t  lu_ret;

    find_ret = findVSpaceForASID(asid);
    if (find_ret.status != EXCEPTION_NONE) {
        return;
    }

    // RVTODO: use a lookup leaf function instead of looking up at a particular level
    lu_ret = lookupPTSlot(find_ret.vspace_root, vptr, RISCVpageAtPTLevel(page_size));
    if (unlikely(lu_ret.status != EXCEPTION_NONE)) {
        return;
    }

    lu_ret.ptSlot[0] = pte_pte_invalid_new();

    asm volatile ("sfence.vma");
}

void
setVMRoot(tcb_t *tcb)
{
    cap_t threadRoot;
    asid_t asid;
    pte_t *lvl1pt;
    findVSpaceForASID_ret_t  find_ret;

    threadRoot = TCB_PTR_CTE_PTR(tcb, tcbVTable)->cap;

    if (cap_get_capType(threadRoot) != cap_page_table_cap) {
        setVSpaceRoot(addrFromPPtr(&kernel_pageTables[0]), 0);
        return;
    }

    lvl1pt = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(threadRoot));

    asid = cap_page_table_cap_get_capPTMappedASID(threadRoot);
    find_ret = findVSpaceForASID(asid);
    if (unlikely(find_ret.status != EXCEPTION_NONE || find_ret.vspace_root != lvl1pt)) {
        setVSpaceRoot(addrFromPPtr(&kernel_pageTables[0]), asid);
        return;
    }

    setVSpaceRoot(addrFromPPtr(lvl1pt), asid);
}

// RVTODO: is this abstraction around vtables needed?
bool_t CONST
isValidVTableRoot(cap_t cap)
{
    return (cap_get_capType(cap) == cap_page_table_cap);
}

exception_t
checkValidIPCBuffer(vptr_t vptr, cap_t cap)
{
    if (unlikely(cap_get_capType(cap) != cap_frame_cap)) {
        userError("Requested IPC Buffer is not a frame cap.");
        current_syscall_error.type = seL4_IllegalOperation;
        return EXCEPTION_SYSCALL_ERROR;
    }

    // RVTODO: check for that frame is not a device frame

    // RVTODO use seL4_IPCBufferSizeBits constant as the ipc buffer is not 9
    // bits on 64-bit
    if (unlikely(vptr & MASK(9))) {
        userError("Requested IPC Buffer location 0x%x is not aligned.",
                  (int)vptr);
        current_syscall_error.type = seL4_AlignmentError;
        return EXCEPTION_SYSCALL_ERROR;
    }

    return EXCEPTION_NONE;
}

vm_rights_t CONST
maskVMRights(vm_rights_t vm_rights, seL4_CapRights_t cap_rights_mask)
{
    if (vm_rights == VMNoAccess) {
        return VMNoAccess;
    }
    if (vm_rights == VMReadOnly &&
            seL4_CapRights_get_capAllowRead(cap_rights_mask)) {
        return VMReadOnly;
    }
    if (vm_rights == VMReadWrite &&
            (seL4_CapRights_get_capAllowRead(cap_rights_mask) || seL4_CapRights_get_capAllowWrite(cap_rights_mask))) {
        if (!seL4_CapRights_get_capAllowWrite(cap_rights_mask)) {
            return VMReadOnly;
        } else if (!seL4_CapRights_get_capAllowRead(cap_rights_mask)) {
            return VMWriteOnly;
        } else {
            return VMReadWrite;
        }
    }
    if (vm_rights == VMWriteOnly &&
            seL4_CapRights_get_capAllowWrite(cap_rights_mask)) {
        return VMWriteOnly;
    }
    if (vm_rights == VMKernelOnly) {
        return VMKernelOnly;
    }
    return VMNoAccess;
}

/* The rest of the file implements the RISCV object invocations */

static pte_t CONST
makeUserPTE(paddr_t paddr, bool_t executable, vm_rights_t vm_rights)
{
    return pte_new(
               paddr >> RISCV_4K_PageBits,
               0, /* sw */
               1, /* dirty */
               1, /* accessed */
               0, /* global */
               RISCVGetUserFromVMRights(vm_rights),   /* user */
               executable, /* execute */
               RISCVGetWriteFromVMRights(vm_rights),  /* write */
               RISCVGetReadFromVMRights(vm_rights), /* read */
               1 /* valid */
           );
}

static inline bool_t CONST
checkVPAlignment(vm_page_size_t sz, word_t w)
{
    return (w & MASK(pageBitsForSize(sz))) == 0;
}

static exception_t
decodeRISCVPageTableInvocation(word_t label, unsigned int length,
                               cte_t *cte, cap_t cap, extra_caps_t extraCaps,
                               word_t *buffer)
{
    word_t vaddr;
    cap_t lvl1ptCap;
    pte_t *lvl1pt, *ptSlot;
    pte_t pte;
    paddr_t paddr;
    lookupPTSlot_ret_t lu_ret;
    asid_t          asid;

    /* No invocations at level 1 page table (aka page directory) are not supported */
    if (isVTableRoot(cap)) {
        current_syscall_error.type = seL4_IllegalOperation;
        return EXCEPTION_SYSCALL_ERROR;
    }

    if (label == RISCVPageTableUnmap) {
        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performPageTableInvocationUnmap (cap, cte);
    }

    if (unlikely((label != RISCVPageTableMap))) {
        current_syscall_error.type = seL4_IllegalOperation;
        return EXCEPTION_SYSCALL_ERROR;
    }

    if (unlikely(length < 2 || extraCaps.excaprefs[0] == NULL)) {
        current_syscall_error.type = seL4_TruncatedMessage;
        return EXCEPTION_SYSCALL_ERROR;
    }

    vaddr = getSyscallArg(0, buffer);
    lvl1ptCap = extraCaps.excaprefs[0]->cap;

    if (!isValidNativeRoot(lvl1ptCap)) {
        current_syscall_error.type = seL4_InvalidCapability;
        current_syscall_error.invalidCapNumber = 1;

        return EXCEPTION_SYSCALL_ERROR;
    }

    if (unlikely(cap_get_capType(lvl1ptCap) != cap_page_table_cap)) {
        current_syscall_error.type = seL4_InvalidCapability;
        current_syscall_error.invalidCapNumber = 1;

        return EXCEPTION_SYSCALL_ERROR;
    }

    // RVTODO: we have checked for validnative root, compared against page table cap
    // and are now checking for vtable root, this seems excessive and redundant
    if (unlikely(!isVTableRoot(lvl1ptCap))) {
        current_syscall_error.type = seL4_InvalidCapability;
        current_syscall_error.invalidCapNumber = 1;

        return EXCEPTION_SYSCALL_ERROR;
    }

    lvl1pt = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(lvl1ptCap));
    asid = cap_page_table_cap_get_capPTMappedASID(lvl1ptCap);

    if (unlikely(vaddr >= kernelBase)) {
        current_syscall_error.type = seL4_InvalidArgument;
        current_syscall_error.invalidArgumentNumber = 0;

        return EXCEPTION_SYSCALL_ERROR;
    }

    {
        findVSpaceForASID_ret_t find_ret;

        find_ret = findVSpaceForASID(asid);
        if (find_ret.status != EXCEPTION_NONE) {
            current_syscall_error.type = seL4_FailedLookup;
            current_syscall_error.failedLookupWasSource = false;

            return EXCEPTION_SYSCALL_ERROR;
        }

        if (find_ret.vspace_root != lvl1pt) {
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;

            return EXCEPTION_SYSCALL_ERROR;
        }
    }

    /* Check if there's a "ptLevel - 1" PT to map "ptLevel" PT in */

    /* Try to map the PT at the last level to figure out which level should
     * this PT be mapped in. The functions returns the ptSlot of ptLevel -1
     * that this newly PT should be installed at.
     */
    // RVTODO: we just want to lookup the leaf and then see how many bits are left to
    // translate (i.e. the level of the leaf)
    lu_ret = lookupPTSlot(lvl1pt, vaddr, CONFIG_PT_LEVELS);

    /* Get the slot to install the PT in */
    ptSlot = lu_ret.ptSlot;

    if (unlikely(ptSlot->words[0] != 0) ) {
        current_syscall_error.type = seL4_DeleteFirst;
        return EXCEPTION_SYSCALL_ERROR;
    }

    paddr = addrFromPPtr(
                PTE_PTR(cap_page_table_cap_get_capPTBasePtr(cap)));

    // RVTODO: why is there makeUserPTE helper and yet we make a user pte manually here
    pte = pte_new(
              (paddr >> RISCV_4K_PageBits),
              0, /* sw */
              1, /* dirty */
              1, /* accessed */
              0,  /* global */
              0,  /* user */
              0,  /* execute */
              0,  /* write */
              0,  /* read */
              1 /* valid */
          );

    cap = cap_page_table_cap_set_capPTIsMapped(cap, 1);
    cap = cap_page_table_cap_set_capPTMappedASID(cap, asid);
    cap = cap_page_table_cap_set_capPTMappedAddress(cap, vaddr);

    setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
    return performPageTableInvocationMap(cap, cte, pte, ptSlot);
}

struct create_mappings_pte_return {
    exception_t status;
    pte_t pte;
    // RVTODO: does riscv support pte ranges for 'larger' frames?
    pte_range_t pte_entries;
};
typedef struct create_mappings_pte_return create_mappings_pte_return_t;

// RVTODO: what is the point of this safe mapping entries?
static create_mappings_pte_return_t
createSafeMappingEntries_PTE
(paddr_t base, word_t vaddr, vm_page_size_t frameSize,
 vm_rights_t vmRights, vm_attributes_t attr, pte_t *lvl1pt)
{
    create_mappings_pte_return_t ret;
    lookupPTSlot_ret_t lu_ret;
    bool_t executable = vm_attributes_get_riscvExecuteNever(attr) ? 0 : 1;

    ret.pte_entries.base = 0;
    ret.pte_entries.length = 1;

    ret.pte = makeUserPTE(base, executable, vmRights);

    lu_ret = lookupPTSlot(lvl1pt, vaddr, RISCVpageAtPTLevel(frameSize));
    if (unlikely(lu_ret.status != EXCEPTION_NONE)) {
        current_syscall_error.type =
            seL4_FailedLookup;
        current_syscall_error.failedLookupWasSource =
            false;
        ret.status = EXCEPTION_SYSCALL_ERROR;
        /* current_lookup_fault will have been set by
         * lookupPTSlot */
        return ret;
    }

    ret.pte_entries.base = lu_ret.ptSlot;

    ret.status = EXCEPTION_NONE;
    return ret;
}

static exception_t
decodeRISCVFrameInvocation(word_t label, unsigned int length,
                           cte_t *cte, cap_t cap, extra_caps_t extraCaps,
                           word_t *buffer)
{
    switch (label) {
    case RISCVPageMap: {

        word_t vaddr, vtop, w_rightsMask;
        paddr_t frame_paddr;
        cap_t lvl1ptCap;
        pte_t *lvl1pt;
        asid_t asid;
        vm_rights_t capVMRights, vmRights;
        vm_page_size_t frameSize;
        vm_attributes_t attr;

        if (unlikely(length < 3 || extraCaps.excaprefs[0] == NULL)) {
            userError("RISCVPageMap: Truncated message.");
            current_syscall_error.type =
                seL4_TruncatedMessage;

            return EXCEPTION_SYSCALL_ERROR;
        }

        vaddr = getSyscallArg(0, buffer);
        w_rightsMask = getSyscallArg(1, buffer);
        attr = vmAttributesFromWord(getSyscallArg(2, buffer));
        lvl1ptCap = extraCaps.excaprefs[0]->cap;

        frameSize = cap_frame_cap_get_capFSize(cap);
        capVMRights = cap_frame_cap_get_capFVMRights(cap);

        if (unlikely(cap_frame_cap_get_capFMappedASID(cap)) != asidInvalid) {
            // RVTODO: this is nonsense. a mapped frame *CANNOT* be mapped
            if (cap_frame_cap_get_capFMappedAddress(cap) != vaddr) {
                userError("RISCVPageMap: Trying to map the same frame cap to different vaddr %p", (void*)vaddr);
                current_syscall_error.type =
                    seL4_InvalidCapability;
                current_syscall_error.invalidCapNumber = 0;
                return EXCEPTION_SYSCALL_ERROR;
            }

        }

        if (unlikely(cap_get_capType(lvl1ptCap) != cap_page_table_cap)) {
            userError("RISCVPageMap: Invalid level 1 pt cap.");
            current_syscall_error.type =
                seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;

            return EXCEPTION_SYSCALL_ERROR;
        }

        if (unlikely(!isVTableRoot(lvl1ptCap))) {
            userError("RISCVPageMap: Invalid level 1 pt cap.");
            current_syscall_error.type =
                seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;

            return EXCEPTION_SYSCALL_ERROR;
        }

        lvl1pt = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(
                             lvl1ptCap));

        /* Check if this page is already mapped */
        // RVTODO: lookup leaf node and see if remaining bits == frameSize
        lookupPTSlot_ret_t lu_ret = lookupPTSlot(lvl1pt, vaddr, RISCVpageAtPTLevel(frameSize));

        if (unlikely(lu_ret.status != EXCEPTION_NONE)) {
            current_syscall_error.type =
                seL4_FailedLookup;
            current_syscall_error.failedLookupWasSource =
                false;
            return EXCEPTION_SYSCALL_ERROR;
        }

        asid = cap_page_table_cap_get_capPTMappedASID(lvl1ptCap);

        // RVTODO: how are we checking the lvl1pt *AFTER* having just done lookups with it
        {
            findVSpaceForASID_ret_t find_ret;

            find_ret = findVSpaceForASID(asid);
            if (find_ret.status != EXCEPTION_NONE) {
                current_syscall_error.type = seL4_FailedLookup;
                current_syscall_error.failedLookupWasSource = false;

                return EXCEPTION_SYSCALL_ERROR;
            }

            if (find_ret.vspace_root != lvl1pt) {
                current_syscall_error.type = seL4_InvalidCapability;
                current_syscall_error.invalidCapNumber = 1;

                return EXCEPTION_SYSCALL_ERROR;
            }
        }

        vtop = vaddr + BIT(pageBitsForSize(frameSize)) - 1;

        if (unlikely(vtop >= kernelBase)) {
            current_syscall_error.type =
                seL4_InvalidArgument;
            current_syscall_error.invalidArgumentNumber = 0;

            return EXCEPTION_SYSCALL_ERROR;
        }

        vmRights =
            maskVMRights(capVMRights, rightsFromWord(w_rightsMask));

        if (unlikely(!checkVPAlignment(frameSize, vaddr))) {
            current_syscall_error.type =
                seL4_AlignmentError;

            return EXCEPTION_SYSCALL_ERROR;
        }

        frame_paddr = addrFromPPtr((void *)
                                   cap_frame_cap_get_capFBasePtr(cap));


        cap = cap_frame_cap_set_capFMappedASID(cap, asid);
        cap = cap_frame_cap_set_capFMappedAddress(cap,  vaddr);

        create_mappings_pte_return_t map_ret;
        map_ret = createSafeMappingEntries_PTE(frame_paddr, vaddr,
                                               frameSize, vmRights,
                                               attr, lvl1pt);

        if (unlikely(map_ret.status != EXCEPTION_NONE)) {
            return map_ret.status;
        }

        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performPageInvocationMapPTE(cap, cte,
                                           map_ret.pte,
                                           map_ret.pte_entries);
    }

    case RISCVPageRemap: {
        word_t vaddr, w_rightsMask;
        paddr_t frame_paddr;
        cap_t lvl1ptCap;
        pte_t *lvl1pt;
        asid_t asid;
        vm_rights_t capVMRights, vmRights;
        vm_page_size_t frameSize;
        vm_attributes_t attr;

        if (unlikely(length < 2 || extraCaps.excaprefs[0] == NULL)) {
            userError("RISCVPageRemap: Truncated message.");
            current_syscall_error.type = seL4_TruncatedMessage;
            return EXCEPTION_SYSCALL_ERROR;
        }

        w_rightsMask = getSyscallArg(0, buffer);
        attr = vmAttributesFromWord(getSyscallArg(1, buffer));
        lvl1ptCap = extraCaps.excaprefs[0]->cap;
        frameSize = cap_frame_cap_get_capFSize(cap);
        capVMRights = cap_frame_cap_get_capFVMRights(cap);


        if (unlikely(cap_get_capType(lvl1ptCap) != cap_page_table_cap)) {
            userError("RISCVPageRemap: Invalid level 1 pt cap.");
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;
            return EXCEPTION_SYSCALL_ERROR;
        }

        if (unlikely(!isVTableRoot(lvl1ptCap))) {
            userError("RISCVPageReMap: Invalid level 1 pt cap.");
            current_syscall_error.type =
                seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;
            return EXCEPTION_SYSCALL_ERROR;
        }

        if (unlikely(cap_frame_cap_get_capFMappedASID(cap)) == asidInvalid) {
            userError("RISCVPageRemap: Cap is not mapped");
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 0;
            return EXCEPTION_SYSCALL_ERROR;
        }

        vaddr = cap_frame_cap_get_capFMappedAddress(cap);
        asid = cap_page_table_cap_get_capPTMappedASID(lvl1ptCap);
        lvl1pt = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(lvl1ptCap));
        findVSpaceForASID_ret_t find_ret = findVSpaceForASID(asid);
        if (find_ret.status != EXCEPTION_NONE) {
            current_syscall_error.type = seL4_FailedLookup;
            current_syscall_error.failedLookupWasSource = false;
            return EXCEPTION_SYSCALL_ERROR;
        }
        if (find_ret.vspace_root != lvl1pt) {
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;

            return EXCEPTION_SYSCALL_ERROR;
        }

        /* Check if this page is already mapped */
        // RVTODO: lookup leaf node and see if remaining bits == frameSize
        lookupPTSlot_ret_t lu_ret = lookupPTSlot(lvl1pt, vaddr, RISCVpageAtPTLevel(frameSize));

        if (unlikely(lu_ret.status != EXCEPTION_NONE)) {
            userError("RISCVPageMap: No PageTable for this page %p", (void*)vaddr);
            current_syscall_error.type = seL4_FailedLookup;
            current_syscall_error.failedLookupWasSource = false;
            return EXCEPTION_SYSCALL_ERROR;
        }

        vmRights = maskVMRights(capVMRights, rightsFromWord(w_rightsMask));
        frame_paddr = addrFromPPtr((void *)
                                   cap_frame_cap_get_capFBasePtr(cap));
        create_mappings_pte_return_t map_ret;
        map_ret = createSafeMappingEntries_PTE(frame_paddr, vaddr,
                                               frameSize, vmRights,
                                               attr, lvl1pt);
        if (unlikely(map_ret.status != EXCEPTION_NONE)) {
            return map_ret.status;
        }

        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performPageInvocationRemapPTE(map_ret.pte, map_ret.pte_entries);
    }
    case RISCVPageUnmap: {
        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performPageInvocationUnmap(cap, cte);
    }

    case RISCVPageGetAddress: {

        /* Check that there are enough message registers */
        assert(n_msgRegisters >= 1);

        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performPageGetAddress((void*)cap_frame_cap_get_capFBasePtr(cap));
    }

    default:
        userError("RISCVPage: Illegal operation.");
        current_syscall_error.type = seL4_IllegalOperation;

        return EXCEPTION_SYSCALL_ERROR;
    }

}

static const resolve_ret_t default_resolve_ret_t;

static inline vptr_t
pageBase(vptr_t vaddr, vm_page_size_t size)
{
    return vaddr & ~MASK(pageBitsForSize(size));
}

exception_t
decodeRISCVMMUInvocation(word_t label, unsigned int length, cptr_t cptr,
                         cte_t *cte, cap_t cap, extra_caps_t extraCaps,
                         word_t *buffer)
{
    switch (cap_get_capType(cap)) {

    case cap_page_table_cap:
        return decodeRISCVPageTableInvocation(label, length, cte, cap, extraCaps, buffer);

    case cap_frame_cap:
        return decodeRISCVFrameInvocation(label, length, cte, cap, extraCaps, buffer);

    case cap_asid_control_cap: {
        word_t     i;
        asid_t           asid_base;
        word_t           index;
        word_t           depth;
        cap_t            untyped;
        cap_t            root;
        cte_t*           parentSlot;
        cte_t*           destSlot;
        lookupSlot_ret_t lu_ret;
        void*            frame;
        exception_t      status;

        if (label != RISCVASIDControlMakePool) {
            current_syscall_error.type = seL4_IllegalOperation;

            return EXCEPTION_SYSCALL_ERROR;
        }

        if (length < 2 || extraCaps.excaprefs[0] == NULL
                || extraCaps.excaprefs[1] == NULL) {
            current_syscall_error.type = seL4_TruncatedMessage;
            return EXCEPTION_SYSCALL_ERROR;
        }

        index = getSyscallArg(0, buffer);
        depth = getSyscallArg(1, buffer);
        parentSlot = extraCaps.excaprefs[0];
        untyped = parentSlot->cap;
        root = extraCaps.excaprefs[1]->cap;

        /* Find first free pool */
        for (i = 0; i < nASIDPools && riscvKSASIDTable[i]; i++);

        if (i == nASIDPools) {
            /* no unallocated pool is found */
            current_syscall_error.type = seL4_DeleteFirst;

            return EXCEPTION_SYSCALL_ERROR;
        }

        asid_base = i << asidLowBits;

        if (cap_get_capType(untyped) != cap_untyped_cap ||
                cap_untyped_cap_get_capBlockSize(untyped) != seL4_ASIDPoolBits ||
                cap_untyped_cap_get_capIsDevice(untyped)) {
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;

            return EXCEPTION_SYSCALL_ERROR;
        }

        status = ensureNoChildren(parentSlot);
        if (status != EXCEPTION_NONE) {
            return status;
        }

        frame = WORD_PTR(cap_untyped_cap_get_capPtr(untyped));

        lu_ret = lookupTargetSlot(root, index, depth);
        if (lu_ret.status != EXCEPTION_NONE) {
            return lu_ret.status;
        }
        destSlot = lu_ret.slot;

        status = ensureEmptySlot(destSlot);
        if (status != EXCEPTION_NONE) {
            return status;
        }

        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performASIDControlInvocation(frame, destSlot, parentSlot, asid_base);
    }

    case cap_asid_pool_cap: {
        cap_t        vspaceCap;
        cte_t*       vspaceCapSlot;
        asid_pool_t* pool;
        word_t i;
        asid_t       asid;

        if (label != RISCVASIDPoolAssign) {
            current_syscall_error.type = seL4_IllegalOperation;

            return EXCEPTION_SYSCALL_ERROR;
        }
        if (extraCaps.excaprefs[0] == NULL) {
            current_syscall_error.type = seL4_TruncatedMessage;

            return EXCEPTION_SYSCALL_ERROR;
        }

        vspaceCapSlot = extraCaps.excaprefs[0];
        vspaceCap = vspaceCapSlot->cap;

        if (cap_page_table_cap_get_capPTMappedASID(vspaceCap) != asidInvalid) {
            userError("RISCVASIDPool: Invalid vspace root.");
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 1;

            return EXCEPTION_SYSCALL_ERROR;
        }

        pool = riscvKSASIDTable[cap_asid_pool_cap_get_capASIDBase(cap) >> asidLowBits];
        if (!pool) {
            current_syscall_error.type = seL4_FailedLookup;
            current_syscall_error.failedLookupWasSource = false;
            current_lookup_fault = lookup_fault_invalid_root_new();
            return EXCEPTION_SYSCALL_ERROR;
        }

        if (pool != ASID_POOL_PTR(cap_asid_pool_cap_get_capASIDPool(cap))) {
            current_syscall_error.type = seL4_InvalidCapability;
            current_syscall_error.invalidCapNumber = 0;
            return EXCEPTION_SYSCALL_ERROR;
        }

        /* Find first free ASID */
        asid = cap_asid_pool_cap_get_capASIDBase(cap);
        for (i = 0; i < BIT(asidLowBits) && (asid + i == 0 || pool->array[i]); i++);

        if (i == BIT(asidLowBits)) {
            current_syscall_error.type = seL4_DeleteFirst;

            return EXCEPTION_SYSCALL_ERROR;
        }

        asid += i;

        setThreadState(NODE_STATE(ksCurThread), ThreadState_Restart);
        return performASIDPoolInvocation(asid, pool, vspaceCapSlot);
    }
    default:
        fail("Invalid arch cap type");
    }
}

exception_t
performPageTableInvocationMap(cap_t cap, cte_t *ctSlot,
                              pte_t pte, pte_t *ptSlot)
{
    ctSlot->cap = cap;
    *ptSlot = pte;

    return EXCEPTION_NONE;
}

exception_t
performPageTableInvocationUnmap(cap_t cap, cte_t *ctSlot)
{
    if (cap_page_table_cap_get_capPTIsMapped(cap)) {
        pte_t *pt = PTE_PTR(cap_page_table_cap_get_capPTBasePtr(cap));
        unmapPageTable(
            cap_page_table_cap_get_capPTMappedASID(cap),
            cap_page_table_cap_get_capPTMappedAddress(cap),
            pt
        );
        clearMemory((void *)pt, RISCV_4K_PageBits);
    }
    cap_page_table_cap_ptr_set_capPTIsMapped(&(ctSlot->cap), 0);

    return EXCEPTION_NONE;
}

static exception_t
performPageGetAddress(void *vbase_ptr)
{
    paddr_t capFBasePtr;

    /* Get the physical address of this frame. */
    capFBasePtr = addrFromPPtr(vbase_ptr);

    /* return it in the first message register */
    setRegister(NODE_STATE(ksCurThread), msgRegisters[0], capFBasePtr);
    setRegister(NODE_STATE(ksCurThread), msgInfoRegister,
                wordFromMessageInfo(seL4_MessageInfo_new(0, 0, 0, 1)));

    return EXCEPTION_NONE;
}

static exception_t updatePTE(pte_t pte, pte_range_t pte_entries)
{
    /* we only need to check the first entries because of how createSafeMappingEntries
     * works to preserve the consistency of tables */

    for (word_t i = 0; i < pte_entries.length; i++) {
        pte_entries.base[i] = pte;
    }
    asm volatile ("sfence.vma");
    return EXCEPTION_NONE;
}

exception_t performPageInvocationMapPTE(cap_t cap, cte_t *ctSlot,
                                        pte_t pte, pte_range_t pte_entries)
{
    ctSlot->cap = cap;
    return updatePTE(pte, pte_entries);
}

exception_t
performPageInvocationRemapPTE(pte_t pte, pte_range_t pte_entries)
{
    return updatePTE(pte, pte_entries);
}

exception_t
performPageInvocationUnmap(cap_t cap, cte_t *ctSlot)
{

    if (cap_frame_cap_get_capFMappedASID(cap) != asidInvalid) {
        unmapPage(cap_frame_cap_get_capFSize(cap),
                  cap_frame_cap_get_capFMappedASID(cap),
                  cap_frame_cap_get_capFMappedAddress(cap),
                  cap_frame_cap_get_capFBasePtr(cap)
                 );
    }
    ctSlot->cap = cap_frame_cap_set_capFMappedAddress(ctSlot->cap, 0);
    ctSlot->cap = cap_frame_cap_set_capFMappedASID(ctSlot->cap, asidInvalid);
    return EXCEPTION_NONE;
}

#ifdef CONFIG_PRINTING
void
Arch_userStackTrace(tcb_t *tptr)
{
    // RVTODO: implement
    /* Not implemented */
    printf("Arch_userStackTrace not implemented\n");
    // RVTODO: not having implemented a strictly informational call is not a halt'able offense
    halt();
}
#endif
