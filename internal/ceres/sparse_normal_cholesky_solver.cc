// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2015 Google Inc. All rights reserved.
// http://ceres-solver.org/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: sameeragarwal@google.com (Sameer Agarwal)

#include "ceres/sparse_normal_cholesky_solver.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>

#include "ceres/compressed_row_sparse_matrix.h"
#include "ceres/cxsparse.h"
#include "ceres/internal/eigen.h"
#include "ceres/internal/scoped_ptr.h"
#include "ceres/linear_solver.h"
#include "ceres/suitesparse.h"
#include "ceres/triplet_sparse_matrix.h"
#include "ceres/types.h"
#include "ceres/wall_time.h"
#include "Eigen/SparseCore"

#ifdef CERES_USE_EIGEN_SPARSE
#include "Eigen/SparseCholesky"
#endif

namespace ceres {
namespace internal {
namespace {

#ifdef CERES_USE_EIGEN_SPARSE
// A templated factorized and solve function, which allows us to use
// the same code independent of whether a AMD or a Natural ordering is
// used.
template <typename SimplicialCholeskySolver, typename SparseMatrixType>
LinearSolver::Summary SimplicialLDLTSolve(
    const SparseMatrixType& lhs,
    const bool do_symbolic_analysis,
    SimplicialCholeskySolver* solver,
    double* rhs_and_solution,
    EventLogger* event_logger) {
  LinearSolver::Summary summary;
  summary.num_iterations = 1;
  summary.termination_type = LINEAR_SOLVER_SUCCESS;
  summary.message = "Success.";

  if (do_symbolic_analysis) {
    solver->analyzePattern(lhs);
    if (VLOG_IS_ON(2)) {
      std::stringstream ss;
      solver->dumpMemory(ss);
      VLOG(2) << "Symbolic Analysis\n"
              << ss.str();
    }
    event_logger->AddEvent("Analyze");
    if (solver->info() != Eigen::Success) {
      summary.termination_type = LINEAR_SOLVER_FATAL_ERROR;
      summary.message =
          "Eigen failure. Unable to find symbolic factorization.";
      return summary;
    }
  }

  solver->factorize(lhs);
  event_logger->AddEvent("Factorize");
  if (solver->info() != Eigen::Success) {
    summary.termination_type = LINEAR_SOLVER_FAILURE;
    summary.message = "Eigen failure. Unable to find numeric factorization.";
    return summary;
  }

  const Vector rhs = VectorRef(rhs_and_solution, lhs.cols());

  VectorRef(rhs_and_solution, lhs.cols()) = solver->solve(rhs);
  event_logger->AddEvent("Solve");
  if (solver->info() != Eigen::Success) {
    summary.termination_type = LINEAR_SOLVER_FAILURE;
    summary.message = "Eigen failure. Unable to do triangular solve.";
    return summary;
  }

  return summary;
}

#endif  // CERES_USE_EIGEN_SPARSE

}  // namespace

SparseNormalCholeskySolver::SparseNormalCholeskySolver(
    const LinearSolver::Options& options)
    : factor_(NULL),
      cxsparse_factor_(NULL),
      options_(options) {
}

void SparseNormalCholeskySolver::FreeFactorization() {
  if (factor_ != NULL) {
    ss_.Free(factor_);
    factor_ = NULL;
  }

  if (cxsparse_factor_ != NULL) {
    cxsparse_.Free(cxsparse_factor_);
    cxsparse_factor_ = NULL;
  }
}

SparseNormalCholeskySolver::~SparseNormalCholeskySolver() {
  FreeFactorization();
}

LinearSolver::Summary SparseNormalCholeskySolver::SolveImpl(
    CompressedRowSparseMatrix* A,
    const double* b,
    const LinearSolver::PerSolveOptions& per_solve_options,
    double * x) {

  const int num_cols = A->num_cols();
  VectorRef(x, num_cols).setZero();
  A->LeftMultiply(b, x);

  if (per_solve_options.D != NULL) {
    // Temporarily append a diagonal block to the A matrix, but undo
    // it before returning the matrix to the user.
    scoped_ptr<CompressedRowSparseMatrix> regularizer;
    if (A->col_blocks().size() > 0) {
      regularizer.reset(CompressedRowSparseMatrix::CreateBlockDiagonalMatrix(
                            per_solve_options.D, A->col_blocks()));
    } else {
      regularizer.reset(new CompressedRowSparseMatrix(
                            per_solve_options.D, num_cols));
    }
    A->AppendRows(*regularizer);
  }

  LinearSolver::Summary summary;
  switch (options_.sparse_linear_algebra_library_type) {
    case SUITE_SPARSE:
      summary = SolveImplUsingSuiteSparse(A, x);
      break;
    case CX_SPARSE:
      summary = SolveImplUsingCXSparse(A, x);
      break;
    case EIGEN_SPARSE:
      summary = SolveImplUsingEigen(A, x);
      break;
    default:
      LOG(FATAL) << "Unknown sparse linear algebra library : "
                 << options_.sparse_linear_algebra_library_type;
  }

  if (per_solve_options.D != NULL) {
    A->DeleteRows(num_cols);
  }

  return summary;
}

LinearSolver::Summary SparseNormalCholeskySolver::SolveImplUsingEigen(
    CompressedRowSparseMatrix* A,
    double * rhs_and_solution) {
#ifndef CERES_USE_EIGEN_SPARSE

  LinearSolver::Summary summary;
  summary.num_iterations = 0;
  summary.termination_type = LINEAR_SOLVER_FATAL_ERROR;
  summary.message =
      "SPARSE_NORMAL_CHOLESKY cannot be used with EIGEN_SPARSE "
      "because Ceres was not built with support for "
      "Eigen's SimplicialLDLT decomposition. "
      "This requires enabling building with -DEIGENSPARSE=ON.";
  return summary;

#else

  EventLogger event_logger("SparseNormalCholeskySolver::Eigen::Solve");
  // Compute the normal equations. J'J delta = J'f and solve them
  // using a sparse Cholesky factorization.

  // Compute outer product as a compressed row lower triangular
  // matrix, because after mapping to a column major matrix, this will
  // become a compressed column upper triangular matrix. Which is the
  // default that Eigen uses.
  if (outer_product_.get() == NULL) {
    outer_product_.reset(
        CompressedRowSparseMatrix::CreateOuterProductMatrixAndProgram(
            *A, CompressedRowSparseMatrix::LOWER_TRIANGULAR, &pattern_));
  }

  CompressedRowSparseMatrix::ComputeOuterProduct(
      *A, pattern_, outer_product_.get());

  // Map to an upper triangular column major matrix.
  //
  // outer_product_ is a compressed row sparse matrix and in lower
  // triangular form, when mapped to a compressed column sparse
  // matrix, it becomes an upper triangular matrix.
  Eigen::MappedSparseMatrix<double, Eigen::ColMajor> lhs(
      outer_product_->num_rows(),
      outer_product_->num_rows(),
      outer_product_->num_nonzeros(),
      outer_product_->mutable_rows(),
      outer_product_->mutable_cols(),
      outer_product_->mutable_values());

  bool do_symbolic_analysis = false;

  // If using post ordering or an old version of Eigen, we cannot
  // depend on a preordered jacobian, so we work with a SimplicialLDLT
  // decomposition with AMD ordering.
  if (options_.use_postordering ||
      !EIGEN_VERSION_AT_LEAST(3, 2, 2)) {
    if (amd_ldlt_.get() == NULL) {
      amd_ldlt_.reset(new SimplicialLDLTWithAMDOrdering);
      do_symbolic_analysis = true;
    }

    return SimplicialLDLTSolve(lhs,
                               do_symbolic_analysis,
                               amd_ldlt_.get(),
                               rhs_and_solution,
                               &event_logger);
  }

#if EIGEN_VERSION_AT_LEAST(3,2,2)
  // The common case
  if (natural_ldlt_.get() == NULL) {
    natural_ldlt_.reset(new SimplicialLDLTWithNaturalOrdering);
    do_symbolic_analysis = true;
  }

  return SimplicialLDLTSolve(lhs,
                             do_symbolic_analysis,
                             natural_ldlt_.get(),
                             rhs_and_solution,
                             &event_logger);
#endif

#endif  // EIGEN_USE_EIGEN_SPARSE
}

LinearSolver::Summary SparseNormalCholeskySolver::SolveImplUsingCXSparse(
    CompressedRowSparseMatrix* A,
    double * rhs_and_solution) {
#ifdef CERES_NO_CXSPARSE

  LinearSolver::Summary summary;
  summary.num_iterations = 0;
  summary.termination_type = LINEAR_SOLVER_FATAL_ERROR;
  summary.message =
      "SPARSE_NORMAL_CHOLESKY cannot be used with CX_SPARSE "
      "because Ceres was not built with support for CXSparse. "
      "This requires enabling building with -DCXSPARSE=ON.";

  return summary;

#else

  EventLogger event_logger("SparseNormalCholeskySolver::CXSparse::Solve");
  LinearSolver::Summary summary;
  summary.num_iterations = 1;
  summary.termination_type = LINEAR_SOLVER_SUCCESS;
  summary.message = "Success.";


  // Compute outer product as a compressed row lower triangular
  // matrix, which would mapped to a compressed column upper
  // triangular matrix, which is the representation used by CXSparse's
  // sparse Cholesky factorization.
  if (outer_product_.get() == NULL) {
    outer_product_.reset(
        CompressedRowSparseMatrix::CreateOuterProductMatrixAndProgram(
            *A, CompressedRowSparseMatrix::LOWER_TRIANGULAR, &pattern_));
  }

  CompressedRowSparseMatrix::ComputeOuterProduct(
      *A, pattern_, outer_product_.get());

  cs_di lhs = cxsparse_.CreateSparseMatrixTransposeView(outer_product_.get());

  event_logger.AddEvent("Setup");

  // Compute symbolic factorization if not available.
  if (cxsparse_factor_ == NULL) {
    if (options_.use_postordering) {
      cxsparse_factor_ = cxsparse_.BlockAnalyzeCholesky(&lhs,
                                                        A->col_blocks(),
                                                        A->col_blocks());
    } else {
      cxsparse_factor_ = cxsparse_.AnalyzeCholeskyWithNaturalOrdering(&lhs);
    }
  }
  event_logger.AddEvent("Analysis");

  if (cxsparse_factor_ == NULL) {
    summary.termination_type = LINEAR_SOLVER_FATAL_ERROR;
    summary.message =
        "CXSparse failure. Unable to find symbolic factorization.";
  } else if (!cxsparse_.SolveCholesky(&lhs,
                                      cxsparse_factor_,
                                      rhs_and_solution)) {
    summary.termination_type = LINEAR_SOLVER_FAILURE;
    summary.message = "CXSparse::SolveCholesky failed.";
  }
  event_logger.AddEvent("Solve");

  return summary;
#endif
}

LinearSolver::Summary SparseNormalCholeskySolver::SolveImplUsingSuiteSparse(
    CompressedRowSparseMatrix* A,
    double * rhs_and_solution) {
#ifdef CERES_NO_SUITESPARSE

  LinearSolver::Summary summary;
  summary.num_iterations = 0;
  summary.termination_type = LINEAR_SOLVER_FATAL_ERROR;
  summary.message =
      "SPARSE_NORMAL_CHOLESKY cannot be used with SUITE_SPARSE "
      "because Ceres was not built with support for SuiteSparse. "
      "This requires enabling building with -DSUITESPARSE=ON.";
  return summary;

#else

  EventLogger event_logger("SparseNormalCholeskySolver::SuiteSparse::Solve");
  LinearSolver::Summary summary;
  summary.termination_type = LINEAR_SOLVER_SUCCESS;
  summary.num_iterations = 1;
  summary.message = "Success.";

  // Compute outer product to compressed row upper triangular matrix,
  // this will be mapped to a compressed-column lower triangular
  // matrix, which is the fastest option for our default natural
  // ordering (see comment in cholmod_factorize.c:205 in SuiteSparse).
  if (outer_product_.get() == NULL) {
    outer_product_.reset(
        CompressedRowSparseMatrix::CreateOuterProductMatrixAndProgram(
            *A, CompressedRowSparseMatrix::UPPER_TRIANGULAR, &pattern_));
  }

  CompressedRowSparseMatrix::ComputeOuterProduct(
      *A, pattern_, outer_product_.get());

  const int num_cols = A->num_cols();
  cholmod_sparse lhs =
      ss_.CreateSparseMatrixTransposeView(outer_product_.get());
  event_logger.AddEvent("Setup");

  if (factor_ == NULL) {
    if (options_.use_postordering) {
      factor_ = ss_.BlockAnalyzeCholesky(&lhs,
                                         A->col_blocks(),
                                         A->col_blocks(),
                                         &summary.message);
    } else {
      factor_ = ss_.AnalyzeCholeskyWithNaturalOrdering(&lhs,
                                                       &summary.message);
    }
  }

  event_logger.AddEvent("Analysis");

  if (factor_ == NULL) {
    summary.termination_type = LINEAR_SOLVER_FATAL_ERROR;
    // No need to set message as it has already been set by the
    // symbolic analysis routines above.
    return summary;
  }

  summary.termination_type = ss_.Cholesky(&lhs, factor_, &summary.message);
  if (summary.termination_type != LINEAR_SOLVER_SUCCESS) {
    return summary;
  }

  cholmod_dense* rhs = ss_.CreateDenseVector(rhs_and_solution,
                                             num_cols,
                                             num_cols);
  cholmod_dense* solution = ss_.Solve(factor_, rhs, &summary.message);
  event_logger.AddEvent("Solve");

  ss_.Free(rhs);
  if (solution != NULL) {
    memcpy(rhs_and_solution, solution->x, num_cols * sizeof(*rhs_and_solution));
    ss_.Free(solution);
  } else {
    // No need to set message as it has already been set by the
    // numeric factorization routine above.
    summary.termination_type = LINEAR_SOLVER_FAILURE;
  }

  event_logger.AddEvent("Teardown");
  return summary;
#endif
}

}   // namespace internal
}   // namespace ceres
