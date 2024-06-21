//  PPProject [kernel Pointer integrity Protecion Project]
//  Copyright (C) 2020 by phantom
//  Email: admin@phvntom.tech
//  This program is under MIT License, see http://phvntom.tech/LICENSE.txt
#include "llvm/Transforms/fpscan/fpscan.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include <cstring>
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

std::string fpscan::src_prefix = "/home/lhr/PPProject/linux/";
std::string fpscan::dumpBCDir = "/home/lhr/PPProject/test/linux-bc/";

void fpscan::dumpByteCode(Module &M)
{
    // std::string moduleName = M.getName().data();
    // moduleName = moduleName.substr(src_prefix.size());
    // std::replace(moduleName.begin(), moduleName.end(), '/', '-');
    // moduleName.back() = 'b';
    // moduleName.push_back('c');
    // std::error_code EC;
    // llvm::raw_fd_ostream OS(dumpBCDir + moduleName, EC, llvm::sys::fs::OF_None);
    // WriteBitcodeToFile(M, OS);
    // OS.flush();
}

PreservedAnalyses fpscan::run(Module &M, ModuleAnalysisManager &)
{
    fptag = new FpTag(&M);
    fpinit = new FpInit(fptag, &M);
    fpinstrument = new FpInstrument(fpinit, &M);

    fptag->Init();
    fptag->TagModule(M);
    fpinit->getAllFp();
    
    bool ret = fpinstrument->genGFPInitFunc();
    ret |= fpinstrument->instrumentLoadStore();
    ret |= fpinstrument->instrumentFPCopy();
    ret |= fpinstrument->instrumentFPZeroInit();
    ret |= fpinstrument->parameterReordering();

    //dumpByteCode(M);

    delete fptag;
    delete fpinit;
    delete fpinstrument;

    return ret ? PreservedAnalyses::none() : PreservedAnalyses::all();
}