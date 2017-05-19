#ifndef RV_TRANSFORM_REMTRANSFORM_H
#define RV_TRANSFORM_REMTRANSFORM_H

#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/IR/Function.h"

#include <set>

namespace llvm {
  class LoopInfo;
  class Loop;
  class DominatorTree;
  class PostDominatorTree;
}

template<class T>
inline
T&
LookUp(llvm::ValueToValueMapTy & valMap, T& key) {
  return *llvm::cast<T>(valMap[&key]);
}


using ValueSet = std::set<llvm::Value*>;

namespace rv {

class ReductionAnalysis;
class VectorizationInfo;

class RemainderTransform {
  llvm::Function & F;
  llvm::DominatorTree & DT;
  llvm::PostDominatorTree & PDT;
  llvm::LoopInfo & LI;
  ReductionAnalysis & reda;


// RemainderTransform capability checks
  // check if remTrans currently handles the loop exit condition
  bool canHandleExitCondition(llvm::Loop & L);
  // if this returns true RemainderTransform must not fail during the transformation and has to return a vectorizable loop
  bool canTransformLoop(llvm::Loop & L);

public:
  RemainderTransform(llvm::Function &_F, llvm::DominatorTree & _DT, llvm::PostDominatorTree & _PDT, llvm::LoopInfo & _LI, ReductionAnalysis & _reda)
  : F(_F)
  , DT(_DT)
  , PDT(_PDT)
  , LI(_LI)
  , reda(_reda)
  {}

  // create a vectorizable loop or return nullptr if remTrans can not currently do it
  llvm::Loop*
  createVectorizableLoop(llvm::Loop & L, ValueSet & uniOverrides, int vectorWidth, int tripAlign);
};

}

#endif // RV_TRANSFORM_REMTRANSFORM_H
