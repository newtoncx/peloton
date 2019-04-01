//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// convert_query_to_op.h
//
// Identification: src/include/optimizer/child_property_generator.h
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once
#include <memory>
#include "optimizer/operator_visitor.h"
#include "optimizer/operator_expression.h"

namespace peloton {

namespace optimizer {
template <class Node, class OpType, class OperatorExpr>
class Memo;
}

namespace optimizer {

// TODO(boweic): Currently we only represent sort as property, later we may want
// to add group, data compression and data distribution(if we go distributed) as
// property
/**
 * @brief Generate child property requirements for physical operators, return pairs of
 *  possible input output properties pairs.
 */
class ChildPropertyDeriver : public OperatorVisitor {
 public:
  std::vector<std::pair<std::shared_ptr<PropertySet>,
                        std::vector<std::shared_ptr<PropertySet>>>>

  GetProperties(GroupExpression<Operator,OpType,OperatorExpression> *gexpr,
                std::shared_ptr<PropertySet> requirements,
                Memo<Operator,OpType,OperatorExpression> *memo);

  void Visit(const DummyScan *) override;
  void Visit(const PhysicalSeqScan *) override;
  void Visit(const PhysicalIndexScan *) override;
  void Visit(const ExternalFileScan *) override;
  void Visit(const QueryDerivedScan *op) override;
  void Visit(const PhysicalOrderBy *) override;
  void Visit(const PhysicalLimit *) override;
  void Visit(const PhysicalInnerNLJoin *) override;
  void Visit(const PhysicalLeftNLJoin *) override;
  void Visit(const PhysicalRightNLJoin *) override;
  void Visit(const PhysicalOuterNLJoin *) override;
  void Visit(const PhysicalInnerHashJoin *) override;
  void Visit(const PhysicalLeftHashJoin *) override;
  void Visit(const PhysicalRightHashJoin *) override;
  void Visit(const PhysicalOuterHashJoin *) override;
  void Visit(const PhysicalInsert *) override;
  void Visit(const PhysicalInsertSelect *) override;
  void Visit(const PhysicalDelete *) override;
  void Visit(const PhysicalUpdate *) override;
  void Visit(const PhysicalHashGroupBy *) override;
  void Visit(const PhysicalSortGroupBy *) override;
  void Visit(const PhysicalDistinct *) override;
  void Visit(const PhysicalAggregate *) override;
  void Visit(const PhysicalExportExternalFile *) override;

 private:
  void DeriveForJoin();
  std::shared_ptr<PropertySet> requirements_;
  /**
   * @brief The derived output property set and input property sets, note that a
   *  operator may have more than one children
   */
  std::vector<std::pair<std::shared_ptr<PropertySet>,
                        std::vector<std::shared_ptr<PropertySet>>>> output_;
  /**
   * @brief We need the memo and gexpr because some property may depend on
   *  child's schema
   */
  Memo<Operator,OpType,OperatorExpression> *memo_;
  GroupExpression<Operator,OpType,OperatorExpression> *gexpr_;
};

}  // namespace optimizer
}  // namespace peloton
