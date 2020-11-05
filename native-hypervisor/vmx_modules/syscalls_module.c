#include <vmx_modules/syscalls_module.h>
#include <vmx_modules/kpp_module.h>
#include <vmm/msr.h>
#include <debug.h>
#include <win_kernel/memory_manager.h>
#include <intrinsics.h>
#include <vmm/vm_operations.h>
#include <vmm/vmcs.h>
#include <win_kernel/syscall_handlers.h>
#include <vmm/memory_manager.h>
#include <vmm/vmm.h>

SYSCALL_HANDLER GetHandlerById(IN QWORD syscallId)
{
    switch(syscallId)
    {
        case NT_OPEN_PROCESS:
            return HandleNtOpenPrcoess;
    }
    return NULL;
}

STATUS SyscallsModuleInitializeAllCores(IN PSHARED_CPU_DATA sharedData, IN PMODULE module, IN PGENERIC_MODULE_DATA initData)
{
    PrintDebugLevelDebug("Starting initialization of syscalls module for all cores\n");
    sharedData->heap.allocate(&sharedData->heap, sizeof(SYSCALLS_MODULE_EXTENSION), &module->moduleExtension);
    SetMemory(module->moduleExtension, 0, sizeof(SYSCALLS_MODULE_EXTENSION));
    PSYSCALLS_MODULE_EXTENSION extension = module->moduleExtension;
    extension->kppModule = initData->syscallsModule.kppModule;
    extension->startExitCount = FALSE;
    extension->exitCount = 0;
    extension->syscallsData = &__ntDataStart;
    MapCreate(&extension->addressToSyscall, BasicHashFunction, BASIC_HASH_LEN);
    PrintDebugLevelDebug("Shared cores data successfully initialized for syscalls module\n");
    return STATUS_SUCCESS;
}

STATUS SyscallsModuleInitializeSingleCore(IN PSINGLE_CPU_DATA data)
{
    PrintDebugLevelDebug("Starting initialization of syscalls module on core #%d\n", data->coreIdentifier);
    // Hook the event of writing to the LSTAR MSR
    UpdateMsrAccessPolicy(data, MSR_IA32_LSTAR, FALSE, TRUE);
    PrintDebugLevelDebug("Finished initialization of syscalls module on core #%d\n", data->coreIdentifier);
    return STATUS_SUCCESS;
}

STATUS SyscallsDefaultHandler(IN PCURRENT_GUEST_STATE sharedData, IN PMODULE module)
{
    PSYSCALLS_MODULE_EXTENSION ext = (PSYSCALLS_MODULE_EXTENSION)module->moduleExtension;
    if(ext->exitCount++ >= COUNT_UNTIL_HOOK)
    {
        module->hasDefaultHandler = FALSE;
        BYTE_PTR ssdt, ntoskrnl, win32k;
        LocateSSDT(ext->lstar, &ssdt, ext->guestCr3);
        GetSystemTables(ssdt, &ext->ntoskrnl, &ext->win32k, ext->guestCr3);
        ASSERT(HookSystemCalls(ext->guestCr3, ext->ntoskrnl, ext->win32k, 1, NT_OPEN_PROCESS)
            == STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    return STATUS_VM_EXIT_NOT_HANDLED;
}

STATUS LocateSSDT(IN BYTE_PTR lstar, OUT BYTE_PTR* ssdt, IN QWORD guestCr3)
{
    // Pattern:
    // mov edi,eax
    // shr edi,7
    // and edi,20h
    // and eax,0FFFh
    BYTE pattern[] = { 0x8B, 0xF8, 0xC1, 0xEF, 0x07, 0x83, 0xE7, 0x20, 0x25, 0xFF, 0x0F, 0x00, 0x00 };
    BYTE kernelChunk[13];
    BYTE_PTR patternAddress;
    PrintDebugLevelDebug("Starting to search for the pattern: %.b in kernel's address space\n", 13, pattern);
    QWORD offset = 0x60D359;

    for(; offset < 0xffffffff; offset++)
    {
        if(CopyGuestMemory(kernelChunk, lstar - offset, 13) != STATUS_SUCCESS)
            continue;
        if(!CompareMemory(kernelChunk, pattern, 13))
        {
            Print("Pattern found in kernel's address space %8\n", offset);
            patternAddress = lstar - offset;
            goto SSDTFound;
        }
    }
    Print("Pattern was NOT found in kernel's address space\n");
    return STATUS_SSDT_NOT_FOUND;

SSDTFound:
    patternAddress += 13; // pattern
    patternAddress += 7;  // lea r10,[nt!KeServiceDescriptorTable]
    ASSERT(CopyGuestMemory(&offset, patternAddress + 3, sizeof(DWORD)) == STATUS_SUCCESS);
    *ssdt = (patternAddress + 7) + offset;
    return STATUS_SUCCESS;
}

VOID GetSystemTables(IN BYTE_PTR ssdt, OUT BYTE_PTR* ntoskrnl, OUT BYTE_PTR* win32k, IN QWORD guestCr3)
{
    ASSERT(CopyGuestMemory(ntoskrnl, ssdt, sizeof(QWORD)) == STATUS_SUCCESS);
    ASSERT(CopyGuestMemory(win32k, ssdt + 32, sizeof(QWORD)) == STATUS_SUCCESS);
}

STATUS HookSystemCalls(IN PMODULE module, IN QWORD guestCr3, IN BYTE_PTR ntoskrnl, IN BYTE_PTR win32k, 
    IN QWORD count, ...)
{
    va_list args;
    va_start(args, count);
    PSHARED_CPU_DATA shared = GetVMMStruct()->currentCPU->sharedData;
    PSYSCALLS_MODULE_EXTENSION ext = module->moduleExtension;

    while(count--)
    {
        // Get the syscall id from va_arg
        QWORD syscallId = va_arg(args, QWORD), offset = 0, functionAddress;
        // Get the offset of the syscall handler (in ntoskrnl.exe) from the shadowed SSDT
        ASSERT(CopyGuestMemory(&offset, ntoskrnl + syscallId * sizeof(DWORD), 
            sizeof(DWORD)) == STATUS_SUCCESS);
        // Get the guest physical address of the syscall handler
        ASSERT(TranslateGuestVirtualToGuestPhysicalUsingCr3(ntoskrnl + (offset >> 4), &functionAddress,
            guestCr3) == STATUS_SUCCESS);
        Print("Syscall ID: %d, Virtual: %8, Guest Physical: %8\n", syscallId, ntoskrnl + (offset >> 4),
             functionAddress);
        
        // Save hook information in system calls database
        QWORD physicalHookAddress = functionAddress + ext->syscallsData[syscallId].hookInstructionOffset;
        ext->syscallsData[syscallId].hookedInstructionAddress = physicalHookAddress;
        ext->syscallsData[syscallId].returnHookAddress = physicalHookAddress + 1;
        ext->syscallsData[syscallId].handler = GetHandlerById(syscallId);
        CopyMemory(ext->syscallsData[syscallId].hookedInstrucion, 
            TranslateGuestPhysicalToHostVirtual(physicalHookAddress),
            ext->syscallsData[syscallId].hookedInstructionLength);
        // Build the hook instruction
        BYTE hookInstruction[X86_MAX_INSTRUCTION_LEN] = { INT3_OPCODE };
        hookInstruction[1] = (ext->syscallsData[syscallId].hookReturnEvent) ? INT3_OPCODE : NOP_OPCODE;
        SetMemory(hookInstruction + 2, NOP_OPCODE, ext->syscallsData[syscallId].hookedInstructionLength - 2);
        // Inject the hooked instruction to the guest
        CopyMemory(TranslateGuestPhysicalToHostVirtual(physicalHookAddress), hookInstruction, 
            ext->syscallsData[syscallId].hookedInstructionLength);
        // Save the translation between the address and the syscall id
        MapSet(&ext->addressToSyscall, physicalHookAddress, syscallId);
        if(ext->syscallsData[syscallId].hookReturnEvent)
            MapSet(&ext->addressToSyscall, physicalHookAddress + 1, syscallId & RETURN_EVENT_FLAG);
        // Mark the page as unreadable & unwritable
        for(QWORD i = 0; i < shared->numberOfCores; i++)
            UpdateEptAccessPolicy(shared->cpuData[i], ALIGN_DOWN((QWORD)physicalHookAddress, PAGE_SIZE), 
                PAGE_SIZE, EPT_EXECUTE);
    }
    va_end(args);
    return STATUS_SUCCESS;
}

STATUS SyscallsHandleMsrWrite(IN PCURRENT_GUEST_STATE data, IN PMODULE module)
{
    // PatchGaurd might put a fake LSTAR value later, hence we save it now
    PREGISTERS regs = &data->guestRegisters;
    if(regs->rcx != MSR_IA32_LSTAR)
        return STATUS_VM_EXIT_NOT_HANDLED;
    QWORD msrValue = ((regs->rdx & 0xffffffff) << 32) | (regs->rax & 0xffffffff);
    Print("Guest attempted to write to LSTAR %8 MSR: %8\n", regs->rcx, msrValue);
    __writemsr(MSR_IA32_LSTAR, msrValue);
    regs->rip += vmread(VM_EXIT_INSTRUCTION_LEN);
    PSYSCALLS_MODULE_EXTENSION ext = module->moduleExtension;
    ext->lstar = msrValue;
    ext->startExitCount = TRUE;
    /* 
        Due to Meltdown mitigations, the address might be not mapped later. 
        Hence, we are saving the current CR3 for future usage.
    */
    ext->guestCr3 = vmread(GUEST_CR3);
    return STATUS_SUCCESS;
}

VOID BuildSyscallParamsArray(OUT QWORD_PTR arr, BYTE count)
{
    PREGISTERS regs = &GetVMMStruct()->guestRegisters;
    if(count >= 1)
        arr[0] = regs->rax;
    // continue this function
}

STATUS SyscallsHandleException(IN PCURRENT_GUEST_STATE data, IN PMODULE module)
{
    BYTE vector = vmread(VM_EXIT_INTR_INFO) & 0xff;
    if(vector != INT_BREAKPOINT)
        return STATUS_VM_EXIT_NOT_HANDLED;
    QWORD syscallId;
    PSYSCALLS_MODULE_EXTENSION ext = module->moduleExtension;
    if((syscallId = MapGet(&ext->addressToSyscall, data->guestRegisters.rip)) != MAP_KEY_NOT_FOUND)
    {
        QWORD params[17];
        BuildSyscallParamsArray(params, ext->syscallsData[syscallId].params);
        ext->syscallsData[syscallId].handler(params);
    }
    else
        InjectGuestInterrupt(INT_BREAKPOINT, 0);
    return STATUS_SUCCESS;
}