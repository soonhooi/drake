#include "drake/common/symbolic_polynomial.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "drake/common/monomial.h"
#include "drake/common/monomial_util.h"
#include "drake/common/test/symbolic_test_util.h"

namespace drake {
namespace symbolic {
namespace {

using std::vector;
using std::to_string;

using test::ExprEqual;

class SymbolicPolynomialTest : public ::testing::Test {
 protected:
  const Variable var_x_{"x"};
  const Variable var_y_{"y"};
  const Variable var_z_{"z"};
  const Variables var_xy_{var_x_, var_y_};
  const Variables var_xyz_{var_x_, var_y_, var_z_};
  const drake::VectorX<symbolic::Monomial> monomials_{
      MonomialBasis(var_xyz_, 3)};
  const Expression x_{var_x_};
  const Expression y_{var_y_};
  const Expression z_{var_z_};
  const Expression xy_{var_x_ + var_y_};
  const Expression xyz_{var_x_ + var_y_ + var_z_};
  const vector<Expression> exprs_{
      0,
      -1,
      3.14,
      x_,
      5 * x_,
      -3 * x_,
      y_,
      x_* y_,
      2 * x_* x_,
      2 * x_* x_,
      6 * x_* y_,
      3 * x_* x_* y_ + 4 * pow(y_, 3) * z_ + 2,
      y_*(3 * x_ * x_ + 4 * y_ * y_ * z_) + 2,
      6 * pow(x_, 3) * pow(y_, 2),
      2 * pow(x_, 3) * 3 * pow(y_, 2),
      pow(x_, 3) - 4 * x_* y_* y_ + 2 * x_* x_* y_ - 8 * pow(y_, 3),
      pow(x_ + 2 * y_, 2) * (x_ - 2 * y_),
      (x_ + 2 * y_) * (x_ * x_ - 4 * y_ * y_),
      (x_ * x_ + 4 * x_ * y_ + 4 * y_ * y_) * (x_ - 2 * y_),
      pow(x_ + y_ + 1, 4),
      pow(x_ + y_ + 1, 3),
      1 + x_* x_ + 2 * (y_ - 0.5 * x_ * x_ - 0.5)};
};

TEST_F(SymbolicPolynomialTest, DefaultConstructor) {
  const Polynomial p{};
  EXPECT_TRUE(p.monomial_to_coefficient_map().empty());
  EXPECT_PRED2(ExprEqual, p.ToExpression(), Expression{0.0});
}

TEST_F(SymbolicPolynomialTest, ConstructFromExpression) {
  // Expression -------------------> Polynomial
  //     |                               |
  //     | .Expand()                     | .ToExpression()
  //    \/                              \/
  // Expanded Expression     ==      Expression

  for (const Expression& e : exprs_) {
    const Polynomial p{e};
    const Expression expanded_expr{e.Expand()};
    const Expression expr_from_poly{p.ToExpression()};
    EXPECT_PRED2(ExprEqual, expanded_expr, expr_from_poly);

    Variables vars;
    for (const std::pair<Monomial, Expression>& map :
         p.monomial_to_coefficient_map()) {
      const Monomial& m_i{map.first};
      vars += m_i.GetVariables();
    }
    EXPECT_EQ(vars, e.Expand().GetVariables());
  }
}

TEST_F(SymbolicPolynomialTest, ConstructFromMonomial) {
  for (int i = 0; i < monomials_.size(); ++i) {
    const Monomial& x{monomials_[i]};
    const Polynomial p{x};
    for (const std::pair<Monomial, Expression>& map :
         p.monomial_to_coefficient_map()) {
      EXPECT_EQ(map.first, x);
      EXPECT_EQ(map.second, 1);
    }
  }
}

TEST_F(SymbolicPolynomialTest, ConstructFromExpressionWithIndeterminates) {
  const Polynomial p_0{xy_, Variables{var_x_}};
  Variables vars_in_monomial_0;
  Variables vars_in_expression_0;
  for (const std::pair<Monomial, Expression>& map :
       p_0.monomial_to_coefficient_map()) {
    vars_in_monomial_0 += map.first.GetVariables();
    vars_in_expression_0 += map.second.Expand().GetVariables();
  }
  EXPECT_EQ(vars_in_monomial_0, Variables{var_x_});
  EXPECT_EQ(vars_in_expression_0, Variables{var_y_});

  const Polynomial p_1{xyz_, Variables{var_xy_}};
  Variables vars_in_monomial_1;
  Variables vars_in_expression_1;
  for (const std::pair<Monomial, Expression>& map :
       p_1.monomial_to_coefficient_map()) {
    vars_in_monomial_1 += map.first.GetVariables();
    vars_in_expression_1 += map.second.Expand().GetVariables();
  }
  EXPECT_EQ(vars_in_monomial_1, Variables{var_xy_});
  EXPECT_EQ(vars_in_expression_1, Variables{var_z_});
}

TEST_F(SymbolicPolynomialTest, GetterIndeterminates) {
  for (const Expression& e : exprs_) {
    const Polynomial p_0{e};
    EXPECT_EQ(p_0.indeterminates(), e.Expand().GetVariables());
  }

  const Polynomial p_1{xy_, Variables{var_x_}};
  EXPECT_EQ(p_1.indeterminates(), Variables{var_x_});
  const Polynomial p_2{xyz_, var_xy_};
  EXPECT_EQ(p_2.indeterminates(), var_xy_);
}

TEST_F(SymbolicPolynomialTest, GetterDecisionVariables) {
  for (const Expression& e : exprs_) {
    const Polynomial p_0{e};
    EXPECT_EQ(p_0.decision_variables(), Variables());
  }

  const Polynomial p_1{xyz_, var_xy_};
  EXPECT_EQ(p_1.decision_variables(), Variables{var_z_});
  const Polynomial p_2{xyz_, Variables{var_z_}};
  EXPECT_EQ(p_2.decision_variables(), var_xy_);
}

TEST_F(SymbolicPolynomialTest, Addition) {
  //   (Polynomial(e₁) + Polynomial(e₂)).ToExpression()
  // = e₁.Expand() + e₂.Expand()
  for (const Expression& e1 : exprs_) {
    for (const Expression& e2 : exprs_) {
      EXPECT_PRED2(ExprEqual, (Polynomial{e1} + Polynomial{e2}).ToExpression(),
                   e1.Expand() + e2.Expand());
    }
  }
  // No need to test Polynomial operator+(Polynomial p, const Monomial& m) as
  // TEST_F(SymbolicPolynomialTest, ConstructFromMonomial) is passed.
}

TEST_F(SymbolicPolynomialTest, Subtraction) {
  //   (Polynomial(e₁) - Polynomial(e₂)).ToExpression()
  // = e₁.Expand() - e₂.Expand()
  for (const Expression& e1 : exprs_) {
    for (const Expression& e2 : exprs_) {
      EXPECT_PRED2(ExprEqual, (Polynomial{e1} - Polynomial{e2}).ToExpression(),
                   e1.Expand() - e2.Expand());
    }
  }
  // No need to test Polynomial operator-(Polynomial p, const Monomial& m) as
  // TEST_F(SymbolicPolynomialTest, ConstructFromMonomial) is passed.
}

TEST_F(SymbolicPolynomialTest, Multiplication) {
  //   (Polynomial(e₁) * Polynomial(e₂)).ToExpression()
  // = (e₁.Expand() * e₂.Expand()).Expand()
  for (const Expression& e1 : exprs_) {
    for (const Expression& e2 : exprs_) {
      EXPECT_PRED2(ExprEqual, (Polynomial{e1} * Polynomial{e2}).ToExpression(),
                   (e1.Expand() * e2.Expand()).Expand());
    }
  }
  // Evaluates (1 + x) * (1 - x) to confirm that the cross term 0 * x is erased
  // from the product.
  const Polynomial p1(1 + x_);
  const Polynomial p2(1 - x_);
  Polynomial::MapType product_map_expected{};
  product_map_expected.emplace(Monomial(), 1);
  product_map_expected.emplace(Monomial(var_x_, 2), -1);
  EXPECT_EQ(product_map_expected, (p1 * p2).monomial_to_coefficient_map());
}

// It checks if we can compute Xᵀ*Q*X in SOS. For now, this test doesn't have
// any assertions and simply checks if it compiles.
TEST_F(SymbolicPolynomialTest, SOSTest) {
  // X = MonomialBasis({x}, 2) = {x², x, 1}.
  const drake::VectorX<Monomial> X{MonomialBasis({var_x_}, 2)};
  // Set up Q. You can do this by using
  // MathematicalProgram::NewSymmetricContinuousVariables.
  Eigen::Matrix<Variable, 3, 3> Q;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      Q(i, j) = Variable("q_" + to_string(i) + "_" + to_string(j));
    }
  }
  const Polynomial p = (X.transpose() * Q * X)(0);
}

}  // namespace
}  // namespace symbolic
}  // namespace drake
