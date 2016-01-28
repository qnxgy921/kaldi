// chain/chain-training.cc

// Copyright      2015   Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "chain/chain-training.h"
#include "chain/chain-kernels-ansi.h"
#include "chain/chain-numerator.h"
#include "chain/chain-denominator.h"

namespace kaldi {
namespace chain {

void ComputeChainObjfAndDeriv(const ChainTrainingOptions &opts,
                              const DenominatorGraph &den_graph,
                              const Supervision &supervision,
                              const CuMatrixBase<BaseFloat> &nnet_output,
                              const CuMatrixBase<BaseFloat> &xent_output,
                              BaseFloat *objf,
                              BaseFloat *l2_term,                              
                              BaseFloat *weight,
                              CuMatrixBase<BaseFloat> *nnet_output_deriv,
                              CuMatrixBase<BaseFloat> *xent_output_deriv) {
  BaseFloat num_logprob_weighted;
  if (nnet_output_deriv)
    nnet_output_deriv->SetZero();
  {
    NumeratorComputation numerator(supervision, nnet_output);
    // note: supervision.weight is included as a factor in the derivative from
    // the numerator object, and the logprob too.
    num_logprob_weighted = numerator.Forward();
    if (nnet_output_deriv) {
      numerator.Backward(nnet_output_deriv);
      if (xent_output_deriv)
        xent_output_deriv->CopyFromMat(*nnet_output_deriv);
    } else if (xent_output_deriv) {
      // this branch will be taken if xent_output_deriv but not
      // nnet_output_deriv is set- which could happen if you want to compute the
      // cross-entropy objective but not the derivatives.
      xent_output_deriv->SetZero();
      numerator.Backward(xent_output_deriv);
    }
  }
  DenominatorComputation denominator(opts, den_graph,
                                     supervision.num_sequences,
                                     nnet_output);

  BaseFloat den_logprob = denominator.Forward();
  bool ok = true;
  if (nnet_output_deriv)
    ok = denominator.Backward(-supervision.weight,
                              nnet_output_deriv);

  *objf = num_logprob_weighted - supervision.weight * den_logprob;
  *weight = supervision.weight * supervision.num_sequences *
      supervision.frames_per_sequence;
  if (!((*objf) - (*objf) == 0) || !ok) {
    // inf or NaN detected, or denominator computation returned false.
    if (nnet_output_deriv)
      nnet_output_deriv->SetZero();
    if (xent_output_deriv)
      xent_output_deriv->SetZero();
    BaseFloat default_objf = -10;
    KALDI_WARN << "Objective function is " << (*objf)
               << " and denominator computation (if done) returned "
               << std::boolalpha << ok
               << ", setting objective function to " << default_objf
               << " per frame.";
    *objf  = default_objf * *weight;
  }

  // This code helps us see how big the derivatives are, on average,
  // for different frames of the sequences.  As expected, they are
  // smaller towards the edges of the sequences (due to the penalization
  // of 'incorrect' pdf-ids.
  if (GetVerboseLevel() >= 1) {
    int32 tot_frames = nnet_output_deriv->NumRows(),
 frames_per_sequence = supervision.frames_per_sequence,
       num_sequences = supervision.num_sequences;
    CuVector<BaseFloat> row_products(tot_frames);
    row_products.AddDiagMat2(1.0, *nnet_output_deriv, kNoTrans, 0.0);
    Vector<BaseFloat> row_products_cpu(row_products);
    Vector<BaseFloat> row_products_per_frame(frames_per_sequence);
    for (int32 i = 0; i < tot_frames; i++)
      row_products_per_frame(i / num_sequences) += row_products_cpu(i);
    KALDI_LOG << "Derivs per frame are " << row_products_per_frame;
  }

  if (opts.l2_regularize == 0.0) {
    *l2_term = 0.0;
  } else {
    // compute the l2 penalty term and its derivative
    BaseFloat scale_coeff = supervision.weight * opts.l2_regularize;
    // If xent_output provided, l2 penalty is trying to regress the chain output
    // to be a linear function of cross-entropy output.
    // It minimizes -0.5 * l2_regularize * l2_norm(diag(scale) * x + offset - y)^2, 
    // where x is cross-entropy output and y is chain output.
    if (xent_output.NumRows() != 0) {
      //compute offset and scale
      // The objecitve is to minimize L w.r.t scale_i, offset_i, 
      // L = -0.5 * l2_regularize * 
      //    \sum_{j=1}^{minibatch_size}(\sum_i (nnet_output_ji - target_ji)^2),
      // where the target_ji = scale_i * xent_output_ji + offset_i. 
      // scale_i = \sum_j (nnet_output_ji * xent_output_ji) / \sum_j(xent_output_ji^2)
      // offset_i = 1 ./ minibatch_size * \sum_j (nnet_output_ji - scale_i * xent_output_ji)
      CuVector<BaseFloat> scale(xent_output.NumCols()), 
        offset(xent_output.NumCols()), ones(xent_output.NumRows()),
        scaled_xent_col_sum(xent_output.NumRows());
      CuVector<BaseFloat> nnet_nnet_product(nnet_output.NumCols());
      scale.AddDiagMatMat(1.0, xent_output, kTrans, nnet_output, kNoTrans, 0.0);
      nnet_nnet_product.AddDiagMat2(1.0, xent_output, kTrans, 0.0);
      scale.DivElements(nnet_nnet_product);
      
      offset.AddMatVec(1.0 / xent_output.NumRows(), nnet_output, kTrans, ones, 0.0);
      scaled_xent_col_sum.AddMatVec(1.0, xent_output, kTrans, ones, 0.0);
      scaled_xent_col_sum.MulElements(scale);
      offset.AddVec(-1.0 / xent_output.NumRows(), scaled_xent_col_sum);

      //output_diff = (scale * xent_output + offset) - nnet_output;
      CuMatrix<BaseFloat> output_diff(xent_output.NumRows(), xent_output.NumCols());
      output_diff.AddMatDiagVec(1.0, xent_output, kNoTrans, scale, 0.0);
      output_diff.AddVecToRows(1.0, offset);
      output_diff.AddMat(-1.0, nnet_output);
      *l2_term = -0.5 * scale_coeff * TraceMatMat(output_diff, output_diff, kTrans);

      //update the nnet_output and xent_output derivative w.r.t. regularizer term.
      if (nnet_output_deriv)
        nnet_output_deriv->AddMat(scale_coeff, output_diff);

      if (xent_output_deriv) 
        xent_output_deriv->AddMatDiagVec(-1.0 * scale_coeff, output_diff, kNoTrans, scale, 1.0);

    } else {
      *l2_term = -0.5 * scale_coeff * TraceMatMat(nnet_output, nnet_output, kTrans);
      if (nnet_output_deriv)
        nnet_output_deriv->AddMat(-1.0 * scale_coeff, nnet_output);
    }
  }
}
// This function computes scale and offset parameters, where
// (scale, offset) = argmin \sum_j[diag(scale) * xj + offset - yj], 
// where x_j and y_j are jth example in input1 and input2 respectively. 
void ComputeScaleOffset(const CuMatrixBase<BaseFloat> &input1,
                        const CuMatrixBase<BaseFloat> &input2,
                        CuVector<BaseFloat> *scale,
                        CuVector<BaseFloat> *offset) {
  
}
}  // namespace chain
}  // namespace kaldi
