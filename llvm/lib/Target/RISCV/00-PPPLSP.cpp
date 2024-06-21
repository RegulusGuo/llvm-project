#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVMachineFunctionInfo.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

//===== Add by SimonSungm =====//

using namespace llvm;

#define DEBUG_TYPE "riscv-lsp"
#define STORETAG 10000
#define LOADTAG 20000

static cl::opt<bool>
    lspProtect("ppp-lsp", cl::init(false),
               cl::desc("Subproject in PPProject for RISC-V load & store protection"), cl::Hidden);

namespace
{
    class RISCVPPPLSP : public MachineFunctionPass
    {
    public:
        static char ID;
        explicit RISCVPPPLSP() : MachineFunctionPass(ID)
        {
            initializeRISCVPPPLSPPass(*PassRegistry::getPassRegistry());
        }
        bool runOnMachineFunction(MachineFunction &MF) override;

        StringRef getPassName() const override { return "Subproject in PPProject for RISC-V load & store protection"; }
    };

    char RISCVPPPLSP::ID = 0;
} // namespace

INITIALIZE_PASS(RISCVPPPLSP, DEBUG_TYPE, "RISCV Load & Store Protect", false, false)

bool RISCVPPPLSP::runOnMachineFunction(MachineFunction &MF)
{
    return false;
}

FunctionPass *llvm::createRISCVPPPLSPPass()
{
    return new RISCVPPPLSP();
}