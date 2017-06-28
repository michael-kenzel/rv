//===- Utils.cpp -----------------------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// @author montada

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <utils/rvTools.h>
#include "Utils.h"

using namespace llvm;
using namespace rv;

Type *getVectorType(Type *type, unsigned width) {
  if (type->isVoidTy())
    return type;
  else
    return VectorType::get(type, width);
}

Value *createContiguousVector(unsigned width, Type *type, int start, int stride) {
  std::vector<Constant*> constants(width, nullptr);
  for (unsigned i = 0; i < width; ++i) {
    unsigned int val = i*stride + start;
    Constant *constant = type->isFloatingPointTy() ? ConstantFP::get(type, val) : ConstantInt::get(type, val);
    constants[i] = constant;
  }
  return ConstantVector::get(constants);
}

llvm::Value *getConstantVector(unsigned width, Type *type, unsigned value) {
    Constant *constant = type->isFloatingPointTy() ? ConstantFP::get(type, value)
                                                   : ConstantInt::get(type, value);
  return getConstantVector(width, constant);
}

llvm::Value *getConstantVector(unsigned width, Constant *constant) {
  std::vector<Constant *> constants;
  constants.reserve(width);
  for (unsigned i = 0; i < width; ++i) {
    constants.push_back(constant);
  }
  return ConstantVector::get(constants);
}

Value *getConstantVectorPadded(unsigned width, Type *type, std::vector<unsigned> &values, bool padWithZero) {
  std::vector<Constant *> constants(width, nullptr);
  unsigned i = 0;
  for (; i < values.size(); ++i) {
    Constant *constant = type->isFloatingPointTy() ? ConstantFP::get(type, values[i])
                                                   : ConstantInt::get(type, values[i]);
    constants[i] = constant;
  }
  Constant *zeroConst = type->isFloatingPointTy() ? ConstantFP::get(type, 0) : ConstantInt::get(type, 0);
  Constant *padding = padWithZero ? zeroConst : UndefValue::get(type);
  for (; i < width; ++i) {
    constants[i] = padding;
  }
  return ConstantVector::get(constants);
}

Value *getPointerOperand(Instruction *instr) {
  LoadInst *load = dyn_cast<LoadInst>(instr);
  StoreInst *store = dyn_cast<StoreInst>(instr);

  if (load) return load->getPointerOperand();
  else if (store) return store->getPointerOperand();
  else return nullptr;
}

BasicBlock *createCascadeBlocks(Function *insertInto, unsigned vectorWidth,
                                std::vector<BasicBlock *> &condBlocks,
                                std::vector<BasicBlock *> &maskedBlocks) {
  BasicBlock *cond, *mask;
  for (unsigned lane = 0; lane < vectorWidth; ++lane) {
    cond = BasicBlock::Create(insertInto->getContext(), "cascade_cond_" + std::to_string(lane),
                              insertInto);
    mask = BasicBlock::Create(insertInto->getContext(), "cascade_masked_" + std::to_string(lane),
                              insertInto);
    condBlocks.push_back(cond);
    maskedBlocks.push_back(mask);
  }
  return BasicBlock::Create(insertInto->getContext(), "cascade_end", insertInto);
}

bool isSupportedOperation(Instruction *const inst) {
  // binary operations (normal & bitwise), load / stores, conversion operations, returns, and other operations
  // exception: calls with vector or struct return type are not supported
  CallInst *call = dyn_cast<CallInst>(inst);
  if (call) {
    Type *retType = call->getFunctionType()->getReturnType();
    if (retType->isStructTy() || retType->isVectorTy())
      return false;
  }
  return inst->isBinaryOp() || isa<LoadInst>(inst) || isa<StoreInst>(inst) || inst->isCast() || isa<ReturnInst>(inst) ||
         (!isa<ExtractElementInst>(inst) && !isa<ExtractValueInst>(inst) && !isa<InsertElementInst>(inst) &&
          !isa<InsertValueInst>(inst) && !isa<ShuffleVectorInst>(inst) &&
          (inst->getOpcode() >= Instruction::OtherOpsBegin && inst->getOpcode() <= Instruction::OtherOpsEnd));
}

bool isHomogeneousStruct(StructType *const type) {
  assert(type->getStructNumElements() > 0 && "emptry struct!");
  Type *prevElTy = nullptr;
  for (Type *elTy : type->elements()) {
    if (!elTy->isStructTy() && !(elTy->isIntegerTy() || elTy->isFloatTy()))
      return false;

    else if (elTy->isStructTy() && !isHomogeneousStruct(cast<StructType>(elTy)))
      return false;

    if (prevElTy && prevElTy != elTy)
      return false;

    prevElTy = elTy;
  }

  return true;
}

StructType *isStructAccess(Value *const address) {
  assert(address->getType()->isPointerTy() && "not a pointer");

  if (isa<BitCastInst>(address))
    return isStructAccess(cast<BitCastInst>(address)->getOperand(0));

  Type *type;
  if (isa<GetElementPtrInst>(address))
    type = cast<GetElementPtrInst>(address)->getSourceElementType();
  else
    type = address->getType();

  return containsStruct(type);
}

StructType *containsStruct(Type *const type) {
  if (type->isStructTy())
    return cast<StructType>(type);

  if (type->isPointerTy())
    return containsStruct(cast<PointerType>(type)->getPointerElementType());

  else
    return nullptr;
}
