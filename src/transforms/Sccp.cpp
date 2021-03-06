//===---- Inter-procedural Sparse Conditional Constant Propagation --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements sparse conditional constant propagation and merging:
//
// Specifically, this:
//   * Assumes values are constant unless proven otherwise
//   * Assumes BasicBlocks are dead unless proven otherwise
//   * Proves values to be constant, and replaces them with constants
//   * Proves conditional branches to be unconditional
//===----------------------------------------------------------------------===//
// Modifications for OCCAM:
//===----------------------------------------------------------------------===//
// - Teach SCCP to ignore some global initializers. This is
//   communicated to SCCP via metadata. The IPDSE (inter-procedural
//   dead store elimination) pass detects when some global
//   initializers are useless.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/OptBisect.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>

using namespace llvm;

#define DEBUG_TYPE "ipsccp-previrt"

STATISTIC(IPNumDeadBlocks, "Number of basic blocks unreachable by IPSCCP");
STATISTIC(IPNumInstRemoved, "Number of instructions removed by IPSCCP");
STATISTIC(IPNumArgsElimed, "Number of arguments constant propagated by IPSCCP");
STATISTIC(IPNumGlobalConst, "Number of globals found to be constant by IPSCCP");

#define SCCP_LOG(...) LLVM_DEBUG(__VA_ARGS__)
//#define SCCP_LOG(...) __VA_ARGS__

namespace previrt {
namespace transforms {

/// LatticeVal class - This class represents the different lattice values that
/// an LLVM value may occupy.  It is a simple class with value semantics.
///
class LatticeVal {
  enum LatticeValueTy {
    /// unknown - This LLVM Value has no known value yet.
    unknown,

    /// constant - This LLVM Value has a specific constant value.
    constant,

    /// forcedconstant - This LLVM Value was thought to be undef until
    /// ResolvedUndefsIn.  This is treated just like 'constant', but if merged
    /// with another (different) constant, it goes to overdefined, instead of
    /// asserting.
    forcedconstant,

    /// overdefined - This instruction is not known to be constant, and we know
    /// it has a value.
    overdefined
  };

  /// Val: This stores the current lattice value along with the Constant* for
  /// the constant if this is a 'constant' or 'forcedconstant' value.
  PointerIntPair<Constant *, 2, LatticeValueTy> Val;

  LatticeValueTy getLatticeValue() const { return Val.getInt(); }

public:
  LatticeVal() : Val(nullptr, unknown) {}

  bool isUnknown() const { return getLatticeValue() == unknown; }
  bool isConstant() const {
    return getLatticeValue() == constant || getLatticeValue() == forcedconstant;
  }
  bool isOverdefined() const { return getLatticeValue() == overdefined; }

  Constant *getConstant() const {
    assert(isConstant() && "Cannot get the constant of a non-constant!");
    return Val.getPointer();
  }

  /// markOverdefined - Return true if this is a change in status.
  bool markOverdefined() {
    if (isOverdefined())
      return false;

    Val.setInt(overdefined);
    return true;
  }

  /// markConstant - Return true if this is a change in status.
  bool markConstant(Constant *V) {
    if (getLatticeValue() == constant) { // Constant but not forcedconstant.
      assert(getConstant() == V && "Marking constant with different value");
      return false;
    }

    if (isUnknown()) {
      Val.setInt(constant);
      assert(V && "Marking constant with NULL");
      Val.setPointer(V);
    } else {
      assert(getLatticeValue() == forcedconstant &&
             "Cannot move from overdefined to constant!");
      // Stay at forcedconstant if the constant is the same.
      if (V == getConstant())
        return false;

      // Otherwise, we go to overdefined.  Assumptions made based on the
      // forced value are possibly wrong.  Assuming this is another constant
      // could expose a contradiction.
      Val.setInt(overdefined);
    }
    return true;
  }

  /// getConstantInt - If this is a constant with a ConstantInt value, return it
  /// otherwise return null.
  ConstantInt *getConstantInt() const {
    if (isConstant())
      return dyn_cast<ConstantInt>(getConstant());
    return nullptr;
  }

  /// getBlockAddress - If this is a constant with a BlockAddress value, return
  /// it, otherwise return null.
  BlockAddress *getBlockAddress() const {
    if (isConstant())
      return dyn_cast<BlockAddress>(getConstant());
    return nullptr;
  }

  void markForcedConstant(Constant *V) {
    assert(isUnknown() && "Can't force a defined value!");
    Val.setInt(forcedconstant);
    Val.setPointer(V);
  }
};

inline raw_ostream &operator<<(raw_ostream &o, LatticeVal v) {
  if (v.isUnknown()) {
    o << "Unknown";
  } else if (v.isConstant()) {
    o << "Constant(";
    if (Function *CF = dyn_cast<Function>(v.getConstant())) {
      o << CF->getName();
    } else {
      o << *(v.getConstant());
    }
    o << ")";
  } else if (v.isOverdefined()) {
    o << "Top";
  } else {
    llvm_unreachable("unsupported CP lattice value");
  }
  return o;
}

//===----------------------------------------------------------------------===//
//
/// SCCPSolver - This class is a general purpose solver for Sparse Conditional
/// Constant Propagation.
///
class SCCPSolver : public InstVisitor<SCCPSolver> {
  const DataLayout &DL;
  TargetLibraryInfoWrapperPass *TLIWrapper;
  SmallPtrSet<BasicBlock *, 8> BBExecutable; // The BBs that are executable.
  DenseMap<Value *, LatticeVal> ValueState;  // The state each value is in.

  /// StructValueState - This maintains ValueState for values that have
  /// StructType, for example for formal arguments, calls, insertelement, etc.
  ///
  DenseMap<std::pair<Value *, unsigned>, LatticeVal> StructValueState;

  /// GlobalValue - If we are tracking any values for the contents of a global
  /// variable, we keep a mapping from the constant accessor to the element of
  /// the global, to the currently known value.  If the value becomes
  /// overdefined, it's entry is simply removed from this map.
  DenseMap<GlobalVariable *, LatticeVal> TrackedGlobals;

  /// TrackedRetVals - If we are tracking arguments into and the return
  /// value out of a function, it will have an entry in this map, indicating
  /// what the known return value for the function is.
  DenseMap<Function *, LatticeVal> TrackedRetVals;

  /// TrackedMultipleRetVals - Same as TrackedRetVals, but used for functions
  /// that return multiple values.
  DenseMap<std::pair<Function *, unsigned>, LatticeVal> TrackedMultipleRetVals;

  /// MRVFunctionsTracked - Each function in TrackedMultipleRetVals is
  /// represented here for efficient lookup.
  SmallPtrSet<Function *, 16> MRVFunctionsTracked;

  /// TrackingIncomingArguments - This is the set of functions for whose
  /// arguments we make optimistic assumptions about and try to prove as
  /// constants.
  SmallPtrSet<Function *, 16> TrackingIncomingArguments;

  /// The reason for two worklists is that overdefined is the lowest state
  /// on the lattice, and moving things to overdefined as fast as possible
  /// makes SCCP converge much faster.
  ///
  /// By having a separate worklist, we accomplish this because everything
  /// possibly overdefined will become overdefined at the soonest possible
  /// point.
  SmallVector<Value *, 64> OverdefinedInstWorkList;
  SmallVector<Value *, 64> InstWorkList;

  SmallVector<BasicBlock *, 64> BBWorkList; // The BasicBlock work list

  /// KnownFeasibleEdges - Entries in this set are edges which have already had
  /// PHI nodes retriggered.
  typedef std::pair<BasicBlock *, BasicBlock *> Edge;
  DenseSet<Edge> KnownFeasibleEdges;

public:
  SCCPSolver(const DataLayout &DL, TargetLibraryInfoWrapperPass *tliWrapper)
      : DL(DL), TLIWrapper(tliWrapper) {}

  /// MarkBlockExecutable - This method can be used by clients to mark all of
  /// the blocks that are known to be intrinsically live in the processed unit.
  ///
  /// This returns true if the block was not considered live before.
  bool MarkBlockExecutable(BasicBlock *BB) {
    if (!BBExecutable.insert(BB).second)
      return false;
    SCCP_LOG(errs() << "Marking Block Executable: " << BB->getName() << '\n');
    BBWorkList.push_back(BB); // Add the block to the work list!
    return true;
  }

  /// TrackValueOfGlobalVariable - inform the SCCPSolver that it
  /// should track loads and stores to the specified global variable
  /// if it can.  This is only legal to call if performing
  /// Interprocedural SCCP.
  void TrackValueOfGlobalVariable(GlobalVariable *GV) {
    // === OCCAM === //
    if (GV->getValueType()->isSingleValueType()) {
      ///Key modification wrt to original LLVM SCCP:
      if (GV->getMetadata("ipdse.useless_initializer")) {
	TrackedGlobals.insert({GV, LatticeVal()});
      } else {
    	LatticeVal &IV = TrackedGlobals[GV];
    	if (!isa<UndefValue>(GV->getInitializer()))
	  IV.markConstant(GV->getInitializer());
      }
    }
  }

  /// AddTrackedFunction - If the SCCP solver is supposed to track calls into
  /// and out of the specified function (which cannot have its address taken),
  /// this method must be called.
  void AddTrackedFunction(Function *F) {
    // Add an entry, F -> undef.
    if (auto *STy = dyn_cast<StructType>(F->getReturnType())) {
      MRVFunctionsTracked.insert(F);
      for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
        TrackedMultipleRetVals.insert(
            std::make_pair(std::make_pair(F, i), LatticeVal()));
    } else
      TrackedRetVals.insert(std::make_pair(F, LatticeVal()));
  }

  void AddArgumentTrackedFunction(Function *F) {
    TrackingIncomingArguments.insert(F);
  }

  /// Solve - Solve for constants and executable blocks.
  ///
  void Solve();

  /// ResolvedUndefsIn - While solving the dataflow for a function, we assume
  /// that branches on undef values cannot reach any of their successors.
  /// However, this is not a safe assumption.  After we solve dataflow, this
  /// method should be use to handle this.  If this returns true, the solver
  /// should be rerun.
  bool ResolvedUndefsIn(Function &F);

  bool isBlockExecutable(BasicBlock *BB) const {
    return BBExecutable.count(BB);
  }

  std::vector<LatticeVal> getStructLatticeValueFor(Value *V) const {
    std::vector<LatticeVal> StructValues;
    auto *STy = dyn_cast<StructType>(V->getType());
    assert(STy && "getStructLatticeValueFor() can be called only on structs");
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      auto I = StructValueState.find(std::make_pair(V, i));
      assert(I != StructValueState.end() && "Value not in valuemap!");
      StructValues.push_back(I->second);
    }
    return StructValues;
  }

  LatticeVal getLatticeValueFor(Value *V) const {
    DenseMap<Value *, LatticeVal>::const_iterator I = ValueState.find(V);
    assert(I != ValueState.end() && "V is not in valuemap!");
    return I->second;
  }

  /// getTrackedRetVals - Get the inferred return value map.
  ///
  const DenseMap<Function *, LatticeVal> &getTrackedRetVals() {
    return TrackedRetVals;
  }

  /// getTrackedGlobals - Get and return the set of inferred initializers for
  /// global variables.
  const DenseMap<GlobalVariable *, LatticeVal> &getTrackedGlobals() {
    return TrackedGlobals;
  }

  /// getMRVFunctionsTracked - Get the set of functions which return multiple
  /// values tracked by the pass.
  const SmallPtrSet<Function *, 16> getMRVFunctionsTracked() {
    return MRVFunctionsTracked;
  }

  /// markOverdefined - Mark the specified value overdefined.  This
  /// works with both scalars and structs.
  void markOverdefined(Value *V) {
    if (auto *STy = dyn_cast<StructType>(V->getType()))
      for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
        markOverdefined(getStructValueState(V, i), V);
    else
      markOverdefined(ValueState[V], V);
  }

  // isStructLatticeConstant - Return true if all the lattice values
  // corresponding to elements of the structure are not overdefined,
  // false otherwise.
  bool isStructLatticeConstant(Function *F, StructType *STy) {
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      const auto &It = TrackedMultipleRetVals.find(std::make_pair(F, i));
      assert(It != TrackedMultipleRetVals.end());
      LatticeVal LV = It->second;
      if (LV.isOverdefined())
        return false;
    }
    return true;
  }

  void printValueState(raw_ostream &o) {
    o << "ValueState\n";
    for (auto &kv : ValueState) {
      if (!kv.first->hasName() || !kv.second.isConstant()) {
        continue;
      }
      o << "\t" << kv.first->getName() << " --> " << kv.second << "\n";
    }
    o << "StructValueState\n";
    for (auto &kv : StructValueState) {
      if (!kv.first.first->hasName() || !kv.second.isConstant()) {
        continue;
      }
      o << "\t" << kv.first.first->getName() << "." << kv.first.second
        << " --> " << kv.second << "\n";
    }
    o << "TrackedGlobals\n";
    for (auto &kv : TrackedGlobals) {
      if (!kv.first->hasName() || !kv.second.isConstant()) {
        continue;
      }
      o << "\t" << kv.first->getName() << " --> " << kv.second << "\n";
    }
    o << "TrackedRetVals\n";
    for (auto &kv : TrackedRetVals) {
      if (!kv.first->hasName() || !kv.second.isConstant()) {
        continue;
      }
      o << "\tret_val(" << kv.first->getName() << ")  --> " << kv.second
        << "\n";
    }
    // TODO: print TrackedMultipleRetVals
  }

private:
  // pushToWorkList - Helper for markConstant/markForcedConstant/markOverdefined
  void pushToWorkList(LatticeVal &IV, Value *V) {
    if (IV.isOverdefined())
      return OverdefinedInstWorkList.push_back(V);
    InstWorkList.push_back(V);
  }

  // markConstant - Make a value be marked as "constant".  If the value
  // is not already a constant, add it to the instruction work list so that
  // the users of the instruction are updated later.
  //
  void markConstant(LatticeVal &IV, Value *V, Constant *C) {
    if (!IV.markConstant(C))
      return;

    SCCP_LOG(errs() << "markConstant: " << *C << ": " << *V << '\n');
    pushToWorkList(IV, V);
  }

  void markConstant(Value *V, Constant *C) {
    assert(!V->getType()->isStructTy() && "structs should use mergeInValue");
    markConstant(ValueState[V], V, C);
  }

  void markForcedConstant(Value *V, Constant *C) {
    assert(!V->getType()->isStructTy() && "structs should use mergeInValue");

    LatticeVal &IV = ValueState[V];
    IV.markForcedConstant(C);
    SCCP_LOG(errs() << "markForcedConstant: " << *C << ": " << *V << '\n');
    pushToWorkList(IV, V);
  }

  // markOverdefined - Make a value be marked as "overdefined". If the
  // value is not already overdefined, add it to the overdefined instruction
  // work list so that the users of the instruction are updated later.
  void markOverdefined(LatticeVal &IV, Value *V) {
    if (!IV.markOverdefined())
      return;

    SCCP_LOG(errs() << "markOverdefined: ";
             if (auto *F = dyn_cast<Function>(V)) dbgs()
             << "Function '" << F->getName() << "'\n";
             else dbgs() << *V << '\n');
    // Only instructions go on the work list
    pushToWorkList(IV, V);
  }

  void mergeInValue(LatticeVal &IV, Value *V, LatticeVal MergeWithV) {
    if (IV.isOverdefined() || MergeWithV.isUnknown())
      return; // Noop.
    if (MergeWithV.isOverdefined())
      return markOverdefined(IV, V);
    if (IV.isUnknown())
      return markConstant(IV, V, MergeWithV.getConstant());
    if (IV.getConstant() != MergeWithV.getConstant()) {
      return markOverdefined(IV, V);
    }
  }

  void mergeInValue(Value *V, LatticeVal MergeWithV) {
    assert(!V->getType()->isStructTy() &&
           "non-structs should use markConstant");
    mergeInValue(ValueState[V], V, MergeWithV);
  }

  /// getValueState - Return the LatticeVal object that corresponds to the
  /// value.  This function handles the case when the value hasn't been seen yet
  /// by properly seeding constants etc.
  LatticeVal &getValueState(Value *V) {
    assert(!V->getType()->isStructTy() && "Should use getStructValueState");

    std::pair<DenseMap<Value *, LatticeVal>::iterator, bool> I =
        ValueState.insert(std::make_pair(V, LatticeVal()));
    LatticeVal &LV = I.first->second;

    if (!I.second) {
      return LV; // Common case, already in the map.
    }

    if (auto *C = dyn_cast<Constant>(V)) {
      // Undef values remain unknown.
      if (!isa<UndefValue>(V))
        LV.markConstant(C); // Constants are constant
    }

    // All others are underdefined by default.
    return LV;
  }

  /// getStructValueState - Return the LatticeVal object that corresponds to the
  /// value/field pair.  This function handles the case when the value hasn't
  /// been seen yet by properly seeding constants etc.
  LatticeVal &getStructValueState(Value *V, unsigned i) {
    assert(V->getType()->isStructTy() && "Should use getValueState");
    assert(i < cast<StructType>(V->getType())->getNumElements() &&
           "Invalid element #");

    std::pair<DenseMap<std::pair<Value *, unsigned>, LatticeVal>::iterator,
              bool>
        I = StructValueState.insert(
            std::make_pair(std::make_pair(V, i), LatticeVal()));
    LatticeVal &LV = I.first->second;

    if (!I.second)
      return LV; // Common case, already in the map.

    if (auto *C = dyn_cast<Constant>(V)) {
      Constant *Elt = C->getAggregateElement(i);

      if (!Elt)
        LV.markOverdefined(); // Unknown sort of constant.
      else if (isa<UndefValue>(Elt))
        ; // Undef values remain unknown.
      else
        LV.markConstant(Elt); // Constants are constant.
    }

    // All others are underdefined by default.
    return LV;
  }

  /// markEdgeExecutable - Mark a basic block as executable, adding it to the BB
  /// work list if it is not already executable.
  void markEdgeExecutable(BasicBlock *Source, BasicBlock *Dest) {
    if (!KnownFeasibleEdges.insert(Edge(Source, Dest)).second)
      return; // This edge is already known to be executable!

    if (!MarkBlockExecutable(Dest)) {
      // If the destination is already executable, we just made an *edge*
      // feasible that wasn't before.  Revisit the PHI nodes in the block
      // because they have potentially new operands.
      SCCP_LOG(errs() << "Marking Edge Executable: " << Source->getName()
                      << " -> " << Dest->getName() << '\n');

      PHINode *PN;
      for (BasicBlock::iterator I = Dest->begin(); (PN = dyn_cast<PHINode>(I));
           ++I)
        visitPHINode(*PN);
    }
  }

  // getFeasibleSuccessors - Return a vector of booleans to indicate which
  // successors are reachable from a given terminator instruction.
  //
  void getFeasibleSuccessors(Instruction &TI, SmallVectorImpl<bool> &Succs);

  // isEdgeFeasible - Return true if the control flow edge from the 'From' basic
  // block to the 'To' basic block is currently feasible.
  //
  bool isEdgeFeasible(BasicBlock *From, BasicBlock *To);

  // OperandChangedState - This method is invoked on all of the users of an
  // instruction that was just changed state somehow.  Based on this
  // information, we need to update the specified user of this instruction.
  //
  void OperandChangedState(Instruction *I) {
    if (BBExecutable.count(I->getParent())) // Inst is executable?
      visit(*I);
  }

private:
  friend class InstVisitor<SCCPSolver>;

  // visit implementations - Something changed in this instruction.  Either an
  // operand made a transition, or the instruction is newly executable.  Change
  // the value type of I to reflect these changes if appropriate.
  void visitPHINode(PHINode &I);

  // Terminators
  void visitReturnInst(ReturnInst &I);
  void visitTerminator(Instruction &TI);

  void visitCastInst(CastInst &I);
  void visitSelectInst(SelectInst &I);
  void visitBinaryOperator(Instruction &I);
  void visitCmpInst(CmpInst &I);
  void visitExtractValueInst(ExtractValueInst &EVI);
  void visitInsertValueInst(InsertValueInst &IVI);
  void visitCatchSwitchInst(CatchSwitchInst &CPI) {
    markOverdefined(&CPI);
    visitTerminator(CPI);
  }

  // Instructions that cannot be folded away.
  void visitStoreInst(StoreInst &I);
  void visitLoadInst(LoadInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitCallInst(CallInst &I) { visitCallSite(&I); }
  void visitInvokeInst(InvokeInst &II) {
    visitCallSite(&II);
    visitTerminator(II);
  }
  void visitCallSite(CallSite CS);
  void visitResumeInst(ResumeInst &I) { /*returns void*/
  }
  void visitUnreachableInst(UnreachableInst &I) { /*returns void*/
  }
  void visitFenceInst(FenceInst &I) { /*returns void*/
  }
  void visitInstruction(Instruction &I) {
    // All the instructions we don't do any special handling for just
    // go to overdefined.
    SCCP_LOG(errs() << "SCCP: Don't know how to handle: " << I << '\n');
    markOverdefined(&I);
  }
};

// getFeasibleSuccessors - Return a vector of booleans to indicate which
// successors are reachable from a given terminator instruction.
//
void SCCPSolver::getFeasibleSuccessors(Instruction &TI,
                                       SmallVectorImpl<bool> &Succs) {
  Succs.resize(TI.getNumSuccessors());
  if (auto *BI = dyn_cast<BranchInst>(&TI)) {
    if (BI->isUnconditional()) {
      Succs[0] = true;
      return;
    }

    LatticeVal BCValue = getValueState(BI->getCondition());
    ConstantInt *CI = BCValue.getConstantInt();
    if (!CI) {
      // Overdefined condition variables, and branches on unfoldable constant
      // conditions, mean the branch could go either way.
      if (!BCValue.isUnknown())
        Succs[0] = Succs[1] = true;
      return;
    }

    // Constant condition variables mean the branch can only go a single way.
    Succs[CI->isZero()] = true;
    return;
  }

  // Unwinding instructions successors are always executable.
  if (TI.isExceptionalTerminator()) {
    Succs.assign(TI.getNumSuccessors(), true);
    return;
  }

  if (auto *SI = dyn_cast<SwitchInst>(&TI)) {
    if (!SI->getNumCases()) {
      Succs[0] = true;
      return;
    }
    LatticeVal SCValue = getValueState(SI->getCondition());
    ConstantInt *CI = SCValue.getConstantInt();

    if (!CI) { // Overdefined or unknown condition?
      // All destinations are executable!
      if (!SCValue.isUnknown())
        Succs.assign(TI.getNumSuccessors(), true);
      return;
    }

    Succs[SI->findCaseValue(CI)->getSuccessorIndex()] = true;
    return;
  }

  // In case of indirect branch and its address is a blockaddress, we mark
  // the target as executable.
  if (auto *IBR = dyn_cast<IndirectBrInst>(&TI)) {
    // Casts are folded by visitCastInst.
    LatticeVal IBRValue = getValueState(IBR->getAddress());
    BlockAddress *Addr = IBRValue.getBlockAddress();
    if (!Addr) { // Overdefined or unknown condition?
      // All destinations are executable!
      if (!IBRValue.isUnknown())
        Succs.assign(TI.getNumSuccessors(), true);
      return;
    }

    BasicBlock *T = Addr->getBasicBlock();
    assert(Addr->getFunction() == T->getParent() &&
           "Block address of a different function ?");
    for (unsigned i = 0; i < IBR->getNumSuccessors(); ++i) {
      // This is the target.
      if (IBR->getDestination(i) == T) {
        Succs[i] = true;
        return;
      }
    }

    // If we didn't find our destination in the IBR successor list, then we
    // have undefined behavior. Its ok to assume no successor is executable.
    return;
  }

  SCCP_LOG(errs() << "Unknown terminator instruction: " << TI << '\n');
  llvm_unreachable("SCCP: Don't know how to handle this terminator!");
}

// isEdgeFeasible - Return true if the control flow edge from the 'From' basic
// block to the 'To' basic block is currently feasible.
//
bool SCCPSolver::isEdgeFeasible(BasicBlock *From, BasicBlock *To) {
  assert(BBExecutable.count(To) && "Dest should always be alive!");

  // Make sure the source basic block is executable!!
  if (!BBExecutable.count(From))
    return false;

  // Check to make sure this edge itself is actually feasible now.
  Instruction *TI = From->getTerminator();
  if (auto *BI = dyn_cast<BranchInst>(TI)) {
    if (BI->isUnconditional())
      return true;

    LatticeVal BCValue = getValueState(BI->getCondition());

    // Overdefined condition variables mean the branch could go either way,
    // undef conditions mean that neither edge is feasible yet.
    ConstantInt *CI = BCValue.getConstantInt();
    if (!CI)
      return !BCValue.isUnknown();

    // Constant condition variables mean the branch can only go a single way.
    return BI->getSuccessor(CI->isZero()) == To;
  }

  // Unwinding instructions successors are always executable.
  if (TI->isExceptionalTerminator())
    return true;

  if (auto *SI = dyn_cast<SwitchInst>(TI)) {
    if (SI->getNumCases() < 1)
      return true;

    LatticeVal SCValue = getValueState(SI->getCondition());
    ConstantInt *CI = SCValue.getConstantInt();

    if (!CI)
      return !SCValue.isUnknown();

    return SI->findCaseValue(CI)->getCaseSuccessor() == To;
  }

  // In case of indirect branch and its address is a blockaddress, we mark
  // the target as executable.
  if (auto *IBR = dyn_cast<IndirectBrInst>(TI)) {
    LatticeVal IBRValue = getValueState(IBR->getAddress());
    BlockAddress *Addr = IBRValue.getBlockAddress();

    if (!Addr)
      return !IBRValue.isUnknown();

    // At this point, the indirectbr is branching on a blockaddress.
    return Addr->getBasicBlock() == To;
  }

  SCCP_LOG(errs() << "Unknown terminator instruction: " << *TI << '\n');
  llvm_unreachable("SCCP: Don't know how to handle this terminator!");
}

// visit Implementations - Something changed in this instruction, either an
// operand made a transition, or the instruction is newly executable.  Change
// the value type of I to reflect these changes if appropriate.  This method
// makes sure to do the following actions:
//
// 1. If a phi node merges two constants in, and has conflicting value coming
//    from different branches, or if the PHI node merges in an overdefined
//    value, then the PHI node becomes overdefined.
// 2. If a phi node merges only constants in, and they all agree on value, the
//    PHI node becomes a constant value equal to that.
// 3. If V <- x (op) y && isConstant(x) && isConstant(y) V = Constant
// 4. If V <- x (op) y && (isOverdefined(x) || isOverdefined(y)) V = Overdefined
// 5. If V <- MEM or V <- CALL or V <- (unknown) then V = Overdefined
// 6. If a conditional branch has a value that is constant, make the selected
//    destination executable
// 7. If a conditional branch has a value that is overdefined, make all
//    successors executable.
//
void SCCPSolver::visitPHINode(PHINode &PN) {
  // If this PN returns a struct, just mark the result overdefined.
  // TODO: We could do a lot better than this if code actually uses this.
  if (PN.getType()->isStructTy())
    return markOverdefined(&PN);

  if (getValueState(&PN).isOverdefined())
    return; // Quick exit

  // Super-extra-high-degree PHI nodes are unlikely to ever be marked constant,
  // and slow us down a lot.  Just mark them overdefined.
  if (PN.getNumIncomingValues() > 64)
    return markOverdefined(&PN);

  // Look at all of the executable operands of the PHI node.  If any of them
  // are overdefined, the PHI becomes overdefined as well.  If they are all
  // constant, and they agree with each other, the PHI becomes the identical
  // constant.  If they are constant and don't agree, the PHI is overdefined.
  // If there are no executable operands, the PHI remains unknown.
  //
  Constant *OperandVal = nullptr;
  for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i) {
    LatticeVal IV = getValueState(PN.getIncomingValue(i));
    if (IV.isUnknown())
      continue; // Doesn't influence PHI node.

    if (!isEdgeFeasible(PN.getIncomingBlock(i), PN.getParent()))
      continue;

    if (IV.isOverdefined()) // PHI node becomes overdefined!
      return markOverdefined(&PN);

    if (!OperandVal) { // Grab the first value.
      OperandVal = IV.getConstant();
      continue;
    }

    // There is already a reachable operand.  If we conflict with it,
    // then the PHI node becomes overdefined.  If we agree with it, we
    // can continue on.

    // Check to see if there are two different constants merging, if so, the PHI
    // node is overdefined.
    if (IV.getConstant() != OperandVal)
      return markOverdefined(&PN);
  }

  // If we exited the loop, this means that the PHI node only has constant
  // arguments that agree with each other(and OperandVal is the constant) or
  // OperandVal is null because there are no defined incoming arguments.  If
  // this is the case, the PHI remains unknown.
  //
  if (OperandVal)
    markConstant(&PN, OperandVal); // Acquire operand value
}

void SCCPSolver::visitReturnInst(ReturnInst &I) {
  if (I.getNumOperands() == 0)
    return; // ret void

  Function *F = I.getParent()->getParent();
  Value *ResultOp = I.getOperand(0);

  // If we are tracking the return value of this function, merge it in.
  if (!TrackedRetVals.empty() && !ResultOp->getType()->isStructTy()) {
    DenseMap<Function *, LatticeVal>::iterator TFRVI = TrackedRetVals.find(F);
    if (TFRVI != TrackedRetVals.end()) {
      mergeInValue(TFRVI->second, F, getValueState(ResultOp));
      return;
    }
  }

  // Handle functions that return multiple values.
  if (!TrackedMultipleRetVals.empty()) {
    if (auto *STy = dyn_cast<StructType>(ResultOp->getType()))
      if (MRVFunctionsTracked.count(F))
        for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
          mergeInValue(TrackedMultipleRetVals[std::make_pair(F, i)], F,
                       getStructValueState(ResultOp, i));
  }
}

void SCCPSolver::visitTerminator(Instruction &TI) {
  SmallVector<bool, 16> SuccFeasible;
  getFeasibleSuccessors(TI, SuccFeasible);

  BasicBlock *BB = TI.getParent();

  // Mark all feasible successors executable.
  for (unsigned i = 0, e = SuccFeasible.size(); i != e; ++i)
    if (SuccFeasible[i])
      markEdgeExecutable(BB, TI.getSuccessor(i));
}

void SCCPSolver::visitCastInst(CastInst &I) {
  LatticeVal OpSt = getValueState(I.getOperand(0));
  if (OpSt.isOverdefined()) // Inherit overdefinedness of operand
    markOverdefined(&I);
  else if (OpSt.isConstant()) {
    // Fold the constant as we build.
    Constant *C = ConstantFoldCastOperand(I.getOpcode(), OpSt.getConstant(),
                                          I.getType(), DL);
    if (isa<UndefValue>(C))
      return;
    // Propagate constant value
    markConstant(&I, C);
  }
}

void SCCPSolver::visitExtractValueInst(ExtractValueInst &EVI) {
  // If this returns a struct, mark all elements over defined, we don't track
  // structs in structs.
  if (EVI.getType()->isStructTy())
    return markOverdefined(&EVI);

  // If this is extracting from more than one level of struct, we don't know.
  if (EVI.getNumIndices() != 1)
    return markOverdefined(&EVI);

  Value *AggVal = EVI.getAggregateOperand();
  if (AggVal->getType()->isStructTy()) {
    unsigned i = *EVI.idx_begin();
    LatticeVal EltVal = getStructValueState(AggVal, i);
    mergeInValue(getValueState(&EVI), &EVI, EltVal);
  } else {
    // Otherwise, must be extracting from an array.
    return markOverdefined(&EVI);
  }
}

void SCCPSolver::visitInsertValueInst(InsertValueInst &IVI) {
  auto *STy = dyn_cast<StructType>(IVI.getType());
  if (!STy)
    return markOverdefined(&IVI);

  // If this has more than one index, we can't handle it, drive all results to
  // undef.
  if (IVI.getNumIndices() != 1)
    return markOverdefined(&IVI);

  Value *Aggr = IVI.getAggregateOperand();
  unsigned Idx = *IVI.idx_begin();

  // Compute the result based on what we're inserting.
  for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
    // This passes through all values that aren't the inserted element.
    if (i != Idx) {
      LatticeVal EltVal = getStructValueState(Aggr, i);
      mergeInValue(getStructValueState(&IVI, i), &IVI, EltVal);
      continue;
    }

    Value *Val = IVI.getInsertedValueOperand();
    if (Val->getType()->isStructTy())
      // We don't track structs in structs.
      markOverdefined(getStructValueState(&IVI, i), &IVI);
    else {
      LatticeVal InVal = getValueState(Val);
      mergeInValue(getStructValueState(&IVI, i), &IVI, InVal);
    }
  }
}

void SCCPSolver::visitSelectInst(SelectInst &I) {
  // If this select returns a struct, just mark the result overdefined.
  // TODO: We could do a lot better than this if code actually uses this.
  if (I.getType()->isStructTy())
    return markOverdefined(&I);

  LatticeVal CondValue = getValueState(I.getCondition());
  if (CondValue.isUnknown())
    return;

  if (ConstantInt *CondCB = CondValue.getConstantInt()) {
    Value *OpVal = CondCB->isZero() ? I.getFalseValue() : I.getTrueValue();
    mergeInValue(&I, getValueState(OpVal));
    return;
  }

  // Otherwise, the condition is overdefined or a constant we can't evaluate.
  // See if we can produce something better than overdefined based on the T/F
  // value.
  LatticeVal TVal = getValueState(I.getTrueValue());
  LatticeVal FVal = getValueState(I.getFalseValue());

  // select ?, C, C -> C.
  if (TVal.isConstant() && FVal.isConstant() &&
      TVal.getConstant() == FVal.getConstant())
    return markConstant(&I, FVal.getConstant());

  if (TVal.isUnknown()) // select ?, undef, X -> X.
    return mergeInValue(&I, FVal);
  if (FVal.isUnknown()) // select ?, X, undef -> X.
    return mergeInValue(&I, TVal);
  markOverdefined(&I);
}

// Handle Binary Operators.
void SCCPSolver::visitBinaryOperator(Instruction &I) {
  LatticeVal V1State = getValueState(I.getOperand(0));
  LatticeVal V2State = getValueState(I.getOperand(1));

  LatticeVal &IV = ValueState[&I];
  if (IV.isOverdefined())
    return;

  if (V1State.isConstant() && V2State.isConstant()) {
    Constant *C = ConstantExpr::get(I.getOpcode(), V1State.getConstant(),
                                    V2State.getConstant());
    // X op Y -> undef.
    if (isa<UndefValue>(C))
      return;
    return markConstant(IV, &I, C);
  }

  // If something is undef, wait for it to resolve.
  if (!V1State.isOverdefined() && !V2State.isOverdefined())
    return;

  // Otherwise, one of our operands is overdefined.  Try to produce something
  // better than overdefined with some tricks.
  // If this is 0 / Y, it doesn't matter that the second operand is
  // overdefined, and we can replace it with zero.
  if (I.getOpcode() == Instruction::UDiv || I.getOpcode() == Instruction::SDiv)
    if (V1State.isConstant() && V1State.getConstant()->isNullValue())
      return markConstant(IV, &I, V1State.getConstant());

  // If this is:
  // -> AND/MUL with 0
  // -> OR with -1
  // it doesn't matter that the other operand is overdefined.
  if (I.getOpcode() == Instruction::And || I.getOpcode() == Instruction::Mul ||
      I.getOpcode() == Instruction::Or) {
    LatticeVal *NonOverdefVal = nullptr;
    if (!V1State.isOverdefined())
      NonOverdefVal = &V1State;
    else if (!V2State.isOverdefined())
      NonOverdefVal = &V2State;

    if (NonOverdefVal) {
      if (NonOverdefVal->isUnknown())
        return;

      if (I.getOpcode() == Instruction::And ||
          I.getOpcode() == Instruction::Mul) {
        // X and 0 = 0
        // X * 0 = 0
        if (NonOverdefVal->getConstant()->isNullValue())
          return markConstant(IV, &I, NonOverdefVal->getConstant());
      } else {
        // X or -1 = -1
        if (ConstantInt *CI = NonOverdefVal->getConstantInt())
          if (CI->isMinusOne())
            return markConstant(IV, &I, NonOverdefVal->getConstant());
      }
    }
  }

  markOverdefined(&I);
}

// Handle ICmpInst instruction.
void SCCPSolver::visitCmpInst(CmpInst &I) {
  LatticeVal V1State = getValueState(I.getOperand(0));
  LatticeVal V2State = getValueState(I.getOperand(1));

  LatticeVal &IV = ValueState[&I];
  if (IV.isOverdefined())
    return;

  if (V1State.isConstant() && V2State.isConstant()) {
    Constant *C = ConstantExpr::getCompare(
        I.getPredicate(), V1State.getConstant(), V2State.getConstant());
    if (isa<UndefValue>(C))
      return;
    return markConstant(IV, &I, C);
  }

  // If operands are still unknown, wait for it to resolve.
  if (!V1State.isOverdefined() && !V2State.isOverdefined())
    return;

  markOverdefined(&I);
}

// Handle getelementptr instructions.  If all operands are constants then we
// can turn this into a getelementptr ConstantExpr.
//
void SCCPSolver::visitGetElementPtrInst(GetElementPtrInst &I) {
  if (ValueState[&I].isOverdefined())
    return;

  SmallVector<Constant *, 8> Operands;
  Operands.reserve(I.getNumOperands());

  for (unsigned i = 0, e = I.getNumOperands(); i != e; ++i) {
    LatticeVal State = getValueState(I.getOperand(i));
    if (State.isUnknown())
      return; // Operands are not resolved yet.

    if (State.isOverdefined())
      return markOverdefined(&I);

    assert(State.isConstant() && "Unknown state!");
    Operands.push_back(State.getConstant());
  }

  Constant *Ptr = Operands[0];
  auto Indices = makeArrayRef(Operands.begin() + 1, Operands.end());
  Constant *C =
      ConstantExpr::getGetElementPtr(I.getSourceElementType(), Ptr, Indices);
  if (isa<UndefValue>(C))
    return;
  markConstant(&I, C);
}

void SCCPSolver::visitStoreInst(StoreInst &SI) {
  SCCP_LOG(errs() << "[SccpSolver]: Analyzing " << SI << "\n");

  // If this store is of a struct, ignore it.
  if (SI.getValueOperand()->getType()->isStructTy())
    return;

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(SI.getPointerOperand())) {
    DenseMap<GlobalVariable *, LatticeVal>::iterator I =
        TrackedGlobals.find(GV);
    if (I == TrackedGlobals.end()) {
      return;
    }
    if (I->second.isOverdefined()) {
      return;
    }
    // Get the value we are storing into the global, then merge it.
    mergeInValue(I->second, GV, getValueState(SI.getValueOperand()));
    if (I->second.isOverdefined())
      TrackedGlobals.erase(I); // No need to keep tracking this!
  }
}

// Handle load instructions.  If the operand is a constant pointer to a constant
// global, we can replace the load with the loaded constant value!
void SCCPSolver::visitLoadInst(LoadInst &I) {
  SCCP_LOG(errs() << "[SccpSolver]: Analyzing " << I << "\n");

  // If this load is of a struct, just mark the result overdefined.
  if (I.getType()->isStructTy())
    return markOverdefined(&I);

  LatticeVal PtrVal = getValueState(I.getPointerOperand());
  if (PtrVal.isUnknown()) {
    return; // The pointer is not resolved yet!
  }

  LatticeVal &IV = ValueState[&I];
  if (IV.isOverdefined()) {
    return;
  }

  if (!PtrVal.isConstant() || I.isVolatile()) {
    return markOverdefined(IV, &I);
  }

  Constant *Ptr = PtrVal.getConstant();

  // load null is undefined.
  if (isa<ConstantPointerNull>(Ptr) && I.getPointerAddressSpace() == 0)
    return;

  // Transform load (constant global) into the value loaded.
  if (auto *GV = dyn_cast<GlobalVariable>(Ptr)) {
    if (!TrackedGlobals.empty()) {
      // If we are tracking this global, merge in the known value for it.
      DenseMap<GlobalVariable *, LatticeVal>::iterator It =
          TrackedGlobals.find(GV);
      if (It != TrackedGlobals.end()) {
        mergeInValue(IV, &I, It->second);
        return;
      }
    }
  }

  // Transform load from a constant into a constant if possible.
  if (Constant *C = ConstantFoldLoadFromConstPtr(Ptr, I.getType(), DL)) {
    if (isa<UndefValue>(C))
      return;
    return markConstant(IV, &I, C);
  }

  // Otherwise we cannot say for certain what value this load will produce.
  // Bail out.
  markOverdefined(IV, &I);
}

void SCCPSolver::visitCallSite(CallSite CS) {
  Function *F = CS.getCalledFunction();
  Instruction *I = CS.getInstruction();

  // The common case is that we aren't tracking the callee, either because we
  // are not doing interprocedural analysis or the callee is indirect, or is
  // external.  Handle these cases first.
  if (!F || F->isDeclaration()) {
  CallOverdefined:
    // Void return and not tracking callee, just bail.
    if (I->getType()->isVoidTy())
      return;

    // Otherwise, if we have a single return value case, and if the function is
    // a declaration, maybe we can constant fold it.
    if (F && F->isDeclaration() && !I->getType()->isStructTy() &&
        canConstantFoldCallTo(cast<CallBase>(CS.getInstruction()), F)) {

      SmallVector<Constant *, 8> Operands;
      for (CallSite::arg_iterator AI = CS.arg_begin(), E = CS.arg_end();
           AI != E; ++AI) {
        LatticeVal State = getValueState(*AI);

        if (State.isUnknown())
          return; // Operands are not resolved yet.
        if (State.isOverdefined())
          return markOverdefined(I);
        assert(State.isConstant() && "Unknown state!");
        Operands.push_back(State.getConstant());
      }

      if (getValueState(I).isOverdefined())
        return;

      // If we can constant fold this, mark the result of the call as a
      // constant.
      if (Constant *C = ConstantFoldCall(cast<CallBase>(CS.getInstruction()), F,
                                         Operands, &(TLIWrapper->getTLI(*F)))) {
        // call -> undef.
        if (isa<UndefValue>(C))
          return;
        return markConstant(I, C);
      }
    }

    // Otherwise, we don't know anything about this call, mark it overdefined.
    return markOverdefined(I);
  }

  // If this is a local function that doesn't have its address taken, mark its
  // entry block executable and merge in the actual arguments to the call into
  // the formal arguments of the function.
  if (!TrackingIncomingArguments.empty() &&
      TrackingIncomingArguments.count(F)) {
    MarkBlockExecutable(&F->front());

    // Propagate information from this call site into the callee.
    CallSite::arg_iterator CAI = CS.arg_begin();
    for (Function::arg_iterator AI = F->arg_begin(), E = F->arg_end(); AI != E;
         ++AI, ++CAI) {
      // If this argument is byval, and if the function is not readonly, there
      // will be an implicit copy formed of the input aggregate.
      if (AI->hasByValAttr() && !F->onlyReadsMemory()) {
        markOverdefined(&*AI);
        continue;
      }

      if (auto *STy = dyn_cast<StructType>(AI->getType())) {
        for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
          LatticeVal CallArg = getStructValueState(*CAI, i);
          mergeInValue(getStructValueState(&*AI, i), &*AI, CallArg);
        }
      } else {
        mergeInValue(&*AI, getValueState(*CAI));
      }
    }
  }

  // If this is a single/zero retval case, see if we're tracking the function.
  if (auto *STy = dyn_cast<StructType>(F->getReturnType())) {
    if (!MRVFunctionsTracked.count(F))
      goto CallOverdefined; // Not tracking this callee.

    // If we are tracking this callee, propagate the result of the function
    // into this call site.
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
      mergeInValue(getStructValueState(I, i), I,
                   TrackedMultipleRetVals[std::make_pair(F, i)]);
  } else {
    DenseMap<Function *, LatticeVal>::iterator TFRVI = TrackedRetVals.find(F);
    if (TFRVI == TrackedRetVals.end())
      goto CallOverdefined; // Not tracking this callee.

    // If so, propagate the return value of the callee into this call result.
    mergeInValue(I, TFRVI->second);
  }
}

void SCCPSolver::Solve() {
  // Process the work lists until they are empty!
  while (!BBWorkList.empty() || !InstWorkList.empty() ||
         !OverdefinedInstWorkList.empty()) {
    // Process the overdefined instruction's work list first, which drives other
    // things to overdefined more quickly.
    while (!OverdefinedInstWorkList.empty()) {
      Value *I = OverdefinedInstWorkList.pop_back_val();

      SCCP_LOG(errs() << "\nPopped off OI-WL: " << *I << '\n');
      // "I" got into the work list because it either made the transition from
      // bottom to constant, or to overdefined.
      //
      // Anything on this worklist that is overdefined need not be visited
      // since all of its users will have already been marked as overdefined
      // Update all of the users of this instruction's value.
      //
      for (User *U : I->users())
        if (auto *UI = dyn_cast<Instruction>(U))
          OperandChangedState(UI);
    }

    // Process the instruction work list.
    while (!InstWorkList.empty()) {
      Value *I = InstWorkList.pop_back_val();

      SCCP_LOG(errs() << "\nPopped off I-WL: " << *I << '\n');

      // "I" got into the work list because it made the transition from undef to
      // constant.
      //
      // Anything on this worklist that is overdefined need not be visited
      // since all of its users will have already been marked as overdefined.
      // Update all of the users of this instruction's value.
      //
      if (I->getType()->isStructTy() || !getValueState(I).isOverdefined())
        for (User *U : I->users())
          if (auto *UI = dyn_cast<Instruction>(U))
            OperandChangedState(UI);
    }

    // Process the basic block work list.
    while (!BBWorkList.empty()) {
      BasicBlock *BB = BBWorkList.back();
      BBWorkList.pop_back();

      SCCP_LOG(errs() << "\nPopped off BBWL: " << *BB << '\n');

      // Notify all instructions in this basic block that they are newly
      // executable.
      visit(BB);
    }
  }
}

/// ResolvedUndefsIn - While solving the dataflow for a function, we assume
/// that branches on undef values cannot reach any of their successors.
/// However, this is not a safe assumption.  After we solve dataflow, this
/// method should be use to handle this.  If this returns true, the solver
/// should be rerun.
///
/// This method handles this by finding an unresolved branch and marking it one
/// of the edges from the block as being feasible, even though the condition
/// doesn't say it would otherwise be.  This allows SCCP to find the rest of the
/// CFG and only slightly pessimizes the analysis results (by marking one,
/// potentially infeasible, edge feasible).  This cannot usefully modify the
/// constraints on the condition of the branch, as that would impact other users
/// of the value.
///
/// This scan also checks for values that use undefs, whose results are actually
/// defined.  For example, 'zext i8 undef to i32' should produce all zeros
/// conservatively, as "(zext i8 X -> i32) & 0xFF00" must always return zero,
/// even if X isn't defined.
bool SCCPSolver::ResolvedUndefsIn(Function &F) {
  for (BasicBlock &BB : F) {
    if (!BBExecutable.count(&BB))
      continue;

    for (Instruction &I : BB) {
      // Look for instructions which produce undef values.
      if (I.getType()->isVoidTy())
        continue;

      if (auto *STy = dyn_cast<StructType>(I.getType())) {
        // Only a few things that can be structs matter for undef.

        // Tracked calls must never be marked overdefined in ResolvedUndefsIn.
        if (CallSite CS = CallSite(&I))
          if (Function *F = CS.getCalledFunction())
            if (MRVFunctionsTracked.count(F))
              continue;

        // extractvalue and insertvalue don't need to be marked; they are
        // tracked as precisely as their operands.
        if (isa<ExtractValueInst>(I) || isa<InsertValueInst>(I))
          continue;

        // Send the results of everything else to overdefined.  We could be
        // more precise than this but it isn't worth bothering.
        for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
          LatticeVal &LV = getStructValueState(&I, i);
          if (LV.isUnknown())
            markOverdefined(LV, &I);
        }
        continue;
      }

      LatticeVal &LV = getValueState(&I);
      if (!LV.isUnknown())
        continue;

      // extractvalue is safe; check here because the argument is a struct.
      if (isa<ExtractValueInst>(I))
        continue;

      // Compute the operand LatticeVals, for convenience below.
      // Anything taking a struct is conservatively assumed to require
      // overdefined markings.
      if (I.getOperand(0)->getType()->isStructTy()) {
        markOverdefined(&I);
        return true;
      }
      LatticeVal Op0LV = getValueState(I.getOperand(0));
      LatticeVal Op1LV;
      if (I.getNumOperands() == 2) {
        if (I.getOperand(1)->getType()->isStructTy()) {
          markOverdefined(&I);
          return true;
        }

        Op1LV = getValueState(I.getOperand(1));
      }
      // If this is an instructions whose result is defined even if the input is
      // not fully defined, propagate the information.
      Type *ITy = I.getType();
      switch (I.getOpcode()) {
      case Instruction::Add:
      case Instruction::Sub:
      case Instruction::Trunc:
      case Instruction::FPTrunc:
      case Instruction::BitCast:
        break; // Any undef -> undef
      case Instruction::FSub:
      case Instruction::FAdd:
      case Instruction::FMul:
      case Instruction::FDiv:
      case Instruction::FRem:
        // Floating-point binary operation: be conservative.
        if (Op0LV.isUnknown() && Op1LV.isUnknown())
          markForcedConstant(&I, Constant::getNullValue(ITy));
        else
          markOverdefined(&I);
        return true;
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::FPToUI:
      case Instruction::FPToSI:
      case Instruction::FPExt:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      case Instruction::SIToFP:
      case Instruction::UIToFP:
        // undef -> 0; some outputs are impossible
        markForcedConstant(&I, Constant::getNullValue(ITy));
        return true;
      case Instruction::Mul:
      case Instruction::And:
        // Both operands undef -> undef
        if (Op0LV.isUnknown() && Op1LV.isUnknown())
          break;
        // undef * X -> 0.   X could be zero.
        // undef & X -> 0.   X could be zero.
        markForcedConstant(&I, Constant::getNullValue(ITy));
        return true;

      case Instruction::Or:
        // Both operands undef -> undef
        if (Op0LV.isUnknown() && Op1LV.isUnknown())
          break;
        // undef | X -> -1.   X could be -1.
        markForcedConstant(&I, Constant::getAllOnesValue(ITy));
        return true;

      case Instruction::Xor:
        // undef ^ undef -> 0; strictly speaking, this is not strictly
        // necessary, but we try to be nice to people who expect this
        // behavior in simple cases
        if (Op0LV.isUnknown() && Op1LV.isUnknown()) {
          markForcedConstant(&I, Constant::getNullValue(ITy));
          return true;
        }
        // undef ^ X -> undef
        break;

      case Instruction::SDiv:
      case Instruction::UDiv:
      case Instruction::SRem:
      case Instruction::URem:
        // X / undef -> undef.  No change.
        // X % undef -> undef.  No change.
        if (Op1LV.isUnknown())
          break;

        // X / 0 -> undef.  No change.
        // X % 0 -> undef.  No change.
        if (Op1LV.isConstant() && Op1LV.getConstant()->isZeroValue())
          break;

        // undef / X -> 0.   X could be maxint.
        // undef % X -> 0.   X could be 1.
        markForcedConstant(&I, Constant::getNullValue(ITy));
        return true;

      case Instruction::AShr:
        // X >>a undef -> undef.
        if (Op1LV.isUnknown())
          break;

        // Shifting by the bitwidth or more is undefined.
        if (Op1LV.isConstant()) {
          if (auto *ShiftAmt = Op1LV.getConstantInt())
            if (ShiftAmt->getLimitedValue() >=
                ShiftAmt->getType()->getScalarSizeInBits())
              break;
        }

        // undef >>a X -> 0
        markForcedConstant(&I, Constant::getNullValue(ITy));
        return true;
      case Instruction::LShr:
      case Instruction::Shl:
        // X << undef -> undef.
        // X >> undef -> undef.
        if (Op1LV.isUnknown())
          break;

        // Shifting by the bitwidth or more is undefined.
        if (Op1LV.isConstant()) {
          if (auto *ShiftAmt = Op1LV.getConstantInt())
            if (ShiftAmt->getLimitedValue() >=
                ShiftAmt->getType()->getScalarSizeInBits())
              break;
        }

        // undef << X -> 0
        // undef >> X -> 0
        markForcedConstant(&I, Constant::getNullValue(ITy));
        return true;
      case Instruction::Select:
        Op1LV = getValueState(I.getOperand(1));
        // undef ? X : Y  -> X or Y.  There could be commonality between X/Y.
        if (Op0LV.isUnknown()) {
          if (!Op1LV.isConstant()) // Pick the constant one if there is any.
            Op1LV = getValueState(I.getOperand(2));
        } else if (Op1LV.isUnknown()) {
          // c ? undef : undef -> undef.  No change.
          Op1LV = getValueState(I.getOperand(2));
          if (Op1LV.isUnknown())
            break;
          // Otherwise, c ? undef : x -> x.
        } else {
          // Leave Op1LV as Operand(1)'s LatticeValue.
        }

        if (Op1LV.isConstant())
          markForcedConstant(&I, Op1LV.getConstant());
        else
          markOverdefined(&I);
        return true;
      case Instruction::Load:
        // A load here means one of two things: a load of undef from a global,
        // a load from an unknown pointer.  Either way, having it return undef
        // is okay.
        break;
      case Instruction::ICmp:
        // X == undef -> undef.  Other comparisons get more complicated.
        if (cast<ICmpInst>(&I)->isEquality())
          break;
        markOverdefined(&I);
        return true;
      case Instruction::Call:
      case Instruction::Invoke: {
        // There are two reasons a call can have an undef result
        // 1. It could be tracked.
        // 2. It could be constant-foldable.
        // Because of the way we solve return values, tracked calls must
        // never be marked overdefined in ResolvedUndefsIn.
        if (Function *F = CallSite(&I).getCalledFunction())
          if (TrackedRetVals.count(F))
            break;

        // If the call is constant-foldable, we mark it overdefined because
        // we do not know what return values are valid.
        markOverdefined(&I);
        return true;
      }
      default:
        // If we don't know what should happen here, conservatively mark it
        // overdefined.
        markOverdefined(&I);
        return true;
      }
    }

    // Check to see if we have a branch or switch on an undefined value.  If so
    // we force the branch to go one way or the other to make the successor
    // values live.  It doesn't really matter which way we force it.
    Instruction *TI = BB.getTerminator();
    if (auto *BI = dyn_cast<BranchInst>(TI)) {
      if (!BI->isConditional())
        continue;
      if (!getValueState(BI->getCondition()).isUnknown())
        continue;

      // If the input to SCCP is actually branch on undef, fix the undef to
      // false.
      if (isa<UndefValue>(BI->getCondition())) {
        BI->setCondition(ConstantInt::getFalse(BI->getContext()));
        markEdgeExecutable(&BB, TI->getSuccessor(1));
        return true;
      }

      // Otherwise, it is a branch on a symbolic value which is currently
      // considered to be undef.  Handle this by forcing the input value to the
      // branch to false.
      markForcedConstant(BI->getCondition(),
                         ConstantInt::getFalse(TI->getContext()));
      return true;
    }

    if (auto *IBR = dyn_cast<IndirectBrInst>(TI)) {
      // Indirect branch with no successor ?. Its ok to assume it branches
      // to no target.
      if (IBR->getNumSuccessors() < 1)
        continue;

      if (!getValueState(IBR->getAddress()).isUnknown())
        continue;

      // If the input to SCCP is actually branch on undef, fix the undef to
      // the first successor of the indirect branch.
      if (isa<UndefValue>(IBR->getAddress())) {
        IBR->setAddress(BlockAddress::get(IBR->getSuccessor(0)));
        markEdgeExecutable(&BB, IBR->getSuccessor(0));
        return true;
      }

      // Otherwise, it is a branch on a symbolic value which is currently
      // considered to be undef.  Handle this by forcing the input value to the
      // branch to the first successor.
      markForcedConstant(IBR->getAddress(),
                         BlockAddress::get(IBR->getSuccessor(0)));
      return true;
    }

    if (auto *SI = dyn_cast<SwitchInst>(TI)) {
      if (!SI->getNumCases() || !getValueState(SI->getCondition()).isUnknown())
        continue;

      // If the input to SCCP is actually switch on undef, fix the undef to
      // the first constant.
      if (isa<UndefValue>(SI->getCondition())) {
        SI->setCondition(SI->case_begin()->getCaseValue());
        markEdgeExecutable(&BB, SI->case_begin()->getCaseSuccessor());
        return true;
      }

      markForcedConstant(SI->getCondition(), SI->case_begin()->getCaseValue());

      return true;
    }
  }

  return false;
}

static bool tryToReplaceWithConstant(SCCPSolver &Solver, Value *V) {
  Constant *Const = nullptr;
  if (V->getType()->isStructTy()) {
    std::vector<LatticeVal> IVs = Solver.getStructLatticeValueFor(V);
    if (any_of(IVs, [](const LatticeVal &LV) { return LV.isOverdefined(); }))
      return false;
    std::vector<Constant *> ConstVals;
    auto *ST = dyn_cast<StructType>(V->getType());
    for (unsigned i = 0, e = ST->getNumElements(); i != e; ++i) {
      LatticeVal V = IVs[i];
      ConstVals.push_back(V.isConstant()
                              ? V.getConstant()
                              : UndefValue::get(ST->getElementType(i)));
    }
    Const = ConstantStruct::get(ST, ConstVals);
  } else {
    LatticeVal IV = Solver.getLatticeValueFor(V);
    if (IV.isOverdefined()) {
      return false;
    }
    Const = IV.isConstant() ? IV.getConstant() : UndefValue::get(V->getType());
  }
  assert(Const && "Constant is nullptr here!");

  SCCP_LOG(if (!V->getName().startswith("mem.ssa")) {
    errs() << "Replace all uses of " << *V << " with Constant: " << *Const
           << "\n";
  });

  // Replaces all of the uses of a variable with uses of the constant.
  V->replaceAllUsesWith(Const);
  return true;
}

static bool AddressIsTaken(const GlobalValue *GV) {
  // Delete any dead constantexpr klingons.
  GV->removeDeadConstantUsers();

  for (const Use &U : GV->uses()) {
    const User *UR = U.getUser();
    if (const auto *SI = dyn_cast<StoreInst>(UR)) {
      if (SI->getOperand(0) == GV || SI->isVolatile())
        return true; // Storing addr of GV.
    } else if (isa<InvokeInst>(UR) || isa<CallInst>(UR)) {
      // Make sure we are calling the function, not passing the address.
      ImmutableCallSite CS(cast<Instruction>(UR));
      if (!CS.isCallee(&U))
        return true;
    } else if (const auto *LI = dyn_cast<LoadInst>(UR)) {
      if (LI->isVolatile())
        return true;
    } else if (isa<BlockAddress>(UR)) {
      // blockaddress doesn't take the address of the function, it takes addr
      // of label.
    } else {
      return true;
    }
  }
  return false;
}

static void findReturnsToZap(Function &F,
                             SmallPtrSet<Function *, 32> &AddressTakenFunctions,
                             SmallVector<ReturnInst *, 8> &ReturnsToZap) {
  // We can only do this if we know that nothing else can call the function.
  if (!F.hasLocalLinkage() || AddressTakenFunctions.count(&F))
    return;

  for (BasicBlock &BB : F)
    if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
      if (!isa<UndefValue>(RI->getOperand(0)))
        ReturnsToZap.push_back(RI);
}

// runIPSCCP() - Run the interprocedural Sparse Conditional Constant
// Propagation algorithm, and return true if the function was
// modified.
//
static bool runIPSCCP(Module &M, SCCPSolver &Solver) {
  // AddressTakenFunctions - This set keeps track of the address-taken functions
  // that are in the input.  As IPSCCP runs through and simplifies code,
  // functions that were address taken can end up losing their
  // address-taken-ness.  Because of this, we keep track of their addresses from
  // the first pass so we can use them for the later simplification pass.
  SmallPtrSet<Function *, 32> AddressTakenFunctions;

  errs() << "IPSCCP analysis started ... \n";

  // Loop over all functions, marking arguments to those with their addresses
  // taken or that are external as overdefined.
  //
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // If this is an exact definition of this function, then we can propagate
    // information about its result into callsites of it.
    // Don't touch naked functions. They may contain asm returning a
    // value we don't see, so we may end up interprocedurally propagating
    // the return value incorrectly.
    if (F.hasExactDefinition() && !F.hasFnAttribute(Attribute::Naked))
      Solver.AddTrackedFunction(&F);

    // If this function only has direct calls that we can see, we can track its
    // arguments and return value aggressively, and can assume it is not called
    // unless we see evidence to the contrary.
    if (F.hasLocalLinkage()) {
      if (F.hasAddressTaken()) {
        AddressTakenFunctions.insert(&F);
      } else {
        Solver.AddArgumentTrackedFunction(&F);
        continue;
      }
    }

    // Assume the function is called.
    Solver.MarkBlockExecutable(&F.front());

    // Assume nothing about the incoming arguments.
    for (Argument &AI : F.args())
      Solver.markOverdefined(&AI);
  }

  // Loop over global variables.  We inform the solver about any
  // internal global variables that do not have their 'addresses
  // taken'.  If they don't have their addresses taken, we can
  // propagate constants through them.
  for (GlobalVariable &G : M.globals()) {
    if (!G.isConstant() && G.hasLocalLinkage() &&
        G.hasDefinitiveInitializer() && !AddressIsTaken(&G)) {
      Solver.TrackValueOfGlobalVariable(&G);
      SCCP_LOG(errs() << "[Sccp]: " << G.getName() << " is tracked\n");
    } else {
      SCCP_LOG(errs() << "[Sccp]: " << G.getName()
                      << " NOT TRACKABLE because: \n";
               if (G.isConstant()) {
                 errs() << "\t it is constant\n";
               } if (!G.hasLocalLinkage()) {
                 errs() << "\t it does not have local linkage: linkage id "
                        << G.getLinkage() << "\n";
               } if (!G.hasDefinitiveInitializer()) {
                 errs() << "\t it does not have definite initializer\n";
               } if (AddressIsTaken(&G)) {
                 errs() << "\t its address may be taken\n";
               });
    }
  }

  errs() << "IPSCCP resolution of undefs started.\n";
  // Solve for constants.
  bool ResolvedUndefs = true;
  while (ResolvedUndefs) {
    Solver.Solve();

    SCCP_LOG(errs() << "RESOLVING UNDEFS\n");
    ResolvedUndefs = false;
    for (Function &F : M) {
      ResolvedUndefs |= Solver.ResolvedUndefsIn(F);
    }
  }
  errs() << "IPSCCP resolution of undefs finished.\n";

  errs() << "IPSCCP analysis finished.\n";

  SCCP_LOG(errs() << "============ Begin IPSCCP ============== \n";
           Solver.printValueState(errs());
           errs() << "============ End IPSCCP ================ \n";);

  bool MadeChanges = false;

  errs() << "IPSCCP constant folding transformation started ...\n";

  // Iterate over all of the instructions in the module, replacing them with
  // constants if we have found them to be of constant values.
  //
  SmallVector<BasicBlock *, 512> BlocksToErase;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    if (Solver.isBlockExecutable(&F.front())) {
      for (Function::arg_iterator AI = F.arg_begin(), E = F.arg_end(); AI != E;
           ++AI)
        if (!AI->use_empty() && tryToReplaceWithConstant(Solver, &*AI)) {
          ++IPNumArgsElimed;
        }
    }

    for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
      if (!Solver.isBlockExecutable(&*BB)) {
        LLVM_DEBUG(dbgs() << "  BasicBlock Dead:" << *BB);

        ++IPNumDeadBlocks;
        IPNumInstRemoved +=
            changeToUnreachable(BB->getFirstNonPHI(), /*UseLLVMTrap=*/false);

        MadeChanges = true;

        if (&*BB != &F.front())
          BlocksToErase.push_back(&*BB);
        continue;
      }

      for (BasicBlock::iterator BI = BB->begin(), E = BB->end(); BI != E;) {
        Instruction *Inst = &*BI++;
        if (Inst->getType()->isVoidTy())
          continue;
        if (tryToReplaceWithConstant(Solver, Inst)) {
          if (isInstructionTriviallyDead(Inst)) {
            Inst->eraseFromParent();
          }
          // Hey, we just changed something!
          MadeChanges = true;
          ++IPNumInstRemoved;
        }
      }
    }

    // Now that all instructions in the function are constant folded, erase dead
    // blocks, because we can now use ConstantFoldTerminator to get rid of
    // in-edges.
    for (unsigned i = 0, e = BlocksToErase.size(); i != e; ++i) {
      // If there are any PHI nodes in this successor, drop entries for BB now.
      BasicBlock *DeadBB = BlocksToErase[i];
      for (Value::user_iterator UI = DeadBB->user_begin(),
                                UE = DeadBB->user_end();
           UI != UE;) {
        // Grab the user and then increment the iterator early, as the user
        // will be deleted. Step past all adjacent uses from the same user.
        auto *I = dyn_cast<Instruction>(*UI);
        do {
          ++UI;
        } while (UI != UE && *UI == I);

        // Ignore blockaddress users; BasicBlock's dtor will handle them.
        if (!I)
          continue;

        bool Folded = ConstantFoldTerminator(I->getParent());
        assert(Folded &&
               "Expect TermInst on constantint or blockaddress to be folded");
        (void)Folded;
      }

      // Finally, delete the basic block.
      F.getBasicBlockList().erase(DeadBB);
    }
    BlocksToErase.clear();
  }

  // If we inferred constant or undef return values for a function, we replaced
  // all call uses with the inferred value.  This means we don't need to bother
  // actually returning anything from the function.  Replace all return
  // instructions with return undef.
  //
  // Do this in two stages: first identify the functions we should process, then
  // actually zap their returns.  This is important because we can only do this
  // if the address of the function isn't taken.  In cases where a return is the
  // last use of a function, the order of processing functions would affect
  // whether other functions are optimizable.
  SmallVector<ReturnInst *, 8> ReturnsToZap;

  const DenseMap<Function *, LatticeVal> &RV = Solver.getTrackedRetVals();
  for (const auto &I : RV) {
    Function *F = I.first;
    if (I.second.isOverdefined() || F->getReturnType()->isVoidTy())
      continue;
    findReturnsToZap(*F, AddressTakenFunctions, ReturnsToZap);
  }

  for (const auto &F : Solver.getMRVFunctionsTracked()) {
    assert(F->getReturnType()->isStructTy() &&
           "The return type should be a struct");
    StructType *STy = cast<StructType>(F->getReturnType());
    if (Solver.isStructLatticeConstant(F, STy))
      findReturnsToZap(*F, AddressTakenFunctions, ReturnsToZap);
  }

  // Zap all returns which we've identified as zap to change.
  for (unsigned i = 0, e = ReturnsToZap.size(); i != e; ++i) {
    Function *F = ReturnsToZap[i]->getParent()->getParent();
    ReturnsToZap[i]->setOperand(0, UndefValue::get(F->getReturnType()));
  }

  // If we inferred constant or undef values for globals variables, we can
  // delete the global and any stores that remain to it.
  const DenseMap<GlobalVariable *, LatticeVal> &TG = Solver.getTrackedGlobals();
  for (DenseMap<GlobalVariable *, LatticeVal>::const_iterator I = TG.begin(),
                                                              E = TG.end();
       I != E; ++I) {

    GlobalVariable *GV = I->first;

    assert(!I->second.isOverdefined() &&
           "Overdefined values should have been taken out of the map!");
    LLVM_DEBUG(dbgs() << "Found that GV '" << GV->getName()
                      << "' is constant!\n");

#if 0
    // Original code: assume that at this point all users of a
    // constant global must be StoreInst.
    while (!GV->use_empty()) {
      StoreInst *SI = cast<StoreInst>(GV->user_back());
      SI->eraseFromParent();
    }
    M.getGlobalList().erase(GV);
    ++IPNumGlobalConst;
#else
    // FIXME: we saw cases (e.g., openssh) where we have "%x = load
    // %gv" for which we know that %gv is a constant of a non-zero
    // value. However, that instruction is not analyzed until
    // ResolvedUndefsIn is called because is defined in some basic
    // block whose reachability depends on some undef value. For some
    // reason, ResolvedUndefsIn marks as as forceconstant %x and
    // assigns to it the default value zero. That value assigned to %x
    // is merged with the content of %gv (a non-zero value) and
    // therefore the final constant value for %x% is overdefined
    // (i.e., top). This makes the instruction "%x = load %gv" to
    // stay. So we reach this point having a constant global variable
    // but some of its users is not StoreInst.
    //
    // ResolvedUndefsIn marks %x as forceconstant because it's marked
    // as unknown and it's used as branch conditional.
    //
    // JN: I don't know how to solve properly this problem for
    // now. The problem, as I described it, might happen also with the
    // original SCCP but I seriously doubt that it would happen so I'm
    // missing something. Instead, I try to be conservative and remove
    // the global only if all its users are StoreInst.

    bool CanGvBeDeleted = true;
    std::vector<StoreInst *> UsersToRemove;
    for (auto u : GV->users()) {
      if (StoreInst *SI = dyn_cast<StoreInst>(u)) {
        UsersToRemove.push_back(SI);
      } else {
        CanGvBeDeleted = false;
      }
    }

    for (StoreInst *SI : UsersToRemove) {
      SI->eraseFromParent();
    }

    if (CanGvBeDeleted) {
      M.getGlobalList().erase(GV);
      ++IPNumGlobalConst;
    }
#endif
  }
  errs() << "IPSCCP constant folding transformation finished.\n";

  return MadeChanges;
}

// Similar to SCCPLegacyPass in llvm 5.0: This class uses the SCCPSolver to
// implement a per-function Sparse Conditional Constant Propagator.
class IPSCCPPass : public ModulePass {
private:
  bool skipFunction(const Function &F) const {

    auto getDescription = [](const Function &F) {
      return "module (" + F.getParent()->getName().str() + ")";
    };
    OptPassGate &Gate = F.getContext().getOptPassGate();
    if (Gate.isEnabled() && !Gate.shouldRunPass(this, getDescription(F)))
      return true;

    // if (F.hasOptNone()) {
    //   errs() << "Skipping pass '" << getPassName() << "' on function "
    // 	     << F.getName() << "\n";
    //   return true;
    // }
    return false;
  }

  const DataLayout *DL;
  TargetLibraryInfoWrapperPass *TLIWrapper;

public:
  static char ID; // Pass identification, replacement for typeid

  IPSCCPPass() : ModulePass(ID), DL(nullptr), TLIWrapper(nullptr) {}

  bool runOnModule(Module &M) override {
    if (skipModule(M)) {
      return false;
    }

    SCCP_LOG(errs() << "Running IPSCCP pass ... \n");

    DL = &M.getDataLayout();
    TLIWrapper = &getAnalysis<TargetLibraryInfoWrapperPass>();

    SCCPSolver Solver(*DL, TLIWrapper);
    return runIPSCCP(M, Solver);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addPreserved<GlobalsAAWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<CallGraphWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
  }

  StringRef getPassName() const override {
    return "IPSCCP customized for OCCAM";
  }
};

} // end namespace transforms
} // end namespace previrt

char previrt::transforms::IPSCCPPass::ID = 0;
static RegisterPass<previrt::transforms::IPSCCPPass>
    X("Pipsccp", "Inter-procedural sparse conditional constant propagation "
                 "customized for OCCAM",
      false, false);
