/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for legalizing HLO to TensorFlow.

#include <memory>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/xla/ir/chlo_ops.h"
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.h"

namespace mlir {
namespace TF {
namespace {

class ConvertSliceOp : public OpConversionPattern<xla_hlo::SliceOp> {
 public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      xla_hlo::SliceOp slice_op, ArrayRef<Value> args,
      ConversionPatternRewriter &rewriter) const final {
    DenseIntElementsAttr strides = slice_op.strides();
    // Strides must be 1 otherwise we cannot legalize this `xla_hlo.slice` op.
    if (!strides.isSplat() ||
        strides.getSplatValue().cast<IntegerAttr>().getInt() != 1)
      return failure();

    rewriter.setInsertionPointAfter(slice_op);
    auto start_indices = slice_op.start_indices();
    auto limit_indices = slice_op.limit_indices();
    std::vector<int64_t> size_values;
    for (auto pair : llvm::zip(start_indices.getValues<APInt>(),
                               limit_indices.getValues<APInt>())) {
      size_values.emplace_back(std::get<1>(pair).getSExtValue() -
                               std::get<0>(pair).getSExtValue());
    }

    RankedTensorType ty =
        RankedTensorType::get({static_cast<int64_t>(size_values.size())},
                              rewriter.getIntegerType(64));
    auto start = rewriter.create<ConstOp>(slice_op.getLoc(), start_indices);
    auto size = rewriter.create<ConstOp>(
        slice_op.getLoc(), DenseIntElementsAttr::get(ty, size_values));
    rewriter.replaceOpWithNewOp<SliceOp>(slice_op, slice_op.getType(),
                                         slice_op.operand(), start, size);
    return success();
  };
};

class LegalizeHloToTf : public PassWrapper<LegalizeHloToTf, FunctionPass> {
 public:
  LegalizeHloToTf() = default;
  LegalizeHloToTf(const LegalizeHloToTf &) {}

  /// Performs the legalization to the TF dialect.
  void runOnFunction() override;
};

// Returns whether the two values are guaranteed to be broadcastable to the
// same shape, this broadcasts size 1 tensors up to any rank.
// TODO(jpienaar): Move this to more general location.
static bool AreBroadcastCompatible(Value x, Value y) {
  auto x_ranked = x.getType().dyn_cast<RankedTensorType>();
  auto y_ranked = y.getType().dyn_cast<RankedTensorType>();
  if (!x_ranked || !y_ranked) {
    return true;
  }
  SmallVector<int64_t, 4> resultShape;
  return OpTrait::util::getBroadcastedShape(x_ranked.getShape(),
                                            y_ranked.getShape(), resultShape);
}

// Returns the shape of the given value in a Constant Op.
ConstantOp ShapeToConst(PatternRewriter &rewriter, Value value) {
  ArrayRef<int64_t> shape = value.getType().cast<ShapedType>().getShape();
  auto attr_type = RankedTensorType::get({static_cast<int64_t>(shape.size())},
                                         rewriter.getIntegerType(64));
  auto attr = DenseElementsAttr::get(attr_type, shape);
  return rewriter.create<ConstantOp>(value.getLoc(), attr_type, attr);
}

// Converts xla_hlo.dot to tf.MatMul. Reshape ops will be inserted when
// necessary.
Value ConvertDotOp(PatternRewriter &rewriter, Operation *old_op) {
  auto dot_op = cast<xla_hlo::DotOp>(old_op);
  const mlir::Location loc = dot_op.getLoc();
  // Normalizes a ShapedType to 2d if the ShapedType is less than 2d by
  // inserting dummy 1-element dimensions in the begining. Does nothing if the
  // old shape is already 2d or higher. This is necessary because tf.MatMul
  // requires input tensors to be at least 2d.
  const auto normalize_rank = [](ShapedType type) -> ShapedType {
    if (type.getRank() >= 2) {
      return type;
    }

    const int rank = type.getRank();
    llvm::SmallVector<int64_t, 2> shape_2d(type.getShape().begin(),
                                           type.getShape().end());
    for (int i = 0; i < 2 - rank; ++i) {
      shape_2d.insert(shape_2d.begin(), 1);
    }
    return RankedTensorType::get(shape_2d, type.getElementType());
  };

  // Reshapes a tensor value to 2d if it is 1d or scalar. Otherwise does
  // nothing.
  const auto reshape_to_2d = [&rewriter, &loc,
                              &normalize_rank](mlir::Value input) {
    const auto input_type = input.getType().cast<ShapedType>();
    if (input_type.getRank() >= 2) {
      return input;
    }

    auto reshape = rewriter.create<xla_hlo::ReshapeOp>(
        loc, normalize_rank(input_type), input);
    return reshape.getResult();
  };

  // Reshapes both operand to be 2d for tf.MatMul op.
  auto a = reshape_to_2d(dot_op.lhs());
  auto b = reshape_to_2d(dot_op.rhs());
  // Operand `b` needs to be transposed if it is 1d. This is because dot op will
  // contract on the only dimension if rhs is 1d.
  auto b_old_type = dot_op.rhs().getType().cast<ShapedType>();
  BoolAttr transpose_b = rewriter.getBoolAttr(b_old_type.getRank() == 1);
  auto output_type = dot_op.getResult().getType().cast<ShapedType>();
  auto matmul = rewriter.create<TF::MatMulOp>(
      loc, normalize_rank(output_type), a, b,
      /*transpose_a=*/rewriter.getBoolAttr(false), transpose_b);
  auto reshape =
      rewriter.create<xla_hlo::ReshapeOp>(loc, output_type, matmul.product());
  return reshape.getResult();
}

#include "tensorflow/compiler/mlir/tensorflow/transforms/generated_legalize_hlo.inc"

/// Performs the lowering to XLA dialect.
void LegalizeHloToTf::runOnFunction() {
  MLIRContext &context = getContext();

  // Add legalization patterns to the list.
  OwningRewritePatternList patterns;
  populateWithGenerated(&context, &patterns);
  patterns.insert<ConvertSliceOp>(&context);

  ConversionTarget target(context);
  target.addLegalDialect<TensorFlowDialect>();
  target.addLegalOp<CallOp, ConstantOp>();
  if (failed(applyPartialConversion(getFunction(), target, patterns))) {
    getFunction().emitError("xla_hlo to TF legalization failed.");
    signalPassFailure();
  }
}

static PassRegistration<LegalizeHloToTf> pass(
    "tf-legalize-hlo", "Legalize from HLO to the TF dialect");

}  // end namespace

std::unique_ptr<OperationPass<FuncOp>> CreateLegalizeHloToTfPass() {
  return std::make_unique<LegalizeHloToTf>();
}

}  // end namespace TF
}  // end namespace mlir
