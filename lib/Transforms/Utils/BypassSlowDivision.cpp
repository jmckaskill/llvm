//===-- BypassSlowDivision.cpp - Bypass slow division ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains an optimization for div and rem on architectures that
// execute short instructions significantly faster than longer instructions.
// For example, on Intel Atom 32-bit divides are slow enough that during
// runtime it is profitable to check the value of the operands, and if they are
// positive and less than 256 use an unsigned 8-bit divide.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/BypassSlowDivision.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "bypass-slow-division"

namespace {
  struct DivOpInfo {
    bool SignedOp;
    Value *Dividend;
    Value *Divisor;

    DivOpInfo(bool InSignedOp, Value *InDividend, Value *InDivisor)
      : SignedOp(InSignedOp), Dividend(InDividend), Divisor(InDivisor) {}
  };

  struct DivPhiNodes {
    PHINode *Quotient;
    PHINode *Remainder;

    DivPhiNodes(PHINode *InQuotient, PHINode *InRemainder)
      : Quotient(InQuotient), Remainder(InRemainder) {}
  };
}

namespace llvm {
  template<>
  struct DenseMapInfo<DivOpInfo> {
    static bool isEqual(const DivOpInfo &Val1, const DivOpInfo &Val2) {
      return Val1.SignedOp == Val2.SignedOp &&
             Val1.Dividend == Val2.Dividend &&
             Val1.Divisor == Val2.Divisor;
    }

    static DivOpInfo getEmptyKey() {
      return DivOpInfo(false, nullptr, nullptr);
    }

    static DivOpInfo getTombstoneKey() {
      return DivOpInfo(true, nullptr, nullptr);
    }

    static unsigned getHashValue(const DivOpInfo &Val) {
      return (unsigned)(reinterpret_cast<uintptr_t>(Val.Dividend) ^
                        reinterpret_cast<uintptr_t>(Val.Divisor)) ^
                        (unsigned)Val.SignedOp;
    }
  };

  typedef DenseMap<DivOpInfo, DivPhiNodes> DivCacheTy;
}

// insertFastDiv - Substitutes the div/rem instruction with code that checks the
// value of the operands and uses a shorter-faster div/rem instruction when
// possible and the longer-slower div/rem instruction otherwise.
static bool insertFastDiv(Instruction *I, IntegerType *BypassType,
                          bool UseDivOp, bool UseSignedOp,
                          DivCacheTy &PerBBDivCache) {
  Function *F = I->getParent()->getParent();
  // Get instruction operands
  Value *Dividend = I->getOperand(0);
  Value *Divisor = I->getOperand(1);

  if (isa<ConstantInt>(Divisor) ||
      (isa<ConstantInt>(Dividend) && isa<ConstantInt>(Divisor))) {
    // Operations with immediate values should have
    // been solved and replaced during compile time.
    return false;
  }

  // Basic Block is split before divide
  BasicBlock *MainBB = &*I->getParent();
  BasicBlock *SuccessorBB = MainBB->splitBasicBlock(I);

  // Add new basic block for slow divide operation
  BasicBlock *SlowBB =
      BasicBlock::Create(F->getContext(), "", MainBB->getParent(), SuccessorBB);
  SlowBB->moveBefore(SuccessorBB);
  IRBuilder<> SlowBuilder(SlowBB, SlowBB->begin());
  Value *SlowQuotientV;
  Value *SlowRemainderV;
  if (UseSignedOp) {
    SlowQuotientV = SlowBuilder.CreateSDiv(Dividend, Divisor);
    SlowRemainderV = SlowBuilder.CreateSRem(Dividend, Divisor);
  } else {
    SlowQuotientV = SlowBuilder.CreateUDiv(Dividend, Divisor);
    SlowRemainderV = SlowBuilder.CreateURem(Dividend, Divisor);
  }
  SlowBuilder.CreateBr(SuccessorBB);

  // Add new basic block for fast divide operation
  BasicBlock *FastBB =
      BasicBlock::Create(F->getContext(), "", MainBB->getParent(), SuccessorBB);
  FastBB->moveBefore(SlowBB);
  IRBuilder<> FastBuilder(FastBB, FastBB->begin());
  Value *ShortDivisorV = FastBuilder.CreateCast(Instruction::Trunc, Divisor,
                                                BypassType);
  Value *ShortDividendV = FastBuilder.CreateCast(Instruction::Trunc, Dividend,
                                                 BypassType);

  // udiv/urem because optimization only handles positive numbers
  Value *ShortQuotientV = FastBuilder.CreateUDiv(ShortDividendV, ShortDivisorV);
  Value *ShortRemainderV = FastBuilder.CreateURem(ShortDividendV,
                                                  ShortDivisorV);
  Value *FastQuotientV = FastBuilder.CreateCast(Instruction::ZExt,
                                                ShortQuotientV,
                                                Dividend->getType());
  Value *FastRemainderV = FastBuilder.CreateCast(Instruction::ZExt,
                                                 ShortRemainderV,
                                                 Dividend->getType());
  FastBuilder.CreateBr(SuccessorBB);

  // Phi nodes for result of div and rem
  IRBuilder<> SuccessorBuilder(SuccessorBB, SuccessorBB->begin());
  PHINode *QuoPhi = SuccessorBuilder.CreatePHI(I->getType(), 2);
  QuoPhi->addIncoming(SlowQuotientV, SlowBB);
  QuoPhi->addIncoming(FastQuotientV, FastBB);
  PHINode *RemPhi = SuccessorBuilder.CreatePHI(I->getType(), 2);
  RemPhi->addIncoming(SlowRemainderV, SlowBB);
  RemPhi->addIncoming(FastRemainderV, FastBB);

  // Replace I with appropriate phi node
  if (UseDivOp)
    I->replaceAllUsesWith(QuoPhi);
  else
    I->replaceAllUsesWith(RemPhi);
  I->eraseFromParent();

  // Combine operands into a single value with OR for value testing below
  MainBB->getInstList().back().eraseFromParent();
  IRBuilder<> MainBuilder(MainBB, MainBB->end());
  Value *OrV = MainBuilder.CreateOr(Dividend, Divisor);

  // BitMask is inverted to check if the operands are
  // larger than the bypass type
  uint64_t BitMask = ~BypassType->getBitMask();
  Value *AndV = MainBuilder.CreateAnd(OrV, BitMask);

  // Compare operand values and branch
  Value *ZeroV = ConstantInt::getSigned(Dividend->getType(), 0);
  Value *CmpV = MainBuilder.CreateICmpEQ(AndV, ZeroV);
  MainBuilder.CreateCondBr(CmpV, FastBB, SlowBB);

  // Cache phi nodes to be used later in place of other instances
  // of div or rem with the same sign, dividend, and divisor
  DivOpInfo Key(UseSignedOp, Dividend, Divisor);
  DivPhiNodes Value(QuoPhi, RemPhi);
  PerBBDivCache.insert(std::pair<DivOpInfo, DivPhiNodes>(Key, Value));
  return true;
}

// reuseOrInsertFastDiv - Reuses previously computed dividend or remainder from
// the current BB if operands and operation are identical. Otherwise calls
// insertFastDiv to perform the optimization and caches the resulting dividend
// and remainder.
static bool reuseOrInsertFastDiv(Instruction *I, IntegerType *BypassType,
                                 bool UseDivOp, bool UseSignedOp,
                                 DivCacheTy &PerBBDivCache) {
  // Get instruction operands
  DivOpInfo Key(UseSignedOp, I->getOperand(0), I->getOperand(1));
  DivCacheTy::iterator CacheI = PerBBDivCache.find(Key);

  if (CacheI == PerBBDivCache.end()) {
    // If previous instance does not exist, insert fast div
    return insertFastDiv(I, BypassType, UseDivOp, UseSignedOp, PerBBDivCache);
  }

  // Replace operation value with previously generated phi node
  DivPhiNodes &Value = CacheI->second;
  if (UseDivOp) {
    // Replace all uses of div instruction with quotient phi node
    I->replaceAllUsesWith(Value.Quotient);
  } else {
    // Replace all uses of rem instruction with remainder phi node
    I->replaceAllUsesWith(Value.Remainder);
  }

  // Remove redundant operation
  I->eraseFromParent();
  return true;
}

// bypassSlowDivision - This optimization identifies DIV instructions in a BB
// that can be profitably bypassed and carried out with a shorter, faster
// divide.
bool llvm::bypassSlowDivision(
    BasicBlock *BB, const DenseMap<unsigned int, unsigned int> &BypassWidths) {
  DivCacheTy DivCache;

  bool MadeChange = false;
  Instruction* Next = &*BB->begin();
  while (Next != nullptr) {
    // We may add instructions immediately after I, but we want to skip over
    // them.
    Instruction* I = Next;
    Next = Next->getNextNode();

    // Get instruction details
    unsigned Opcode = I->getOpcode();
    bool UseDivOp = Opcode == Instruction::SDiv || Opcode == Instruction::UDiv;
    bool UseRemOp = Opcode == Instruction::SRem || Opcode == Instruction::URem;
    bool UseSignedOp = Opcode == Instruction::SDiv ||
                       Opcode == Instruction::SRem;

    // Only optimize div or rem ops
    if (!UseDivOp && !UseRemOp)
      continue;

    // Skip division on vector types, only optimize integer instructions
    if (!I->getType()->isIntegerTy())
      continue;

    // Get bitwidth of div/rem instruction
    IntegerType *T = cast<IntegerType>(I->getType());
    unsigned int bitwidth = T->getBitWidth();

    // Continue if bitwidth is not bypassed
    DenseMap<unsigned int, unsigned int>::const_iterator BI = BypassWidths.find(bitwidth);
    if (BI == BypassWidths.end())
      continue;

    // Get type for div/rem instruction with bypass bitwidth
    IntegerType *BT = IntegerType::get(I->getContext(), BI->second);

    MadeChange |= reuseOrInsertFastDiv(I, BT, UseDivOp, UseSignedOp, DivCache);
  }

  // Above we eagerly create divs and rems, as pairs, so that we can efficiently
  // create divrem machine instructions.  Now erase any unused divs / rems so we
  // don't leave extra instructions sitting around.
  for (auto &KV : DivCache)
    for (Instruction *Phi : {KV.second.Quotient, KV.second.Remainder})
      RecursivelyDeleteTriviallyDeadInstructions(Phi);

  return MadeChange;
}
