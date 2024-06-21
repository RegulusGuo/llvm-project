#include "llvm/Transforms/fpscan/fpscan.h"
#include "llvm/Support/raw_ostream.h"
using namespace ::llvm;

void printBB(const BasicBlock *BB) {
    for (auto &Inst : *BB) {
        const DebugLoc &dbg = Inst.getDebugLoc();
        // if (dbg) {
        //     // outs() << "In \033[32m" << dbg->getFilename() << ":\033[1m";
        //     if (dbg->getInlinedAt())
        //         outs() << dbg->getInlinedAt()->getLine() << "\033[0m inline from \033[34m" << dbg->getFilename() << ":\033[1m" << dbg->getLine();
        //     else
        //         outs() << dbg->getLine();
        //     outs() << "\033[0m:\n";
        // }
        outs() << "\033[31m" << Inst << "\033[0m\n";
    }
}

void printFunc(const Function *F) {
    for (auto &BB : *F) {
        for (auto &Inst : BB) {
            const DebugLoc &dbg = Inst.getDebugLoc();
            // if (dbg) {
            //     // outs() << "In \033[32m" << dbg->getFilename() << ":\033[1m";
            //     if (dbg->getInlinedAt())
            //         outs() << dbg->getInlinedAt()->getLine() << "\033[0m inline from \033[34m" << dbg->getFilename() << ":\033[1m" << dbg->getLine();
            //     else
            //         outs() << dbg->getLine();
            //     outs() << "\033[0m:\n";
            // }
            outs() << "\033[31m" << Inst << "\033[0m\n";
        }
    }
}


void FpInstrument::addFPLoad(LoadInst *Inst)
{
    FunctionCallee insertDecryptFunc = decryptFunc;
    if (Inst->getParent()->getParent()->getName() == "serial8250_register_8250_port" ||
        moduleName.find("fs/block_dev.c") != std::string::npos ||
        moduleName.find("drivers/mmc/host/mmc_spi.c") != std::string::npos ||
        Inst->getParent()->getParent()->getName() == "__kfree_skb" ||
        Inst->getParent()->getParent()->getName() == "module_attr_show") {
        insertDecryptFunc = decryptFuncCheck;
    }
    IRBuilder<> irBuilder(Inst->getNextNode());
    if (Inst->getType() == int64Type)
    {
        auto decryptedValue =
            irBuilder.CreateCall(insertDecryptFunc, {Inst, Inst->getPointerOperand()});
        auto ShouldReplace = [decryptedValue](Use &U) -> bool
        {
            return U.getUser() != decryptedValue;
        };
        Inst->replaceUsesWithIf(decryptedValue, ShouldReplace);
    }
    else
    {
        auto IntFP = irBuilder.CreatePtrToInt(Inst, int64Type);
        auto IntPtrFPAddr =
            irBuilder.CreateBitCast(Inst->getPointerOperand(), int64PtrType);
        auto decryptedValue =
            irBuilder.CreateCall(insertDecryptFunc, {IntFP, IntPtrFPAddr});
        auto decryptedPtr = irBuilder.CreateIntToPtr(decryptedValue, Inst->getType());
        auto ShouldReplace = [IntFP, decryptedValue](Use &U) -> bool
        {
            return U.getUser() != IntFP && U.getUser() != decryptedValue;
        };
        Inst->replaceUsesWithIf(decryptedPtr, ShouldReplace);
    }
}

void FpInstrument::addFPStore(StoreInst *Inst)
{
    FunctionCallee insertEncryptFunc = encryptFunc;
    // if (moduleName.find("arch/riscv/kernel/process.c") != std::string::npos)
    // {
    //     insertEncryptFunc = encryptFuncNoCheck;
    // }
    IRBuilder<> irBuilder(Inst);
    if (Inst->getType() == int64Type)
    {
        auto decryptedValue =
            irBuilder.CreateCall(insertEncryptFunc, {Inst->getValueOperand(), Inst->getPointerOperand()});
        irBuilder.CreateStore(decryptedValue, Inst->getPointerOperand());
    }
    else
    {
        auto IntFP = irBuilder.CreatePtrToInt(Inst->getValueOperand(), int64Type);
        auto IntPtrFPAddr =
            irBuilder.CreateBitCast(Inst->getPointerOperand(), int64PtrType);
        auto decryptedValue =
            irBuilder.CreateCall(insertEncryptFunc, {IntFP, IntPtrFPAddr});
        irBuilder.CreateStore(decryptedValue, IntPtrFPAddr);
    }
    Inst->eraseFromParent();
}

bool FpInstrument::instrumentLoadStore()
{
    bool changed = false;
    for (auto Fit = module->begin(), Fend = module->end(); Fit != Fend; ++Fit)
    {
        /* 
        1. vdso
            FIX:  Extra cr[e|d]tk in arch/riscv/kernel/vdso/vdso.so
            SOL:  Bypass functions in vgettimeofday.c:  __vdso_clock_gettime
                                                        __vdso_gettimeofday
                                                        __vdso_clock_getres
        2. kernel_thread
            pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
            fn will be assigned to struct kernel_clone_args.stack
            but we cannot mark kernel_clone_args.stack as fp since it may contains 
            user stack too.
        3. PPPGFPInit
            Global Function Pointer Initialization
        4. perCPUCopy
            memcpy patch for percpu data
        */
        std::string funcName = Fit->getName().str();
        if (funcName.find("__vdso") == 0 ||
            funcName == "kernel_thread" ||
            funcName == "PPPGFPInit" ||
            funcName == "perCPUCopy")
            continue;
        for (auto BBit = Fit->begin(), BBend = Fit->end(); BBit != BBend; ++BBit)
            for (auto Inst = BBit->begin(); Inst != BBit->end();)
            {
                auto nextInst = std::next(Inst, 1);
                if (Inst->getMetadata("StoreTag"))
                {
                    addFPStore(cast<StoreInst>(&*Inst));
                    // if (Inst->getParent())
                    //     printBB(Inst->getParent());
                    changed = true;
                }
                else if (Inst->getMetadata("LoadTag"))
                {
                    addFPLoad(cast<LoadInst>(&*Inst));
                    // if (Inst->getParent())
                    //     printBB(Inst->getParent());
                    changed = true;
                }
                Inst = nextInst;
            }
    }
    return changed;
}

bool FpInstrument::genGFPInitFunc()
{
    std::string thisModuleName = moduleName;
    // outs() << "\033[1;31m module name is " << thisModuleName << "\033[0m\n";
    std::replace(thisModuleName.begin(), thisModuleName.end(), '/', '_');
    std::replace(thisModuleName.begin(), thisModuleName.end(), '.', '_');
    std::replace(thisModuleName.begin(), thisModuleName.end(), '-', '_');
    std::string functionName = "PPPGFPInit_" + thisModuleName;
    // errs() << "\033[1;31m function name is " << functionName << "\033[0m\n";
    FunctionType *FT =
        FunctionType::get(Type::getVoidTy(module->getContext()), false);
    Function *initFunc =
        Function::Create(FT, Function::ExternalLinkage, functionName, *module);
    initFunc->setSection(".init.text");
    initFunc->addFnAttr(Attribute::Cold);
    initFunc->addFnAttr(Attribute::NoUnwind);
    auto BB = BasicBlock::Create(module->getContext(), "entry", initFunc);
    IRBuilder<> irBuilder(BB);

    bool changed = false;
    for (FpInfo &fpinfo : fpinit->GlobalFpInfo)
    {
        if (functionName.find("linux_arch_riscv_mm_init_c") != std::string::npos)
            continue;
        if (fpinfo.base->hasExternalLinkage() && !fpinfo.base->hasInitializer())
            continue;
        if (fpinfo.base->hasAtLeastLocalUnnamedAddr())
        {
            fpinfo.base->setUnnamedAddr(GlobalVariable::UnnamedAddr::None);
        }
        changed = true;
        IntegerType *int64Type = IntegerType::get(module->getContext(), 64); 
        
        auto baseAddr = irBuilder.CreatePtrToInt(fpinfo.base, int64Type);
        auto targetAddr = irBuilder.CreateAdd(
            baseAddr, ConstantInt::get(int64Type, fpinfo.offset));
        
        auto targetPtr =
            irBuilder.CreateIntToPtr(targetAddr, PointerType::get(int64Type, 0));
        auto loadInst = irBuilder.CreateLoad(int64Type, targetPtr);
        auto storeInst = irBuilder.CreateStore(loadInst, targetPtr);
        addFPStore(storeInst);
    }
    irBuilder.CreateRet(nullptr);
    if (!changed)
    {
        // outs() << "\033[1;31mNot changed\033[0m\n";
        initFunc->eraseFromParent();
    }
    else
    {
        // outs() << "\033[1;31mChanged\033[0m\n";
        auto FPtype = PointerType::getUnqual(FT);
        auto GFPInitFuncFP = cast<GlobalVariable>(module->getOrInsertGlobal("FP_" + functionName, FPtype));
        GFPInitFuncFP->setInitializer(initFunc);
        GFPInitFuncFP->setSection(".init.GFPInitFP");
    }
    return changed;
}


bool FpInstrument::instrumentMemcpy()
{
    FunctionCallee insertEncryptFunc = encryptFuncNoCheck, insertDecryptFunc = decryptFuncNoCheck;
    // if (module->getName().str().find("arch/riscv/kernel/process.c") != std::string::npos)
    // {
    //     errs() << "\033[31m[instrument]\033[0m find process.c\n";
    //     insertEncryptFunc = encryptFuncNoCheck;
    //     insertDecryptFunc = encryptFuncNoCheck;
    // }

    // auto MemcpyLogFunc = module->getOrInsertFunction("Logmemcpy", MemCpyType);

    // collect all memcpy/memmove/kmemdup call
    std::vector<CallInst *> MemcpyCallVec;
    for (auto &F : *module)
    {
        for (auto &BB : F)
            for (auto &Inst : BB)
            {
                if (auto CallPtr = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = CallPtr->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    std::string funcName = calledFunc->getName().str();
                    if (funcName.find("llvm.memcpy") == 0 ||
                        funcName.find("llvm.memmove") == 0 ||
                        funcName == "__memcpy" ||
                        funcName == "memcpy" ||
                        funcName == "memmove" ||
                        funcName == "__memmove" ||
                        funcName == "kmemdup")
                        MemcpyCallVec.push_back(CallPtr);
                }
            }
    }
    if (MemcpyCallVec.empty())
        return false;

    // patch percpu data memcpy in percpu.c
    if (moduleName.find("mm/percpu.c") != std::string::npos)
    {
        FunctionType *perCPUCopyFuncType = FunctionType::get(voidType, {voidPtrType, voidPtrType}, false);
        auto perCPUCopyFunc = module->getOrInsertFunction("perCPUCopy", perCPUCopyFuncType);
        for (auto &F : *module)
        {
            auto funcName = F.getName().str();
            if (funcName != "pcpu_embed_first_chunk" &&
                funcName != "pcpu_page_first_chunk")
                continue;
            for (auto &BB : F)
                for (auto &Inst : BB)
                {
                    if (auto CallPtr = dyn_cast<CallInst>(&Inst))
                    {
                        auto calledFunc = CallPtr->getCalledFunction();
                        if (!calledFunc)
                            continue;
                        std::string funcName = calledFunc->getName().str();
                        if (funcName.find("llvm.memcpy") == 0)
                        {
                            IRBuilder<> irBuilder(CallPtr->getNextNode());
                            irBuilder.CreateCall(perCPUCopyFunc,
                                                 {CallPtr->getArgOperand(0), CallPtr->getArgOperand(1)});
                            break;
                        }
                    }
                }
        }
    }

    bool changed = false;
    for (auto CallPtr : MemcpyCallVec)
    {
        // for all callsites, check whether one of the memory region contains function pointers
        // get the original type of dest and src, check whether one of the type contains function pointers
        Value *srcMem, *destMem, *copyLen;
        auto funcName = CallPtr->getCalledFunction()->getName().str();
        if (funcName == "kmemdup")
        {
            destMem = CallPtr;
            srcMem = CallPtr->getArgOperand(0);
            copyLen = CallPtr->getArgOperand(1);
        }
        else
        {
            destMem = CallPtr->getArgOperand(0);
            srcMem = CallPtr->getArgOperand(1);
            copyLen = CallPtr->getArgOperand(2);
        }
        Type *destType = srcMem->getType()->getPointerElementType();
        Type *srcType = destMem->getType()->getPointerElementType();
        if (auto srcBitCastOperator = dyn_cast<BitCastOperator>(srcMem))
        {
            if (srcBitCastOperator->getSrcTy()->isPointerTy())
                srcType = srcBitCastOperator->getSrcTy()->getPointerElementType();
        }
        else if (auto srcCastInst = dyn_cast<CastInst>(srcMem))
        {
            if (srcCastInst->getSrcTy()->isPointerTy())
                srcType = srcCastInst->getSrcTy()->getPointerElementType();
        }
        if (auto destBitCastOperator = dyn_cast<BitCastOperator>(destMem))
        {
            if (destBitCastOperator->getSrcTy()->isPointerTy())
                destType = destBitCastOperator->getSrcTy()->getPointerElementType();
        }
        else if (auto destCastInst = dyn_cast<CastInst>(destMem))
        {
            if (destCastInst->getSrcTy()->isPointerTy())
                destType = destCastInst->getSrcTy()->getPointerElementType();
        }
        while (srcType->isArrayTy())
            srcType = cast<ArrayType>(srcType)->getElementType();
        while (destType->isArrayTy())
            destType = cast<ArrayType>(destType)->getElementType();

        auto &srcFPOffset = fpinit->GetTypeFPOffset(srcType);
        auto &destFPOffset = fpinit->GetTypeFPOffset(destType);

        if (srcFPOffset.empty() && destFPOffset.empty())
            continue;
        changed = true;

        // replace the llvm.memcpy and llvm.memmove with memcpy and memmove
        // Reason: further optimization may remove them, causing our instruments to be wrong
        // TODO: place the pass after the memcpy optimization pass
        auto oldCallPtr = CallPtr;
        IRBuilder<> irBuilder(oldCallPtr);
        if (cast<IntegerType>(copyLen->getType())->getBitWidth() != 64)
            copyLen = irBuilder.CreateZExt(copyLen, int64Type);
        if (funcName.find("llvm.memcpy") == 0)
        {
            CallPtr = irBuilder.CreateCall(memcpyFunc, {destMem, srcMem, copyLen});
            oldCallPtr->eraseFromParent();
        }
        else if (funcName.find("llvm.memmove") == 0)
        {
            CallPtr = irBuilder.CreateCall(memmoveFunc, {destMem, srcMem, copyLen});
            oldCallPtr->eraseFromParent();
        }

        // Determine the function pointers offset
        // desttype and srctype may contains different numbers of function pointers
        // we use the larger one, and truncate it according to memcpy/memmove size if it is constant
        auto FPOffset = destFPOffset.size() > srcFPOffset.size() ? destFPOffset : srcFPOffset;
        if (isa<ConstantInt>(copyLen))
        {
            auto len = cast<ConstantInt>(copyLen)->getSExtValue();
            for (auto it = FPOffset.begin(), end = FPOffset.end(); it != end; ++it)
            {
                if (*it >= len)
                {
                    FPOffset.erase(it, end);
                    break;
                }
            }
        }

        // instrument part
        auto prevBasicBlock = CallPtr->getParent();
        auto InsertCondBB = SplitBlock(prevBasicBlock, CallPtr->getNextNode());
        // auto InsertCondBB = SplitBlock(prevBasicBlock, CallPtr);
        auto nextBasicBlock = SplitEdge(prevBasicBlock, InsertCondBB);

        auto InsertBodyBB = BasicBlock::Create(CallPtr->getContext(), "", CallPtr->getParent()->getParent());

        auto CondBB = BasicBlock::Create(CallPtr->getContext(), "", CallPtr->getParent()->getParent(), InsertCondBB);
        // IRBuilder<> BaseVarBuilder(CallPtr->getNextNode()), CondBuilder(InsertCondBB), BodyBuilder(InsertBodyBB);
        IRBuilder<> BaseVarBuilder(CallPtr->getNextNode()), CondBuilder(CondBB), BodyBuilder(InsertBodyBB);
        IRBuilder<> NextBuilder(nextBasicBlock);
        nextBasicBlock->begin()->eraseFromParent();
        NextBuilder.CreateBr(CondBB);
        // InsertCondBB->begin()->eraseFromParent();

        auto baseVarAddr = BaseVarBuilder.CreateAlloca(int64Type);
        BaseVarBuilder.CreateStore(ConstantInt::get(int64Type, 0), baseVarAddr);

        if (cast<IntegerType>(copyLen->getType())->getBitWidth() != 64)
            copyLen = CondBuilder.CreateZExt(copyLen, int64Type);
            // copyLen = BaseVarBuilder.CreateZExt(copyLen, int64Type);
        
        auto baseVarInCond = CondBuilder.CreateLoad(baseVarAddr);
        // auto baseVarInCond = BaseVarBuilder.CreateLoad(baseVarAddr);

        auto condValue = CondBuilder.CreateICmpULT(baseVarInCond, copyLen);
        // auto condValue = BaseVarBuilder.CreateICmpULT(baseVarInCond, copyLen);

        CondBuilder.CreateCondBr(condValue, InsertBodyBB, InsertCondBB);
        // CondBuilder.CreateCondBr(condValue, InsertBodyBB, nextBasicBlock);
        // BaseVarBuilder.CreateCondBr(condValue, InsertBodyBB, nextBasicBlock);

        auto baseVarBody = BodyBuilder.CreateLoad(baseVarAddr);
        auto destAddrInt = BodyBuilder.CreatePtrToInt(destMem, int64Type);
        auto srcAddrInt = BodyBuilder.CreatePtrToInt(srcMem, int64Type);

        for (auto offset : FPOffset)
        {
            auto containFPOffset = BodyBuilder.CreateAdd(baseVarBody, ConstantInt::get(int64Type, offset));
            auto srcFPAddrInt = BodyBuilder.CreateAdd(srcAddrInt, containFPOffset);
            auto destFPAddrInt = BodyBuilder.CreateAdd(destAddrInt, containFPOffset);
            auto srcFPAddr = BodyBuilder.CreateIntToPtr(srcFPAddrInt, int64PtrType);
            auto destFPAddr = BodyBuilder.CreateIntToPtr(destFPAddrInt, int64PtrType);
            auto loadFP = BodyBuilder.CreateLoad(destFPAddr);
            auto decryptedFP = BodyBuilder.CreateCall(insertDecryptFunc, {loadFP, srcFPAddr});
            auto encryptedFP = BodyBuilder.CreateCall(insertEncryptFunc, {decryptedFP, destFPAddr});
            BodyBuilder.CreateStore(encryptedFP, destFPAddr);
        }
        unsigned destSize = module->getDataLayout().getTypeAllocSize(destType);
        unsigned srcSize = module->getDataLayout().getTypeAllocSize(srcType);
        unsigned elementSize = destSize > srcSize ? destSize : srcSize;
        auto newBaseVar = BodyBuilder.CreateAdd(baseVarBody, ConstantInt::get(int64Type, elementSize));
        BodyBuilder.CreateStore(newBaseVar, baseVarAddr);
        // BodyBuilder.CreateBr(InsertCondBB);
        BodyBuilder.CreateBr(CondBB);
        // outs() << "\033[32mprint prevBB\033[0m\n";
        // printBB(prevBasicBlock);
        // outs() << "\033[32mprint nextBB\033[0m\n";
        // printBB(nextBasicBlock);
        // outs() << "\033[32mprint CondBB\033[0m\n";
        // printBB(CondBB);
        // outs() << "\033[32mprint InsertCondBB\033[0m\n";
        // printBB(InsertCondBB);
        // outs() << "\033[32mprint InsertBodyBB\033[0m\n";
        // printBB(InsertBodyBB);
        // outs() << "\033[32mprint CallPtr->getParent()->getParent() 1\033[0m\n";
        // printFunc(CallPtr->getParent()->getParent());
    }
    return changed;
}

bool FpInstrument::getPerCPUFP()
{
    static const std::set<std::string> perCPUSection = {
        ".data..percpu..first",
        ".data..percpu..page_aligned",
        ".data..percpu..read_mostly",
        ".data..percpu",
        ".data..percpu..shared_aligned",
    };

    FunctionType *perCPUCopyFuncType = FunctionType::get(voidType, {voidPtrType, voidPtrType}, false);
    std::string thisModuleName = moduleName;
    std::replace(thisModuleName.begin(), thisModuleName.end(), '/', '_');
    std::replace(thisModuleName.begin(), thisModuleName.end(), '.', '_');
    std::replace(thisModuleName.begin(), thisModuleName.end(), '-', '_');
    std::string functionName = "PPPPerCPUDataCopy_" + thisModuleName;
    Function *F =
        Function::Create(perCPUCopyFuncType, Function::ExternalLinkage, functionName, *module);
    auto BB = BasicBlock::Create(module->getContext(), "entry", F);
    IRBuilder<> irBuilder(BB);

    bool changed = false;
    for (FpInfo &fpinfo : fpinit->GlobalFpInfo)
    {
        if (fpinfo.base->hasExternalLinkage() && !fpinfo.base->hasInitializer())
            continue;
        if (perCPUSection.find(fpinfo.base->getSection().str()) != perCPUSection.end())
        {
            changed = true;
            auto objAddrInt = irBuilder.CreatePtrToInt(fpinfo.base, int64Type);
            auto srcAddrInt = irBuilder.CreatePtrToInt(F->getArg(1), int64Type);
            auto destAddrInt = irBuilder.CreatePtrToInt(F->getArg(0), int64Type);
            auto objOffset = irBuilder.CreateSub(objAddrInt, srcAddrInt);
            auto targetOffset = irBuilder.CreateAdd(objOffset, ConstantInt::get(int64Type, fpinfo.offset));
            auto srcTargetAddrInt = irBuilder.CreateAdd(srcAddrInt, targetOffset);
            auto destTargetAddrInt = irBuilder.CreateAdd(destAddrInt, targetOffset);
            auto srcTargetAddr = irBuilder.CreateIntToPtr(srcTargetAddrInt, int64PtrType);
            auto destTargetAddr = irBuilder.CreateIntToPtr(destTargetAddrInt, int64PtrType);
            auto oldValue = irBuilder.CreateLoad(destTargetAddr);
            auto decryptedValue = irBuilder.CreateCall(decryptFunc, {oldValue, srcTargetAddr});
            auto encryptedValue = irBuilder.CreateCall(encryptFunc, {decryptedValue, destTargetAddr});
            irBuilder.CreateStore(encryptedValue, destTargetAddr);
        }
    }
    irBuilder.CreateRet(nullptr);
    if (!changed)
    {
        F->eraseFromParent();
    }
    else
    {
        auto FPtype = PointerType::getUnqual(perCPUCopyFuncType);
        auto GFPInitFuncFP = cast<GlobalVariable>(module->getOrInsertGlobal("perCPUCopyFP_" + functionName, FPtype));
        GFPInitFuncFP->setInitializer(F);
        GFPInitFuncFP->setSection(".perCPUCopy");
    }
    return changed;
}

bool FpInstrument::instrumentZalloc()
{
    std::vector<CallInst *> CallVec;
    for (auto &F : *module)
    {
        for (auto &BB : F)
            for (auto &Inst : BB)
            {
                if (auto CallPtr = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = CallPtr->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    auto funcName = calledFunc->getName().str();
                    if (funcName == "kmem_cache_alloc" ||
                        funcName == "__kmalloc")
                    {
                        // get flag
                        auto allocFlag = CallPtr->getArgOperand(1);
                        if (isa<ConstantInt>(allocFlag))
                        {
                            auto flag = cast<ConstantInt>(allocFlag)->getZExtValue();
                            if (!(flag & 0x100))
                                continue;
                        }
                        CallVec.push_back(CallPtr);
                    }
                    else if (funcName == "kzalloc")
                    {
                        CallVec.push_back(CallPtr);
                    }
                }
            }
    }

    bool changed = false;
    int cnt = 0;
    for (auto CallPtr : CallVec)
    {
        // get type
        Type *allocType = nullptr;
        std::string BBName = CallPtr->getParent()->getName().str();
        std::string FuncName = CallPtr->getParent()->getParent()->getName().str();

        bool hasCast = false;
        for (User *U : CallPtr->users())
        {
            if (auto CastPtr = dyn_cast<CastInst>(U))
            {
                if (!CastPtr->getDestTy()->isPointerTy())
                    continue;
                if (!CastPtr->getDestTy()->getPointerElementType()->isStructTy())
                    continue;
                if (!hasCast)
                {
                    hasCast = true;
                    allocType = CastPtr->getDestTy()->getPointerElementType();
                }
                else
                {
                    allocType = nullptr;
                    break;
                }
            }
        }

        // patch1: net/core/rtnetlink rtnl_register_internal
        // line 202 struct rtnl_link *link = kzalloc(sizeof(*link), GFP_KERNEL);
        // LLVM IR, BasicBlock if.else66
        if (BBName == "if.else66" && FuncName == "rtnl_register_internal")
        {
            for (auto type : module->getIdentifiedStructTypes())
            {
                if (type->hasName() && type->getName() == "struct.rtnl_link")
                {
                    allocType = type;
                    break;
                }
            }
        }
        // patch2: kernel/params add_sysfs_param
        // line 619 mk->mp = kzalloc(sizeof(*mk->mp), GFP_KERNEL);
        // type: struct module_param_attrs
        // LLVM IR, BasicBlock if.then14
        if (BBName == "if.then14" && FuncName == "add_sysfs_param")
        {
            for (auto type : module->getIdentifiedStructTypes())
            {
                if (type->hasName() && type->getName() == "struct.module_param_attrs")
                {
                    allocType = type;
                    break;
                }
            }
        }
        // patch3: driver/spi/spi.c spi_alloc_device
        // line 508 spi = kzalloc(sizeof(*spi), GFP_KERNEL);
        // type struct spi_device
        if (BBName == "if.end" && FuncName == "spi_alloc_device")
        {
            for (auto type : module->getIdentifiedStructTypes())
            {
                if (type->hasName() && type->getName() == "struct.spi_device")
                {
                    allocType = type;
                    break;
                }
            }
        }
        // patch4: driver/spi/spi.c __spi_alloc_controller
        // line 2405: ctlr = kzalloc(size + ctlr_size, GFP_KERNEL);
        // type struct spi_controller
        if (BBName == "if.end" && FuncName == "__spi_alloc_controller")
        {
            for (auto type : module->getIdentifiedStructTypes())
            {
                if (type->hasName() && type->getName() == "struct.spi_controller")
                {
                    allocType = type;
                    break;
                }
            }
        }

        if (!allocType)
        {
            continue;
        }

        auto FPOffset = fpinit->GetTypeFPOffset(allocType);
        if (FPOffset.empty())
            continue;
        changed = true;
        // instrument
        auto prevBasicBlock = CallPtr->getParent();
        auto nextBasicBlock = SplitBlock(prevBasicBlock, CallPtr->getNextNode());
        auto instrumentBB = BasicBlock::Create(CallPtr->getContext(),
                                               "kmem_cache_zalloc_instrument" + std::to_string(cnt++), CallPtr->getParent()->getParent());

        prevBasicBlock->back().eraseFromParent();
        IRBuilder<> prevIrBuilder(prevBasicBlock);
        auto baseAddr = prevIrBuilder.CreatePtrToInt(CallPtr, int64Type);
        auto cond = prevIrBuilder.CreateICmpNE(CallPtr, ConstantPointerNull::get(voidPtrType));
        prevIrBuilder.CreateCondBr(cond, instrumentBB, nextBasicBlock);

        IRBuilder<> instrumentIrBuilder(instrumentBB);
        for (auto offset : FPOffset)
        {
            auto fpAddrInt = instrumentIrBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
            auto fpAddr = instrumentIrBuilder.CreateIntToPtr(fpAddrInt, int64PtrType);
            auto encryptedValue = instrumentIrBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), fpAddr});
            instrumentIrBuilder.CreateStore(encryptedValue, fpAddr);
        }
        instrumentIrBuilder.CreateBr(nextBasicBlock);
    }
    return changed;
}

bool FpInstrument::instrumentMemset()
{

    // collect all memset call
    std::vector<CallInst *> MemsetCallVec;
    for (auto &F : *module)
    {
        if (F.getName().str() == "radix_tree_node_rcu_free")
            continue;
        for (auto &BB : F)
            for (auto &Inst : BB)
            {
                if (auto CallPtr = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = CallPtr->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    std::string funcName = calledFunc->getName().str();
                    if (funcName.find("llvm.memset") == 0)
                    {
                        if (auto setValue = dyn_cast<ConstantInt>(CallPtr->getArgOperand(1)))
                        {
                            if (setValue->getZExtValue() == 0)
                                MemsetCallVec.push_back(CallPtr);
                        }
                    }
                }
            }
    }
    if (MemsetCallVec.empty())
        return false;

    bool changed = false;
    for (auto CallPtr : MemsetCallVec)
    {
        // for all callsites, check whether one of the memory region contains function pointers
        // get the original type of dest and src, check whether one of the type contains function pointers
        Type *type = nullptr;
        auto setAddr = CallPtr->getArgOperand(0);
        if (auto srcCastInst = dyn_cast<CastInst>(setAddr))
        {
            if (srcCastInst->getSrcTy()->isPointerTy())
                type = srcCastInst->getSrcTy()->getPointerElementType();
        }
        else
        {
            if (!isa<Instruction>(setAddr))
                continue;
            bool hasCast = false;
            for (User *U : setAddr->users())
            {
                if (auto CastPtr = dyn_cast<CastInst>(U))
                {
                    if (!CastPtr->getDestTy()->isPointerTy())
                        continue;
                    if (!CastPtr->getDestTy()->getPointerElementType()->isStructTy())
                        continue;
                    if (!hasCast)
                    {
                        hasCast = true;
                        type = CastPtr->getDestTy()->getPointerElementType();
                    }
                    else
                    {
                        type = nullptr;
                        break;
                    }
                }
            }
        }

        // patch
        if (CallPtr->getParent()->getName() == "if.end16" &&
            CallPtr->getParent()->getParent()->getName() == "__alloc_skb")
        {
            for (auto mytype : module->getIdentifiedStructTypes())
            {
                if (mytype->hasName() && mytype->getName() == "struct.sk_buff")
                {
                    type = mytype;
                    break;
                }
            }
        }

        if (!type)
            continue;
        auto FPOffset = fpinit->GetTypeFPOffset(type);
        if (FPOffset.empty())
            continue;

        auto setLen = CallPtr->getArgOperand(2);
        if (isa<ConstantInt>(setLen))
        {
            auto len = cast<ConstantInt>(setLen)->getSExtValue();
            for (auto it = FPOffset.begin(), end = FPOffset.end(); it != end; ++it)
            {
                if (*it >= len)
                {
                    FPOffset.erase(it, end);
                    break;
                }
            }
        }

        auto oldCallPtr = CallPtr;
        IRBuilder<> irBuilder(oldCallPtr);
        CallPtr = irBuilder.CreateCall(memsetFunc, {CallPtr->getArgOperand(0),
                                                    CallPtr->getArgOperand(1),
                                                    CallPtr->getArgOperand(2)});
        oldCallPtr->eraseFromParent();

        changed = true;
        // instrument part
        IRBuilder<> instrumentIrBuilder(CallPtr->getNextNode());
        auto baseAddr = instrumentIrBuilder.CreatePtrToInt(setAddr, int64Type);
        for (auto offset : FPOffset)
        {
            auto fpAddrInt = instrumentIrBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
            auto fpAddr = instrumentIrBuilder.CreateIntToPtr(fpAddrInt, int64PtrType);
            auto encryptedValue = instrumentIrBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), fpAddr});
            instrumentIrBuilder.CreateStore(encryptedValue, fpAddr);
        }
    }
    return changed;
}

bool FpInstrument::instrumentPopulateProperties()
{ // drivers/of/fdt.c
    // function: populate_properties
    // 2 times: pp = unflatten_dt_alloc(mem, sizeof(struct property), __alignof__(struct property));
    // instrument line 164(BasicBlock if.end31) and line 192(BasicBlock if.end67.thread134:)
    // pp address: and.i and and.i131
    if (module->getName().str().find("drivers/of/fdt.c") == std::string::npos)
        return false;

    Type *PropertyType;
    for (auto type : module->getIdentifiedStructTypes())
    {
        if (type->hasName() && type->getName() == "struct.property")
        {
            PropertyType = type;
            break;
        }
    }
    auto &FPOffset = fpinit->GetTypeFPOffset(PropertyType);

    auto F = module->getFunction("populate_properties");
    Value *baseAddr1, *baseAddr2;
    for (auto &BB : *F)
        for (auto &inst : BB)
        {
            if (inst.hasName())
            {
                auto name = inst.getName().str();
                if (name == "and.i")
                {
                    baseAddr1 = &inst;
                }
                else if (name == "and.i131")
                {
                    baseAddr2 = &inst;
                }
            }
        }

    for (auto &BB : *F)
    {
        if (BB.hasName())
        {
            auto name = BB.getName().str();
            Value *baseAddr = nullptr;
            if (name == "if.end31")
                baseAddr = baseAddr1;
            else if (name == "if.end67.thread134")
                baseAddr = baseAddr2;
            if (!baseAddr)
                continue;
            IRBuilder<> irBuilder(&*(BB.begin()));
            for (auto offset : FPOffset)
            {
                auto fpAddrInt = irBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
                auto fpAddr = irBuilder.CreateIntToPtr(fpAddrInt, int64PtrType);
                auto encryptedValue = irBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), fpAddr});
                irBuilder.CreateStore(encryptedValue, fpAddr);
            }
        }
    }
    return true;
}

bool FpInstrument::instrumentAddSysfsParam()
{ // file: kernel/params.c function: add_sysfs_param, line 632
    // new_mp = krealloc(mk->mp, sizeof(*mk->mp) + sizeof(mk->mp->attrs[0]) * (mk->mp->num + 1), GFP_KERNEL);
    // LLVM IR BasicBlock if.end31
    if (module->getName().str().find("kernel/params.c") == std::string::npos)
        return false;
    Type *allocType;
    for (auto type : module->getIdentifiedStructTypes())
    {
        if (type->hasName() && type->getName() == "struct.module_param_attrs")
        {
            allocType = type;
            break;
        }
    }
    Function *F = module->getFunction("add_sysfs_param");
    for (auto &BB : *F)
    {
        if (BB.getName() != "if.end31")
            continue;
        Instruction *inst;
        for (inst = &*BB.begin();; inst = inst->getNextNode())
        {
            if (auto callInst = dyn_cast<CallInst>(inst))
            {
                auto calledFunc = callInst->getCalledFunction();
                if (!calledFunc)
                    continue;
                if (calledFunc->getName().str() == "krealloc")
                    break;
            }
        }
        IRBuilder<> prevBuilder(inst), nextBuilder(inst->getNextNode());
        Value *befAddr = cast<CallInst>(inst)->getArgOperand(0);
        Value *befAddrInt = prevBuilder.CreatePtrToInt(befAddr, int64Type);
        Value *afterAddrInt = nextBuilder.CreatePtrToInt(inst, int64Type);
        auto &FPOffset = fpinit->GetTypeFPOffset(allocType);
        for (auto offset : FPOffset)
        {
            auto befFpAddrInt = prevBuilder.CreateAdd(befAddrInt, ConstantInt::get(int64Type, offset));
            auto befFpAddr = prevBuilder.CreateIntToPtr(befFpAddrInt, int64PtrType);
            auto value = prevBuilder.CreateLoad(befFpAddr);
            auto decryptedValue = prevBuilder.CreateCall(decryptFunc, {value, befFpAddr});
            auto afterFpAddrInt = nextBuilder.CreateAdd(afterAddrInt, ConstantInt::get(int64Type, offset));
            auto afterFpAddr = nextBuilder.CreateIntToPtr(afterFpAddrInt, int64PtrType);
            auto encryptedValue = nextBuilder.CreateCall(encryptFunc, {decryptedValue, afterFpAddr});
            nextBuilder.CreateStore(encryptedValue, afterFpAddr);
        }
        break;
    }
    return true;
}

bool FpInstrument::instrumentXasInit()
{
    // Patch XA_STATE and XA_STATE_ORDER
    // pattern:
    // %xa_alloc(.i) = getelementptr inbounds %struct.xa_state, %struct.xa_state* %xas.i, i64 0, i32 7,
    // %2 = bitcast %struct.xa_node** %xa_alloc(.i) to i8*,
    // call void @llvm.memset.p0i8.i64(i8* align 8 dereferenceable(16) %2, i8 0, i64 16, i1 false)

    std::vector<CallInst *> insertPosVec;
    for (auto &F : *module)
        for (auto &BB : F)
            for (auto &Inst : BB)
            {
                if (auto callInst = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = callInst->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    if (calledFunc->getName().str().find("llvm.memset") == std::string::npos)
                        continue;
                    auto setValue = dyn_cast<ConstantInt>(callInst->getArgOperand(1));
                    auto setLen = dyn_cast<ConstantInt>(callInst->getArgOperand(2));
                    if (!setValue || !setLen ||
                        setValue->getZExtValue() != 0 || setLen->getZExtValue() != 16)
                        continue;
                    auto befInst = callInst->getPrevNode();
                    if (!befInst)
                        continue;
                    if (auto CastPtr = dyn_cast<CastInst>(befInst))
                    {
                        auto srcOperand = CastPtr->getOperand(0);
                        if (srcOperand->hasName() && srcOperand->getName().str().find("xa_alloc") == 0)
                        {
                            insertPosVec.push_back(callInst);
                        }
                    }
                }
            }

    for (auto insertInst : insertPosVec)
    {
        IRBuilder<> irBuilder(insertInst->getNextNode());
        auto baseAddrInt = irBuilder.CreatePtrToInt(insertInst->getOperand(0), int64Type);
        auto FPAddrInt = irBuilder.CreateAdd(baseAddrInt, ConstantInt::get(int64Type, 8));
        auto FPAddr = irBuilder.CreateIntToPtr(FPAddrInt, int64PtrType);
        auto encryptedValue = irBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), FPAddr});
        irBuilder.CreateStore(encryptedValue, FPAddr);
    }
    return !insertPosVec.empty();
}

bool FpInstrument::instrumentCryptoCreateTfm()
{ // file: crypto/api.c function: crypto_create_tfm line: 448
    // mem = kzalloc(total, GFP_KERNEL);
    // tfm = (struct crypto_tfm *)(mem + tfmsize);
    // tfm should be initialized
    // LLVM IR:
    // %call6 = call fastcc i8* @kzalloc(i64 %conv5)
    // ...
    // if.end:
    //  %add.ptr = getelementptr i8, i8* %call6, i64 %conv
    //  %6 = bitcast i8* %add.ptr to %struct.crypto_tfm*

    if (module->getName().str().find("crypto/api.c") == std::string::npos)
        return false;
    for (auto &F : *module)
    {
        if (F.getName() != "crypto_create_tfm")
            continue;
        for (auto &BB : F)
        {
            if (BB.getName() != "if.end")
                continue;
            for (auto &Inst : BB)
            {
                if (auto castInst = dyn_cast<CastInst>(&Inst))
                {
                    if (castInst->getOperand(0)->getName() != "add.ptr")
                        continue;
                    auto FPOffset = fpinit->GetTypeFPOffset(castInst->getType()->getPointerElementType());
                    IRBuilder<> irBuilder(castInst->getNextNode());
                    auto baseAddr = irBuilder.CreatePtrToInt(castInst, int64Type);
                    for (auto offset : FPOffset)
                    {
                        auto fpAddrInt = irBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
                        auto fpAddr = irBuilder.CreateIntToPtr(fpAddrInt, int64PtrType);
                        auto encryptedValue = irBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), fpAddr});
                        irBuilder.CreateStore(encryptedValue, fpAddr);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

bool FpInstrument::instrumentAddSectAttrs()
{
    // kernel/module.c function:add_sect_attrs line:1577
    // struct module_sect_attrs *sect_attrs;
    // sect_attrs = kzalloc(size[0] + size[1], GFP_KERNEL);
    // LLVM IR(BasicBlock for.end.i:)
    // %call15.i = call fastcc i8* @kzalloc(i64 %conv14.i)
    // %79 = bitcast i8* %call15.i to %struct.module_sect_attrs*
    // inlined in `mod_sysfs_setup` function
    if (module->getName().str().find("kernel/module.c") == std::string::npos)
        return false;
    for (auto &F : *module)
    {
        if (F.getName() != "mod_sysfs_setup")
            continue;
        for (auto &BB : F)
        {
            if (BB.getName() != "for.end.i")
                continue;
            for (auto &Inst : BB)
            {
                if (auto CallPtr = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = CallPtr->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    std::string funcName = calledFunc->getName().str();
                    if (funcName != "kzalloc")
                        continue;
                    auto type = CallPtr->getNextNode()->getType()->getPointerElementType();
                    auto FPOffset = fpinit->GetTypeFPOffset(type);
                    IRBuilder<> irBuilder(CallPtr->getNextNode());
                    auto baseAddr = irBuilder.CreatePtrToInt(CallPtr, int64Type);
                    for (auto offset : FPOffset)
                    {
                        auto fpAddrInt = irBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
                        auto fpAddr = irBuilder.CreateIntToPtr(fpAddrInt, int64PtrType);
                        auto encryptedValue = irBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), fpAddr});
                        irBuilder.CreateStore(encryptedValue, fpAddr);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

bool FpInstrument::netdeviceAlloc()
{
    if (module->getName().str().find("net/core/dev.c") == std::string::npos)
        return false;
    Type *type;
    for (auto mytype : module->getIdentifiedStructTypes())
    {
        if (mytype->hasName() && mytype->getName() == "struct.net_device")
        {
            type = mytype;
            break;
        }
    }
    for (auto &F : *module)
    {
        if (F.getName() != "alloc_netdev_mqs")
            continue;
        for (auto &BB : F)
        {
            if (BB.getName() != "if.end30")
                continue;
            for (auto &Inst : BB)
            {
                if (CastInst *CastPtr = dyn_cast<CastInst>(&Inst))
                { //   %1 = inttoptr i64 %and32 to %struct.net_device*
                    if (CastPtr->getDestTy()->isPointerTy() &&
                        CastPtr->getDestTy()->getPointerElementType() == type)
                    {
                        auto FPOffset = fpinit->GetTypeFPOffset(type);
                        IRBuilder<> irBuilder(CastPtr->getNextNode());
                        auto baseAddr = irBuilder.CreatePtrToInt(CastPtr, int64Type);
                        for (auto offset : FPOffset)
                        {
                            auto fpAddrInt = irBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
                            auto fpAddr = irBuilder.CreateIntToPtr(fpAddrInt, int64PtrType);
                            auto encryptedValue = irBuilder.CreateCall(encryptFunc, {ConstantInt::get(int64Type, 0), fpAddr});
                            irBuilder.CreateStore(encryptedValue, fpAddr);
                        }
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool FpInstrument::skCloneLock()
{
    if (module->getName().str().find("net/core/sock.c") == std::string::npos)
        return false;
    Type *type;
    for (auto mytype : module->getIdentifiedStructTypes())
    {
        if (mytype->hasName() && mytype->getName() == "struct.sock")
        {
            type = mytype;
            break;
        }
    }
    for (auto &F : *module)
    {
        if (F.getName() != "sk_clone_lock")
            continue;
        for (auto &BB : F)
            for (auto &Inst : BB)
            {
                if (CallInst *CallPtr = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = CallPtr->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    std::string funcName = calledFunc->getName().str();
                    if (funcName == "__memcpy")
                    {
                        auto FPOffset = fpinit->GetTypeFPOffset(type);
                        IRBuilder<> Builder(CallPtr->getNextNode());
                        auto srcBase = Builder.CreatePtrToInt(CallPtr->getArgOperand(1), int64Type);
                        auto destBase = Builder.CreatePtrToInt(CallPtr->getArgOperand(0), int64Type);
                        for (auto offset : FPOffset)
                        {
                            auto srcOffset = Builder.CreateAdd(srcBase, ConstantInt::get(int64Type, offset));
                            auto destOffset = Builder.CreateAdd(destBase, ConstantInt::get(int64Type, offset));
                            auto srcAddr = Builder.CreateIntToPtr(srcOffset, int64PtrType);
                            auto destAddr = Builder.CreateIntToPtr(destOffset, int64PtrType);
                            auto loadSrc = Builder.CreateLoad(srcAddr);
                            auto decryptFP = Builder.CreateCall(decryptFunc, {loadSrc, srcAddr});
                            auto encryptFP = Builder.CreateCall(encryptFunc, {decryptFP, destAddr});
                            Builder.CreateStore(encryptFP, destAddr);
                        }
                        return true;
                    }
                }
            }
    }
    return false;
}

bool FpInstrument::parameterReordering()
{ // if the callee function has more than 8 parameters and the exceed part contains
    // function pointers, they will be passed through stack. So we should reorder these
    // functions' parameters to prevent this situation.
    // 1. int parse_one(char *param, char *val, const char *doing,
    //    const struct kernel_param *params, unsigned num_params, s16 min_level, s16 max_level,
    //    void *arg, int (*handle_unknown)(char *param, char *val, const char *doing, void *arg))
    //2. int ndo_dflt_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
    //   struct net_device *dev, u16 mode, u32 flags, u32 mask, int nlflags, 32 filter_mask,
    //   int (*vlan_fill)(struct sk_buff *skb, struct net_device *dev, u32 filter_mask))
    auto reorderFPType = [=](Function *F)
    {
        auto FT = F->getFunctionType();
        std::vector<Type *> paramType = FT->params();
        Type *FpType = paramType.back();
        paramType.back() = paramType[0];
        paramType[0] = FpType;
        auto NewFT = FunctionType::get(FT->getReturnType(), paramType, false);
        auto name = F->getName();
        if (F->isDeclaration())
        {
            F->eraseFromParent();
            module->getOrInsertFunction(name, NewFT);
        }
        else
        {
            auto NewF = Function::Create(NewFT, Function::ExternalLinkage, name.str() + "TEMP", *module);
            ValueToValueMapTy VMap;
            unsigned argsize = F->arg_size();
            NewF->getArg(0)->setName(F->getArg(argsize - 1)->getName());
            NewF->getArg(argsize - 1)->setName(F->getArg(0)->getName());
            VMap[F->getArg(0)] = NewF->getArg(argsize - 1);
            VMap[F->getArg(argsize - 1)] = NewF->getArg(0);
            for (unsigned i = 1; i < argsize - 1; ++i)
            {
                NewF->getArg(i)->setName(F->getArg(i)->getName());
                VMap[F->getArg(i)] = NewF->getArg(i);
            }
            SmallVector<ReturnInst *, 8> Returns;
            CloneFunctionInto(NewF, F, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);
            F->eraseFromParent();
            NewF->setName(name);
        }
    };

    auto oldParseOne = module->getGlobalVariable("parse_one");
    auto oldNdoDfltBridgeGetlink = module->getGlobalVariable("ndo_dflt_bridge_getlink");
    if (!oldParseOne || !oldNdoDfltBridgeGetlink)
        return false;
    if (oldParseOne)
        reorderFPType(cast<Function>(oldParseOne));
    if (oldNdoDfltBridgeGetlink)
        reorderFPType(cast<Function>(oldNdoDfltBridgeGetlink));

    for (auto &F : *module)
        for (auto &BB : F)
            for (auto &Inst : BB)
            {
                if (auto callInst = dyn_cast<CallInst>(&Inst))
                {
                    auto calledFunc = callInst->getCalledFunction();
                    if (!calledFunc)
                        continue;
                    auto funcName = calledFunc->getName().str();
                    if (funcName == "parse_one" || funcName == "ndo_dflt_bridge_getlink")
                    {
                        unsigned argsize = calledFunc->arg_size();
                        auto firstOperand = callInst->getArgOperand(0);
                        auto fpOperand = callInst->getArgOperand(argsize - 1);
                        callInst->setArgOperand(0, fpOperand);
                        callInst->setArgOperand(argsize - 1, firstOperand);
                    }
                }
            }
    return true;
}

bool FpInstrument::instrumentFPCopy()
{
    bool ret = false;
    ret |= instrumentMemcpy();
    ret |= getPerCPUFP();
    return ret;
}

bool FpInstrument::instrumentFPZeroInit()
{
    bool ret = false;
    ret |= instrumentZalloc();
    ret |= instrumentMemset();
    ret |= instrumentAddSysfsParam();
    ret |= instrumentXasInit();
    ret |= instrumentPopulateProperties();
    ret |= instrumentCryptoCreateTfm();
    ret |= instrumentAddSectAttrs();
    ret |= netdeviceAlloc();
    ret |= skCloneLock();
    return ret;
}