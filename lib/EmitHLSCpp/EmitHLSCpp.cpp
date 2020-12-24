//===------------------------------------------------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//

#include "EmitHLSCpp.h"
#include "Dialect/HLSCpp/Visitor.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/AffineExprVisitor.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/Translation.h"
#include "llvm/Support/raw_ostream.h"

using namespace std;
using namespace mlir;
using namespace scalehls;
using namespace hlscpp;

//===----------------------------------------------------------------------===//
// Some Base Classes
//===----------------------------------------------------------------------===//

namespace {
/// This class maintains the mutable state that cross-cuts and is shared by the
/// various emitters.
class HLSCppEmitterState {
public:
  explicit HLSCppEmitterState(raw_ostream &os) : os(os) {}

  // The stream to emit to.
  raw_ostream &os;

  bool encounteredError = false;
  unsigned currentIndent = 0;

  // This table contains all declared values.
  DenseMap<Value, SmallString<8>> nameTable;

private:
  HLSCppEmitterState(const HLSCppEmitterState &) = delete;
  void operator=(const HLSCppEmitterState &) = delete;
};
} // namespace

namespace {
/// This is the base class for all of the HLSCpp Emitter components.
class HLSCppEmitterBase {
public:
  explicit HLSCppEmitterBase(HLSCppEmitterState &state)
      : state(state), os(state.os) {}

  InFlightDiagnostic emitError(Operation *op, const Twine &message) {
    state.encounteredError = true;
    return op->emitError(message);
  }

  raw_ostream &indent() { return os.indent(state.currentIndent); }

  void addIndent() { state.currentIndent += 2; }
  void reduceIndent() { state.currentIndent -= 2; }

  // All of the mutable state we are maintaining.
  HLSCppEmitterState &state;

  // The stream to emit to.
  raw_ostream &os;

  /// Value name management methods.
  SmallString<8> addName(Value val, bool isPtr = false);

  SmallString<8> addAlias(Value val, Value alias);

  SmallString<8> getName(Value val);

  bool isDeclared(Value val) {
    if (getName(val).empty()) {
      return false;
    } else
      return true;
  }

private:
  HLSCppEmitterBase(const HLSCppEmitterBase &) = delete;
  void operator=(const HLSCppEmitterBase &) = delete;
};
} // namespace

// TODO: update naming rule.
SmallString<8> HLSCppEmitterBase::addName(Value val, bool isPtr) {
  assert(!isDeclared(val) && "has been declared before.");

  SmallString<8> valName;
  if (isPtr)
    valName += "*";

  valName += StringRef("val" + to_string(state.nameTable.size()));
  state.nameTable[val] = valName;

  // If the definition operation is ArrayOp, then the ArrayOp operand should be
  // an alias of the current value.
  if (auto defOp = val.getDefiningOp())
    if (auto arrayOp = dyn_cast<ArrayOp>(defOp))
      state.nameTable[arrayOp.getOperand()] = valName;

  // If the first user is ArrayOp, then the ArrayOp result should be an alias of
  // the current value.
  if (!val.getUsers().empty())
    if (auto arrayOp = dyn_cast<ArrayOp>(*val.getUsers().begin()))
      return state.nameTable[arrayOp.getResult()] = valName;

  return valName;
}

SmallString<8> HLSCppEmitterBase::addAlias(Value val, Value alias) {
  assert(!isDeclared(alias) && "has been declared before.");
  assert(isDeclared(val) && "hasn't been declared before.");

  auto valName = getName(val);
  state.nameTable[alias] = valName;

  // If the definition operation is ArrayOp, then the ArrayOp operand should be
  // an alias of the current value.
  if (auto defOp = alias.getDefiningOp())
    if (auto arrayOp = dyn_cast<ArrayOp>(defOp))
      state.nameTable[arrayOp.getOperand()] = valName;

  // If the first user is ArrayOp, then the ArrayOp result should be an alias of
  // the current value.
  if (!alias.getUsers().empty())
    if (auto arrayOp = dyn_cast<ArrayOp>(*alias.getUsers().begin()))
      return state.nameTable[arrayOp.getResult()] = valName;

  return valName;
}

SmallString<8> HLSCppEmitterBase::getName(Value val) {
  // For constant scalar operations, the constant number will be returned rather
  // than the value name.
  if (auto defOp = val.getDefiningOp()) {
    if (auto constOp = dyn_cast<mlir::ConstantOp>(defOp)) {
      auto constAttr = constOp.getValue();
      if (auto floatAttr = constAttr.dyn_cast<FloatAttr>()) {
        auto value = floatAttr.getValueAsDouble();
        if (isfinite(value))
          return SmallString<8>(to_string(value));
        else if (value > 0)
          return SmallString<8>("INFINITY");
        else
          return SmallString<8>("-INFINITY");

      } else if (auto intAttr = constAttr.dyn_cast<IntegerAttr>()) {
        auto value = intAttr.getInt();
        return SmallString<8>(to_string(value));

      } else if (auto boolAttr = constAttr.dyn_cast<BoolAttr>())
        return SmallString<8>(to_string(boolAttr.getValue()));
    }
  }
  return state.nameTable.lookup(val);
}

//===----------------------------------------------------------------------===//
// ModuleEmitter Class Declaration
//===----------------------------------------------------------------------===//

namespace {
class ModuleEmitter : public HLSCppEmitterBase {
public:
  using operand_range = Operation::operand_range;
  explicit ModuleEmitter(HLSCppEmitterState &state)
      : HLSCppEmitterBase(state) {}

  /// SCF statement emitters.
  void emitScfFor(scf::ForOp *op);
  void emitScfIf(scf::IfOp *op);
  void emitScfYield(scf::YieldOp *op);

  /// Affine statement emitters.
  void emitAffineFor(AffineForOp *op);
  void emitAffineIf(AffineIfOp *op);
  void emitAffineParallel(AffineParallelOp *op);
  void emitAffineApply(AffineApplyOp *op);
  template <typename OpType>
  void emitAffineMaxMin(OpType *op, const char *syntax);
  void emitAffineLoad(AffineLoadOp *op);
  void emitAffineStore(AffineStoreOp *op);
  void emitAffineYield(AffineYieldOp *op);

  /// Memref-related statement emitters.
  template <typename OpType> void emitAlloc(OpType *op);
  void emitLoad(LoadOp *op);
  void emitStore(StoreOp *op);

  /// Tensor-related statement emitters.
  void emitTensorLoad(TensorLoadOp *op);
  void emitTensorStore(TensorStoreOp *op);
  void emitTensorToMemref(TensorToMemrefOp *op);
  void emitDim(DimOp *op);
  void emitRank(RankOp *op);

  /// Standard expression emitters.
  void emitBinary(Operation *op, const char *syntax);
  void emitUnary(Operation *op, const char *syntax);

  /// Special operation emitters.
  void emitSelect(SelectOp *op);
  void emitConstant(ConstantOp *op);
  void emitIndexCast(IndexCastOp *op);
  void emitCall(CallOp *op);

  /// Structure operations emitters.
  void emitAssign(AssignOp *op);
  void emitArray(ArrayOp *op);

  /// Top-level MLIR module emitter.
  void emitModule(ModuleOp module);

private:
  /// C++ component emitters.
  void emitValue(Value val, unsigned rank = 0, bool isPtr = false);
  void emitArrayDecl(Value array);
  unsigned emitNestedLoopHead(Value val);
  void emitNestedLoopTail(unsigned rank);
  void emitInfoAndNewLine(Operation *op);

  /// MLIR component emitters.
  void emitOperation(Operation *op);
  void emitBlock(Block &block);
  void emitFunction(FuncOp func);
};
} // namespace

//===----------------------------------------------------------------------===//
// AffineEmitter Class
//===----------------------------------------------------------------------===//

namespace {
class AffineExprEmitter : public HLSCppEmitterBase,
                          public AffineExprVisitor<AffineExprEmitter> {
public:
  using operand_range = Operation::operand_range;
  explicit AffineExprEmitter(HLSCppEmitterState &state, unsigned numDim,
                             operand_range operands)
      : HLSCppEmitterBase(state), numDim(numDim), operands(operands) {}

  void visitAddExpr(AffineBinaryOpExpr expr) { emitAffineBinary(expr, "+"); }
  void visitMulExpr(AffineBinaryOpExpr expr) { emitAffineBinary(expr, "*"); }
  void visitModExpr(AffineBinaryOpExpr expr) { emitAffineBinary(expr, "%"); }
  void visitFloorDivExpr(AffineBinaryOpExpr expr) {
    emitAffineBinary(expr, "/");
  }
  void visitCeilDivExpr(AffineBinaryOpExpr expr) {
    // This is super inefficient.
    os << "(";
    visit(expr.getLHS());
    os << " + ";
    visit(expr.getRHS());
    os << " - 1) / ";
    visit(expr.getRHS());
    os << ")";
  }

  void visitConstantExpr(AffineConstantExpr expr) { os << expr.getValue(); }

  void visitDimExpr(AffineDimExpr expr) {
    os << getName(operands[expr.getPosition()]);
  }
  void visitSymbolExpr(AffineSymbolExpr expr) {
    os << getName(operands[numDim + expr.getPosition()]);
  }

  /// Affine expression emitters.
  void emitAffineBinary(AffineBinaryOpExpr expr, const char *syntax) {
    os << "(";
    if (auto constRHS = expr.getRHS().dyn_cast<AffineConstantExpr>()) {
      if ((unsigned)*syntax == (unsigned)*"*" && constRHS.getValue() == -1) {
        os << "-";
        visit(expr.getLHS());
        os << ")";
        return;
      }
      if ((unsigned)*syntax == (unsigned)*"+" && constRHS.getValue() < 0) {
        visit(expr.getLHS());
        os << " - ";
        os << -constRHS.getValue();
        os << ")";
        return;
      }
    }
    if (auto binaryRHS = expr.getRHS().dyn_cast<AffineBinaryOpExpr>()) {
      if (auto constRHS = binaryRHS.getRHS().dyn_cast<AffineConstantExpr>()) {
        if ((unsigned)*syntax == (unsigned)*"+" && constRHS.getValue() == -1 &&
            binaryRHS.getKind() == AffineExprKind::Mul) {
          visit(expr.getLHS());
          os << " - ";
          visit(binaryRHS.getLHS());
          os << ")";
          return;
        }
      }
    }
    visit(expr.getLHS());
    os << " " << syntax << " ";
    visit(expr.getRHS());
    os << ")";
  }

  void emitAffineExpr(AffineExpr expr) { visit(expr); }

private:
  unsigned numDim;
  operand_range operands;
};
} // namespace

//===----------------------------------------------------------------------===//
// StmtVisitor, ExprVisitor, and PragmaVisitor Classes
//===----------------------------------------------------------------------===//

namespace {
class StmtVisitor : public HLSCppVisitorBase<StmtVisitor, bool> {
public:
  StmtVisitor(ModuleEmitter &emitter) : emitter(emitter) {}

  using HLSCppVisitorBase::visitOp;
  /// SCF statements.
  bool visitOp(scf::ForOp op) { return emitter.emitScfFor(&op), true; };
  bool visitOp(scf::IfOp op) { return emitter.emitScfIf(&op), true; };
  bool visitOp(scf::ParallelOp op) { return true; };
  bool visitOp(scf::ReduceOp op) { return true; };
  bool visitOp(scf::ReduceReturnOp op) { return true; };
  bool visitOp(scf::YieldOp op) { return emitter.emitScfYield(&op), true; };

  /// Affine statements.
  bool visitOp(AffineForOp op) { return emitter.emitAffineFor(&op), true; }
  bool visitOp(AffineIfOp op) { return emitter.emitAffineIf(&op), true; }
  bool visitOp(AffineParallelOp op) {
    return emitter.emitAffineParallel(&op), true;
  }
  bool visitOp(AffineApplyOp op) { return emitter.emitAffineApply(&op), true; }
  bool visitOp(AffineMaxOp op) {
    return emitter.emitAffineMaxMin<AffineMaxOp>(&op, "max"), true;
  }
  bool visitOp(AffineMinOp op) {
    return emitter.emitAffineMaxMin<AffineMinOp>(&op, "min"), true;
  }
  bool visitOp(AffineLoadOp op) { return emitter.emitAffineLoad(&op), true; }
  bool visitOp(AffineStoreOp op) { return emitter.emitAffineStore(&op), true; }
  bool visitOp(AffineYieldOp op) { return emitter.emitAffineYield(&op), true; }

  /// Memref-related statements.
  bool visitOp(AllocOp op) { return emitter.emitAlloc<AllocOp>(&op), true; }
  bool visitOp(AllocaOp op) { return emitter.emitAlloc<AllocaOp>(&op), true; }
  bool visitOp(LoadOp op) { return emitter.emitLoad(&op), true; }
  bool visitOp(StoreOp op) { return emitter.emitStore(&op), true; }
  bool visitOp(DeallocOp op) { return true; }

  /// Tensor-related statements.
  bool visitOp(TensorLoadOp op) { return emitter.emitTensorLoad(&op), true; }
  bool visitOp(TensorStoreOp op) { return emitter.emitTensorStore(&op), true; }
  bool visitOp(TensorToMemrefOp op) {
    return emitter.emitTensorToMemref(&op), true;
  }
  bool visitOp(DimOp op) { return emitter.emitDim(&op), true; }
  bool visitOp(RankOp op) { return emitter.emitRank(&op), true; }

private:
  ModuleEmitter &emitter;
};
} // namespace

namespace {
class ExprVisitor : public HLSCppVisitorBase<ExprVisitor, bool> {
public:
  ExprVisitor(ModuleEmitter &emitter) : emitter(emitter) {}

  using HLSCppVisitorBase::visitOp;
  /// Float binary expressions.
  bool visitOp(CmpFOp op);
  bool visitOp(AddFOp op) { return emitter.emitBinary(op, "+"), true; }
  bool visitOp(SubFOp op) { return emitter.emitBinary(op, "-"), true; }
  bool visitOp(MulFOp op) { return emitter.emitBinary(op, "*"), true; }
  bool visitOp(DivFOp op) { return emitter.emitBinary(op, "/"), true; }
  bool visitOp(RemFOp op) { return emitter.emitBinary(op, "%"), true; }

  /// Integer binary expressions.
  bool visitOp(CmpIOp op);
  bool visitOp(AddIOp op) { return emitter.emitBinary(op, "+"), true; }
  bool visitOp(SubIOp op) { return emitter.emitBinary(op, "-"), true; }
  bool visitOp(MulIOp op) { return emitter.emitBinary(op, "*"), true; }
  bool visitOp(SignedDivIOp op) { return emitter.emitBinary(op, "/"), true; }
  bool visitOp(SignedRemIOp op) { return emitter.emitBinary(op, "%"), true; }
  bool visitOp(UnsignedDivIOp op) { return emitter.emitBinary(op, "/"), true; }
  bool visitOp(UnsignedRemIOp op) { return emitter.emitBinary(op, "%"), true; }
  bool visitOp(XOrOp op) { return emitter.emitBinary(op, "^"), true; }
  bool visitOp(AndOp op) { return emitter.emitBinary(op, "&"), true; }
  bool visitOp(OrOp op) { return emitter.emitBinary(op, "|"), true; }
  bool visitOp(ShiftLeftOp op) { return emitter.emitBinary(op, "<<"), true; }
  bool visitOp(SignedShiftRightOp op) {
    return emitter.emitBinary(op, ">>"), true;
  }
  bool visitOp(UnsignedShiftRightOp op) {
    return emitter.emitBinary(op, ">>"), true;
  }

  /// Unary expressions.
  bool visitOp(AbsFOp op) { return emitter.emitUnary(op, "abs"), true; }
  bool visitOp(CeilFOp op) { return emitter.emitUnary(op, "ceil"), true; }
  bool visitOp(NegFOp op) { return emitter.emitUnary(op, "-"), true; }
  bool visitOp(CosOp op) { return emitter.emitUnary(op, "cos"), true; }
  bool visitOp(SinOp op) { return emitter.emitUnary(op, "sin"), true; }
  bool visitOp(TanhOp op) { return emitter.emitUnary(op, "tanh"), true; }
  bool visitOp(SqrtOp op) { return emitter.emitUnary(op, "sqrt"), true; }
  bool visitOp(RsqrtOp op) { return emitter.emitUnary(op, "1.0 / sqrt"), true; }
  bool visitOp(ExpOp op) { return emitter.emitUnary(op, "exp"), true; }
  bool visitOp(Exp2Op op) { return emitter.emitUnary(op, "exp2"), true; }
  bool visitOp(LogOp op) { return emitter.emitUnary(op, "log"), true; }
  bool visitOp(Log2Op op) { return emitter.emitUnary(op, "log2"), true; }
  bool visitOp(Log10Op op) { return emitter.emitUnary(op, "log10"), true; }

  /// Special operations.
  bool visitOp(SelectOp op) { return emitter.emitSelect(&op), true; }
  bool visitOp(ConstantOp op) { return emitter.emitConstant(&op), true; }
  bool visitOp(IndexCastOp op) { return emitter.emitIndexCast(&op), true; }
  bool visitOp(CallOp op) { return emitter.emitCall(&op), true; }
  bool visitOp(ReturnOp op) { return true; }

private:
  ModuleEmitter &emitter;
};
} // namespace

bool ExprVisitor::visitOp(CmpFOp op) {
  switch (op.getPredicate()) {
  case CmpFPredicate::OEQ:
  case CmpFPredicate::UEQ:
    return emitter.emitBinary(op, "=="), true;
  case CmpFPredicate::ONE:
  case CmpFPredicate::UNE:
    return emitter.emitBinary(op, "!="), true;
  case CmpFPredicate::OLT:
  case CmpFPredicate::ULT:
    return emitter.emitBinary(op, "<"), true;
  case CmpFPredicate::OLE:
  case CmpFPredicate::ULE:
    return emitter.emitBinary(op, "<="), true;
  case CmpFPredicate::OGT:
  case CmpFPredicate::UGT:
    return emitter.emitBinary(op, ">"), true;
  case CmpFPredicate::OGE:
  case CmpFPredicate::UGE:
    return emitter.emitBinary(op, ">="), true;
  default:
    op.emitError("has unsupported compare type.");
    return false;
  }
}

bool ExprVisitor::visitOp(CmpIOp op) {
  switch (op.getPredicate()) {
  case CmpIPredicate::eq:
    return emitter.emitBinary(op, "=="), true;
  case CmpIPredicate::ne:
    return emitter.emitBinary(op, "!="), true;
  case CmpIPredicate::slt:
  case CmpIPredicate::ult:
    return emitter.emitBinary(op, "<"), true;
  case CmpIPredicate::sle:
  case CmpIPredicate::ule:
    return emitter.emitBinary(op, "<="), true;
  case CmpIPredicate::sgt:
  case CmpIPredicate::ugt:
    return emitter.emitBinary(op, ">"), true;
  case CmpIPredicate::sge:
  case CmpIPredicate::uge:
    return emitter.emitBinary(op, ">="), true;
  default:
    op.emitError("has unsupported compare type.");
    return false;
  }
}

namespace {
class PragmaVisitor : public HLSCppVisitorBase<PragmaVisitor, bool> {
public:
  PragmaVisitor(ModuleEmitter &emitter) : emitter(emitter) {}

  using HLSCppVisitorBase::visitOp;

  /// Structure operations.
  bool visitOp(AssignOp op) { return emitter.emitAssign(&op), true; }
  bool visitOp(ArrayOp op) { return emitter.emitArray(&op), true; }
  bool visitOp(EndOp op) { return true; }

private:
  ModuleEmitter &emitter;
};
} // namespace

//===----------------------------------------------------------------------===//
// ModuleEmitter Class Definition
//===----------------------------------------------------------------------===//

/// SCF statement emitters.
void ModuleEmitter::emitScfFor(scf::ForOp *op) {
  indent();
  os << "for (";
  auto iterVar = op->getInductionVar();

  // Emit lower bound.
  emitValue(iterVar);
  os << " = ";
  emitValue(op->lowerBound());
  os << "; ";

  // Emit upper bound.
  emitValue(iterVar);
  os << " < ";
  emitValue(op->upperBound());
  os << "; ";

  // Emit increase step.
  emitValue(iterVar);
  os << " += ";
  emitValue(op->step());
  os << ") {";
  emitInfoAndNewLine(*op);

  addIndent();

  if (auto pipeline = op->getAttrOfType<BoolAttr>("pipeline")) {
    if (pipeline.getValue()) {
      indent();
      os << "#pragma HLS pipeline\n";
    }
  }

  // if (auto flatten = op->getAttrOfType<BoolAttr>("flatten")) {
  //  indent();
  //  if (flatten.getValue())
  //    os << "#pragma HLS loop_flatten\n";
  //  else
  //    os << "#pragma HLS loop_flatten off\n";
  //}

  if (auto unroll = op->getAttrOfType<BoolAttr>("unroll")) {
    if (unroll.getValue()) {
      indent();
      os << "#pragma HLS unroll\n";
    }
  }

  emitBlock(op->getLoopBody().front());
  reduceIndent();

  indent();
  os << "}\n";
}

void ModuleEmitter::emitScfIf(scf::IfOp *op) {
  // Declare all values returned by scf::YieldOp. They will be further handled
  // by the scf::YieldOp emitter.
  for (auto result : op->getResults()) {
    if (!isDeclared(result)) {
      indent();
      if (result.getType().isa<ShapedType>())
        emitArrayDecl(result);
      else
        emitValue(result);
      os << ";\n";
    }
  }

  indent();
  os << "if (";
  emitValue(op->condition());
  os << ") {";
  emitInfoAndNewLine(*op);

  addIndent();
  emitBlock(op->thenRegion().front());
  reduceIndent();

  if (!op->elseRegion().empty()) {
    indent();
    os << "} else {\n";
    addIndent();
    emitBlock(op->elseRegion().front());
    reduceIndent();
  }

  indent();
  os << "}\n";
}

void ModuleEmitter::emitScfYield(scf::YieldOp *op) {
  if (op->getNumOperands() == 0)
    return;

  // For now, only and scf::If operations will use scf::Yield to return
  // generated values.
  if (auto parentOp = dyn_cast<scf::IfOp>(op->getParentOp())) {
    unsigned resultIdx = 0;
    for (auto result : parentOp.getResults()) {
      unsigned rank = emitNestedLoopHead(result);
      indent();
      emitValue(result, rank);
      os << " = ";
      emitValue(op->getOperand(resultIdx++), rank);
      os << ";";
      emitInfoAndNewLine(*op);
      emitNestedLoopTail(rank);
    }
  }
}

/// Affine statement emitters.
void ModuleEmitter::emitAffineFor(AffineForOp *op) {
  indent();
  os << "for (";
  auto iterVar = op->getInductionVar();

  // Emit lower bound.
  emitValue(iterVar);
  os << " = ";
  auto lowerMap = op->getLowerBoundMap();
  AffineExprEmitter lowerEmitter(state, lowerMap.getNumDims(),
                                 op->getLowerBoundOperands());
  if (lowerMap.getNumResults() == 1)
    lowerEmitter.emitAffineExpr(lowerMap.getResult(0));
  else {
    for (unsigned i = 0, e = lowerMap.getNumResults() - 1; i < e; ++i)
      os << "max(";
    lowerEmitter.emitAffineExpr(lowerMap.getResult(0));
    for (auto &expr : llvm::drop_begin(lowerMap.getResults(), 1)) {
      os << ", ";
      lowerEmitter.emitAffineExpr(expr);
      os << ")";
    }
  }
  os << "; ";

  // Emit upper bound.
  emitValue(iterVar);
  os << " < ";
  auto upperMap = op->getUpperBoundMap();
  AffineExprEmitter upperEmitter(state, upperMap.getNumDims(),
                                 op->getUpperBoundOperands());
  if (upperMap.getNumResults() == 1)
    upperEmitter.emitAffineExpr(upperMap.getResult(0));
  else {
    for (unsigned i = 0, e = upperMap.getNumResults() - 1; i < e; ++i)
      os << "min(";
    upperEmitter.emitAffineExpr(upperMap.getResult(0));
    for (auto &expr : llvm::drop_begin(upperMap.getResults(), 1)) {
      os << ", ";
      upperEmitter.emitAffineExpr(expr);
      os << ")";
    }
  }
  os << "; ";

  // Emit increase step.
  emitValue(iterVar);
  os << " += " << op->getStep() << ") {";
  emitInfoAndNewLine(*op);

  addIndent();

  if (auto pipeline = op->getAttrOfType<BoolAttr>("pipeline")) {
    if (pipeline.getValue()) {
      indent();
      os << "#pragma HLS pipeline\n";
    }
  }

  // if (auto flatten = op->getAttrOfType<BoolAttr>("flatten")) {
  //  indent();
  //  if (flatten.getValue())
  //    os << "#pragma HLS loop_flatten\n";
  //  else
  //    os << "#pragma HLS loop_flatten off\n";
  //}

  if (auto unroll = op->getAttrOfType<BoolAttr>("unroll")) {
    if (unroll.getValue()) {
      indent();
      os << "#pragma HLS unroll\n";
    }
  }

  emitBlock(op->getLoopBody().front());
  reduceIndent();

  indent();
  os << "}\n";
}

void ModuleEmitter::emitAffineIf(AffineIfOp *op) {
  // Declare all values returned by AffineYieldOp. They will be further
  // handled by the AffineYieldOp emitter.
  for (auto result : op->getResults()) {
    if (!isDeclared(result)) {
      indent();
      if (result.getType().isa<ShapedType>())
        emitArrayDecl(result);
      else
        emitValue(result);
      os << ";\n";
    }
  }

  indent();
  os << "if (";
  auto constrSet = op->getIntegerSet();
  AffineExprEmitter constrEmitter(state, constrSet.getNumDims(),
                                  op->getOperands());

  // Emit all constraints.
  unsigned constrIdx = 0;
  for (auto &expr : constrSet.getConstraints()) {
    constrEmitter.emitAffineExpr(expr);
    if (constrSet.isEq(constrIdx))
      os << " == 0";
    else
      os << " >= 0";

    if (constrIdx++ != constrSet.getNumConstraints() - 1)
      os << " && ";
  }
  os << ") {";
  emitInfoAndNewLine(*op);

  addIndent();
  emitBlock(*op->getThenBlock());
  reduceIndent();

  if (op->hasElse()) {
    indent();
    os << "} else {\n";
    addIndent();
    emitBlock(*op->getElseBlock());
    reduceIndent();
  }

  indent();
  os << "}\n";
}

void ModuleEmitter::emitAffineParallel(AffineParallelOp *op) {
  // Declare all values returned by AffineParallelOp. They will be further
  // handled by the AffineYieldOp emitter.
  for (auto result : op->getResults()) {
    if (!isDeclared(result)) {
      indent();
      if (result.getType().isa<ShapedType>())
        emitArrayDecl(result);
      else
        emitValue(result);
      os << ";\n";
    }
  }

  for (unsigned i = 0, e = op->getNumDims(); i < e; ++i) {
    indent();
    os << "for (";
    auto iterVar = op->getBody()->getArgument(i);

    // Emit lower bound.
    emitValue(iterVar);
    os << " = ";
    auto lowerMap = op->getLowerBoundsValueMap().getAffineMap();
    AffineExprEmitter lowerEmitter(state, lowerMap.getNumDims(),
                                   op->getLowerBoundsOperands());
    lowerEmitter.emitAffineExpr(lowerMap.getResult(i));
    os << "; ";

    // Emit upper bound.
    emitValue(iterVar);
    os << " < ";
    auto upperMap = op->getUpperBoundsValueMap().getAffineMap();
    AffineExprEmitter upperEmitter(state, upperMap.getNumDims(),
                                   op->getUpperBoundsOperands());
    upperEmitter.emitAffineExpr(upperMap.getResult(i));
    os << "; ";

    // Emit increase step.
    emitValue(iterVar);
    auto step = op->getAttrOfType<ArrayAttr>(op->getStepsAttrName())[i]
                    .cast<IntegerAttr>()
                    .getInt();
    os << " += " << step << ") {\n";

    addIndent();
  }

  emitBlock(op->getLoopBody().front());

  for (unsigned i = 0, e = op->getNumDims(); i < e; ++i) {
    reduceIndent();

    indent();
    if (i == e - 1)
      os << "}";
    else
      os << "}\n";
  }
  emitInfoAndNewLine(*op);
}

void ModuleEmitter::emitAffineApply(AffineApplyOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  auto affineMap = op->getAffineMap();
  AffineExprEmitter(state, affineMap.getNumDims(), op->getOperands())
      .emitAffineExpr(affineMap.getResult(0));
  os << ";";
  emitInfoAndNewLine(*op);
}

template <typename OpType>
void ModuleEmitter::emitAffineMaxMin(OpType *op, const char *syntax) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getOperands());
  for (unsigned i = 0, e = affineMap.getNumResults() - 1; i < e; ++i)
    os << syntax << "(";
  affineEmitter.emitAffineExpr(affineMap.getResult(0));
  for (auto &expr : llvm::drop_begin(affineMap.getResults(), 1)) {
    os << ", ";
    affineEmitter.emitAffineExpr(expr);
    os << ")";
  }
  os << ";";
  emitInfoAndNewLine(*op);
}

void ModuleEmitter::emitAffineLoad(AffineLoadOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  emitValue(op->getMemRef());
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getMapOperands());
  for (auto index : affineMap.getResults()) {
    os << "[";
    affineEmitter.emitAffineExpr(index);
    os << "]";
  }
  os << ";";
  emitInfoAndNewLine(*op);
}

void ModuleEmitter::emitAffineStore(AffineStoreOp *op) {
  indent();
  emitValue(op->getMemRef());
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getMapOperands());
  for (auto index : affineMap.getResults()) {
    os << "[";
    affineEmitter.emitAffineExpr(index);
    os << "]";
  }
  os << " = ";
  emitValue(op->getValueToStore());
  os << ";";
  emitInfoAndNewLine(*op);
}

// TODO: For now, all values created in the AffineIf region will be declared
// in the generated C++. However, values which will be returned by affine
// yield operation should not be declared again. How to "bind" the pair of
// values inside/outside of AffineIf region needs to be considered.
void ModuleEmitter::emitAffineYield(AffineYieldOp *op) {
  if (op->getNumOperands() == 0)
    return;

  // For now, only AffineParallel and AffineIf operations will use
  // AffineYield to return generated values.
  if (auto parentOp = dyn_cast<AffineIfOp>(op->getParentOp())) {
    unsigned resultIdx = 0;
    for (auto result : parentOp.getResults()) {
      unsigned rank = emitNestedLoopHead(result);
      indent();
      emitValue(result, rank);
      os << " = ";
      emitValue(op->getOperand(resultIdx++), rank);
      os << ";";
      emitInfoAndNewLine(*op);
      emitNestedLoopTail(rank);
    }
  } else if (auto parentOp =
                 dyn_cast<mlir::AffineParallelOp>(op->getParentOp())) {
    indent();
    os << "if (";
    unsigned ivIdx = 0;
    for (auto iv : parentOp.getBody()->getArguments()) {
      emitValue(iv);
      os << " == 0";
      if (ivIdx++ != parentOp.getBody()->getNumArguments() - 1)
        os << " && ";
    }
    os << ") {\n";

    // When all induction values are 0, generated values will be directly
    // assigned to the current results, correspondingly.
    addIndent();
    unsigned resultIdx = 0;
    for (auto result : parentOp.getResults()) {
      unsigned rank = emitNestedLoopHead(result);
      indent();
      emitValue(result, rank);
      os << " = ";
      emitValue(op->getOperand(resultIdx++), rank);
      os << ";";
      emitInfoAndNewLine(*op);
      emitNestedLoopTail(rank);
    }
    reduceIndent();

    indent();
    os << "} else {\n";

    // Otherwise, generated values will be accumulated/reduced to the
    // current results with corresponding AtomicRMWKind operations.
    addIndent();
    resultIdx = 0;
    for (auto result : parentOp.getResults()) {
      unsigned rank = emitNestedLoopHead(result);
      indent();
      emitValue(result, rank);
      switch ((AtomicRMWKind)parentOp
                  .getAttrOfType<ArrayAttr>(
                      parentOp.getReductionsAttrName())[resultIdx]
                  .cast<IntegerAttr>()
                  .getInt()) {
      case (AtomicRMWKind::addf):
      case (AtomicRMWKind::addi):
        os << " += ";
        emitValue(op->getOperand(resultIdx++), rank);
        break;
      case (AtomicRMWKind::assign):
        os << " = ";
        emitValue(op->getOperand(resultIdx++), rank);
        break;
      case (AtomicRMWKind::maxf):
      case (AtomicRMWKind::maxs):
      case (AtomicRMWKind::maxu):
        os << " = max(";
        emitValue(result, rank);
        os << ", ";
        emitValue(op->getOperand(resultIdx++), rank);
        os << ")";
        break;
      case (AtomicRMWKind::minf):
      case (AtomicRMWKind::mins):
      case (AtomicRMWKind::minu):
        os << " = min(";
        emitValue(result, rank);
        os << ", ";
        emitValue(op->getOperand(resultIdx++), rank);
        os << ")";
        break;
      case (AtomicRMWKind::mulf):
      case (AtomicRMWKind::muli):
        os << " *= ";
        emitValue(op->getOperand(resultIdx++), rank);
        break;
      }
      os << ";";
      emitInfoAndNewLine(*op);
      emitNestedLoopTail(rank);
    }
    reduceIndent();

    indent();
    os << "}\n";
  }
}

/// Memref-related statement emitters.
template <typename OpType> void ModuleEmitter::emitAlloc(OpType *op) {
  // A declared result indicates that the memref is output of the function, and
  // has been declared in the function signature.
  if (isDeclared(op->getResult()))
    return;

  // Vivado HLS only supports static shape on-chip memory.
  if (!op->getType().hasStaticShape())
    emitError(*op, "is unranked or has dynamic shape.");

  indent();
  emitArrayDecl(op->getResult());
  os << ";";
  emitInfoAndNewLine(*op);
}

void ModuleEmitter::emitLoad(LoadOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  emitValue(op->getMemRef());
  for (auto index : op->getIndices()) {
    os << "[";
    emitValue(index);
    os << "]";
  }
  os << ";";
  emitInfoAndNewLine(*op);
}

void ModuleEmitter::emitStore(StoreOp *op) {
  indent();
  emitValue(op->getMemRef());
  for (auto index : op->getIndices()) {
    os << "[";
    emitValue(index);
    os << "]";
  }
  os << " = ";
  emitValue(op->getValueToStore());
  os << ";";
  emitInfoAndNewLine(*op);
}

/// Tensor-related statement emitters.
void ModuleEmitter::emitTensorLoad(TensorLoadOp *op) {
  auto rank = emitNestedLoopHead(op->getResult());
  indent();
  emitValue(op->getResult(), rank);
  os << " = ";
  emitValue(op->getOperand(), rank);
  os << ";";
  emitInfoAndNewLine(*op);
  emitNestedLoopTail(rank);
}

void ModuleEmitter::emitTensorStore(TensorStoreOp *op) {
  auto rank = emitNestedLoopHead(op->getOperand(0));
  indent();
  emitValue(op->getOperand(1), rank);
  os << " = ";
  emitValue(op->getOperand(0), rank);
  os << ";";
  emitInfoAndNewLine(*op);
  emitNestedLoopTail(rank);
}

void ModuleEmitter::emitTensorToMemref(TensorToMemrefOp *op) {
  // A declared result indicates that the memref is output of the function, and
  // has been declared in the function signature.
  if (isDeclared(op->getResult())) {
    auto rank = emitNestedLoopHead(op->getResult());
    indent();
    emitValue(op->getResult(), rank);
    os << " = ";
    emitValue(op->getOperand(), rank);
    os << ";";
    emitInfoAndNewLine(*op);
    emitNestedLoopTail(rank);
  } else
    addAlias(op->getOperand(), op->getResult());
}

void ModuleEmitter::emitDim(DimOp *op) {
  if (auto constOp = dyn_cast<ConstantOp>(op->getOperand(1).getDefiningOp())) {
    auto constVal = constOp.getValue().cast<IntegerAttr>().getInt();
    auto type = op->getOperand(0).getType().cast<ShapedType>();

    if (type.hasStaticShape()) {
      if (constVal >= 0 && constVal < (int64_t)type.getShape().size()) {
        indent();
        emitValue(op->getResult());
        os << " = ";
        os << type.getShape()[constVal] << ";";
        emitInfoAndNewLine(*op);
      } else
        emitError(*op, "index is out of range.");
    } else
      emitError(*op, "is unranked or has dynamic shape.");
  } else
    emitError(*op, "index is not a constant.");
}

void ModuleEmitter::emitRank(RankOp *op) {
  auto type = op->getOperand().getType().cast<ShapedType>();
  if (type.hasRank()) {
    indent();
    emitValue(op->getResult());
    os << " = ";
    os << type.getRank() << ";";
    emitInfoAndNewLine(*op);
  } else
    emitError(*op, "is unranked.");
}

/// Standard expression emitters.
void ModuleEmitter::emitBinary(Operation *op, const char *syntax) {
  auto rank = emitNestedLoopHead(op->getResult(0));
  indent();
  emitValue(op->getResult(0), rank);
  os << " = ";
  emitValue(op->getOperand(0), rank);
  os << " " << syntax << " ";
  emitValue(op->getOperand(1), rank);
  os << ";";
  emitInfoAndNewLine(op);
  emitNestedLoopTail(rank);
}

void ModuleEmitter::emitUnary(Operation *op, const char *syntax) {
  auto rank = emitNestedLoopHead(op->getResult(0));
  indent();
  emitValue(op->getResult(0), rank);
  os << " = " << syntax << "(";
  emitValue(op->getOperand(0), rank);
  os << ");";
  emitInfoAndNewLine(op);
  emitNestedLoopTail(rank);
}

/// Special operation emitters.
void ModuleEmitter::emitSelect(SelectOp *op) {
  unsigned rank = emitNestedLoopHead(op->getResult());
  unsigned conditionRank = rank;
  if (!op->getCondition().getType().isa<ShapedType>())
    conditionRank = 0;

  indent();
  emitValue(op->getResult(), rank);
  os << " = ";
  emitValue(op->getCondition(), conditionRank);
  os << " ? ";
  emitValue(op->getTrueValue(), rank);
  os << " : ";
  emitValue(op->getFalseValue(), rank);
  os << ";";
  emitInfoAndNewLine(*op);
  emitNestedLoopTail(rank);
}

void ModuleEmitter::emitConstant(ConstantOp *op) {
  // This indicates the constant type is scalar (float, integer, or bool).
  if (isDeclared(op->getResult()))
    return;

  if (auto denseAttr = op->getValue().dyn_cast<DenseElementsAttr>()) {
    indent();
    emitArrayDecl(op->getResult());
    os << " = {";
    auto type = op->getResult().getType().cast<ShapedType>().getElementType();

    unsigned elementIdx = 0;
    for (auto element : denseAttr.getAttributeValues()) {
      if (type.isF32()) {
        auto value = element.cast<FloatAttr>().getValue().convertToFloat();
        if (isfinite(value))
          os << value;
        else if (value > 0)
          os << "INFINITY";
        else
          os << "-INFINITY";

      } else if (type.isF64()) {
        auto value = element.cast<FloatAttr>().getValue().convertToDouble();
        if (isfinite(value))
          os << value;
        else if (value > 0)
          os << "INFINITY";
        else
          os << "-INFINITY";

      } else if (type.isInteger(1))
        os << element.cast<BoolAttr>().getValue();
      else if (type.isIntOrIndex())
        os << element.cast<IntegerAttr>().getValue();
      else
        emitError(*op, "array has unsupported element type.");

      if (elementIdx++ != denseAttr.getNumElements() - 1)
        os << ", ";
    }
    os << "};";
    emitInfoAndNewLine(*op);
  } else
    emitError(*op, "has unsupported constant type.");
}

void ModuleEmitter::emitIndexCast(IndexCastOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  emitValue(op->getOperand());
  os << ";";
  emitInfoAndNewLine(*op);
}

void ModuleEmitter::emitCall(CallOp *op) {
  // Handle returned value by the callee.
  for (auto result : op->getResults()) {
    if (!isDeclared(result)) {
      indent();
      if (result.getType().isa<ShapedType>())
        emitArrayDecl(result);
      else
        emitValue(result);
      os << ";\n";
    }
  }

  // Emit the function call.
  indent();
  os << op->getCallee() << "(";

  // Handle input arguments.
  unsigned argIdx = 0;
  for (auto arg : op->getOperands()) {
    emitValue(arg);

    if (argIdx++ != op->getNumOperands() - 1)
      os << ", ";
  }

  // Handle output arguments.
  for (auto result : op->getResults()) {
    // The address should be passed in for scalar result arguments.
    if (result.getType().isa<ShapedType>())
      os << ", ";
    else
      os << ", &";

    emitValue(result);
  }

  os << ");";
  emitInfoAndNewLine(*op);
}

/// Structure operation emitters.
void ModuleEmitter::emitAssign(AssignOp *op) {
  unsigned rank = emitNestedLoopHead(op->getResult());
  indent();
  emitValue(op->getResult(), rank);
  os << " = ";
  emitValue(op->getOperand(), rank);
  os << ";";
  emitInfoAndNewLine(*op);
  emitNestedLoopTail(rank);
}

void ModuleEmitter::emitArray(ArrayOp *op) {
  bool emitPragmaFlag = false;

  // Emit interface pragma.
  if (op->interface()) {
    emitPragmaFlag = true;
    indent();
    os << "#pragma HLS interface";
    os << " " << op->interface_mode();
    os << " port=";
    emitValue(op->getOperand());
    if (op->interface_mode() == "m_axi") {
      os << " depth=" << op->interface_depth();
      os << " offset=slave";
    }
    os << "\n";
  }

  // Emit resource pragma.
  if (op->storage()) {
    emitPragmaFlag = true;
    indent();
    os << "#pragma HLS resource";
    os << " variable=";
    emitValue(op->getOperand());
    os << " core=";
    os << op->storage_type();
    os << "\n";
  }

  auto type = op->getOperand().getType().cast<ShapedType>();
  if (op->partition() && type.hasStaticShape()) {

    // Emit array_partition pragma(s).
    for (unsigned dim = 0; dim < type.getRank(); ++dim) {
      auto partitionType =
          op->partition_type()[dim].cast<StringAttr>().getValue();
      if (partitionType != "none") {
        emitPragmaFlag = true;

        indent();
        os << "#pragma HLS array_partition";
        os << " variable=";
        emitValue(op->getOperand());
        os << " " << partitionType;
        if (partitionType != "complete")
          os << " factor="
             << op->partition_factor()[dim].cast<IntegerAttr>().getUInt();
        os << " dim=" << dim + 1 << "\n";
      }
    }
  }

  // Emit an empty line.
  if (emitPragmaFlag)
    os << "\n";
}

/// C++ component emitters.
void ModuleEmitter::emitValue(Value val, unsigned rank, bool isPtr) {
  assert(!(rank && isPtr) && "should be either an array or a pointer.");

  // Value has been declared before or is a constant number.
  if (isDeclared(val)) {
    os << getName(val);
    for (unsigned i = 0; i < rank; ++i)
      os << "[idx" << i << "]";
    return;
  }

  // Handle memref, tensor, and vector types.
  auto valType = val.getType();
  if (auto arrayType = val.getType().dyn_cast<ShapedType>())
    valType = arrayType.getElementType();

  // Handle float types.
  if (valType.isa<Float32Type>())
    os << "float ";
  else if (valType.isa<Float64Type>())
    os << "double ";

  // Handle integer types.
  else if (valType.isa<IndexType>())
    os << "int ";
  else if (auto intType = valType.dyn_cast<IntegerType>()) {
    os << "ap_";
    if (intType.getSignedness() == IntegerType::SignednessSemantics::Unsigned)
      os << "u";
    os << "int<" << intType.getWidth() << "> ";
  } else
    emitError(val.getDefiningOp(), "has unsupported type.");

  // Add the new value to nameTable and emit its name.
  os << addName(val, isPtr);
  for (unsigned i = 0; i < rank; ++i)
    os << "[idx" << i << "]";
}

void ModuleEmitter::emitArrayDecl(Value array) {
  assert(!isDeclared(array) && "has been declared before.");

  auto arrayType = array.getType().cast<ShapedType>();
  if (arrayType.hasStaticShape()) {
    emitValue(array);
    for (auto &shape : arrayType.getShape())
      os << "[" << shape << "]";
  } else
    emitValue(array, /*rank=*/0, /*isPtr=*/true);
}

unsigned ModuleEmitter::emitNestedLoopHead(Value val) {
  unsigned rank = 0;

  if (auto type = val.getType().dyn_cast<ShapedType>()) {
    if (!type.hasStaticShape()) {
      emitError(val.getDefiningOp(), "is unranked or has dynamic shape.");
      return 0;
    }

    // Declare a new array.
    if (!isDeclared(val)) {
      indent();
      emitArrayDecl(val);
      os << ";\n";
    }

    // Create nested loop.
    unsigned dimIdx = 0;
    for (auto &shape : type.getShape()) {
      indent();
      os << "for (int idx" << dimIdx << " = 0; ";
      os << "idx" << dimIdx << " < " << shape << "; ";
      os << "++idx" << dimIdx++ << ") {\n";

      addIndent();
    }
    rank = type.getRank();
  }

  return rank;
}

void ModuleEmitter::emitNestedLoopTail(unsigned rank) {
  for (unsigned i = 0; i < rank; ++i) {
    reduceIndent();

    indent();
    os << "}\n";
  }
}

void ModuleEmitter::emitInfoAndNewLine(Operation *op) {
  os << "\t//";
  // Print line number.
  if (auto loc = op->getLoc().dyn_cast<FileLineColLoc>())
    os << " L" << loc.getLine();

  // Print schedule information.
  if (auto begin = op->getAttrOfType<IntegerAttr>("schedule_begin"))
    os << ", S[" << begin.getUInt();
  if (auto end = op->getAttrOfType<IntegerAttr>("schedule_end"))
    os << "," << end.getUInt() << ")";

  // Print loop information.
  if (auto II = op->getAttrOfType<IntegerAttr>("init_interval"))
    os << ", II=" << II.getUInt();
  if (auto latency = op->getAttrOfType<IntegerAttr>("iter_latency"))
    os << ", latency=" << latency.getUInt();

  os << "\n";
}

/// MLIR component emitters.
void ModuleEmitter::emitOperation(Operation *op) {
  if (ExprVisitor(*this).dispatchVisitor(op))
    return;

  if (StmtVisitor(*this).dispatchVisitor(op))
    return;

  if (PragmaVisitor(*this).dispatchVisitor(op))
    return;

  emitError(op, "can't be correctly emitted.");
}

void ModuleEmitter::emitBlock(Block &block) {
  for (auto &op : block)
    emitOperation(&op);
}

void ModuleEmitter::emitFunction(FuncOp func) {
  if (func.getBlocks().size() != 1)
    emitError(func, "has zero or more than one basic blocks.");

  if (auto top = func.getAttrOfType<BoolAttr>("top_function"))
    if (top.getValue())
      os << "/// This is top function.\n";

  if (auto latency = func.getAttrOfType<IntegerAttr>("latency"))
    os << "/// Latency=" << latency.getUInt() << "\n";

  if (auto dsp = func.getAttrOfType<IntegerAttr>("dsp"))
    os << "/// DSP=" << dsp.getUInt() << "\n";

  // Emit function signature.
  os << "void " << func.getName() << "(\n";
  addIndent();

  // This vector is to record all scalar ports.
  SmallVector<Value, 8> portList;

  // Handle input arguments.
  unsigned argIdx = 0;
  for (auto &arg : func.getArguments()) {
    indent();
    if (arg.getType().isa<ShapedType>())
      emitArrayDecl(arg);
    else {
      emitValue(arg);
      portList.push_back(arg);
    }

    if (argIdx++ != func.getNumArguments() - 1)
      os << ",\n";
  }

  // Handle results.
  if (auto funcReturn = dyn_cast<ReturnOp>(func.front().getTerminator())) {
    for (auto result : funcReturn.getOperands()) {
      os << ",\n";
      indent();
      // TODO: a known bug, cannot return a value twice, e.g. return %0, %0 :
      // index, index. However, typically this should not happen.
      if (result.getType().isa<ShapedType>())
        emitArrayDecl(result);
      else {
        // In Vivado HLS, pointer indicates the value is an output.
        emitValue(result, /*rank=*/0, /*isPtr=*/true);
        portList.push_back(result);
      }
    }
  } else
    emitError(func, "doesn't have a return operation as terminator.");

  reduceIndent();
  os << "\n) {";
  emitInfoAndNewLine(func);

  // Emit function body.
  addIndent();

  if (auto dataflow = func.getAttrOfType<BoolAttr>("dataflow")) {
    if (dataflow.getValue()) {
      indent();
      os << "#pragma HLS dataflow\n";

      // An empty line.
      os << "\n";
    }
  }

  // Only top function should emit these pragmas.
  if (auto topFlag = func.getAttrOfType<BoolAttr>("top_function")) {
    if (topFlag.getValue()) {
      indent();
      os << "#pragma HLS interface s_axilite port=return bundle=ctrl\n";

      // Interface pragma for array ports will be seperately handled since
      // they are much more complicated.
      for (auto &port : portList) {
        indent();
        os << "#pragma HLS interface s_axilite";
        os << " port=";

        // TODO: This is a temporary solution.
        auto name = getName(port);
        if (name.front() == "*"[0])
          name.erase(name.begin());
        os << name;
        os << " bundle=ctrl\n";
      }
      os << "\n";
    }
  }

  emitBlock(func.front());
  reduceIndent();
  os << "}\n";

  // An empty line.
  os << "\n";
}

/// Top-level MLIR module emitter.
void ModuleEmitter::emitModule(ModuleOp module) {
  os << R"XXX(
//===------------------------------------------------------------*- C++ -*-===//
//
// Automatically generated file for High-level Synthesis (HLS).
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <ap_axi_sdata.h>
#include <ap_fixed.h>
#include <ap_int.h>
#include <hls_math.h>
#include <hls_stream.h>
#include <math.h>
#include <stdint.h>

using namespace std;

)XXX";

  for (auto &op : *module.getBody()) {
    if (auto func = dyn_cast<FuncOp>(op))
      emitFunction(func);
    else if (!isa<ModuleTerminatorOp>(op))
      emitError(&op, "is unsupported operation.");
  }
}

//===----------------------------------------------------------------------===//
// Entry of scalehls-translate
//===----------------------------------------------------------------------===//

static LogicalResult emitHLSCpp(ModuleOp module, llvm::raw_ostream &os) {
  HLSCppEmitterState state(os);
  ModuleEmitter(state).emitModule(module);
  return failure(state.encounteredError);
}

void scalehls::registerHLSCppEmitterTranslation() {
  static TranslateFromMLIRRegistration toHLSCpp("emit-hlscpp", emitHLSCpp);
}
