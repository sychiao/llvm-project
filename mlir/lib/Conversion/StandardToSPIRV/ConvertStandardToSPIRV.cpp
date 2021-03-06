//===- ConvertStandardToSPIRV.cpp - Standard to SPIR-V dialect conversion--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements patterns to convert standard ops to SPIR-V ops.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/SPIRV/LayoutUtils.h"
#include "mlir/Dialect/SPIRV/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/SPIRVLowering.h"
#include "mlir/Dialect/SPIRV/SPIRVOps.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "std-to-spirv-pattern"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//

/// Returns true if the given `type` is a boolean scalar or vector type.
static bool isBoolScalarOrVector(Type type) {
  if (type.isInteger(1))
    return true;
  if (auto vecType = type.dyn_cast<VectorType>())
    return vecType.getElementType().isInteger(1);
  return false;
}

/// Converts the given `srcAttr` into a boolean attribute if it holds an
/// integral value. Returns null attribute if conversion fails.
static BoolAttr convertBoolAttr(Attribute srcAttr, Builder builder) {
  if (auto boolAttr = srcAttr.dyn_cast<BoolAttr>())
    return boolAttr;
  if (auto intAttr = srcAttr.dyn_cast<IntegerAttr>())
    return builder.getBoolAttr(intAttr.getValue().getBoolValue());
  return BoolAttr();
}

/// Converts the given `srcAttr` to a new attribute of the given `dstType`.
/// Returns null attribute if conversion fails.
static IntegerAttr convertIntegerAttr(IntegerAttr srcAttr, IntegerType dstType,
                                      Builder builder) {
  // If the source number uses less active bits than the target bitwidth, then
  // it should be safe to convert.
  if (srcAttr.getValue().isIntN(dstType.getWidth()))
    return builder.getIntegerAttr(dstType, srcAttr.getInt());

  // XXX: Try again by interpreting the source number as a signed value.
  // Although integers in the standard dialect are signless, they can represent
  // a signed number. It's the operation decides how to interpret. This is
  // dangerous, but it seems there is no good way of handling this if we still
  // want to change the bitwidth. Emit a message at least.
  if (srcAttr.getValue().isSignedIntN(dstType.getWidth())) {
    auto dstAttr = builder.getIntegerAttr(dstType, srcAttr.getInt());
    LLVM_DEBUG(llvm::dbgs() << "attribute '" << srcAttr << "' converted to '"
                            << dstAttr << "' for type '" << dstType << "'\n");
    return dstAttr;
  }

  LLVM_DEBUG(llvm::dbgs() << "attribute '" << srcAttr
                          << "' illegal: cannot fit into target type '"
                          << dstType << "'\n");
  return IntegerAttr();
}

/// Converts the given `srcAttr` to a new attribute of the given `dstType`.
/// Returns null attribute if `dstType` is not 32-bit or conversion fails.
static FloatAttr convertFloatAttr(FloatAttr srcAttr, FloatType dstType,
                                  Builder builder) {
  // Only support converting to float for now.
  if (!dstType.isF32())
    return FloatAttr();

  // Try to convert the source floating-point number to single precision.
  APFloat dstVal = srcAttr.getValue();
  bool losesInfo = false;
  APFloat::opStatus status =
      dstVal.convert(APFloat::IEEEsingle(), APFloat::rmTowardZero, &losesInfo);
  if (status != APFloat::opOK || losesInfo) {
    LLVM_DEBUG(llvm::dbgs()
               << srcAttr << " illegal: cannot fit into converted type '"
               << dstType << "'\n");
    return FloatAttr();
  }

  return builder.getF32FloatAttr(dstVal.convertToFloat());
}

/// Returns the offset of the value in `targetBits` representation. `srcIdx` is
/// an index into a 1-D array with each element having `sourceBits`. When
/// accessing an element in the array treating as having elements of
/// `targetBits`, multiple values are loaded in the same time. The method
/// returns the offset where the `srcIdx` locates in the value. For example, if
/// `sourceBits` equals to 8 and `targetBits` equals to 32, the x-th element is
/// located at (x % 4) * 8. Because there are four elements in one i32, and one
/// element has 8 bits.
static Value getOffsetForBitwidth(Location loc, Value srcIdx, int sourceBits,
                                  int targetBits, OpBuilder &builder) {
  assert(targetBits % sourceBits == 0);
  IntegerType targetType = builder.getIntegerType(targetBits);
  IntegerAttr idxAttr =
      builder.getIntegerAttr(targetType, targetBits / sourceBits);
  auto idx = builder.create<spirv::ConstantOp>(loc, targetType, idxAttr);
  IntegerAttr srcBitsAttr = builder.getIntegerAttr(targetType, sourceBits);
  auto srcBitsValue =
      builder.create<spirv::ConstantOp>(loc, targetType, srcBitsAttr);
  auto m = builder.create<spirv::SModOp>(loc, srcIdx, idx);
  return builder.create<spirv::IMulOp>(loc, targetType, m, srcBitsValue);
}

/// Returns an adjusted spirv::AccessChainOp. Based on the
/// extension/capabilities, certain integer bitwidths `sourceBits` might not be
/// supported. During conversion if a memref of an unsupported type is used,
/// load/stores to this memref need to be modified to use a supported higher
/// bitwidth `targetBits` and extracting the required bits. For an accessing a
/// 1D array (spv.array or spv.rt_array), the last index is modified to load the
/// bits needed. The extraction of the actual bits needed are handled
/// separately. Note that this only works for a 1-D tensor.
static Value adjustAccessChainForBitwidth(SPIRVTypeConverter &typeConverter,
                                          spirv::AccessChainOp op,
                                          int sourceBits, int targetBits,
                                          OpBuilder &builder) {
  assert(targetBits % sourceBits == 0);
  const auto loc = op.getLoc();
  IntegerType targetType = builder.getIntegerType(targetBits);
  IntegerAttr attr =
      builder.getIntegerAttr(targetType, targetBits / sourceBits);
  auto idx = builder.create<spirv::ConstantOp>(loc, targetType, attr);
  auto lastDim = op.getOperation()->getOperand(op.getNumOperands() - 1);
  auto indices = llvm::to_vector<4>(op.indices());
  // There are two elements if this is a 1-D tensor.
  assert(indices.size() == 2);
  indices.back() = builder.create<spirv::SDivOp>(loc, lastDim, idx);
  Type t = typeConverter.convertType(op.component_ptr().getType());
  return builder.create<spirv::AccessChainOp>(loc, t, op.base_ptr(), indices);
}

/// Returns the shifted `targetBits`-bit value with the given offset.
Value shiftValue(Location loc, Value value, Value offset, Value mask,
                 int targetBits, OpBuilder &builder) {
  Type targetType = builder.getIntegerType(targetBits);
  Value result = builder.create<spirv::BitwiseAndOp>(loc, value, mask);
  return builder.create<spirv::ShiftLeftLogicalOp>(loc, targetType, result,
                                                   offset);
}

//===----------------------------------------------------------------------===//
// Operation conversion
//===----------------------------------------------------------------------===//

// Note that DRR cannot be used for the patterns in this file: we may need to
// convert type along the way, which requires ConversionPattern. DRR generates
// normal RewritePattern.

namespace {

/// Converts unary and binary standard operations to SPIR-V operations.
template <typename StdOp, typename SPIRVOp>
class UnaryAndBinaryOpPattern final : public SPIRVOpLowering<StdOp> {
public:
  using SPIRVOpLowering<StdOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(StdOp operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    assert(operands.size() <= 2);
    auto dstType = this->typeConverter.convertType(operation.getType());
    if (!dstType)
      return failure();
    rewriter.template replaceOpWithNewOp<SPIRVOp>(operation, dstType, operands,
                                                  ArrayRef<NamedAttribute>());
    return success();
  }
};

/// Converts bitwise standard operations to SPIR-V operations. This is a special
/// pattern other than the BinaryOpPatternPattern because if the operands are
/// boolean values, SPIR-V uses different operations (`SPIRVLogicalOp`). For
/// non-boolean operands, SPIR-V should use `SPIRVBitwiseOp`.
template <typename StdOp, typename SPIRVLogicalOp, typename SPIRVBitwiseOp>
class BitwiseOpPattern final : public SPIRVOpLowering<StdOp> {
public:
  using SPIRVOpLowering<StdOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(StdOp operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    assert(operands.size() == 2);
    auto dstType =
        this->typeConverter.convertType(operation.getResult().getType());
    if (!dstType)
      return failure();
    if (isBoolScalarOrVector(operands.front().getType())) {
      rewriter.template replaceOpWithNewOp<SPIRVLogicalOp>(
          operation, dstType, operands, ArrayRef<NamedAttribute>());
    } else {
      rewriter.template replaceOpWithNewOp<SPIRVBitwiseOp>(
          operation, dstType, operands, ArrayRef<NamedAttribute>());
    }
    return success();
  }
};

/// Converts composite std.constant operation to spv.constant.
class ConstantCompositeOpPattern final : public SPIRVOpLowering<ConstantOp> {
public:
  using SPIRVOpLowering<ConstantOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(ConstantOp constOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts scalar std.constant operation to spv.constant.
class ConstantScalarOpPattern final : public SPIRVOpLowering<ConstantOp> {
public:
  using SPIRVOpLowering<ConstantOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(ConstantOp constOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts floating-point comparison operations to SPIR-V ops.
class CmpFOpPattern final : public SPIRVOpLowering<CmpFOp> {
public:
  using SPIRVOpLowering<CmpFOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(CmpFOp cmpFOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts integer compare operation on i1 type opearnds to SPIR-V ops.
class BoolCmpIOpPattern final : public SPIRVOpLowering<CmpIOp> {
public:
  using SPIRVOpLowering<CmpIOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(CmpIOp cmpIOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts integer compare operation to SPIR-V ops.
class CmpIOpPattern final : public SPIRVOpLowering<CmpIOp> {
public:
  using SPIRVOpLowering<CmpIOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(CmpIOp cmpIOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts std.load to spv.Load.
class IntLoadOpPattern final : public SPIRVOpLowering<LoadOp> {
public:
  using SPIRVOpLowering<LoadOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(LoadOp loadOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts std.load to spv.Load.
class LoadOpPattern final : public SPIRVOpLowering<LoadOp> {
public:
  using SPIRVOpLowering<LoadOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(LoadOp loadOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts std.return to spv.Return.
class ReturnOpPattern final : public SPIRVOpLowering<ReturnOp> {
public:
  using SPIRVOpLowering<ReturnOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(ReturnOp returnOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts std.select to spv.Select.
class SelectOpPattern final : public SPIRVOpLowering<SelectOp> {
public:
  using SPIRVOpLowering<SelectOp>::SPIRVOpLowering;
  LogicalResult
  matchAndRewrite(SelectOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts std.store to spv.Store on integers.
class IntStoreOpPattern final : public SPIRVOpLowering<StoreOp> {
public:
  using SPIRVOpLowering<StoreOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(StoreOp storeOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts std.store to spv.Store.
class StoreOpPattern final : public SPIRVOpLowering<StoreOp> {
public:
  using SPIRVOpLowering<StoreOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(StoreOp storeOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Converts type-casting standard operations to SPIR-V operations.
template <typename StdOp, typename SPIRVOp>
class TypeCastingOpPattern final : public SPIRVOpLowering<StdOp> {
public:
  using SPIRVOpLowering<StdOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(StdOp operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    assert(operands.size() == 1);
    auto dstType =
        this->typeConverter.convertType(operation.getResult().getType());
    if (dstType == operands.front().getType()) {
      // Due to type conversion, we are seeing the same source and target type.
      // Then we can just erase this operation by forwarding its operand.
      rewriter.replaceOp(operation, operands.front());
    } else {
      rewriter.template replaceOpWithNewOp<SPIRVOp>(
          operation, dstType, operands, ArrayRef<NamedAttribute>());
    }
    return success();
  }
};

/// Converts std.xor to SPIR-V operations.
class XOrOpPattern final : public SPIRVOpLowering<XOrOp> {
public:
  using SPIRVOpLowering<XOrOp>::SPIRVOpLowering;

  LogicalResult
  matchAndRewrite(XOrOp xorOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

} // namespace

//===----------------------------------------------------------------------===//
// ConstantOp with composite type.
//===----------------------------------------------------------------------===//

LogicalResult ConstantCompositeOpPattern::matchAndRewrite(
    ConstantOp constOp, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  auto srcType = constOp.getType().dyn_cast<ShapedType>();
  if (!srcType)
    return failure();

  // std.constant should only have vector or tenor types.
  assert(srcType.isa<VectorType>() || srcType.isa<RankedTensorType>());

  auto dstType = typeConverter.convertType(srcType);
  if (!dstType)
    return failure();

  auto dstElementsAttr = constOp.value().dyn_cast<DenseElementsAttr>();
  ShapedType dstAttrType = dstElementsAttr.getType();
  if (!dstElementsAttr)
    return failure();

  // If the composite type has more than one dimensions, perform linearization.
  if (srcType.getRank() > 1) {
    if (srcType.isa<RankedTensorType>()) {
      dstAttrType = RankedTensorType::get(srcType.getNumElements(),
                                          srcType.getElementType());
      dstElementsAttr = dstElementsAttr.reshape(dstAttrType);
    } else {
      // TODO(antiagainst): add support for large vectors.
      return failure();
    }
  }

  Type srcElemType = srcType.getElementType();
  Type dstElemType;
  // Tensor types are converted to SPIR-V array types; vector types are
  // converted to SPIR-V vector/array types.
  if (auto arrayType = dstType.dyn_cast<spirv::ArrayType>())
    dstElemType = arrayType.getElementType();
  else
    dstElemType = dstType.cast<VectorType>().getElementType();

  // If the source and destination element types are different, perform
  // attribute conversion.
  if (srcElemType != dstElemType) {
    SmallVector<Attribute, 8> elements;
    if (srcElemType.isa<FloatType>()) {
      for (Attribute srcAttr : dstElementsAttr.getAttributeValues()) {
        FloatAttr dstAttr = convertFloatAttr(
            srcAttr.cast<FloatAttr>(), dstElemType.cast<FloatType>(), rewriter);
        if (!dstAttr)
          return failure();
        elements.push_back(dstAttr);
      }
    } else if (srcElemType.isInteger(1)) {
      return failure();
    } else {
      for (Attribute srcAttr : dstElementsAttr.getAttributeValues()) {
        IntegerAttr dstAttr =
            convertIntegerAttr(srcAttr.cast<IntegerAttr>(),
                               dstElemType.cast<IntegerType>(), rewriter);
        if (!dstAttr)
          return failure();
        elements.push_back(dstAttr);
      }
    }

    // Unfortunately, we cannot use dialect-specific types for element
    // attributes; element attributes only works with standard types. So we need
    // to prepare another converted standard types for the destination elements
    // attribute.
    if (dstAttrType.isa<RankedTensorType>())
      dstAttrType = RankedTensorType::get(dstAttrType.getShape(), dstElemType);
    else
      dstAttrType = VectorType::get(dstAttrType.getShape(), dstElemType);

    dstElementsAttr = DenseElementsAttr::get(dstAttrType, elements);
  }

  rewriter.replaceOpWithNewOp<spirv::ConstantOp>(constOp, dstType,
                                                 dstElementsAttr);
  return success();
}

//===----------------------------------------------------------------------===//
// ConstantOp with scalar type.
//===----------------------------------------------------------------------===//

LogicalResult ConstantScalarOpPattern::matchAndRewrite(
    ConstantOp constOp, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  Type srcType = constOp.getType();
  if (!srcType.isIntOrIndexOrFloat())
    return failure();

  Type dstType = typeConverter.convertType(srcType);
  if (!dstType)
    return failure();

  // Floating-point types.
  if (srcType.isa<FloatType>()) {
    auto srcAttr = constOp.value().cast<FloatAttr>();
    auto dstAttr = srcAttr;

    // Floating-point types not supported in the target environment are all
    // converted to float type.
    if (srcType != dstType) {
      dstAttr = convertFloatAttr(srcAttr, dstType.cast<FloatType>(), rewriter);
      if (!dstAttr)
        return failure();
    }

    rewriter.replaceOpWithNewOp<spirv::ConstantOp>(constOp, dstType, dstAttr);
    return success();
  }

  // Bool type.
  if (srcType.isInteger(1)) {
    // std.constant can use 0/1 instead of true/false for i1 values. We need to
    // handle that here.
    auto dstAttr = convertBoolAttr(constOp.value(), rewriter);
    if (!dstAttr)
      return failure();
    rewriter.replaceOpWithNewOp<spirv::ConstantOp>(constOp, dstType, dstAttr);
    return success();
  }

  // IndexType or IntegerType. Index values are converted to 32-bit integer
  // values when converting to SPIR-V.
  auto srcAttr = constOp.value().cast<IntegerAttr>();
  auto dstAttr =
      convertIntegerAttr(srcAttr, dstType.cast<IntegerType>(), rewriter);
  if (!dstAttr)
    return failure();
  rewriter.replaceOpWithNewOp<spirv::ConstantOp>(constOp, dstType, dstAttr);
  return success();
}

//===----------------------------------------------------------------------===//
// CmpFOp
//===----------------------------------------------------------------------===//

LogicalResult
CmpFOpPattern::matchAndRewrite(CmpFOp cmpFOp, ArrayRef<Value> operands,
                               ConversionPatternRewriter &rewriter) const {
  CmpFOpOperandAdaptor cmpFOpOperands(operands);

  switch (cmpFOp.getPredicate()) {
#define DISPATCH(cmpPredicate, spirvOp)                                        \
  case cmpPredicate:                                                           \
    rewriter.replaceOpWithNewOp<spirvOp>(cmpFOp, cmpFOp.getResult().getType(), \
                                         cmpFOpOperands.lhs(),                 \
                                         cmpFOpOperands.rhs());                \
    return success();

    // Ordered.
    DISPATCH(CmpFPredicate::OEQ, spirv::FOrdEqualOp);
    DISPATCH(CmpFPredicate::OGT, spirv::FOrdGreaterThanOp);
    DISPATCH(CmpFPredicate::OGE, spirv::FOrdGreaterThanEqualOp);
    DISPATCH(CmpFPredicate::OLT, spirv::FOrdLessThanOp);
    DISPATCH(CmpFPredicate::OLE, spirv::FOrdLessThanEqualOp);
    DISPATCH(CmpFPredicate::ONE, spirv::FOrdNotEqualOp);
    // Unordered.
    DISPATCH(CmpFPredicate::UEQ, spirv::FUnordEqualOp);
    DISPATCH(CmpFPredicate::UGT, spirv::FUnordGreaterThanOp);
    DISPATCH(CmpFPredicate::UGE, spirv::FUnordGreaterThanEqualOp);
    DISPATCH(CmpFPredicate::ULT, spirv::FUnordLessThanOp);
    DISPATCH(CmpFPredicate::ULE, spirv::FUnordLessThanEqualOp);
    DISPATCH(CmpFPredicate::UNE, spirv::FUnordNotEqualOp);

#undef DISPATCH

  default:
    break;
  }
  return failure();
}

//===----------------------------------------------------------------------===//
// CmpIOp
//===----------------------------------------------------------------------===//

LogicalResult
BoolCmpIOpPattern::matchAndRewrite(CmpIOp cmpIOp, ArrayRef<Value> operands,
                                   ConversionPatternRewriter &rewriter) const {
  CmpIOpOperandAdaptor cmpIOpOperands(operands);

  Type operandType = cmpIOp.lhs().getType();
  if (!operandType.isa<IntegerType>() ||
      operandType.cast<IntegerType>().getWidth() != 1)
    return failure();

  switch (cmpIOp.getPredicate()) {
#define DISPATCH(cmpPredicate, spirvOp)                                        \
  case cmpPredicate:                                                           \
    rewriter.replaceOpWithNewOp<spirvOp>(cmpIOp, cmpIOp.getResult().getType(), \
                                         cmpIOpOperands.lhs(),                 \
                                         cmpIOpOperands.rhs());                \
    return success();

    DISPATCH(CmpIPredicate::eq, spirv::LogicalEqualOp);
    DISPATCH(CmpIPredicate::ne, spirv::LogicalNotEqualOp);

#undef DISPATCH
  default:;
  }
  return failure();
}

LogicalResult
CmpIOpPattern::matchAndRewrite(CmpIOp cmpIOp, ArrayRef<Value> operands,
                               ConversionPatternRewriter &rewriter) const {
  CmpIOpOperandAdaptor cmpIOpOperands(operands);

  Type operandType = cmpIOp.lhs().getType();
  if (operandType.isa<IntegerType>() &&
      operandType.cast<IntegerType>().getWidth() == 1)
    return failure();

  switch (cmpIOp.getPredicate()) {
#define DISPATCH(cmpPredicate, spirvOp)                                        \
  case cmpPredicate:                                                           \
    rewriter.replaceOpWithNewOp<spirvOp>(cmpIOp, cmpIOp.getResult().getType(), \
                                         cmpIOpOperands.lhs(),                 \
                                         cmpIOpOperands.rhs());                \
    return success();

    DISPATCH(CmpIPredicate::eq, spirv::IEqualOp);
    DISPATCH(CmpIPredicate::ne, spirv::INotEqualOp);
    DISPATCH(CmpIPredicate::slt, spirv::SLessThanOp);
    DISPATCH(CmpIPredicate::sle, spirv::SLessThanEqualOp);
    DISPATCH(CmpIPredicate::sgt, spirv::SGreaterThanOp);
    DISPATCH(CmpIPredicate::sge, spirv::SGreaterThanEqualOp);
    DISPATCH(CmpIPredicate::ult, spirv::ULessThanOp);
    DISPATCH(CmpIPredicate::ule, spirv::ULessThanEqualOp);
    DISPATCH(CmpIPredicate::ugt, spirv::UGreaterThanOp);
    DISPATCH(CmpIPredicate::uge, spirv::UGreaterThanEqualOp);

#undef DISPATCH
  }
  return failure();
}

//===----------------------------------------------------------------------===//
// LoadOp
//===----------------------------------------------------------------------===//

LogicalResult
IntLoadOpPattern::matchAndRewrite(LoadOp loadOp, ArrayRef<Value> operands,
                                  ConversionPatternRewriter &rewriter) const {
  LoadOpOperandAdaptor loadOperands(operands);
  auto loc = loadOp.getLoc();
  auto memrefType = loadOp.memref().getType().cast<MemRefType>();
  if (!memrefType.getElementType().isSignlessInteger())
    return failure();
  spirv::AccessChainOp accessChainOp =
      spirv::getElementPtr(typeConverter, memrefType, loadOperands.memref(),
                           loadOperands.indices(), loc, rewriter);

  int srcBits = memrefType.getElementType().getIntOrFloatBitWidth();
  auto dstType = typeConverter.convertType(memrefType)
                     .cast<spirv::PointerType>()
                     .getPointeeType()
                     .cast<spirv::StructType>()
                     .getElementType(0)
                     .cast<spirv::ArrayType>()
                     .getElementType();
  int dstBits = dstType.getIntOrFloatBitWidth();
  assert(dstBits % srcBits == 0);

  // If the rewrited load op has the same bit width, use the loading value
  // directly.
  if (srcBits == dstBits) {
    rewriter.replaceOpWithNewOp<spirv::LoadOp>(loadOp,
                                               accessChainOp.getResult());
    return success();
  }

  // Assume that getElementPtr() works linearizely. If it's a scalar, the method
  // still returns a linearized accessing. If the accessing is not linearized,
  // there will be offset issues.
  assert(accessChainOp.indices().size() == 2);
  Value adjustedPtr = adjustAccessChainForBitwidth(typeConverter, accessChainOp,
                                                   srcBits, dstBits, rewriter);
  Value spvLoadOp = rewriter.create<spirv::LoadOp>(
      loc, dstType, adjustedPtr,
      loadOp.getAttrOfType<IntegerAttr>(
          spirv::attributeName<spirv::MemoryAccess>()),
      loadOp.getAttrOfType<IntegerAttr>("alignment"));

  // Shift the bits to the rightmost.
  // ____XXXX________ -> ____________XXXX
  Value lastDim = accessChainOp.getOperation()->getOperand(
      accessChainOp.getNumOperands() - 1);
  Value offset = getOffsetForBitwidth(loc, lastDim, srcBits, dstBits, rewriter);
  Value result = rewriter.create<spirv::ShiftRightArithmeticOp>(
      loc, spvLoadOp.getType(), spvLoadOp, offset);

  // Apply the mask to extract corresponding bits.
  Value mask = rewriter.create<spirv::ConstantOp>(
      loc, dstType, rewriter.getIntegerAttr(dstType, (1 << srcBits) - 1));
  result = rewriter.create<spirv::BitwiseAndOp>(loc, dstType, result, mask);
  rewriter.replaceOp(loadOp, result);

  assert(accessChainOp.use_empty());
  rewriter.eraseOp(accessChainOp);

  return success();
}

LogicalResult
LoadOpPattern::matchAndRewrite(LoadOp loadOp, ArrayRef<Value> operands,
                               ConversionPatternRewriter &rewriter) const {
  LoadOpOperandAdaptor loadOperands(operands);
  auto memrefType = loadOp.memref().getType().cast<MemRefType>();
  if (memrefType.getElementType().isSignlessInteger())
    return failure();
  auto loadPtr =
      spirv::getElementPtr(typeConverter, memrefType, loadOperands.memref(),
                           loadOperands.indices(), loadOp.getLoc(), rewriter);
  rewriter.replaceOpWithNewOp<spirv::LoadOp>(loadOp, loadPtr);
  return success();
}

//===----------------------------------------------------------------------===//
// ReturnOp
//===----------------------------------------------------------------------===//

LogicalResult
ReturnOpPattern::matchAndRewrite(ReturnOp returnOp, ArrayRef<Value> operands,
                                 ConversionPatternRewriter &rewriter) const {
  if (returnOp.getNumOperands()) {
    return failure();
  }
  rewriter.replaceOpWithNewOp<spirv::ReturnOp>(returnOp);
  return success();
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

LogicalResult
SelectOpPattern::matchAndRewrite(SelectOp op, ArrayRef<Value> operands,
                                 ConversionPatternRewriter &rewriter) const {
  SelectOpOperandAdaptor selectOperands(operands);
  rewriter.replaceOpWithNewOp<spirv::SelectOp>(op, selectOperands.condition(),
                                               selectOperands.true_value(),
                                               selectOperands.false_value());
  return success();
}

//===----------------------------------------------------------------------===//
// StoreOp
//===----------------------------------------------------------------------===//

LogicalResult
IntStoreOpPattern::matchAndRewrite(StoreOp storeOp, ArrayRef<Value> operands,
                                   ConversionPatternRewriter &rewriter) const {
  StoreOpOperandAdaptor storeOperands(operands);
  auto memrefType = storeOp.memref().getType().cast<MemRefType>();
  if (!memrefType.getElementType().isSignlessInteger())
    return failure();

  auto loc = storeOp.getLoc();
  spirv::AccessChainOp accessChainOp =
      spirv::getElementPtr(typeConverter, memrefType, storeOperands.memref(),
                           storeOperands.indices(), loc, rewriter);
  int srcBits = memrefType.getElementType().getIntOrFloatBitWidth();
  auto dstType = typeConverter.convertType(memrefType)
                     .cast<spirv::PointerType>()
                     .getPointeeType()
                     .cast<spirv::StructType>()
                     .getElementType(0)
                     .cast<spirv::ArrayType>()
                     .getElementType();
  int dstBits = dstType.getIntOrFloatBitWidth();
  assert(dstBits % srcBits == 0);

  if (srcBits == dstBits) {
    rewriter.replaceOpWithNewOp<spirv::StoreOp>(
        storeOp, accessChainOp.getResult(), storeOperands.value());
    return success();
  }

  // Since there are multi threads in the processing, the emulation will be done
  // with atomic operations. E.g., if the storing value is i8, rewrite the
  // StoreOp to
  // 1) load a 32-bit integer
  // 2) clear 8 bits in the loading value
  // 3) store 32-bit value back
  // 4) load a 32-bit integer
  // 5) modify 8 bits in the loading value
  // 6) store 32-bit value back
  // The step 1 to step 3 are done by AtomicAnd as one atomic step, and the step
  // 4 to step 6 are done by AtomicOr as another atomic step.
  assert(accessChainOp.indices().size() == 2);
  Value lastDim = accessChainOp.getOperation()->getOperand(
      accessChainOp.getNumOperands() - 1);
  Value offset = getOffsetForBitwidth(loc, lastDim, srcBits, dstBits, rewriter);

  // Create a mask to clear the destination. E.g., if it is the second i8 in
  // i32, 0xFFFF00FF is created.
  Value mask = rewriter.create<spirv::ConstantOp>(
      loc, dstType, rewriter.getIntegerAttr(dstType, (1 << srcBits) - 1));
  Value clearBitsMask =
      rewriter.create<spirv::ShiftLeftLogicalOp>(loc, dstType, mask, offset);
  clearBitsMask = rewriter.create<spirv::NotOp>(loc, dstType, clearBitsMask);

  Value storeVal =
      shiftValue(loc, storeOperands.value(), offset, mask, dstBits, rewriter);
  Value adjustedPtr = adjustAccessChainForBitwidth(typeConverter, accessChainOp,
                                                   srcBits, dstBits, rewriter);
  Value result = rewriter.create<spirv::AtomicAndOp>(
      loc, dstType, adjustedPtr, spirv::Scope::Device,
      spirv::MemorySemantics::AcquireRelease, clearBitsMask);
  result = rewriter.create<spirv::AtomicOrOp>(
      loc, dstType, adjustedPtr, spirv::Scope::Device,
      spirv::MemorySemantics::AcquireRelease, storeVal);

  // The AtomicOrOp has no side effect. Since it is already inserted, we can
  // just remove the original StoreOp. Note that rewriter.replaceOp()
  // doesn't work because it only accepts that the numbers of result are the
  // same.
  rewriter.eraseOp(storeOp);

  assert(accessChainOp.use_empty());
  rewriter.eraseOp(accessChainOp);

  return success();
}

LogicalResult
StoreOpPattern::matchAndRewrite(StoreOp storeOp, ArrayRef<Value> operands,
                                ConversionPatternRewriter &rewriter) const {
  StoreOpOperandAdaptor storeOperands(operands);
  auto memrefType = storeOp.memref().getType().cast<MemRefType>();
  if (memrefType.getElementType().isSignlessInteger())
    return failure();
  auto storePtr =
      spirv::getElementPtr(typeConverter, memrefType, storeOperands.memref(),
                           storeOperands.indices(), storeOp.getLoc(), rewriter);
  rewriter.replaceOpWithNewOp<spirv::StoreOp>(storeOp, storePtr,
                                              storeOperands.value());
  return success();
}

//===----------------------------------------------------------------------===//
// XorOp
//===----------------------------------------------------------------------===//

LogicalResult
XOrOpPattern::matchAndRewrite(XOrOp xorOp, ArrayRef<Value> operands,
                              ConversionPatternRewriter &rewriter) const {
  assert(operands.size() == 2);

  if (isBoolScalarOrVector(operands.front().getType()))
    return failure();

  auto dstType = typeConverter.convertType(xorOp.getType());
  if (!dstType)
    return failure();
  rewriter.replaceOpWithNewOp<spirv::BitwiseXorOp>(xorOp, dstType, operands,
                                                   ArrayRef<NamedAttribute>());

  return success();
}

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

namespace mlir {
void populateStandardToSPIRVPatterns(MLIRContext *context,
                                     SPIRVTypeConverter &typeConverter,
                                     OwningRewritePatternList &patterns) {
  patterns.insert<
      UnaryAndBinaryOpPattern<AbsFOp, spirv::GLSLFAbsOp>,
      UnaryAndBinaryOpPattern<AddFOp, spirv::FAddOp>,
      UnaryAndBinaryOpPattern<AddIOp, spirv::IAddOp>,
      UnaryAndBinaryOpPattern<CeilFOp, spirv::GLSLCeilOp>,
      UnaryAndBinaryOpPattern<CosOp, spirv::GLSLCosOp>,
      UnaryAndBinaryOpPattern<DivFOp, spirv::FDivOp>,
      UnaryAndBinaryOpPattern<ExpOp, spirv::GLSLExpOp>,
      UnaryAndBinaryOpPattern<LogOp, spirv::GLSLLogOp>,
      UnaryAndBinaryOpPattern<MulFOp, spirv::FMulOp>,
      UnaryAndBinaryOpPattern<MulIOp, spirv::IMulOp>,
      UnaryAndBinaryOpPattern<NegFOp, spirv::FNegateOp>,
      UnaryAndBinaryOpPattern<RemFOp, spirv::FRemOp>,
      UnaryAndBinaryOpPattern<RsqrtOp, spirv::GLSLInverseSqrtOp>,
      UnaryAndBinaryOpPattern<ShiftLeftOp, spirv::ShiftLeftLogicalOp>,
      UnaryAndBinaryOpPattern<SignedDivIOp, spirv::SDivOp>,
      UnaryAndBinaryOpPattern<SignedRemIOp, spirv::SRemOp>,
      UnaryAndBinaryOpPattern<SignedShiftRightOp,
                              spirv::ShiftRightArithmeticOp>,
      UnaryAndBinaryOpPattern<SinOp, spirv::GLSLSinOp>,
      UnaryAndBinaryOpPattern<SqrtOp, spirv::GLSLSqrtOp>,
      UnaryAndBinaryOpPattern<SubFOp, spirv::FSubOp>,
      UnaryAndBinaryOpPattern<SubIOp, spirv::ISubOp>,
      UnaryAndBinaryOpPattern<TanhOp, spirv::GLSLTanhOp>,
      UnaryAndBinaryOpPattern<UnsignedDivIOp, spirv::UDivOp>,
      UnaryAndBinaryOpPattern<UnsignedRemIOp, spirv::UModOp>,
      UnaryAndBinaryOpPattern<UnsignedShiftRightOp, spirv::ShiftRightLogicalOp>,
      BitwiseOpPattern<AndOp, spirv::LogicalAndOp, spirv::BitwiseAndOp>,
      BitwiseOpPattern<OrOp, spirv::LogicalOrOp, spirv::BitwiseOrOp>,
      BoolCmpIOpPattern, ConstantCompositeOpPattern, ConstantScalarOpPattern,
      CmpFOpPattern, CmpIOpPattern, IntLoadOpPattern, LoadOpPattern,
      ReturnOpPattern, SelectOpPattern, IntStoreOpPattern, StoreOpPattern,
      TypeCastingOpPattern<IndexCastOp, spirv::SConvertOp>,
      TypeCastingOpPattern<SIToFPOp, spirv::ConvertSToFOp>,
      TypeCastingOpPattern<ZeroExtendIOp, spirv::UConvertOp>,
      TypeCastingOpPattern<TruncateIOp, spirv::SConvertOp>,
      TypeCastingOpPattern<FPToSIOp, spirv::ConvertFToSOp>,
      TypeCastingOpPattern<FPExtOp, spirv::FConvertOp>,
      TypeCastingOpPattern<FPTruncOp, spirv::FConvertOp>, XOrOpPattern>(
      context, typeConverter);
}
} // namespace mlir
