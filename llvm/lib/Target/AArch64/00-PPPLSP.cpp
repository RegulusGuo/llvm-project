#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
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

#define DEBUG_TYPE "aarch64-lsp"
#define STORETAG 10000
#define LOADTAG 20000

static cl::opt<bool>
    lspProtect("ppp-lsp", cl::init(false),
               cl::desc("Subproject in PPProject for AArch64 load & store protection"), cl::Hidden);

namespace
{
    class AArch64PPPLSP : public MachineFunctionPass
    {
    public:
        static char ID;
        explicit AArch64PPPLSP() : MachineFunctionPass(ID)
        {
            initializeAArch64PPPLSPPass(*PassRegistry::getPassRegistry());
        }
        bool runOnMachineFunction(MachineFunction &MF) override;

        StringRef getPassName() const override { return "Subproject in PPProject for RISC-V load & store protection"; }
    };

    char AArch64PPPLSP::ID = 0;
} // namespace

INITIALIZE_PASS(AArch64PPPLSP, DEBUG_TYPE, "AArch64 Load & Store Protect", false, false)

bool AArch64PPPLSP::runOnMachineFunction(MachineFunction &MF)
{
    return false;
}

FunctionPass *llvm::createAArch64PPPLSPPass()
{
    return new AArch64PPPLSP();
}