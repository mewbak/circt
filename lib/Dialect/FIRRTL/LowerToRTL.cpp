//===- LowerToRTL.cpp - Lower FIRRTL -> RTL dialect -----------------------===//
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/Ops.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/RTL/Ops.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
using namespace circt;
using namespace firrtl;
using namespace rtl;

//===----------------------------------------------------------------------===//
// RTLTypeConverter
//===----------------------------------------------------------------------===//

namespace {
class RTLTypeConverter : public TypeConverter {
public:
  RTLTypeConverter();

  static Optional<Type> convertType(FIRRTLType type);
};
} // end anonymous namespace

RTLTypeConverter::RTLTypeConverter() { addConversion(convertType); }

Optional<Type> RTLTypeConverter::convertType(FIRRTLType type) {
  auto width = type.getBitWidthOrSentinel();
  if (width >= 0)
    return IntegerType::get(width, type.getContext());
  if (width == -1)
    return None; // IntType with unknown width.

  return type;
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Map in the input value into an expected type, using the type converter to
/// do this.  This would ideally be handled by the conversion framework.
static Value mapOperand(Value operand, Operation *op,
                        ConversionPatternRewriter &rewriter) {
  auto opType = operand.getType();
  if (auto firType = opType.dyn_cast<FIRRTLType>()) {
    auto resultType = RTLTypeConverter::convertType(firType.getPassiveType());
    if (!resultType.hasValue())
      return {};

    if (auto intType = resultType.getValue().dyn_cast<IntegerType>()) {
      // Cast firrtl -> standard type.
      return rewriter.create<firrtl::StdIntCast>(op->getLoc(), intType,
                                                 operand);
    }
  }

  return operand;
}

/// Map the specified operation and then extend it to destType width if needed.
/// The sign of the extension follows the sign of destType.
static Value mapAndExtendInt(Value operand, Type destType, Operation *op,
                             ConversionPatternRewriter &rewriter) {
  operand = mapOperand(operand, op, rewriter);
  if (!operand)
    return {};

  auto destIntType = destType.cast<IntType>();
  auto destWidth = destIntType.getWidthOrSentinel();
  if (destWidth == -1)
    return {};

  auto srcWidth = operand.getType().cast<IntegerType>().getWidth();
  if (srcWidth == unsigned(destWidth))
    return operand;

  assert(srcWidth < unsigned(destWidth) && "Only extensions");
  auto resultType = rewriter.getIntegerType(destWidth);

  if (destIntType.isSigned())
    return rewriter.create<rtl::SExtOp>(op->getLoc(), resultType, operand);

  return rewriter.create<rtl::ZExtOp>(op->getLoc(), resultType, operand);
}

//===----------------------------------------------------------------------===//
// Special Operations
//===----------------------------------------------------------------------===//

static LogicalResult lower(firrtl::ConstantOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  rewriter.replaceOpWithNewOp<rtl::ConstantOp>(op, op.value());
  return success();
}

static LogicalResult lower(firrtl::WireOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto resType = op.result().getType().cast<FIRRTLType>();
  auto resultType = RTLTypeConverter::convertType(resType);
  if (!resultType)
    return failure();
  rewriter.replaceOpWithNewOp<rtl::WireOp>(op, resultType.getValue(),
                                           op.nameAttr());
  return success();
}

static LogicalResult lower(firrtl::ConnectOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {

  auto lhs = mapOperand(operands[0], op, rewriter);
  auto rhs = mapOperand(operands[1], op, rewriter);
  if (!lhs || !rhs)
    return failure();

  rewriter.replaceOpWithNewOp<rtl::ConnectOp>(op, lhs, rhs);
  return success();
}

//===----------------------------------------------------------------------===//
// Unary Operations
//===----------------------------------------------------------------------===//

// Lower a cast that is a noop at the RTL level.
static LogicalResult lowerNoopCast(Operation *op, ArrayRef<Value> operands,
                                   ConversionPatternRewriter &rewriter) {
  auto operand = mapOperand(operands[0], op, rewriter);
  if (!operand)
    return failure();

  // Noop cast.
  rewriter.replaceOp(op, operand);
  return success();
}

static LogicalResult lower(firrtl::AsSIntPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerNoopCast(op, operands, rewriter);
}

static LogicalResult lower(firrtl::AsUIntPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerNoopCast(op, operands, rewriter);
}

// Pad is a noop or extension operation.
static LogicalResult lower(firrtl::PadPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto operand = mapAndExtendInt(operands[0], op.getType(), op, rewriter);
  if (!operand)
    return failure();
  rewriter.replaceOp(op, operand);
  return success();
}

static LogicalResult lower(firrtl::CatPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto lhs = mapOperand(operands[0], op, rewriter);
  auto rhs = mapOperand(operands[1], op, rewriter);
  if (!lhs || !rhs)
    return failure();

  rewriter.replaceOpWithNewOp<rtl::ConcatOp>(op, ValueRange({lhs, rhs}));
  return success();
}

//===----------------------------------------------------------------------===//
// Variadic Bitwise Operations
//===----------------------------------------------------------------------===//

template <typename OpType, typename ResultOpType>
static LogicalResult lowerVariadicOp(OpType op, ArrayRef<Value> operands,
                                     ConversionPatternRewriter &rewriter) {
  auto lhs = mapAndExtendInt(operands[0], op.getType(), op, rewriter);
  auto rhs = mapAndExtendInt(operands[1], op.getType(), op, rewriter);

  if (!lhs || !rhs)
    return failure();

  rewriter.replaceOpWithNewOp<ResultOpType>(op, ValueRange({lhs, rhs}));
  return success();
}

static LogicalResult lower(firrtl::AndPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerVariadicOp<firrtl::AndPrimOp, rtl::AndOp>(op, operands, rewriter);
}

static LogicalResult lower(firrtl::OrPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerVariadicOp<firrtl::OrPrimOp, rtl::OrOp>(op, operands, rewriter);
}

static LogicalResult lower(firrtl::XorPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerVariadicOp<firrtl::XorPrimOp, rtl::XorOp>(op, operands, rewriter);
}

//===----------------------------------------------------------------------===//
// Binary Operations
//===----------------------------------------------------------------------===//

template <typename OpType, typename ResultOpType>
static LogicalResult lowerBinOp(OpType op, ArrayRef<Value> operands,
                                ConversionPatternRewriter &rewriter) {
  // Extend the two operands to match the destination type.
  auto lhs = mapAndExtendInt(operands[0], op.getType(), op, rewriter);
  auto rhs = mapAndExtendInt(operands[1], op.getType(), op, rewriter);
  if (!lhs || !rhs)
    return failure();

  // Emit the result operation.
  rewriter.replaceOpWithNewOp<ResultOpType>(op, lhs, rhs);
  return success();
}

static LogicalResult lower(firrtl::AddPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerVariadicOp<firrtl::AddPrimOp, rtl::AddOp>(op, operands, rewriter);
}

static LogicalResult lower(firrtl::SubPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  return lowerBinOp<firrtl::SubPrimOp, rtl::SubOp>(op, operands, rewriter);
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

static LogicalResult lower(firrtl::BitsPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto input = mapOperand(operands[0], op, rewriter);
  if (!input)
    return failure();

  Type resultType = rewriter.getIntegerType(op.getHi() - op.getLo() + 1);
  rewriter.replaceOpWithNewOp<rtl::ExtractOp>(op, resultType, input,
                                              op.getLo());
  return success();
}

static LogicalResult lower(firrtl::HeadPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto input = mapOperand(operands[0], op, rewriter);
  if (!input)
    return failure();

  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  Type resultType = rewriter.getIntegerType(op.getAmount());
  rewriter.replaceOpWithNewOp<rtl::ExtractOp>(op, resultType, input,
                                              inWidth - op.getAmount());
  return success();
}

static LogicalResult lower(firrtl::ShlPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto input = mapOperand(operands[0], op, rewriter);
  if (!input)
    return failure();

  // Handle the degenerate case.
  if (op.getAmount() == 0) {
    rewriter.replaceOp(op, input);
    return success();
  }

  auto zero =
      rewriter.create<rtl::ConstantOp>(op.getLoc(), APInt(op.getAmount(), 0));
  rewriter.replaceOpWithNewOp<rtl::ConcatOp>(op, ValueRange({input, zero}));
  return success();
}

static LogicalResult lower(firrtl::ShrPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto input = mapOperand(operands[0], op, rewriter);
  if (!input)
    return failure();

  // Handle the special degenerate cases.
  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  auto shiftAmount = op.getAmount();
  if (shiftAmount == inWidth) {
    // Unsigned shift by full width returns zero.
    if (op.input().getType().cast<IntType>().isUnsigned()) {
      rewriter.replaceOpWithNewOp<rtl::ConstantOp>(op, APInt(1, 0));
      return success();
    }

    // Signed shift by full width is equivalent to extracting the sign bit.
    --shiftAmount;
  }

  Type resultType = rewriter.getIntegerType(inWidth - shiftAmount);
  rewriter.replaceOpWithNewOp<rtl::ExtractOp>(op, resultType, input,
                                              shiftAmount);
  return success();
}

static LogicalResult lower(firrtl::TailPrimOp op, ArrayRef<Value> operands,
                           ConversionPatternRewriter &rewriter) {
  auto input = mapOperand(operands[0], op, rewriter);
  if (!input)
    return failure();

  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  Type resultType = rewriter.getIntegerType(inWidth - op.getAmount());
  rewriter.replaceOpWithNewOp<rtl::ExtractOp>(op, resultType, input, 0);
  return success();
}

namespace {
/// Utility class for operation conversions targeting that
/// match exactly one source operation.
template <typename OpTy>
class RTLRewriter : public ConversionPattern {
public:
  RTLRewriter(MLIRContext *ctx)
      : ConversionPattern(OpTy::getOperationName(), /*benefit*/ 1, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    return lower(OpTy(op), operands, rewriter);
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// RTLConversionTarget
//===----------------------------------------------------------------------===//

namespace {
class RTLConversionTarget : public ConversionTarget {
public:
  explicit RTLConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
    addLegalDialect<RTLDialect>();
    addLegalOp<firrtl::StdIntCast>();
  }
};
} // end anonymous namespace

#define GEN_PASS_CLASSES
#include "circt/Dialect/FIRRTL/FIRRTLPasses.h.inc"

namespace {
struct FIRRTLLowering : public LowerFIRRTLToRTLBase<FIRRTLLowering> {
  /// Run the dialect converter on the FModule.
  void runOnOperation() override {
    OwningRewritePatternList patterns;
    patterns.insert<
        RTLRewriter<firrtl::ConstantOp>, RTLRewriter<firrtl::WireOp>,
        RTLRewriter<firrtl::ConnectOp>,
        // Binary Operations
        RTLRewriter<firrtl::AddPrimOp>, RTLRewriter<firrtl::SubPrimOp>,
        RTLRewriter<firrtl::AndPrimOp>, RTLRewriter<firrtl::OrPrimOp>,
        RTLRewriter<firrtl::XorPrimOp>, RTLRewriter<firrtl::CatPrimOp>,

        // Unary Operations
        RTLRewriter<firrtl::PadPrimOp>, RTLRewriter<firrtl::AsSIntPrimOp>,
        RTLRewriter<firrtl::AsUIntPrimOp>,

        // Other Operations
        RTLRewriter<firrtl::BitsPrimOp>, RTLRewriter<firrtl::HeadPrimOp>,
        RTLRewriter<firrtl::ShlPrimOp>, RTLRewriter<firrtl::ShrPrimOp>,
        RTLRewriter<firrtl::TailPrimOp>>(&getContext());

    RTLConversionTarget target(getContext());
    RTLTypeConverter typeConverter;
    if (failed(applyPartialConversion(getOperation(), target, patterns)))
      signalPassFailure();
  }
};
} // end anonymous namespace

std::unique_ptr<mlir::Pass> circt::firrtl::createLowerFIRRTLToRTLPass() {
  return std::make_unique<FIRRTLLowering>();
}
