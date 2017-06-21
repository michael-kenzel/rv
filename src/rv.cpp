//===- rv.cpp ----------------*- C++ -*-===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// @authors kloessner
//

#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/IR/LegacyPassManager.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Analysis/MemoryDependenceAnalysis.h>
#include <rv/analysis/MandatoryAnalysis.h>

#include "rv/rv.h"
#include "rv/analysis/DFG.h"
#include "rv/analysis/VectorizationAnalysis.h"

#include "rv/transform/loopExitCanonicalizer.h"
#include "rv/transform/divLoopTrans.h"

#include "rv/PlatformInfo.h"
#include "rv/vectorizationInfo.h"
#include "rv/analysis/DFG.h"
#include "rv/analysis/reductionAnalysis.h"

#include "rv/transform/Linearizer.h"

#include "rv/transform/structOpt.h"
#include "rv/transform/srovTransform.h"
#include "rv/transform/irPolisher.h"

#include "native/NatBuilder.h"

#include "utils/rvTools.h"

#include "rvConfig.h"
#include "report.h"

#include "rv/transform/maskExpander.h"



namespace rv {

VectorizerInterface::VectorizerInterface(PlatformInfo & _platInfo)
        : platInfo(_platInfo)
{
  addIntrinsics();
}

void
VectorizerInterface::addIntrinsics() {
    for (Function & func : platInfo.getModule()) {
        if (func.getName() == "rv_any" ||
            func.getName() == "rv_all") {
          VectorMapping mapping(
            &func,
            &func,
            0, // no specific vector width
            -1, //
            VectorShape::uni(),
            {VectorShape::varying()}
          );
          platInfo.addSIMDMapping(mapping);
        } else if (func.getName() == "rv_extract") {
          VectorMapping mapping(
            &func,
            &func,
            0, // no specific vector width
            -1, //
            VectorShape::uni(),
            {VectorShape::varying(), VectorShape::uni()}
          );
          platInfo.addSIMDMapping(mapping);
        } else if (func.getName() == "rv_ballot") {
          VectorMapping mapping(
            &func,
            &func,
            0, // no specific vector width
            -1, //
            VectorShape::uni(),
            {VectorShape::varying(), VectorShape::varying()}
            );
          platInfo.addSIMDMapping(mapping);
        } else if (func.getName() == "rv_align") {
          VectorMapping mapping(
            &func,
            &func,
            0, // no specific vector width
            -1, //
            VectorShape::undef(),
            {VectorShape::undef(), VectorShape::uni()}
            );
          platInfo.addSIMDMapping(mapping);
        }
    }
}

void
VectorizerInterface::analyze(VectorizationInfo& vecInfo,
                             const CDG& cdg,
                             const DFG& dfg,
                             const LoopInfo& loopInfo,
                             const PostDominatorTree& postDomTree,
                             const DominatorTree& domTree)
{
    auto & scalarFn = vecInfo.getScalarFunction();

    // determines value and control shapes
    VectorizationAnalysis vea(platInfo,
                                  vecInfo,
                                  cdg,
                                  dfg,
                                  loopInfo,
                                  domTree, postDomTree);
    vea.analyze(scalarFn);

    // TODO deprecate MandatoryAnalysis (still used to determine kill exits atm)
    MandatoryAnalysis man(vecInfo, loopInfo, cdg);
    man.analyze(scalarFn);
}

bool
VectorizerInterface::linearize(VectorizationInfo& vecInfo,
                 CDG& cdg,
                 DFG& dfg,
                 LoopInfo& loopInfo,
                 PostDominatorTree& postDomTree,
                 DominatorTree& domTree)
{
    // use a fresh domtree here
    // DominatorTree fixedDomTree(vecInfo.getScalarFunction()); // FIXME someone upstream broke the domtree
    domTree.recalculate(vecInfo.getScalarFunction());

    // lazy mask generator
    MaskExpander maskEx(vecInfo, domTree, postDomTree, loopInfo);

    // convert divergent loops inside the region to uniform loops
    DivLoopTrans divLoopTrans(platInfo, vecInfo, maskEx, domTree, loopInfo);
    divLoopTrans.transformDivergentLoops();

    postDomTree.recalculate(vecInfo.getScalarFunction()); // FIXME
    domTree.recalculate(vecInfo.getScalarFunction()); // FIXME
    IF_DEBUG loopInfo.verify(domTree);

    // expand all remaining masks in the region
    maskEx.expandRegionMasks();

    IF_DEBUG {
      errs() << "--- VecInfo before Linearizer ---\n";
      vecInfo.dump();
    }

    // partially linearize acyclic control in the region
    Linearizer linearizer(vecInfo, maskEx, domTree, loopInfo);
    linearizer.run();

    IF_DEBUG {
      errs() << "--- VecInfo after Linearizer ---\n";
      vecInfo.dump();
    }

    return true;
}

// flag is set if the env var holds a string that starts on a non-'0' char
bool
VectorizerInterface::vectorize(VectorizationInfo &vecInfo, const DominatorTree &domTree, const LoopInfo & loopInfo, ScalarEvolution & SE, MemoryDependenceResults & MDR, ValueToValueMapTy * vecInstMap)
{
  // transform allocas from Array-of-struct into Struct-of-vector where possibe
  if (!CheckFlag("RV_DISABLE_STRUCTOPT")) {
    StructOpt sopt(vecInfo, platInfo.getDataLayout());
    sopt.run();
  } else {
    Report() << "Struct opt disabled (RV_DISABLE_STRUCTOPT != 0)\n";
  }

  // Scalar-Replication-Of-Varying-(Aggregates): split up structs of vectorizable elements to promote use of vector registers
  if (!CheckFlag("RV_DISABLE_SROV")) {
    SROVTransform srovTransform(vecInfo, platInfo);
    srovTransform.run();
  } else {
    Report() << "SROV opt disabled (RV_DISABLE_SROV != 0)\n";
  }

  ReductionAnalysis reda(vecInfo.getScalarFunction(), loopInfo);
  reda.analyze();

  bool embedControl = !vecInstMap;

// vectorize with native
  native::NatBuilder natBuilder(platInfo, vecInfo, domTree, MDR, SE, reda);
  natBuilder.vectorize(embedControl, vecInstMap);

  // IR Polish phase: promote i1 vectors and perform early instruction (read: intrinsic) selection
  if (!CheckFlag("RV_DISABLE_POLISH")) {
    IRPolisher polisher(vecInfo.getVectorFunction());
    polisher.polish();
  } else {
    Report() << "IR Polisher disabled (RV_DISABLE_POLISH != 0)\n";
  }

  IF_DEBUG verifyFunction(vecInfo.getVectorFunction());

  return true;
}

void
VectorizerInterface::finalize() {
  // TODO strip finalize
}

template <typename Impl>
static void lowerIntrinsicCall(CallInst* call, Impl impl) {
  call->replaceAllUsesWith(impl(call));
  call->eraseFromParent();
}

static void lowerIntrinsicCall(CallInst* call) {
  auto * callee = call->getCalledFunction();
  if (callee->getName() == "rv_any" ||
      callee->getName() == "rv_all" ||
      callee->getName() == "rv_extract" ||
      callee->getName() == "rv_align") {
    lowerIntrinsicCall(call, [] (const CallInst* call) {
      return call->getOperand(0);
    });
  } else if (callee->getName() == "rv_ballot") {
    lowerIntrinsicCall(call, [] (CallInst* call) {
      IRBuilder<> builder(call);
      return builder.CreateZExt(call->getOperand(0), builder.getInt32Ty());
    });
  }
}

void
lowerIntrinsics(Module & mod) {
  const char* names[] = {"rv_any", "rv_all", "rv_extract", "rv_ballot", "rv_align"};
  for (int i = 0, n = sizeof(names) / sizeof(names[0]); i < n; i++) {
    auto func = mod.getFunction(names[i]);
    if (!func) continue;

    for (
      auto itUse = func->use_begin();
      itUse != func->use_end();
      itUse = func->use_begin())
    {
      auto *user = itUse->getUser();

      if (!isa<CallInst>(user)) {
        errs() << "Non Call: " << *user << "\n";
      }

      lowerIntrinsicCall(cast<CallInst>(user));
    }
  }
}

void
lowerIntrinsics(Function & func) {
  for (auto & block : func) {
    BasicBlock::iterator itStart = block.begin(), itEnd = block.end();
    for (BasicBlock::iterator it = itStart; it != itEnd; ) {
      auto * inst = &*it++;
      auto * call = dyn_cast<CallInst>(inst);
      if (call) lowerIntrinsicCall(call);
    }
  }
}



} // namespace rv
