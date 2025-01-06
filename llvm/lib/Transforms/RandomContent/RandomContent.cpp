#include "llvm/Transforms/RandomContent/RandomContent.h"
#include "llvm/Bitcode/BitcodeWriter.h"
using namespace ::llvm;

namespace {
std::map<std::string, std::set<int>> randomFields = {
    {"struct.cred",
     {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24}},
    //{"struct.STACK_TARGET", {2,3}},
    // {"struct.selinux_state", {0, 1, 2, 3, 5}},
};
std::map<StructType *, std::set<int>> randomFieldOffset;
std::map<StructType *, std::set<int>> randomArrayOffset;
} // namespace

std::string src_prefix = "/home/lhr/PEC/linux-5.8.18/";
std::string dumpBCDir = "/home/lhr/PEC/PPProject/test/linux-bc/";
void dumpByteCode(Module &M) {
//   std::string ModuleName = M.getName().data();
//   unsigned prefix_len =
//       src_prefix.size() < ModuleName.size() ? src_prefix.size() : 0;
//   ModuleName = ModuleName.substr(prefix_len);
//   std::replace(ModuleName.begin(), ModuleName.end(), '/', '-');
//   ModuleName.back() = 'b', ModuleName.push_back('c');
//   std::error_code EC;
//   llvm::raw_fd_ostream OS(dumpBCDir + ModuleName, EC, llvm::sys::fs::OF_None);
//   WriteBitcodeToFile(M, OS);
//   OS.flush();
}

Instrument::Instrument(Module &M) : M(M) {
  int64Type = Type::getInt64Ty(M.getContext());
  int32Type = Type::getInt32Ty(M.getContext());
  int64PtrType = Type::getInt64PtrTy(M.getContext());
  FunctionType *PEC64InstType =
      FunctionType::get(int64Type, {int64Type, int64PtrType}, false);
  FunctionType *PEC32InstType =
      FunctionType::get(int64Type, {int32Type, int64PtrType}, false);
  enc64Inst =
      InlineAsm::get(PEC64InstType, "mov $0, $1\ncrebk $0, $2, 0, 7\nmsr scrbkeyl, $1", "=r,r,r", false);
  dec64Inst =
      InlineAsm::get(PEC64InstType, "mov $0, $1\ncrdbk $0, $2, 0, 7\nmsr scrbkeyh, $1", "=r,r,r", false);
  enc32Inst =
      InlineAsm::get(PEC32InstType, "mov $0, $1\ncrebk $0, $2, 0, 3\nmsr scrbkeyl, $1", "=r,r,r", false);
  dec32Inst =
      InlineAsm::get(PEC64InstType, "mov $0, $1\ncrdbk $0, $2, 0, 3\nmsr scrbkeyh, $1", "=r,r,r", false);
  enc64Func = FunctionCallee(PEC64InstType, enc64Inst);
  dec64Func = FunctionCallee(PEC64InstType, dec64Inst);
  enc32Func = FunctionCallee(PEC32InstType, enc32Inst);
  dec32Func = FunctionCallee(PEC64InstType, dec32Inst);
}

const std::set<int> Instrument::collectArray(ArrayType *arrType) {
  unsigned num = arrType->getNumElements();
  Type *eleType = arrType->getElementType();
  std::set<int> offset, eleOffset;

  if (eleType->isStructTy()) {
    eleOffset = collectStruct(cast<StructType>(eleType));
  } else if (eleType->isArrayTy()) {
    eleOffset = collectArray(cast<ArrayType>(eleType));
  }

  unsigned eleSize = M.getDataLayout().getTypeAllocSize(eleType);
  for (unsigned i = 0; i < num; ++i) {
    for (int fieldOffset : eleOffset) {
      offset.insert(fieldOffset + i * eleSize);
    }
  }
  return offset;
}

const std::set<int> Instrument::collectStruct(StructType *structType) {
  std::set<int> fieldOffset;
  auto it = randomFieldOffset.find(structType);
  if (it != randomFieldOffset.end()) {
    fieldOffset = it->second;
  }
  for (int i = 0, end = structType->getNumElements(); i != end; ++i) {
    Type *eleType = structType->getElementType(i);
    int thisOffset =
        M.getDataLayout().getStructLayout(structType)->getElementOffset(i);
    std::set<int> thisFieldOffset;
    if (eleType->isArrayTy()) {
      thisFieldOffset = collectArray(cast<ArrayType>(eleType));
    } else if (eleType->isStructTy()) {
      thisFieldOffset = collectStruct(cast<StructType>(eleType));
    }
    for (int offset : thisFieldOffset) {
      fieldOffset.insert(offset + thisOffset);
    }
  }
  return fieldOffset;
}

const std::set<int> Instrument::collectTypeOffset(Type *type) {
  if (type->isStructTy())
    return collectStruct(cast<StructType>(type));
  else if (type->isArrayTy())
    return collectArray(cast<ArrayType>(type));
  return std::set<int>{};
}

int LoadStoreInstrument::RecognizeGEPOffset(GetElementPtrInst *GEP) {
  int offset = 0;
  for (gep_type_iterator GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP);
       GTI != GTE; ++GTI) {
    Value *V = GTI.getOperand();
    StructType *STy = GTI.getStructTypeOrNull();
    if (auto ConstOffset = dyn_cast<ConstantInt>(V)) {
      if (ConstOffset->isZero())
        continue;
      if (STy) {
        unsigned ElementIdx = ConstOffset->getZExtValue();
        offset += M.getDataLayout().getStructLayout(STy)->getElementOffset(
            ElementIdx);
        continue;
      }
      offset += ConstOffset->getZExtValue() *
                M.getDataLayout().getTypeAllocSize(GTI.getIndexedType());
      continue;
    }
    return offset;
  }
  return offset;
}

void LoadStoreInstrument::TagInstruction(Instruction *Inst, StringRef string) {
  Metadata *Tag(MDString::get(Inst->getContext(), string));
  MDNode *N = MDNode::get(Inst->getContext(), {Tag});
  Inst->setMetadata(string, N);
}

void LoadStoreInstrument::TraceArrayBase(Value *v) {
  for (User *U : v->users()) {
    if (StoreInst *StorePtr = dyn_cast<StoreInst>(U)) {
      TagInstruction(StorePtr, "RandomFieldStore");
    } else if (LoadInst *LoadPtr = dyn_cast<LoadInst>(U)) {
      TagInstruction(LoadPtr, "RandomFieldLoad");
      TagInstruction(LoadPtr, "IntegrityCheck");
    }
  }
}

void LoadStoreInstrument::TraceStructField(StructType *STy, Value *base_ptr,
                                           int offset) {
  if (offset == 0) {
    for (User *U : base_ptr->users()) {
      if (StoreInst *StorePtr = dyn_cast<StoreInst>(U)) {
        TagInstruction(StorePtr, "RandomFieldStore");
      } else if (LoadInst *LoadPtr = dyn_cast<LoadInst>(U)) {
        TagInstruction(LoadPtr, "RandomFieldLoad");
      }
    }
  }

  for (User *U : base_ptr->users()) {
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
      APInt gep_offset(64, 0, true);
      if (GEP->accumulateConstantOffset(M.getDataLayout(), gep_offset)) {
        TraceStructField(STy, GEP, offset - gep_offset.getSExtValue());
      } else {
        int offset = RecognizeGEPOffset(GEP);
        auto &arrayOffset = randomArrayOffset[STy];
        if (arrayOffset.find(offset) != arrayOffset.end()) {
          TraceArrayBase(GEP);
        }
      }
    } else if (CastInst *CastPtr = dyn_cast<CastInst>(U)) {
      TraceStructField(STy, CastPtr, offset);
    } else if (PHINode *Phi = dyn_cast<PHINode>(U)) {
      if (offset == 0)
        TraceStructField(STy, Phi, offset);
    }
  }
}

void LoadStoreInstrument::LoadInstrument(LoadInst *LoadPtr) {
  IRBuilder<> builder(LoadPtr->getNextNode());
  Value *OldAddressPtr = LoadPtr->getPointerOperand();
  if (OldAddressPtr->getType() == int64PtrType) {
    auto decValue = builder.CreateCall(dec64Func, {LoadPtr, OldAddressPtr});
    auto ShouldReplace = [decValue](Use &U) -> bool {
      return U.getUser() != decValue;
    };
    LoadPtr->replaceUsesWithIf(decValue, ShouldReplace);
  } else {
    Value *NewAddressPtr = builder.CreateBitCast(OldAddressPtr, int64PtrType);
    Value *encValue = builder.CreateLoad(NewAddressPtr);
    Value *decValue = nullptr;
    Value *newValue = nullptr;
    if (M.getDataLayout().getTypeSizeInBits(LoadPtr->getType()) < 64) {
      decValue = builder.CreateCall(dec32Func, {encValue, NewAddressPtr});
      newValue = builder.CreateTrunc(decValue, LoadPtr->getType());
    } else {
      decValue = builder.CreateCall(dec64Func, {encValue, NewAddressPtr});
      if (LoadPtr->getType()->isPointerTy()) {
        newValue = builder.CreateIntToPtr(decValue, LoadPtr->getType());
      } else {
        newValue = builder.CreateBitCast(decValue, LoadPtr->getType());
      }
    }
    LoadPtr->replaceAllUsesWith(newValue);
    LoadPtr->eraseFromParent();
  }
}

void LoadStoreInstrument::StoreInstrument(StoreInst *StorePtr) {
  IRBuilder<> IRBuilder(StorePtr);
  Value *StoreAddr = StorePtr->getPointerOperand();
  Value *StoreAddrIntPtr = IRBuilder.CreateBitCast(StoreAddr, int64PtrType);
  Value *plaintext = StorePtr->getValueOperand();
  Value *encValue;
  if (M.getDataLayout().getTypeSizeInBits(plaintext->getType()) < 64) {
    if (plaintext->getType() != int32Type) {
      plaintext = IRBuilder.CreateBitCast(plaintext, int32Type);
    }
    encValue = IRBuilder.CreateCall(enc32Func, {plaintext, StoreAddrIntPtr});
  } else {
    if (plaintext->getType()->isPointerTy()) {
      plaintext = IRBuilder.CreatePtrToInt(plaintext, int64Type);
    } else if (plaintext->getType() != int64Type) {
      plaintext = IRBuilder.CreateBitCast(plaintext, int64Type);
    }
    encValue = IRBuilder.CreateCall(enc64Func, {plaintext, StoreAddrIntPtr});
  }
  IRBuilder.CreateStore(encValue, StoreAddrIntPtr);
  StorePtr->eraseFromParent();
}

void LoadStoreInstrument::instrumentLoadStore() {
  auto TraceVariable = [this](Value *v) {
    Type *instType = v->getType();
    if (!instType->isPointerTy())
      return;
    if (auto structType =
            dyn_cast<StructType>(instType->getPointerElementType())) {
      auto it = randomFieldOffset.find(structType);
      if (it != randomFieldOffset.end()) {
        for (int offset : it->second) {
          TraceStructField(structType, v, offset);
        }
      }
    }
  };
  auto TraceCastSource = [this](Value *v) {
    if (auto cast = dyn_cast<CastInst>(v)) {
      if (cast->getDestTy()->isPointerTy())
        if (auto structType = dyn_cast<StructType>(
                cast->getDestTy()->getPointerElementType())) {
          auto it = randomFieldOffset.find(structType);
          if (it != randomFieldOffset.end()) {
            for (int offset : it->second) {
              TraceStructField(structType, cast->getOperand(0), offset);
            }
          }
        }
    }
  };

  for (auto &F : M) {
    for (auto &arg : F.args()) {
      TraceVariable(&arg);
    }
    for (auto &BB : F)
      for (auto &Inst : BB) {
        TraceVariable(&Inst);
        TraceCastSource(&Inst);
      }
  }

  PATCH_put_cred_rcu();

  SmallVector<StoreInst *, 16> StoreList;
  SmallVector<LoadInst *, 16> LoadList;
  for (auto &F : M)
    for (auto &BB : F)
      for (auto &Inst : BB) {
        if (Inst.getMetadata("RandomFieldStore")) {
          StoreList.push_back(cast<StoreInst>(&Inst));
        } else if (Inst.getMetadata("RandomFieldLoad")) {
          LoadList.push_back(cast<LoadInst>(&Inst));
        }
      }
  for (auto &Inst : StoreList) {
    StoreInstrument(Inst);
  }
  for (auto &Inst : LoadList) {
    LoadInstrument(Inst);
  }
}

void LoadStoreInstrument::PATCH_put_cred_rcu() {
  if (M.getName().str().find("cred.c") == std::string::npos)
    return;
  for (auto &F : M) {
    if (F.getName() != "put_cred_rcu")
      continue;
    for (StructType *structPtr : M.getIdentifiedStructTypes()) {
      if (structPtr->getName() == "struct.cred") {
        int rcu_offset = M.getDataLayout().getTypeAllocSize(structPtr) - 16;
        for (int offset : randomFieldOffset[structPtr]) {
          TraceStructField(structPtr, F.getArg(0), offset - rcu_offset);
        }
        break;
      }
    }
    break;
  }
}

void InstrumentZeroInit::instZeroInit() {}

void InstrumentStaticInit::buildInitFunc() {
  std::string moduleName = M.getName().data();
  std::replace(moduleName.begin(), moduleName.end(), '/', '_');
  std::replace(moduleName.begin(), moduleName.end(), '.', '_');
  std::replace(moduleName.begin(), moduleName.end(), '-', '_');
  std::string initFuncName = "PPPRandInit_" + moduleName;
  FunctionType *FT = FunctionType::get(Type::getVoidTy(M.getContext()), false);
  Function *initFunc =
      Function::Create(FT, Function::ExternalLinkage, initFuncName, M);
  initFunc->setSection(".init.text");
  initFunc->addFnAttr(Attribute::Cold);
  initFunc->addFnAttr(Attribute::NoUnwind);
  auto BB = BasicBlock::Create(M.getContext(), "entry", initFunc);
  IRBuilder<> irBuilder(BB);
  bool changed = false;

  for (GlobalVariable &gv : M.getGlobalList()) {
    if (gv.hasExternalLinkage() && !gv.hasInitializer())
      continue;
    auto type = cast<PointerType>(gv.getType())->getElementType();
    std::set<int> fieldOffset = collectTypeOffset(type);
    for (int offset : fieldOffset) {
      changed = true;
      auto baseAddr = irBuilder.CreatePtrToInt(&gv, int64Type);
      auto targetAddr =
          irBuilder.CreateAdd(baseAddr, ConstantInt::get(int64Type, offset));
      auto targetPtr = irBuilder.CreateIntToPtr(
          targetAddr, Type::getInt64PtrTy(M.getContext()));
      auto loadInst = irBuilder.CreateLoad(int64Type, targetPtr);
      auto encValue = irBuilder.CreateCall(enc64Func, {loadInst, targetPtr});
      // auto encValue = irBuilder.CreateCall(encFunc, {loadInst,
      // ConstantPointerNull::get(int64PtrType)});
      irBuilder.CreateStore(encValue, targetPtr);
    }
  }
  irBuilder.CreateRet(nullptr);

  if (!changed) {
    initFunc->eraseFromParent();
  } else {
    auto FPType = PointerType::getUnqual(FT);
    auto initFuncFP =
        cast<GlobalVariable>(M.getOrInsertGlobal("FP_" + initFuncName, FPType));
    initFuncFP->setInitializer(initFunc);
    initFuncFP->setSection(".init.GFPInitFP");
  }
}

void InstrumentMemcpy::instMemcpy() {
  auto instrument = [this](IRBuilder<> &builder, Value *src, Value *dest,
                           bool srcEnc, bool destEnc,
                           std::set<int> &offsets) -> void {
    auto srcBaseInt = builder.CreatePtrToInt(src, int64Type);
    auto destBaseInt = builder.CreatePtrToInt(dest, int64Type);
    for (int off : offsets) {
      auto srcAddrInt =
          builder.CreateAdd(srcBaseInt, ConstantInt::get(int64Type, off));
      auto destAddrInt =
          builder.CreateAdd(destBaseInt, ConstantInt::get(int64Type, off));
      auto srcAddr = builder.CreateIntToPtr(srcAddrInt, int64PtrType);
      auto destAddr = builder.CreateIntToPtr(destAddrInt, int64PtrType);
      auto srcValue = builder.CreateLoad(srcAddr);
      Value *decValue = srcValue;
      if (srcEnc) {
        decValue = builder.CreateCall(dec64Func, {srcValue, srcAddr});
        // decValue = builder.CreateCall(decFunc, {srcValue,
        // ConstantPointerNull::get(int64PtrType)});
      }
      Value *encValue = decValue;
      if (destEnc) {
        encValue = builder.CreateCall(enc64Func, {decValue, destAddr});
        // encValue = builder.CreateCall(encFunc, {decValue,
        // ConstantPointerNull::get(int64PtrType)});
      }
      builder.CreateStore(encValue, destAddr);
    }
  };

  for (auto &F : M)
    for (auto &BB : F)
      for (auto it = BB.begin(), next = it; it != BB.end(); it = next) {
        next = std::next(it);

        if (auto CallPtr = dyn_cast<CallInst>(&*it)) {
          auto callee = CallPtr->getCalledFunction();
          if (!callee)
            continue;
          if (callee->getName().str().find("llvm.memcpy") != 0)
            continue;
          Value *src = CallPtr->getArgOperand(1);
          Value *dest = CallPtr->getArgOperand(0);
          IRBuilder<> builder(it->getNextNode());
          // case 1
          Type *srcType = getTrueType(src);
          Type *destType = getTrueType(dest);
          std::set<int> srcOffset, destOffset;
          if (srcType) {
            srcOffset = collectTypeOffset(srcType);
          }
          if (destType) {
            destOffset = collectTypeOffset(destType);
          }
          std::set<int> &copyOffset =
              srcOffset.size() > destOffset.size() ? srcOffset : destOffset;
          instrument(builder, src, dest, true, true, copyOffset);

          // case 2
          Value *copyLen = CallPtr->getOperand(2);
          if (!isa<ConstantInt>(copyLen))
            continue;
          int len = cast<ConstantInt>(copyLen)->getSExtValue();
          auto srcPartOff = getPartialOffset(src),
               destPartOff = getPartialOffset(dest);
          if (!srcPartOff.first && !destPartOff.first)
            continue;
          if (srcPartOff.first && destPartOff.first &&
              ((srcPartOff.first != destPartOff.first)))
            continue;
          int base = srcPartOff.first ? srcPartOff.second : destPartOff.second;
          std::set<int> partOffset;
          for (int offset :
               randomFieldOffset[srcPartOff.first ? srcPartOff.first
                                                  : destPartOff.first]) {
            if (offset >= base && offset + 8 <= base + len) {
              partOffset.insert(offset - base);
            }
          }
          instrument(builder, src, dest, srcPartOff.first, destPartOff.first,
                     partOffset);
        }
      }
}

Type *InstrumentMemcpy::getTrueType(Value *v) {
  if (auto castOp = dyn_cast<BitCastOperator>(v)) {
    if (castOp->getSrcTy()->isPointerTy()) {
      return castOp->getSrcTy()->getPointerElementType();
    }
  }
  if (auto castInst = dyn_cast<CastInst>(v)) {
    if (castInst->getSrcTy()->isPointerTy()) {
      return castInst->getSrcTy()->getPointerElementType();
    }
  }
  for (User *U : v->users()) {
    if (auto castOp = dyn_cast<BitCastOperator>(U)) {
      if (castOp->getDestTy()->isPointerTy() &&
          castOp->getDestTy()->getPointerElementType()->isStructTy()) {
        return castOp->getDestTy()->getPointerElementType();
      }
    }
    if (auto castInst = dyn_cast<CastInst>(U)) {
      if (castInst->getDestTy()->isPointerTy() &&
          castInst->getDestTy()->getPointerElementType()->isStructTy()) {
        return castInst->getDestTy()->getPointerElementType();
      }
    }
  }
  return nullptr;
}

std::pair<StructType *, int> InstrumentMemcpy::getPartialOffset(Value *v) {
  if (auto cast = dyn_cast<CastInst>(v)) {
    return getPartialOffset(cast->getOperand(0));
  }
  if (auto select = dyn_cast<SelectInst>(v)) {
    return getPartialOffset(select->getTrueValue());
  }
  if (auto gep = dyn_cast<GetElementPtrInst>(v)) {
    Type *srcTy = gep->getSourceElementType();
    if (randomFieldOffset.find(dyn_cast<StructType>(srcTy)) !=
        randomFieldOffset.end()) {
      APInt gep_offset(64, 0, true);
      if (gep->accumulateConstantOffset(M.getDataLayout(), gep_offset)) {
        return {cast<StructType>(srcTy), gep_offset.getSExtValue()};
      }
    }
  }
  return {nullptr, 0};
}

void RandomContent::getRandomOffset(const Module &M) {
  for (StructType *structPtr : M.getIdentifiedStructTypes()) {
    if (structPtr->isOpaque())
      continue;
    auto structName = structPtr->getName().str();
    auto it = randomFields.find(structName);
    std::set<int> offsets, arrayOffsets;
    if (it != randomFields.end()) {
      // errs() << "[getRandomOffset] \033[1;32mstructName: " << structName << "\033[0m\n";
      for (int ind : it->second) {
        int fieldOffset =
            M.getDataLayout().getStructLayout(structPtr)->getElementOffset(ind);
        // errs() << "[getRandomOffset] \033[1;32mind: " << ind << ", fieldOffset: " << fieldOffset << "\033[0m\n";
        Type *fieldType = structPtr->getTypeAtIndex(ind);
        getTypeMemberOffsets(M, fieldType, offsets, fieldOffset);
        getArrayOffsets(M, fieldType, arrayOffsets, fieldOffset);
      }
    }
    randomFieldOffset[structPtr] = offsets;
    randomArrayOffset[structPtr] = arrayOffsets;
  }
}

void RandomContent::getArrayOffsets(const Module &M, Type *type,
                                    std::set<int> &offsets, int base) {
  if (type->isArrayTy()) {
    offsets.insert(base);
  } else if (StructType *structType = dyn_cast<StructType>(type)) {
    for (int i = 0, end = structType->getNumElements(); i != end; ++i) {
      int fieldOffset =
          M.getDataLayout().getStructLayout(structType)->getElementOffset(i);
      Type *fieldType = structType->getTypeAtIndex(i);
      getArrayOffsets(M, fieldType, offsets, base + fieldOffset);
    }
  }
}

void RandomContent::getTypeMemberOffsets(const Module &M, Type *type,
                                         std::set<int> &offsets, int base) {
  if (ArrayType *arrayTy = dyn_cast<ArrayType>(type)) {
    Type *eleType = arrayTy->getElementType();
    int eleSize = M.getDataLayout().getTypeAllocSize(eleType);
    std::set<int> eleOffsets;
    getTypeMemberOffsets(M, eleType, eleOffsets, 0);
    for (int i = 0, end = arrayTy->getNumElements(); i != end; ++i) {
      for (int off : eleOffsets) {
        offsets.insert(base + i * eleSize + off);
      }
    }
  } else if (StructType *structType = dyn_cast<StructType>(type)) {
    for (int i = 0, end = structType->getNumElements(); i != end; ++i) {
      int fieldOffset =
          M.getDataLayout().getStructLayout(structType)->getElementOffset(i);
      Type *fieldType = structType->getTypeAtIndex(i);
      getTypeMemberOffsets(M, fieldType, offsets, base + fieldOffset);
    }
  } else {
    offsets.insert(base);
  }
}

PreservedAnalyses RandomContent::run(Module &M, ModuleAnalysisManager &) {
  loadSotreInstrument = new LoadStoreInstrument(M);
  instrumentZeroInit = new InstrumentZeroInit(M);
  instrumentStaticInit = new InstrumentStaticInit(M);
  instrumentMemcpy = new InstrumentMemcpy(M);

  getRandomOffset(M);
  loadSotreInstrument->instrumentLoadStore();
  instrumentZeroInit->instZeroInit();
  instrumentStaticInit->buildInitFunc();
  instrumentMemcpy->instMemcpy();

  // dumpByteCode(M);

  delete loadSotreInstrument;
  delete instrumentZeroInit;
  delete instrumentStaticInit;
  delete instrumentMemcpy;
  return PreservedAnalyses::none();
}