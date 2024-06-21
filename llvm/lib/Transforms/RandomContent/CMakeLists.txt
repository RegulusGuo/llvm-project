add_llvm_library( LLVMRandomContent
  RandomContent.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/RandomContent

  DEPENDS
  intrinsics_gen
 )