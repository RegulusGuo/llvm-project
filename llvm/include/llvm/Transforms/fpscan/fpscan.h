//  PPProject [kernel Pointer integrity Protecion Project]
//  Copyright ( C) 2020 by phantom
//  Email: admin@phvntom.tech
//  This program is under MIT License, see http://phvntom.tech/LICENSE.txt

#ifndef LLVM_TRANSFORMS_FPSCAN_H
#define LLVM_TRANSFORMS_FPSCAN_H

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DEBUG_TYPE "ppp-fpscan"

bool isFuncPtr(llvm::Type *type);
bool isFuncPtr2(llvm::Type *type);

namespace llvm
{

    class Module;

    class FpInit;

    class FpTag
    {
        friend class FpInit;

    public:
        FpTag(Module *module) : module(module) {}
        void TagModule(Module &M);
        void Init();

    private:
        void ForwardScan(Function &F);
        void BackwardPropagate(Value *v);
        void AnalysisCast(CastInst *CastPtr);
        void AnalysisCmp(CmpInst *CmpPtr);
        void AnalysisPhi(PHINode *PhiPtr);
        void AnalysisStore(StoreInst *StorePtr);
        void AnalysisLoad(LoadInst *LoadPtr);
        void AnalysisGEP(User *GEPPtr);
        void AnalysisCall(CallInst *CallPtr);
        void AnalysisOperator(Value *v);
        void AnalysisSelect(SelectInst *SelectPtr);
        void AddStoreTag(StoreInst *StorePtr);
        void AddLoadTag(LoadInst *LoadPtr);

        Module *module;
        bool is8ByteType(Type *type)
        {
            return module->getDataLayout().getTypeAllocSize(type) == 8;
        }

        std::unordered_set<Value *> taggedFPSet, taggedFPPSet, NoFPPSet;
        //std::map<Function *, std::array<bool, 16>> taggedParameterFP, taggedParameterFPP;
        std::map<StructType *, SmallVector<int, 16>> taggedStructField, NoTaggedStructField;
        static std::map<std::string, std::set<int>> tagStructText, NoTagStructText;
        bool changed;
        bool isTaggedFP(Value *v) { return taggedFPSet.find(v) != taggedFPSet.end(); }
        bool insertTaggedFP(Value *v)
        {
            bool ret = taggedFPSet.insert(v).second;
            if (ret)
                changed = true;
            return ret;
        }
        bool isTaggedFPP(Value *v) { return taggedFPPSet.find(v) != taggedFPPSet.end(); }
        bool insertTaggedFPP(Value *v)
        {
            bool ret = taggedFPPSet.insert(v).second;
            if (ret)
                changed = true;
            return ret;
        }
        bool isTaggedStructField(StructType *type, int ind)
        {
            auto taggedField = taggedStructField.find(type);
            if (taggedField == taggedStructField.end())
                return false;
            for (auto tagged : taggedField->second)
                if (tagged == ind)
                    return true;
            return false;
        }
        void insertTaggedStructField(StructType *type, int ind)
        {
            if (!isTaggedStructField(type, ind))
            {
                changed = true;
                taggedStructField[type].push_back(ind);
            }
        }
        bool isNoTaggedStructField(StructType *type, int ind)
        {
            auto taggedField = NoTaggedStructField.find(type);
            if (taggedField == NoTaggedStructField.end())
                return false;
            for (auto tagged : taggedField->second)
                if (tagged == ind)
                    return true;
            return false;
        }
        void insertNoTaggedStructField(StructType *type, int ind)
        {
            if (!isTaggedStructField(type, ind))
            {
                changed = true;
                NoTaggedStructField[type].push_back(ind);
            }
        }

        //void PrintDbg(Value *v);
        static const std::set<std::string> customPatchFunction;
        // static const std::set<std::string> CheckNoPatchFunction;
        void custom_patch(Instruction *inst, const std::string &tag);
        void kfree_rcu_work_patch(Instruction *inst);
        void neigh_timer_handler_patch(Instruction *inst);
        void proc_pident_instantiate_patch(Instruction *inst);
        void copy_thread_tls_patch(Instruction *inst);
    };

    struct FpInfo
    {
        GlobalVariable *base;
        unsigned offset;

        FpInfo(GlobalVariable *base, unsigned offset = 0) : base(base), offset(offset) {}
    };

    class FpInit
    {
    public:
        FpInit(FpTag *fptag, Module *module) : fptag(fptag), module(module) {}
        std::vector<FpInfo> GlobalFpInfo;
        void getAllFp();
        const std::set<int> &GetTypeFPOffset(Type *type);

    private:
        FpTag *fptag;
        Module *module;
        static const std::set<int> EmptySet;
        std::unordered_map<Type *, std::set<int>> TypeFPOffset;

        const std::set<int> &getStructFp(StructType *structType);
        const std::set<int> &getArrayVecFp(Type *type);
    };

    class FpInstrument
    {
    public:
        FpInstrument(FpInit *fpinit, Module *module) : fpinit(fpinit), module(module), moduleName(module->getName().str())
        {
            int64Type = Type::getInt64Ty(module->getContext());
            int64PtrType = Type::getInt64PtrTy(module->getContext());
            voidType = Type::getVoidTy(module->getContext());
            voidPtrType = Type::getInt8PtrTy(module->getContext());
            int8Type = Type::getInt8Ty(module->getContext());

            PECFT = FunctionType::get(int64Type, {int64Type, int64PtrType}, false);
            MemcpyFT = FunctionType::get(voidType, {voidPtrType, voidPtrType, int64Type}, false);
            MemsetFT = FunctionType::get(voidType, {voidPtrType, int8Type, int64Type}, false);

            CREAK = InlineAsm::get(PECFT, "creak $0, $1, $2, 0, 7", "=r,r,r", false);
            CRDAK = InlineAsm::get(PECFT, "crdak $0, $1, $2, 0, 7", "=r,r,r", false);
            encryptFunc = FunctionCallee(PECFT, CREAK);
            decryptFunc = FunctionCallee(PECFT, CRDAK);
            encryptFuncCheck = module->getOrInsertFunction("encryptFunc", PECFT);
            cast<Function>(encryptFuncCheck.getCallee())->addFnAttr(Attribute::ReadOnly);
            decryptFuncCheck = module->getOrInsertFunction("decryptFunc", PECFT);
            cast<Function>(encryptFuncCheck.getCallee())->addFnAttr(Attribute::ReadOnly);
            encryptFuncNoCheck = FunctionCallee(PECFT, CREAK);
            decryptFuncNoCheck = FunctionCallee(PECFT, CRDAK);

            memsetFunc = module->getOrInsertFunction("__memset", MemsetFT);
            memcpyFunc = module->getOrInsertFunction("__memcpy", MemcpyFT);
            memmoveFunc = module->getOrInsertFunction("memmove", MemcpyFT);
        }

        bool genGFPInitFunc();
        bool instrumentLoadStore();
        bool instrumentFPCopy();
        bool instrumentFPZeroInit();
        bool parameterReordering();

    private:
        FpInit *fpinit;
        Module *module;
        std::string moduleName;

        Type *int8Type, *int64Type, *voidType;
        PointerType *int64PtrType, *voidPtrType;
        FunctionType *PECFT, *MemcpyFT, *MemsetFT;
        Value *CREAK, *CRDAK;
        FunctionCallee encryptFunc, decryptFunc, encryptFuncNoCheck, decryptFuncNoCheck;
        FunctionCallee encryptFuncCheck, decryptFuncCheck;
        FunctionCallee memsetFunc, memcpyFunc, memmoveFunc;

        void addFPStore(StoreInst *);
        void addFPLoad(LoadInst *);
        bool instrumentMemcpy();
        bool getPerCPUFP();
        bool instrumentZalloc();
        bool instrumentMemset();
        bool instrumentPopulateProperties();
        bool instrumentAddSysfsParam();
        bool instrumentXasInit();
        bool instrumentCryptoCreateTfm();
        bool instrumentAddSectAttrs();
        bool netdeviceAlloc();
        bool skCloneLock();
    };

    class fpscan : public PassInfoMixin<fpscan>
    {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &);

        static std::string src_prefix, dumpBCDir;

    private:
        FpTag *fptag;
        FpInit *fpinit;
        FpInstrument *fpinstrument;

        void dumpByteCode(Module &M);
    };

} // end namespace llvm

#endif // LLVM_TRANSFORMS_FPSCAN_H