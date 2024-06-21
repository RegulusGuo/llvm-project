add_llvm_library( LLVMfpscan 
  fpscan.cpp
  fptag.cpp
  fpinit.cpp
  fpinstrument.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/fpscan

  DEPENDS
  intrinsics_gen
)