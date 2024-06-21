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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

//===== Add by SimonSungm =====//

#define DEBUG_TYPE "riscv-rap"

using namespace llvm;

static cl::opt<bool> rddProtect(
    "ppp-rap", cl::init(false),
    cl::desc("Subproject in PPProject for RISC-V return address protection"),
    cl::Hidden);

namespace llvm {

class RISCVPPPRAP : public MachineFunctionPass {
public:
  static char ID;
  explicit RISCVPPPRAP() : MachineFunctionPass(ID) {
    initializeRISCVPPPRAPPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return "Subproject in PPProject for RISC-V return address protection";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
char RISCVPPPRAP::ID = 0;
} // namespace llvm

INITIALIZE_PASS(RISCVPPPRAP, DEBUG_TYPE, "RISCV Rerturn Address Protect", false,
                false)

//  MIR example
//    $x2 = frame-setup ADDI $x2, -48      [prologue]: find store using x1
//    CFI_INSTRUCTION def_cfa_offset 48                |
//    SD killed $x1, $x2, 40                          \|/
//    SD killed $x8, $x2, 32                           V
//    ...                                                       A
//    $x8 = LD $x2, 32                                         /|\
//    $x1 = LD $x2, 40                                          |
//    $x2 = frame-destroy ADDI $x2, 48              [epilogue]: find load using
//    x1 PseudoRET implicit $x10

bool RISCVPPPRAP::runOnMachineFunction(MachineFunction &MF) {
  // errs() << "[PPP] in RAP pass\n";

  bool Changed = false;
  if (!rddProtect) {
    return Changed;
  }

  if (MF.getName().str().find("__vdso") == 0) {
    /* FIX:  Extra cr[e|d]tk in arch/riscv/kernel/vdso/vdso.so
       SOL:  Bypass functions in vgettimeofday.c:  __vdso_clock_gettime
                                                   __vdso_gettimeofday
                                                   __vdso_clock_getres
    */
    errs() << "[PPP] Bypass VDSO: " << MF.getName() << "\n";
    return Changed;
  }

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (MI.getFlag(MachineInstr::FrameSetup)) { // find next store using x1
        MachineInstr *next = MI.getNextNode();
        MachineInstr *insert = MI.getNextNode();
        while (next) {
          if (next->mayStore() && (next->getOperand(0).isReg())) {
            if (next->getOperand(0).getReg() == RISCV::X1) {
              DebugLoc DL = MBB.findDebugLoc(next);
              BuildMI(MBB, insert, DL, TII.get(RISCV::CRETK), RISCV::X1)
                  .addReg(RISCV::X1) /* ra */
                  .addReg(RISCV::X2) /* sp */
                  .addImm(0)
                  .addImm(7);
              Changed = true;
              break;
            }
          }
          next = next->getNextNode();
        }
      } else if (MI.getFlag(
                     MachineInstr::FrameDestroy)) { // find prev load using x1
        MachineInstr *prev = MI.getPrevNode();
        while (prev) {
          if (prev->mayLoad() && (prev->getOperand(0).isReg())) {
            if (prev->getOperand(0).getReg() == RISCV::X1) {
              DebugLoc DL = MBB.findDebugLoc(prev);
              BuildMI(MBB, prev->getNextNode(), DL, TII.get(RISCV::CRDTK),
                      RISCV::X1)
                  .addReg(RISCV::X1) /* ra */
                  .addReg(RISCV::X2) /* sp */
                  .addImm(0)
                  .addImm(5);
              MIBundleBuilder(MBB, prev, prev->getNextNode());
              Changed = true;
              break;
            }
          }
          prev = prev->getPrevNode();
        }
      }
    }
  }
  return Changed;
}

FunctionPass *llvm::createRISCVPPPRAPPass() { return new RISCVPPPRAP(); }