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
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "simple-dce"

STATISTIC(NumDeadInsts, "Number of dead instructions removed");

static bool isAlwaysLive(const Instruction &I, const TargetLibraryInfo &TLI) {
  return !wouldInstructionBeTriviallyDead(&I, &TLI);
}

// Removes every instruction whose result is not needed.
// Returns true if any instruction was removed.
static bool eliminateDeadInstructions(Function &F,
                                      const TargetLibraryInfo &TLI) {
  // Always-live instructions are live, so every instruction
  // that computes an operand of a always-live instruction should be kept.
  SmallPtrSet<Instruction *, 32> Live;
  SmallVector<Instruction *, 32> Worklist;
  SmallVector<LifetimeIntrinsic *, 8> LifetimeMarkers;
  for (Instruction &I : instructions(F)) {
    // A lifetime marker is removable only once all other users of its alloca are gone,
    // which is not known until marking is complete.
    LifetimeIntrinsic *LT = dyn_cast<LifetimeIntrinsic>(&I);
    if (LT && isa<AllocaInst>(LT->getArgOperand(0))) {
      LifetimeMarkers.push_back(LT);
      continue;
    }
    if (isAlwaysLive(I, TLI)) {
      Live.insert(&I);
      Worklist.push_back(&I);
    }
  }

  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();
    for (Value *Op : I->operands())
      if (auto *OpI = dyn_cast<Instruction>(Op))
        if (Live.insert(OpI).second)
          Worklist.push_back(OpI);
  }

  // Keep a lifetime marker only if its alloca is needed for something else.
  for (LifetimeIntrinsic *LT : LifetimeMarkers)
    if (Live.contains(cast<AllocaInst>(LT->getArgOperand(0))))
      Live.insert(LT);

  // Everything else is dead.
  SmallVector<Instruction *, 32> Dead;
  for (Instruction &I : instructions(F)) {
    if (!Live.contains(&I)) {
      LLVM_DEBUG(dbgs() << "SimpleDCE: removing dead instruction: " << I << '\n');
      Dead.push_back(&I);
    }
  }
  if (Dead.empty())
    return false;

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

PreservedAnalyses SimpleDCEPass::run(Function &F,
                                     FunctionAnalysisManager &FAM) {
  const TargetLibraryInfo &TLI = FAM.getResult<TargetLibraryAnalysis>(F);

  if (!eliminateDeadInstructions(F, TLI))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
