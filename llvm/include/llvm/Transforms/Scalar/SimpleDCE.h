//===- SimpleDCE.h - Simple dead code elimination ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the interface for the Simple Dead Code Elimination pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_SIMPLEDCE_H
#define LLVM_TRANSFORMS_SCALAR_SIMPLEDCE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

/// A dead code elimination pass that iterates its transformations until a
/// fixed point is reached.
class SimpleDCEPass : public OptionalPassInfoMixin<SimpleDCEPass> {
public:
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_SIMPLEDCE_H
