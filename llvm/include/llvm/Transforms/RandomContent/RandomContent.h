#ifndef LLVM_TRANSFORMS_RANDOMCONTENT_H
#define LLVM_TRANSFORMS_RANDOMCONTENT_H

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
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

namespace llvm {
class Instrument {
public:
  Instrument(Module &M);

protected:
  Module &M;
  Type *int64Type, *int32Type;
  PointerType *int64PtrType;
  FunctionCallee enc64Func, enc32Func, dec64Func, dec32Func;
  Value *enc64Inst, *enc32Inst, *dec64Inst, *dec32Inst;

  const std::set<int> collectTypeOffset(Type *type);

private:
  const std::set<int> collectStruct(StructType *structType);
  const std::set<int> collectArray(ArrayType *arrType);
};

class LoadStoreInstrument : public Instrument {
public:
  LoadStoreInstrument(Module &M) : Instrument(M) {}
  void instrumentLoadStore();

private:
  void LoadInstrument(LoadInst *LoadPtr);
  void StoreInstrument(StoreInst *StorePtr);
  void TraceStructField(StructType *STy, Value *base_ptr, int offset);
  void TraceArrayBase(Value *v);
  int RecognizeGEPOffset(GetElementPtrInst *GEP);
  void TagInstruction(Instruction *Inst, StringRef string);

  void PATCH_put_cred_rcu();
};

class InstrumentZeroInit : public Instrument {
public:
  InstrumentZeroInit(Module &M) : Instrument(M) {}
  void instZeroInit();
};

class InstrumentStaticInit : public Instrument {
public:
  InstrumentStaticInit(Module &M) : Instrument(M) {}
  void buildInitFunc();
};

class InstrumentMemcpy : public Instrument {
public:
  InstrumentMemcpy(Module &M) : Instrument(M) {}
  void instMemcpy();

private:
  Type *getTrueType(Value *v);
  std::pair<StructType *, int> getPartialOffset(Value *v);
};

class RandomContent : public PassInfoMixin<RandomContent> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &);

private:
  LoadStoreInstrument *loadSotreInstrument;
  InstrumentZeroInit *instrumentZeroInit;
  InstrumentStaticInit *instrumentStaticInit;
  InstrumentMemcpy *instrumentMemcpy;

  void getRandomOffset(const Module &M);
  void getTypeMemberOffsets(const Module &M, Type *type, std::set<int> &offsets,
                            int base);
  void getArrayOffsets(const Module &M, Type *type, std::set<int> &offsets,
                       int base);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_RANDOMCONTEXT_H