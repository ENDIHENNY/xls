// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/passes/arith_simplification_pass.h"

#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/node_util.h"
#include "xls/ir/nodes.h"
#include "xls/ir/value_helpers.h"

namespace xls {
namespace {

// For the given comparison Op, returns the op op_inverse for which the
// following identity holds:
//   op(x, y) == !op_inverse(x, y)
Op CompareOpInverse(Op op) {
  switch (op) {
    case Op::kEq:
      return Op::kNe;
    case Op::kNe:
      return Op::kEq;
    case Op::kSGe:
      return Op::kSLt;
    case Op::kUGe:
      return Op::kULt;
    case Op::kSGt:
      return Op::kSLe;
    case Op::kUGt:
      return Op::kULe;
    case Op::kSLe:
      return Op::kSGt;
    case Op::kULe:
      return Op::kUGt;
    case Op::kSLt:
      return Op::kSGe;
    case Op::kULt:
      return Op::kUGe;
    default:
      XLS_LOG(FATAL) << "Op is not comparison: " << OpToString(op);
  }
}

// MatchArithPatterns matches simple tree patterns to find opportunities
// for simplification, such as adding a zero, multiplying by 1, etc.
//
// Return 'true' if the IR was modified.
absl::StatusOr<bool> MatchArithPatterns(Node* n) {
  absl::Span<Node* const> ops = n->operands();

  // Pattern: Add/Sub/Or/Xor/Shift a value with 0 on the RHS.
  if ((n->op() == Op::kAdd || n->op() == Op::kSub || n->op() == Op::kShll ||
       n->op() == Op::kShrl || n->op() == Op::kShra) &&
      IsLiteralZero(ops[1])) {
    XLS_VLOG(2) << "FOUND: Useless operation of value with zero";
    return n->ReplaceUsesWith(ops[0]);
  }

  const Op op = n->op();
  if (n->Is<NaryOp>() && (op == Op::kAnd || op == Op::kOr || op == Op::kXor) &&
      ops.size() == 1) {
    return n->ReplaceUsesWith(ops[0]);
  }

  // Replaces uses of n with a new node by eliminating operands for which the
  // "predicate" holds. If the predicate holds for all operands, the
  // NaryOpNullaryResult is used as a replacement.
  auto eliminate_operands_where =
      [n](std::function<bool(Node*)> predicate) -> absl::StatusOr<bool> {
    XLS_RET_CHECK(n->Is<NaryOp>());
    std::vector<Node*> new_operands;
    for (Node* operand : n->operands()) {
      if (!predicate(operand)) {
        new_operands.push_back(operand);
      }
    }
    if (new_operands.size() == n->operand_count()) {
      return false;
    }
    if (new_operands.empty()) {
      XLS_RETURN_IF_ERROR(
          n
              ->ReplaceUsesWithNew<Literal>(Value(DoLogicalOp(
                  n->op(), {LogicalOpIdentity(n->op(), n->BitCountOrDie())})))
              .status());
    } else {
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<NaryOp>(new_operands, n->op()).status());
    }
    return true;
  };

  // Or(x, 0, y) => Or(x, y)
  // Xor(x, 0, y) => Xor(x, y)
  // Nor(x, 0, y) => Nor(x, y)
  if ((n->op() == Op::kOr || n->op() == Op::kXor || n->op() == Op::kNor)) {
    XLS_ASSIGN_OR_RETURN(bool changed, eliminate_operands_where(IsLiteralZero));
    if (changed) {
      return true;
    }
  }

  // Or(x, -1, y) => -1
  if (n->op() == Op::kOr && AnyOperandWhere(n, IsLiteralAllOnes)) {
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(AllOnesOfType(n->GetType())).status());
    return true;
  }

  // Nor(x, -1, y) => 0
  if (n->op() == Op::kNor && AnyOperandWhere(n, IsLiteralAllOnes)) {
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(ZeroOfType(n->GetType())).status());
    return true;
  }

  // And(x, -1, y) => And(x, y)
  // Nand(x, -1, y) => Nand(x, y)
  if (n->op() == Op::kAnd || n->op() == Op::kNand) {
    XLS_ASSIGN_OR_RETURN(bool changed,
                         eliminate_operands_where(IsLiteralAllOnes));
    if (changed) {
      return true;
    }
  }

  // And(x, 0) => 0
  if (n->op() == Op::kAnd && AnyOperandWhere(n, IsLiteralZero)) {
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(ZeroOfType(n->GetType())).status());
    return true;
  }

  // And(x, x) => x
  // Or(x, x) => x
  // TODO(rhundt): This should be subsumed by BDD-based optimizations.
  if (n->op() == Op::kAnd || n->op() == Op::kOr) {
    bool all_the_same = true;
    for (int i = 1; i < ops.size(); ++i) {
      if (ops[0] != ops[i]) {
        all_the_same = false;
        break;
      }
    }
    if (all_the_same) {
      XLS_RETURN_IF_ERROR(n->ReplaceUsesWith(ops[0]).status());
      return true;
    }
  }

  // Nand(x, 0) => 1
  if (n->op() == Op::kNand && AnyOperandWhere(n, IsLiteralZero)) {
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(AllOnesOfType(n->GetType())).status());
    return true;
  }

  auto has_inverted_operand = [&] {
    for (Node* operand : ops) {
      if (operand->op() == Op::kNot &&
          std::find(ops.begin(), ops.end(), operand->operand(0)) != ops.end()) {
        return true;
      }
    }
    return false;
  };

  // And(x, Not(x)) => 0
  // And(Not(x), x) => 0
  //
  // Note that this won't be found through the ternary query engine because
  // conservatively it determines `not(UNKNOWN) = UNKNOWN`.
  if (n->op() == Op::kAnd && has_inverted_operand()) {
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(Value(UBits(0, n->BitCountOrDie())))
            .status());
    return true;
  }

  // Xor(x, -1) => Not(x)
  if (n->op() == Op::kXor && n->operand_count() == 2 &&
      IsLiteralAllOnes(ops[1])) {
    XLS_VLOG(2) << "FOUND: Found xor with all ones";
    XLS_RETURN_IF_ERROR(n->ReplaceUsesWithNew<UnOp>(ops[0], Op::kNot).status());
    return true;
  }

  auto is_same_opcode = [&](Node* other) { return n->op() == other->op(); };

  // Flatten nested associative nary ops into their "dependent" op.
  if (OpIsAssociative(n->op()) && n->Is<NaryOp>() &&
      AnyOperandWhere(n, is_same_opcode)) {
    std::vector<Node*> new_operands;
    for (Node* operand : ops) {
      if (operand->op() == n->op()) {
        for (Node* suboperand : operand->operands()) {
          new_operands.push_back(suboperand);
        }
      } else {
        new_operands.push_back(operand);
      }
    }
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<NaryOp>(new_operands, n->op()).status());
    return true;
  }

  // Fold the literal values presented to the nary op.
  if (OpIsCommutative(n->op()) && OpIsAssociative(n->op()) && n->Is<NaryOp>() &&
      AnyTwoOperandsWhere(n, IsLiteral)) {
    std::vector<Node*> new_operands;
    Bits bits = LogicalOpIdentity(n->op(), n->BitCountOrDie());
    for (Node* operand : ops) {
      if (operand->Is<Literal>()) {
        bits = DoLogicalOp(n->op(),
                           {bits, operand->As<Literal>()->value().bits()});
      } else {
        new_operands.push_back(operand);
      }
    }
    Function* f = n->function();
    XLS_ASSIGN_OR_RETURN(Node * literal,
                         f->MakeNode<Literal>(n->loc(), Value(bits)));
    new_operands.push_back(literal);
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<NaryOp>(new_operands, n->op()).status());
    return true;
  }

  // Replace Nary ops with a single-operand with their operand (or inverse in
  // the case of nand and nor).
  if (n->Is<NaryOp>() && n->operand_count() == 1) {
    switch (n->op()) {
      case Op::kAnd:
      case Op::kOr:
      case Op::kXor:
        return n->ReplaceUsesWith(n->operand(0));
      case Op::kNand:
      case Op::kNor:
        XLS_RETURN_IF_ERROR(
            n->ReplaceUsesWithNew<UnOp>(n->operand(0), Op::kNot).status());
        break;
      default:
        XLS_LOG(FATAL) << "Expected nary op op: " << OpToString(n->op());
    }
    return true;
  }

  // Returns a value which is extended or narrowed to the given bit count.
  // Possible cases:
  //
  //  (1) node width == bit_count: return node
  //
  //  (2) node width < bit_count: return node truncated to bit_count
  //
  //  (3) node width > bit_count and is_signed: return node sign-extended to
  //  bit_count
  //
  //  (4) node width > bit_count and !is_signed: return node zero-extended to
  //  bit_count
  auto maybe_extend_or_trunc = [](Node* node, int64 bit_count,
                                  bool is_signed) -> absl::StatusOr<Node*> {
    if (node->BitCountOrDie() == bit_count) {
      return node;
    }
    if (node->BitCountOrDie() > bit_count) {
      return node->function()->MakeNode<BitSlice>(node->loc(), node,
                                                  /*start=*/0,
                                                  /*width=*/bit_count);
    }
    return node->function()->MakeNode<ExtendOp>(
        node->loc(), node,
        /*new_bit_count=*/bit_count, is_signed ? Op::kSignExt : Op::kZeroExt);
  };

  // Pattern: Mul or div by 1
  if ((n->op() == Op::kSMul && IsLiteralSignedOne(ops[1])) ||
      ((n->op() == Op::kUMul || n->op() == Op::kUDiv) &&
       IsLiteralUnsignedOne(ops[1]))) {
    XLS_VLOG(2) << "FOUND: Mul/Div by 1";
    XLS_ASSIGN_OR_RETURN(
        Node * replacement,
        maybe_extend_or_trunc(ops[0], n->BitCountOrDie(),
                              /*is_signed=*/n->op() == Op::kSMul));
    return n->ReplaceUsesWith(replacement);
  }

  // Pattern: Mul by 0
  if ((n->op() == Op::kSMul || n->op() == Op::kUMul) && IsLiteralZero(ops[1])) {
    XLS_VLOG(2) << "FOUND: Mul by 0";
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(Value(UBits(0, n->BitCountOrDie())))
            .status());
    return true;
  }

  // Pattern: Not(Not(x)) => x
  if (n->op() == Op::kNot && ops[0]->op() == Op::kNot) {
    return n->ReplaceUsesWith(ops[0]->operand(0));
  }

  // Logical shift by a constant can be replaced by a slice and concat.
  //    (val << lit) -> Concat(BitSlice(val, ...), UBits(0, ...))
  //    (val >> lit) -> Concat(UBits(0, ...), BitSlice(val, ...))
  // If the shift amount is greater than or equal to the bit width the
  // expression can be replaced with zero.
  //
  // This simplification is desirable as in the canonical lower-level IR a shift
  // implies a barrel shifter which is not necessary for a shift by a constant
  // amount.
  if ((n->op() == Op::kShll || n->op() == Op::kShrl) && ops[1]->Is<Literal>()) {
    int64 bit_count = n->BitCountOrDie();
    const Bits& shift_bits = ops[1]->As<Literal>()->value().bits();
    if (shift_bits.IsAllZeros()) {
      // A shift by zero is a nop.
      return n->ReplaceUsesWith(ops[0]);
    }
    if (bits_ops::UGreaterThanOrEqual(shift_bits, bit_count)) {
      // Replace with zero.
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<Literal>(Value(UBits(0, bit_count))).status());
      return true;
    }
    XLS_ASSIGN_OR_RETURN(uint64 shift_amount, shift_bits.ToUint64());
    XLS_ASSIGN_OR_RETURN(Node * zero,
                         n->function()->MakeNode<Literal>(
                             n->loc(), Value(UBits(0, shift_amount))));
    int64 slice_start = (n->op() == Op::kShll) ? 0 : shift_amount;
    int64 slice_width = bit_count - shift_amount;
    XLS_ASSIGN_OR_RETURN(Node * slice,
                         n->function()->MakeNode<BitSlice>(
                             n->loc(), ops[0], slice_start, slice_width));
    auto concat_operands = (n->op() == Op::kShll)
                               ? std::vector<Node*>{slice, zero}
                               : std::vector<Node*>{zero, slice};
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Concat>(concat_operands).status());
    return true;
  }

  // SignExt(SignExt(x, w_0), w_1) => SignExt(x, w_1)
  if (n->op() == Op::kSignExt && ops[0]->op() == Op::kSignExt) {
    XLS_RETURN_IF_ERROR(n->ReplaceUsesWithNew<ExtendOp>(ops[0]->operand(0),
                                                        n->BitCountOrDie(),
                                                        Op::kSignExt)
                            .status());
    return true;
  }

  // An arithmetic shift right by a constant can be replaced by slice, concat,
  // and a sequence of sign bits of the input.
  //
  //    (val >>> lit) -> Concat({sign_extend(sign_bit), BitSlice(val, ...)})
  //
  // If the shift amount is greater than or equal to the bit width the
  // expression can be replaced with the sign-extended sign bit.
  //
  // This simplification is desirable because in the canonical lower-level IR a
  // shift implies a barrel shifter which is not necessary for a shift by a
  // constant amount.
  if (n->op() == Op::kShra && ops[1]->Is<Literal>()) {
    const int64 bit_count = n->BitCountOrDie();
    const Bits& shift_bits = ops[1]->As<Literal>()->value().bits();
    if (shift_bits.IsAllZeros()) {
      // A shift by zero is a nop.
      return n->ReplaceUsesWith(ops[0]);
    }
    XLS_ASSIGN_OR_RETURN(
        Node * sign_bit,
        n->function()->MakeNode<BitSlice>(
            n->loc(), ops[0], /*start=*/bit_count - 1, /*width=*/1));
    if (bits_ops::UGreaterThanOrEqual(shift_bits, bit_count)) {
      // Replace with all sign bits.
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<ExtendOp>(sign_bit, bit_count, Op::kSignExt)
              .status());
      return true;
    }
    XLS_ASSIGN_OR_RETURN(uint64 shift_amount, shift_bits.ToUint64());
    XLS_RET_CHECK_LT(shift_amount, bit_count);
    XLS_ASSIGN_OR_RETURN(Node * all_sign_bits,
                         n->function()->MakeNode<ExtendOp>(
                             n->loc(), sign_bit, shift_amount, Op::kSignExt));
    XLS_ASSIGN_OR_RETURN(Node * slice,
                         n->function()->MakeNode<BitSlice>(
                             n->loc(), ops[0], /*start=*/shift_amount,
                             /*width=*/bit_count - shift_amount));
    std::vector<Node*> concat_args = {all_sign_bits, slice};
    XLS_RETURN_IF_ERROR(n->ReplaceUsesWithNew<Concat>(concat_args).status());
    return true;
  }

  // Pattern: Double negative.
  //   -(-expr)
  if (n->op() == Op::kNeg && ops[0]->op() == Op::kNeg) {
    XLS_VLOG(2) << "FOUND: Double negative";
    return n->ReplaceUsesWith(ops[0]->operand(0));
  }

  // Patterns (where x is a bits[1] type):
  //   eq(x, 1) => x
  //   eq(x, 0) => not(x)
  //
  // Because eq is commutative, we can rely on the literal being on the right
  // because of canonicalization.
  if (n->op() == Op::kEq && ops[0]->BitCountOrDie() == 1 &&
      ops[1]->Is<Literal>()) {
    if (IsLiteralZero(ops[1])) {
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<UnOp>(ops[0], Op::kNot).status());
      return true;
    }
    XLS_RET_CHECK(IsLiteralUnsignedOne(ops[1]));
    return n->ReplaceUsesWith(ops[0]);
  }

  // Or(Or(x, y), z) => NaryOr(x, y, z)
  // Or(x, Or(y, z)) => NaryOr(x, y, z)
  // Or(Or(x, y), Or(z, a)) => NaryOr(x, y, z, a)
  auto has_operand_with_same_opcode = [&](Op op) {
    return n->op() == op && std::any_of(ops.begin(), ops.end(), [op](Node* n) {
             return n->op() == op;
           });
  };
  if (has_operand_with_same_opcode(Op::kOr) ||
      has_operand_with_same_opcode(Op::kAnd) ||
      has_operand_with_same_opcode(Op::kXor)) {
    std::vector<Node*> new_operands;
    for (Node* operand : ops) {
      if (operand->op() == n->op()) {
        for (Node* sub_operand : operand->operands()) {
          new_operands.push_back(sub_operand);
        }
      } else {
        new_operands.push_back(operand);
      }
    }
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<NaryOp>(new_operands, n->op()).status());
    return true;
  }

  // If either x or y is zero width:
  //   [US]Mul(x, y) => 0
  // This can arise due to narrowing of multiplies.
  if (n->op() == Op::kUMul || n->op() == Op::kSMul) {
    for (Node* operand : n->operands()) {
      if (operand->BitCountOrDie() == 0) {
        XLS_RETURN_IF_ERROR(
            n->ReplaceUsesWithNew<Literal>(Value(UBits(0, n->BitCountOrDie())))
                .status());
        return true;
      }
    }
  }

  // Slt(x, 0) -> msb(x)
  // SGe(x, 0) -> not(msb(x))
  //
  // Canonicalization puts the literal on the right for comparisons.
  if (OpIsCompare(n->op()) && IsLiteralZero(ops[1])) {
    if (n->op() == Op::kSLt) {
      XLS_VLOG(2) << "FOUND: SLt(x, 0)";
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<BitSlice>(
               ops[0], /*start=*/ops[0]->BitCountOrDie() - 1, /*width=*/1)
              .status());
      return true;
    }
    if (n->op() == Op::kSGe) {
      XLS_VLOG(2) << "FOUND: SGe(x, 0)";
      XLS_ASSIGN_OR_RETURN(
          Node * sign_bit,
          n->function()->MakeNode<BitSlice>(
              n->loc(), ops[0],
              /*start=*/ops[0]->BitCountOrDie() - 1, /*width=*/1));
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<UnOp>(sign_bit, Op::kNot).status());
      return true;
    }
  }

  // Not(comparison_op(x, y)) => comparison_op_inverse(x, y)
  if (n->op() == Op::kNot && OpIsCompare(ops[0]->op())) {
    XLS_VLOG(2) << "FOUND: Not(CompareOp(x, y))";
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<CompareOp>(ops[0]->operand(0), ops[0]->operand(1),
                                         CompareOpInverse(ops[0]->op()))
            .status());
    return true;
  }

  int64 leading_zeros, trailing_ones;
  if (OpIsCompare(n->op()) &&
      IsLiteralMask(ops[1], &leading_zeros, &trailing_ones)) {
    XLS_VLOG(2) << "Found comparison to literal mask; leading zeros: "
                << leading_zeros << " trailing ones: " << trailing_ones
                << " :: " << n;
    if (n->op() == Op::kULt) {
      XLS_ASSIGN_OR_RETURN(Node * or_red,
                           OrReduceLeading(n->operand(0), leading_zeros));
      XLS_ASSIGN_OR_RETURN(Node * and_trail,
                           AndReduceTrailing(n->operand(0), trailing_ones));
      std::vector<Node*> args = {or_red, and_trail};
      XLS_RETURN_IF_ERROR(
          n->ReplaceUsesWithNew<NaryOp>(args, Op::kNor).status());
      return true;
    } else if (n->op() == Op::kUGt) {
      XLS_ASSIGN_OR_RETURN(Node * or_red,
                           OrReduceLeading(n->operand(0), leading_zeros));
      XLS_RETURN_IF_ERROR(n->ReplaceUsesWith(or_red).status());
      return true;
    }
  }

  return false;
}

}  // namespace

absl::StatusOr<bool> ArithSimplificationPass::RunOnFunction(
    Function* f, const PassOptions& options, PassResults* results) const {
  bool modified = false;
  bool local_modified = false;
  for (Node* node : TopoSort(f)) {
    XLS_ASSIGN_OR_RETURN(local_modified, MatchArithPatterns(node));
    modified |= local_modified;
  }
  return modified;
}

}  // namespace xls
