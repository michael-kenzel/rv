#include "rv/transform/redOpt.h"

#include "llvm/IR/Dominators.h"
#include "rv/vectorizationInfo.h"
#include "rv/analysis/reductionAnalysis.h"

#include "rv/transform/redTools.h"
#include "report.h"

using namespace llvm;
using namespace rv;


ReductionOptimization::ReductionOptimization(VectorizationInfo & _vecInfo, ReductionAnalysis & _reda, DominatorTree & _dt)
: vecInfo(_vecInfo)
, reda(_reda)
, dt(_dt)
{}

bool
ReductionOptimization::optimize(PHINode & phi, Reduction & red) {
  auto phiShape = vecInfo.getVectorShape(phi);

  if (phi.getNumUses() == 1) return false; // only one user -> nothing to optimize here

  auto & neutral = GetNeutralElement(red.kind, *phi.getType());

  auto * inAtZero = dyn_cast<Instruction>(phi.getIncomingValue(0));
  int latchIdx = (inAtZero && vecInfo.inRegion(*inAtZero)) ? 0 : 1;

  auto * latchInst = dyn_cast<Instruction>(phi.getIncomingValue(latchIdx));
  assert(latchInst && "recurrence was annotated as reduction!");

  errs() << "Optimizing reduction phi " << phi << ":\n";

// replace all phi uses inside region with the neutral element (these are all starts of reduction chains)
  for (auto itUse = phi.use_begin(); itUse != phi.use_end(); ) {
    int opIdx = itUse->getOperandNo();
    auto * user = itUse->getUser();
    itUse++;

    auto * userInst = dyn_cast<Instruction>(user);
    if (!userInst) continue;
    if (!vecInfo.inRegion(*userInst)){
      errs() << "Preserving exernal user: \n";
      continue;  // preserve outside uses
    }

    errs() << "Remapping user to neutral : " << *userInst << "\n";
    userInst->setOperand(opIdx, &neutral);
    errs() << "\t mapped: " << *userInst << "\n";
  }

// fold accumulator into latch update after chains have been merged
  auto itLatch = latchInst->getIterator();
  ++itLatch;

  IRBuilder<> builder(latchInst->getParent(), itLatch);
  auto & latchUpdate = CreateReductInst(builder, red.kind, phi, *latchInst);
  vecInfo.setVectorShape(latchUpdate, phiShape);

  // redirect external users of the old latch value to use the latch update instead
  for (auto itUse = latchInst->use_begin(); itUse != latchInst->use_end(); ) {
    auto * inst = cast<Instruction>(itUse->getUser());
    int opIdx = itUse->getOperandNo();
    itUse++;

    if (vecInfo.inRegion(*inst)) continue;
    inst->setOperand(opIdx, &latchUpdate);
    itUse = latchInst->use_begin();
  }

  // use the late latch update instead
  phi.setIncomingValue(latchIdx, &latchUpdate);

  return true;
}

bool
ReductionOptimization::run() {
  if (!vecInfo.getRegion()) return false; // not applicable in WFV mode (wouldn't help)

  size_t numOptimizedReductions = 0;

  for (auto & inst : vecInfo.getEntry()) {
    auto * phi = dyn_cast<PHINode>(&inst);
    if (!phi) break;

    auto * redInfo = reda.getReductionInfo(*phi);
    if (!redInfo) continue;

    // optimize this reduction header phi
    bool changedRed = optimize(*phi, *redInfo);
    if (changedRed) {
      numOptimizedReductions++;
    }
  }

  Report() << "redOpt: optimized " << numOptimizedReductions << " reduction chains.\n";

  return numOptimizedReductions != 0;
}
