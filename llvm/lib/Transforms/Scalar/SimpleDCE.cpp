//===- SimpleDCE.cpp - Code to perform simple dead code elimination --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements simple dead inst elimination and dead code elimination.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/SimpleDCE.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "simple-dce"

// For debugging.
STATISTIC(NumDeadInsts, "Number of dead instructions removed");
STATISTIC(NumDeadStackSlots, "Number of dead stack slots (allocas) removed");
STATISTIC(NumDeadStackSlotWrites,
          "Number of writes to dead stack slots removed");
STATISTIC(NumUnreachableBlocks, "Number of unreachable blocks removed");
STATISTIC(NumFoldedBranches,
          "Number of branches to a single successor made unconditional");
STATISTIC(NumForwardingBlocks, "Number of forwarding blocks removed");
STATISTIC(NumMergedBlocks, "Number of blocks merged into their predecessor");

static bool isAlwaysLive(const Instruction &I, const TargetLibraryInfo &TLI) {
  return !wouldInstructionBeTriviallyDead(&I, &TLI);
}

// Collects the instructions whose only effect is writing to the stack slot allocated by given AllocaInst.
static void collectStackSlotWrites(AllocaInst &AI,
                                   SmallVectorImpl<Instruction *> &Writes) {
  SmallVector<Instruction *, 8> Pointers = {&AI};
  while (!Pointers.empty()) {
    Instruction *Ptr = Pointers.pop_back_val();
    for (Use &U : Ptr->uses()) {
      auto *User = cast<Instruction>(U.getUser());
      if (isa<GetElementPtrInst, BitCastInst, AddrSpaceCastInst>(User)) {
        // If the pointer is derived from the slot, add it to the Pointers list.
        Pointers.push_back(User);
      } else if (auto *SI = dyn_cast<StoreInst>(User)) {
        // If the store's destination is the slot, add it to the Writes list.
        if (U.getOperandNo() == StoreInst::getPointerOperandIndex() &&
            SI->isUnordered()) // Ordered atomic stores are not dead, because they may be used for communication with other threads.
          Writes.push_back(SI);
      } else if (auto *MI = dyn_cast<MemIntrinsic>(User)) {
        // If the memory intrinsic's destination is the slot, add it to the Writes list.
        if (&U == &MI->getRawDestUse() && !MI->isVolatile()) // Volatile stores are not dead, because they are observable and can have side effects.
          Writes.push_back(MI);
      } else if (isa<LifetimeIntrinsic>(User)) {
        Writes.push_back(User);
      }
    }
  }
}

// Before the dead stack slot is removed, rewrites the debug records of the related variable.
// From #dbg_declare to #dbg_value, and removes the debug records that are no longer valid.
static void salvageStackSlotDebugInfo(AllocaInst &AI,
                                      ArrayRef<Instruction *> Writes) {
  SmallVector<DbgVariableRecord *, 4> DbgUsers;
  findDbgUsers(&AI, DbgUsers);
  if (DbgUsers.empty())
    return;

  DIBuilder DIB(*AI.getModule(), false);
  for (Instruction *W : Writes)
    if (auto *SI = dyn_cast<StoreInst>(W))
      for (DbgVariableRecord *DVR : DbgUsers)
        if (DVR->isAddressOfVariable())
          ConvertDebugDeclareToDebugValue(DVR, SI, DIB);

  for (DbgVariableRecord *DVR : DbgUsers)
    if (DVR->isAddressOfVariable() || DVR->getExpression()->startsWithDeref())
      DVR->eraseFromParent();
}

// Removes every instruction whose result is not needed.
// Returns true if any instruction was removed.
static bool eliminateDeadInstructions(Function &F,
                                      const TargetLibraryInfo &TLI) {
  DenseMap<AllocaInst *, SmallVector<Instruction *, 4>> SlotWrites; // {alloca inst, [writes to the slot]}
  SmallPtrSet<Instruction *, 32> IsSlotWrite;
  for (Instruction &I : instructions(F)) {
    if (auto *AI = dyn_cast<AllocaInst>(&I)) {
      SmallVector<Instruction *, 4> Writes;
      collectStackSlotWrites(*AI, Writes);
      if (!Writes.empty()) {
        IsSlotWrite.insert_range(Writes);
        SlotWrites[AI] = std::move(Writes);
      }
    }
  }

  // Always-live instructions are live, so every instruction
  // that computes an operand of a always-live instruction should be kept.
  SmallPtrSet<Instruction *, 32> Live;
  SmallVector<Instruction *, 32> Worklist;
  
  auto MarkLive = [&](Instruction *I) {
    if (Live.insert(I).second)
      Worklist.push_back(I);
  };

  for (Instruction &I : instructions(F))
    if (!IsSlotWrite.contains(&I) && isAlwaysLive(I, TLI))
      MarkLive(&I);

  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();
    for (Value *Op : I->operands())
      if (auto *OpI = dyn_cast<Instruction>(Op))
        MarkLive(OpI);
    
    // For a live stack slot, the instructions that write to it may be live.
    if (auto *AI = dyn_cast<AllocaInst>(I)) {
      auto It = SlotWrites.find(AI);
      if (It != SlotWrites.end())
        for (Instruction *WrI : It->second)
          MarkLive(WrI);
    }
  }

  // Everything else is dead.
  SmallVector<Instruction *, 32> Dead;
  for (Instruction &I : instructions(F)) {
    if (!Live.contains(&I)) {
      LLVM_DEBUG(dbgs() << "SimpleDCE: removing dead instruction: " << I << '\n');
      Dead.push_back(&I);
      if (isa<AllocaInst>(I))
        NumDeadStackSlots++;
      else if (IsSlotWrite.contains(&I))
        NumDeadStackSlotWrites++;
    }
  }
  if (Dead.empty())
    return false;

  for (Instruction *I : Dead)
    if (auto *AI = dyn_cast<AllocaInst>(I))
      salvageStackSlotDebugInfo(*AI, SlotWrites.lookup(AI));

  // Release each dead instruction.
  SmallPtrSet<Instruction *, 32> Pending(llvm::from_range, Dead);
  SmallVector<Instruction *, 32> Ready;
  
  auto Release = [&](Instruction *I) {
    Pending.erase(I);
    salvageDebugInfo(*I);
    for (Use &Op : I->operands()) {
      Instruction *OpI = dyn_cast<Instruction>(Op.get());
      Op.set(nullptr);
      if (OpI && OpI->use_empty() && Pending.contains(OpI))
        Ready.push_back(OpI);
    }
  };
  
  auto ReleaseAll = [&]() {
    while (!Ready.empty())
      Release(Ready.pop_back_val());
  };

  for (Instruction *I : Dead)
    if (I->use_empty())
      Ready.push_back(I);
  ReleaseAll();
  
  // Instructions on a dead cycle keep each other's uses alive, we should break the cycles.
  for (Instruction *I : llvm::reverse(Dead)) {
    if (Pending.contains(I)) {
      Release(I);
      ReleaseAll();
    }
  }

  for (Instruction *I : Dead)
    I->eraseFromParent();

  NumDeadInsts += Dead.size();
  return true;
}

// Removes the blocks that cannot be reached from the entry block.
// Returns true if any block was removed.
static bool eliminateUnreachableBlocks(Function &F) {
  df_iterator_default_set<BasicBlock *> Reachable;
  for (BasicBlock *BB : depth_first_ext(&F, Reachable))
    (void)BB; // just to stop the unused variable warning

  SmallVector<BasicBlock *, 8> Unreachable;
  for (BasicBlock &BB : F) {
    if (!Reachable.contains(&BB)) {
      LLVM_DEBUG(dbgs() << "SimpleDCE: removing unreachable block: " << BB.getName() << '\n');
      Unreachable.push_back(&BB);
    }
  }
  if (Unreachable.empty())
    return false;

  DeleteDeadBlocks(Unreachable);
  NumUnreachableBlocks += Unreachable.size();
  return true;
}

// Replaces a conditional branch whose destinations are all the same block with an unconditional branch.
// Returns true if the terminator was replaced.
static bool foldBranchToCommonSuccessor(BasicBlock &BB,
                                        const TargetLibraryInfo &TLI) {
  Instruction *Term = BB.getTerminator();
  Value *Cond;
  if (auto *CondBr = dyn_cast<CondBrInst>(Term))
    Cond = CondBr->getCondition();
  else if (auto *SI = dyn_cast<SwitchInst>(Term))
    Cond = SI->getCondition();
  else
    return false;

  if (!all_equal(successors(Term)))
    return false;

  LLVM_DEBUG(dbgs() << "SimpleDCE: folding branch to a single successor: "
                    << *Term << '\n');

  // For The PHIs in Succ that have one entry per edge from BB, but all with the same value, keep one of them.
  BasicBlock *Succ = Term->getSuccessor(0);
  for (unsigned int I = 1, E = Term->getNumSuccessors(); I != E; I++)
    Succ->removePredecessor(&BB, true);

  UncondBrInst *Br = UncondBrInst::Create(Succ, Term->getIterator());
  Br->copyMetadata(*Term, {LLVMContext::MD_dbg, LLVMContext::MD_loop,
                           LLVMContext::MD_annotation});
  Term->eraseFromParent();
  NumFoldedBranches++;

  RecursivelyDeleteTriviallyDeadInstructions(
      Cond, &TLI, nullptr, [](Value *V) {
        LLVM_DEBUG(dbgs() << "SimpleDCE: removing dead instruction: " << *V
                          << '\n');
        NumDeadInsts++;
      });
  return true;
}

// Removes BB if it only contains an unconditional branch (besides PHIs and debug records).
// Redirect its predecessors to its successor.
// Returns true if the CFG was changed.
static bool removeForwardingBlock(BasicBlock &BB) {
  // The entry block cannot be removed.
  auto *Br = dyn_cast<UncondBrInst>(BB.getTerminator());
  if (!Br || BB.isEntryBlock() || &*BB.getFirstNonPHIOrDbg(false) != Br)
    return false;

  [[maybe_unused]] BasicBlock *Succ = Br->getSuccessor();
  if (!TryToSimplifyUncondBranchFromEmptyBlock(&BB))
    return false;

  LLVM_DEBUG(dbgs() << "SimpleDCE: redirected a forwarding block to "
                    << Succ->getName() << '\n');
  NumForwardingBlocks++;
  return true;
}

// Merges BB into its predecessor if that is BB's only predecessor and BB is its only successor.
// Returns true if BB was merged.
static bool mergeIntoPredecessor(BasicBlock &BB) {
  BasicBlock *Pred = BB.getUniquePredecessor();
  if (!Pred || !MergeBlockIntoPredecessor(&BB))
    return false;

  LLVM_DEBUG(dbgs() << "SimpleDCE: merged a block into " << Pred->getName()
                    << '\n');
  NumMergedBlocks++;
  return true;
}

// Simplifies the CFG by folding branches to a single successor, removing forwarding blocks and merging blocks into their predecessors.
// Returns true if the CFG was changed.
static bool simplifyBlocks(Function &F, const TargetLibraryInfo &TLI) {
  bool Changed = false;
  // Visit successors before predecessors so that simplifying a block can immediately enable simplifying its predecessors.
  SmallVector<BasicBlock *, 32> Blocks;
  llvm::copy(post_order(&F), std::back_inserter(Blocks));
  for (BasicBlock *BB : Blocks) {
    Changed |= foldBranchToCommonSuccessor(*BB, TLI);
    if (removeForwardingBlock(*BB) || mergeIntoPredecessor(*BB))
      Changed = true;
  }
  return Changed;
}

PreservedAnalyses SimpleDCEPass::run(Function &F,
                                     FunctionAnalysisManager &FAM) {
  const TargetLibraryInfo &TLI = FAM.getResult<TargetLibraryAnalysis>(F);

  // Iterate until nothing changes.
  bool Changed = false;
  bool CFGChanged = false;
  while (true) {
    bool LocalCFGChanged = eliminateUnreachableBlocks(F);
    bool LocalChanged = eliminateDeadInstructions(F, TLI);
    LocalCFGChanged |= simplifyBlocks(F, TLI);
    if (!LocalChanged && !LocalCFGChanged)
      break;
    Changed = true;
    CFGChanged |= LocalCFGChanged;
  }

  if (!Changed)
    return PreservedAnalyses::all();
  if (CFGChanged)
    return PreservedAnalyses::none();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
