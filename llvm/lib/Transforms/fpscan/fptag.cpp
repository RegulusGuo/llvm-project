#include "llvm/Transforms/fpscan/fpscan.h"
using namespace ::llvm;

// TODO: walk_stackframe
// TODO: container_of: fake fpp propagation

std::map<std::string, std::set<int>> FpTag::tagStructText = {
    {"struct.of_device_id", {3}}, // sometimes fp, sometimes other(even 1,2,3), treat as fp
    {"struct.thread_struct", {0, 1}},
    // {"struct.vm_struct", 7}, // check later
    {"struct.block_device", {5, 6}},
    // {"struct.kernel_clone_args", 5}, // check later
    // {"struct.clone_args", 5}, // check later
};

std::map<std::string, std::set<int>> FpTag::NoTagStructText = {
    {"struct.sigaction", {0}},
};

void InstSetMetaData(Instruction *Inst, const std::string &data)
{
    Metadata *Tag(MDString::get(Inst->getContext(), data));
    MDNode *N = MDNode::get(Inst->getContext(), {Tag});
    Inst->setMetadata(data, N);
}

void FpTag::Init()
{
    auto structVec = module->getIdentifiedStructTypes();
    for (auto structPtr : structVec)
    {
        auto name = structPtr->getName().str();
        auto it = tagStructText.find(name);
        if (it != tagStructText.end())
        {
            for (auto ind : it->second)
                insertTaggedStructField(structPtr, ind);
        }
        it = NoTagStructText.find(name);
        if (it != NoTagStructText.end())
        {
            for (auto ind : it->second)
                insertNoTaggedStructField(structPtr, ind);
        }
    }
}

bool isFuncPtr(Type *type)
{
    if (type->isPointerTy() &&
        type->getPointerElementType()->isFunctionTy())
        return true;
    return false;
}

bool isFuncPtr2(Type *type)
{
    if (type->isPointerTy() && isFuncPtr(type->getPointerElementType()))
        return true;
    return false;
}

// void FpTag::PrintDbg(Value *v)
// {
//     errs() << "\033[32m" << *v << "\033[0m\n";
//     errs() << "\033[33m Moduele Name: " << module->getName().str() << "\033[0m\n";
// }

void FpTag::AnalysisOperator(Value *v)
{
    if (auto BitCastPtr = dyn_cast<BitCastOperator>(v))
    { // consider both fp and fpp transfer
        auto srcOperand = BitCastPtr->getOperand(0);
        auto srcType = BitCastPtr->getSrcTy(), destType = BitCastPtr->getDestTy();
        if (!is8ByteType(destType) || !is8ByteType(srcType))
            return;
        // propagate fp
        bool srcFP = isFuncPtr(srcType), destFP = isFuncPtr(destType);
        bool isSrcTaggedFP = isTaggedFP(srcOperand);
        bool isDestTaggedFP = isTaggedFP(BitCastPtr);
        if (isSrcTaggedFP && !(destFP || isDestTaggedFP))
        { // src is tagged fp, dest is not fp/tagged, tag dest
            insertTaggedFP(BitCastPtr);
        }
        else if (isDestTaggedFP && !(srcFP || isSrcTaggedFP))
        { // dest is tagged fp, src is not fp/tagged, tag src and back propagate
            insertTaggedFP(srcOperand);
            BackwardPropagate(srcOperand);
        }
        else if (srcFP && !destFP)
        { // src is fp, dest is not fp, tag dest
            insertTaggedFP(BitCastPtr);
            //PrintDbg(v);
        }
        else if (destFP && !srcFP)
        { // dest is fp, src is not fp, tag src and back propagate
            insertTaggedFP(srcOperand);
            BackwardPropagate(srcOperand);
            //PrintDbg(v);
        }
        if (!is8ByteType(destType) || !is8ByteType(srcType))
            return;
    }
    else if (auto PtrToIntPtr = dyn_cast<PtrToIntOperator>(v))
    { // consider only fp transfer
        auto srcOperand = PtrToIntPtr->getPointerOperand();
        auto srcType = PtrToIntPtr->getPointerOperandType();
        bool isSrcFP = isFuncPtr(srcType);
        bool isTaggedSrc = isTaggedFP(srcOperand);
        bool isTaggedDest = isTaggedFP(PtrToIntPtr);
        if ((isSrcFP || isTaggedSrc) && !isTaggedDest)
        { // src is fp/tagged, dest is not tagged, tag dest
            insertTaggedFP(PtrToIntPtr);
        }
        // if (isSrcFP && !isTaggedDest)
        //     PrintDbg(v);
    }
    else if (auto GEPPtr = dyn_cast<GEPOperator>(v))
    {
        AnalysisGEP(GEPPtr);
    }
}

void FpTag::AnalysisCast(CastInst *CastPtr)
{
    auto srcValue = CastPtr->getOperand(0);
    AnalysisOperator(srcValue);

    // deal with function pointer propagate
    auto DestType = CastPtr->getDestTy(), SrcType = CastPtr->getSrcTy();
    if (!is8ByteType(DestType) || !is8ByteType(SrcType))
        return;
    bool isDestFP = isFuncPtr(DestType), isSrcFP = isFuncPtr(SrcType);
    bool isDestTag = isTaggedFP(CastPtr), isSrcTag = isTaggedFP(srcValue);

    // debug print
    // if ((isSrcFP && !(isDestTag || isDestFP)) ||
    //     (isDestFP && !(isSrcTag || isSrcFP)))
    // {
    //     PrintDbg(CastPtr);
    // }

    if ((isSrcFP || isSrcTag) && !(isDestFP || isDestTag))
    { // cast fp to non-fp, tag dest
        insertTaggedFP(CastPtr);
        InstSetMetaData(CastPtr, "CastInstTagDestFP");
        return;
    }
    if ((isDestFP || isDestTag) && !(isSrcFP || isSrcTag))
    { // cast non-fp to fp, tag src and back-propagate
        insertTaggedFP(srcValue);
        InstSetMetaData(CastPtr, "CastInstTagSrcFP");
        BackwardPropagate(srcValue);
        return;
    }
}

void FpTag::AnalysisSelect(SelectInst *SelectPtr)
{
    auto lhs = SelectPtr->getTrueValue();
    auto rhs = SelectPtr->getFalseValue();
    AnalysisOperator(lhs);
    AnalysisOperator(rhs);
    if (isa<ConstantData>(lhs) || isa<ConstantData>(rhs))
        return;
    if (!is8ByteType(lhs->getType()) || !is8ByteType(rhs->getType()))
        return;
    bool isLhsFP = isFuncPtr(lhs->getType()) || isTaggedFP(lhs);
    bool isRhsFP = isFuncPtr(rhs->getType()) || isTaggedFP(rhs);
    if (isLhsFP && !isRhsFP)
    {
        insertTaggedFP(rhs);
        InstSetMetaData(SelectPtr, "SelectInstTagRhs");
        BackwardPropagate(rhs);
    }
    else if (isRhsFP && !isLhsFP)
    {
        insertTaggedFP(lhs);
        InstSetMetaData(SelectPtr, "SelectInstTagLhs");
        BackwardPropagate(lhs);
    }
}

void FpTag::AnalysisCmp(CmpInst *CmpPtr)
{
    if (CmpPtr->getNumOperands() != 2)
        return;
    auto lhs = CmpPtr->getOperand(0);
    auto rhs = CmpPtr->getOperand(1);
    AnalysisOperator(lhs);
    AnalysisOperator(rhs);
    // if one of them is constant data, no need to propagate
    if (isa<ConstantData>(lhs) || isa<ConstantData>(rhs))
        return;
    // check both side and propagate if necessary
    if (!is8ByteType(lhs->getType()) || !is8ByteType(rhs->getType()))
        return;
    bool isLhsFP = isFuncPtr(lhs->getType()) || isTaggedFP(lhs);
    bool isRhsFP = isFuncPtr(rhs->getType()) || isTaggedFP(rhs);
    if (isLhsFP && !isRhsFP)
    {
        insertTaggedFP(rhs);
        BackwardPropagate(rhs);
    }
    else if (isRhsFP && !isLhsFP)
    {
        insertTaggedFP(lhs);
        BackwardPropagate(lhs);
    }
}

void FpTag::AnalysisPhi(PHINode *PhiPtr)
{
    auto moduleName = PhiPtr->getParent()->getParent()->getParent()->getName().str();
    if (moduleName.find("arch/riscv/kernel/stacktrace.c") != std::string::npos)
        return;
    // PhiNode have same types
    if (isFuncPtr(PhiPtr->getType()))
        return;
    if (!is8ByteType(PhiPtr->getType()))
        return;

    // propagate FP
    // check whether one of them is tagged
    bool tagged = isTaggedFP(PhiPtr);
    int num = PhiPtr->getNumIncomingValues();
    for (int i = 0; i < num && !tagged; ++i)
    {
        if (isTaggedFP(PhiPtr->getOperand(i)))
            tagged = true;
    }
    if (tagged)
    {
        for (int i = 0; i < num; ++i)
        {
            auto v = PhiPtr->getOperand(i);
            if (isa<ConstantData>(v))
                continue;
            if (insertTaggedFP(v))
                BackwardPropagate(v);
        }
        insertTaggedFP(PhiPtr);
        InstSetMetaData(PhiPtr, "PhiNodeTagPhiFP");
        return;
    }
}

void FpTag::AnalysisStore(StoreInst *StorePtr)
{
    auto value = StorePtr->getValueOperand();
    auto addr = StorePtr->getPointerOperand();
    if (!is8ByteType(value->getType()))
        return;
    AnalysisOperator(value);
    AnalysisOperator(addr);
    if (NoFPPSet.find(addr) != NoFPPSet.end())
        return;
    bool isValueFP = isFuncPtr(value->getType());
    bool isValueTag = isTaggedFP(value);
    bool isAddrTag = isTaggedFPP(addr);
    if (isValueFP)
    {
        AddStoreTag(StorePtr);
    }
    else if (isValueTag && !isAddrTag)
    {
        AddStoreTag(StorePtr);
        insertTaggedFPP(addr);
        InstSetMetaData(StorePtr, "StoreInstValueTag");
        BackwardPropagate(addr);
    }
    else if (isAddrTag && !isValueFP)
    {
        AddStoreTag(StorePtr);
        insertTaggedFP(value);
        InstSetMetaData(StorePtr, "StoreInstAddrTag");
        BackwardPropagate(value);
    }
}

void FpTag::AnalysisLoad(LoadInst *LoadPtr)
{
    auto addr = LoadPtr->getPointerOperand();
    if (!is8ByteType(LoadPtr->getType()))
        return;
    AnalysisOperator(addr);
    if (NoFPPSet.find(addr) != NoFPPSet.end())
        return;
    if (isFuncPtr(addr->getType()->getPointerElementType()))
    {
        AddLoadTag(LoadPtr);
    }
    else if (isTaggedFPP(addr))
    {
        AddLoadTag(LoadPtr);
        insertTaggedFP(LoadPtr);
        InstSetMetaData(LoadPtr, "LoadInstAddrTag");
    }
    else if (isTaggedFP(LoadPtr))
    {
        AddLoadTag(LoadPtr);
        insertTaggedFPP(addr);
        InstSetMetaData(LoadPtr, "LoadInstValueTag");
        BackwardPropagate(addr);
    }
}

void FpTag::AnalysisGEP(User *GEPPtr)
{
    auto GetLastType = [](User *GEPPtr) -> Type * {
        Type *type;
        int num = GEPPtr->getNumOperands();
        if (auto GEPInst = dyn_cast<GetElementPtrInst>(GEPPtr))
            type = GEPInst->getPointerOperandType();
        else if (auto GEPOp = dyn_cast<GEPOperator>(GEPPtr))
            type = GEPOp->getPointerOperandType();
        for (int i = 1; i < num - 1; ++i)
        {
            if (type->isPointerTy())
            {
                type = cast<PointerType>(type)->getElementType();
            }
            else if (type->isVectorTy())
            {
                type = cast<VectorType>(type)->getArrayElementType();
            }
            else if (type->isArrayTy())
            {
                type = cast<ArrayType>(type)->getArrayElementType();
            }
            else if (type->isStructTy())
            {
                ConstantInt *CI = cast<ConstantInt>(GEPPtr->getOperand(i));
                type = cast<StructType>(type)->getTypeAtIndex(CI->getZExtValue());
            }
        }
        return type;
    };
    if (!is8ByteType(GEPPtr->getType()->getPointerElementType()))
        return;
    auto lastType = GetLastType(GEPPtr);
    if (!lastType->isStructTy())
        return;
    auto type = cast<StructType>(lastType);
    ConstantInt *CI = cast<ConstantInt>(GEPPtr->getOperand(GEPPtr->getNumOperands() - 1));
    int ind = CI->getZExtValue();
    if (isTaggedStructField(type, ind))
    {
        insertTaggedFPP(GEPPtr);
        if (isa<GetElementPtrInst>(GEPPtr))
            InstSetMetaData(cast<GetElementPtrInst>(GEPPtr), "GEPTagField");
    }
    if (isNoTaggedStructField(type, ind))
    {
        NoFPPSet.insert(GEPPtr);
    }
}

void FpTag::AnalysisCall(CallInst *CallPtr)
{
    if (CallPtr->isIndirectCall())
    {
        auto CallOperand = CallPtr->getCalledOperand();
        AnalysisOperator(CallOperand);
        InstSetMetaData(CallPtr, "IndirCallTag");
        return;
    }
}

void FpTag::custom_patch(Instruction *inst, const std::string &tag)
{
    Value *addr;
    if (auto LoadPtr = dyn_cast<LoadInst>(inst))
    {
        addr = LoadPtr->getPointerOperand();
    }
    else if (auto StorePtr = dyn_cast<StoreInst>(inst))
    {
        addr = StorePtr->getPointerOperand();
    }
    if (auto BitCastPtr = dyn_cast<BitCastInst>(addr))
    {
        if (isFuncPtr2(BitCastPtr->getSrcTy()) ||
            isTaggedFPP(BitCastPtr->getOperand(0)))
        {
            insertTaggedFPP(BitCastPtr);
            InstSetMetaData(BitCastPtr, tag);
        }
    }
}

void FpTag::kfree_rcu_work_patch(Instruction *inst)
{ // file: linux/kernel/rcu/tree.c
    // line: 3084
    // unsigned long offset = (unsigned long)head->func;
    // llvm IR:
    // %11 = bitcast void (%struct.callback_head*)** %func to i64*
    // %12 = load i64, i64* %11, align 8
    // cannot use costom_patch since the function contains
    // a false positive (container_of)
    if (auto BitCastPtr = dyn_cast<BitCastInst>(inst))
    {
        auto operand = BitCastPtr->getOperand(0);
        if (operand->hasName() && operand->getName().str() == "func")
        {
            insertTaggedFPP(BitCastPtr);
            InstSetMetaData(BitCastPtr, "Patch_kfree_rcu_work");
        }
    }
}

void FpTag::neigh_timer_handler_patch(Instruction *inst)
{ //file: net/core/neighbour.c
    if (auto BitCastPtr = dyn_cast<BitCastInst>(inst))
    {
        auto operand = BitCastPtr->getOperand(0);
        if (operand->hasName() &&
            (operand->getName().str() == "output.i195" ||
             operand->getName().str() == "connected_output.i"))
        {
            insertTaggedFPP(BitCastPtr);
            InstSetMetaData(BitCastPtr, "Patch_neigh_timer_handler");
        }
    }
}

void FpTag::proc_pident_instantiate_patch(Instruction *inst)
{ // file: fs/proc/base.c
    // line: 2602
    // ei->op = p->op;
    // LLVM IR:
    // %op = getelementptr inbounds %struct.list_head, %struct.list_head* %add.ptr.i, i64 1,
    // %9 = bitcast i8* %op14 to i64*
    // %10 = bitcast %struct.list_head* %op to i64*
    // %11 = load i64, i64* %9, align 8
    // store i64 %11, i64* %10, align 8
    if (auto BitCastPtr = dyn_cast<BitCastInst>(inst))
    {
        auto operand = BitCastPtr->getOperand(0);
        if (operand->hasName() &&
            operand->getName().str() == "op14")
        {
            insertTaggedFPP(BitCastPtr);
            InstSetMetaData(BitCastPtr, "Patch_proc_pident_instantiate");
        }
    }
}

void FpTag::copy_thread_tls_patch(Instruction *inst)
{ // file: arch/riscv/kernel/process.c
    // line 118
    // p->thread.s[0] = usp; /* fn */
    // p->thread.s[1] = arg;
    // To protect context switch, thread_struct.s[] should be protected
    // LLVM IR
    // %arrayidx = getelementptr inbounds %struct.task_struct, %struct.task_struct* %p, i64 0, i32 128, i32 2, i64 0
    // store i64 %usp, i64* %arrayidx, align 8,
    // %arrayidx7 = getelementptr %struct.task_struct, %struct.task_struct* %p, i64 0, i32 128, i32 2, i64 1,
    // store i64 %arg, i64* %arrayidx7, align 8,
    if (auto GEPPtr = dyn_cast<GetElementPtrInst>(inst))
    {
        if (GEPPtr->hasName() &&
            (GEPPtr->getName().str() == "arrayidx"))
        {
            auto StorePtr = cast<StoreInst>(inst->getNextNode());
            AddStoreTag(StorePtr);
        }
    }
}

void FpTag::ForwardScan(Function &F)
{
    auto funcName = F.getName().str();
    auto moduleName = F.getParent()->getName().str();
    bool customPatch = customPatchFunction.find(funcName) != customPatchFunction.end();
    for (auto &BB : F)
    {
        for (auto &Inst : BB)
        {
            if (customPatch)
                custom_patch(&Inst, "Patch_" + funcName);
            else if (funcName == "kfree_rcu_work")
                kfree_rcu_work_patch(&Inst);
            else if (funcName == "neigh_timer_handler")
                neigh_timer_handler_patch(&Inst);
            else if (funcName == "proc_pident_instantiate")
                proc_pident_instantiate_patch(&Inst);
            else if (funcName == "copy_thread_tls")
                copy_thread_tls_patch(&Inst);

            if (auto CastPtr = dyn_cast<CastInst>(&Inst))
                AnalysisCast(CastPtr);
            else if (auto CmpPtr = dyn_cast<CmpInst>(&Inst))
                AnalysisCmp(CmpPtr);
            else if (auto PhiPtr = dyn_cast<PHINode>(&Inst))
                AnalysisPhi(PhiPtr);
            else if (auto StorePtr = dyn_cast<StoreInst>(&Inst))
                AnalysisStore(StorePtr);
            else if (auto LoadPtr = dyn_cast<LoadInst>(&Inst))
                AnalysisLoad(LoadPtr);
            else if (auto GEPPtr = dyn_cast<GetElementPtrInst>(&Inst))
                AnalysisGEP(GEPPtr);
            else if (auto CallPtr = dyn_cast<CallInst>(&Inst))
                AnalysisCall(CallPtr);
            else if (auto SelectPtr = dyn_cast<SelectInst>(&Inst))
                AnalysisSelect(SelectPtr);
        }
    }
    // if (CheckNoPatchFunction.find(funcName) != CheckNoPatchFunction.end())
    //     return;
    // bool flag = false;
    // for (auto &BB : F)
    // {
    //     for (auto &Inst : BB)
    //     {
    //         Value *addr;
    //         if (auto LoadPtr = dyn_cast<LoadInst>(&Inst))
    //         {
    //             if (Inst.getMetadata("LoadTag"))
    //                 continue;
    //             // if (!is8ByteType(LoadPtr->getType()))
    //             //     continue;
    //             addr = LoadPtr->getPointerOperand();
    //         }
    //         else if (auto StorePtr = dyn_cast<StoreInst>(&Inst))
    //         {
    //             if (Inst.getMetadata("StoreTag"))
    //                 continue;
    //             // if (!is8ByteType(StorePtr->getValueOperand()->getType()))
    //             //     continue;
    //             addr = StorePtr->getPointerOperand();
    //         }
    //         if (auto BitCastPtr = dyn_cast<BitCastInst>(addr))
    //         {
    //             if (isFuncPtr2(BitCastPtr->getSrcTy()) ||
    //                 isTaggedFPP(BitCastPtr->getOperand(0)))
    //             {
    //                 flag = true;
    //                 InstSetMetaData(BitCastPtr, "PPP-CheckLoadStore");
    //             }
    //         }
    //     }
    // }
    // if (funcName == "kfree_rcu_work")
    //     return;
    // if (funcName == "neigh_timer_handler")
    //     return;
    // if (flag)
    //     errs() << "\033[31m Check File:" << funcName << " " << moduleName << "\033[0m\n";
}

void FpTag::BackwardPropagate(Value *v)
{
    assert((isTaggedFP(v) || isTaggedFPP(v)) && "BackwardPropagate only propagate tagged value\n");
    assert(is8ByteType(v->getType()) && "Tagged Type should be 64-bits");
    if (auto CastPtr = dyn_cast<CastInst>(v))
    {
        AnalysisCast(CastPtr);
    }
    // else if (auto PhiPtr = dyn_cast<PHINode>(v))
    // {
    //     AnalysisPhi(PhiPtr);
    // }
    else if (auto LoadPtr = dyn_cast<LoadInst>(v))
    {
        AnalysisLoad(LoadPtr);
    }
    // else if (auto GEPPtr = dyn_cast<GetElementPtrInst>(v))
    // {
    //     AnalysisGEP(GEPPtr);
    // }
    else if (auto OperatorPtr = dyn_cast<Operator>(v))
    {
        AnalysisOperator(OperatorPtr);
    }
}

void FpTag::AddStoreTag(StoreInst *StorePtr)
{
    assert(is8ByteType(StorePtr->getValueOperand()->getType()) &&
           "Tag Store should be 64-bit\n");
    InstSetMetaData(StorePtr, "StoreTag");
}
void FpTag::AddLoadTag(LoadInst *LoadPtr)
{
    assert(is8ByteType(LoadPtr->getPointerOperandType()->getPointerElementType()) &&
           "Tag Load Should be 64-bit\n");
    InstSetMetaData(LoadPtr, "LoadTag");
}

void FpTag::TagModule(Module &M)
{
    changed = true;
    while (changed)
    {
        changed = false;
        for (auto &F : M)
            ForwardScan(F);
    }
}

const std::set<std::string> FpTag::customPatchFunction = {
    "of_irq_init",                     // drivers/of/irq.c
    "process_one_work",                // kernel/workqueue.c
    "rtnetlink_rcv_msg",               // net/core/rtnetlink.c
    "__netlink_kernel_create",         // net/netlink/af_netlink.c
    "__netlink_dump_start",            // net/netlink/af_netlink.c
    "netlink_create",                  // net/netlink/af_netlink.c
    "printk_late_init",                // kernel/printk/printk.c
    "msi_create_irq_domain",           // kernel/irq/msi.c
    "crypto_ahash_finup",              // crypto/ahash.c
    "crypto_ahash_digest",             // crypto/ahash.c
    "ahash_save_req",                  // crypto/ahash.c
    "ahash_op_unaligned_done",         // crypto/ahash.c
    "crypto_ahash_init_tfm",           // crypto/ahash.c
    "ahash_def_finup",                 // crypto/ahash.c
    "ahash_def_finup_done1",           // crypto/ahash.c
    "ahash_def_finup_done2",           // crypto/ahash.c
    "print_cpu",                       // kernel/time/timer_list.c
    "print_tickdevice",                // kernel/time/timer_list.c
    "blk_insert_flush",                // block/blk-flush.c
    "blk_flush_complete_seq",          // block/blk-flush.c
    "__cpuhp_state_remove_instance",   // kernel/cpu.c
    "__cpuhp_remove_state_cpuslocked", // kernel/cpu.c
    "gpiochip_add_data_with_key",      // drivers/gpio/gpiolib.c
    "gpiochip_irqchip_remove",         // drivers/gpio/gpiolib.c
    "gpiochip_irqchip_add_key",        // drivers/gpio/gpiolib.c
    "kthread_func",                    // kernel/kthread.c
    "of_clk_init",                     // drivers/clk/clk.c
    "skb_segment",                     // net/core/skbuff.c
    "serial8250_register_8250_port",   // drivers/tty/serial/8250/8250_core.c
    "serial8250_probe",                // drivers/tty/serial/8250/8250_core.c
    "io_wq_create",                    // fs/io-wq.c
    "__neigh_update",                  // net/core/neighbour.c
    "__regmap_init",                   // drivers/base/regmap/regmap.c
    "regmap_reinit_cache",             // drivers/base/regmap/regmap.c
    "__regmap_init",                   // drivers/base/regmap/regmap.c
    "regmap_reinit_cache",             // drivers/base/regmap/regmap.c
    "qdisc_alloc",                     // net/sched/sch_generic.c
    "__ata_scsi_queuecmd",             // drivers/ata/libata-scsi.c
    "rhashtable_insert_slow",          // lib/rhashtable.c CHECK LATER
    "rht_deferred_worker",             // lib/rhashtable.c CHECK LATER
    "rt_dst_clone",                    // net/ipv4/route.c
    "tcp_gso_segment",                 // net/ipv4/tcp_offload.c
    "arp_constructor",                 // net/ipv4/arp.c
    "inet_create",                     // net/ipv4/af_inet.c
    "inet_frag_kill",                  // net/ipv4/inet_fragment.c
    "inet_frag_find",                  // net/ipv4/inet_fragment.c
    "ocores_i2c_probe",                // drivers/i2c/busses/i2c-ocores.c
    "nvmem_register",                  // drivers/nvmem/core.c
};

// TODO: Check bpf related functions:
// File: home/lhr/riscv-rss-sdk/linux/kernel/bpf/core.c
// Function:
// bpf_prog_fill_jited_linfo
// bpf_patch_call_args
// bpf_prog_select_runtime
// bpf_prog_free_deferred
// bpf_prog_fill_jited_linfo
// bpf_patch_call_args
// bpf_prog_select_runtime
// bpf_prog_free_deferred