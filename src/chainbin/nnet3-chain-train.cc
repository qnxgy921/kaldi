// nnet3bin/nnet3-chain-train.cc

// Copyright 2015  Johns Hopkins University (author: Daniel Povey)

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

#include "base/kaldi-common.h"
#include "util/common-utils.h"
#include "nnet3/am-nnet-simple.h"
#include "nnet3/nnet-chain-training.h"


int main(int argc, char *argv[]) {
  try {
    using namespace kaldi;
    using namespace kaldi::nnet3;
    using namespace kaldi::chain;
    typedef kaldi::int32 int32;
    typedef kaldi::int64 int64;

    const char *usage =
        "Train nnet3+chain neural network parameters with backprop and stochastic\n"
        "gradient descent.  Minibatches are to be created by nnet3-chain-merge-egs in\n"
        "the input pipeline.  This training program is single-threaded (best to\n"
        "use it with a GPU).\n"
        "\n"
        "Usage:  nnet3-chain-train [options] <raw-nnet-in> <denominator-fst-in> <chain-training-examples-in> <raw-nnet-out>\n"
        "\n"
        "nnet3-chain-train 1.raw den.fst 'ark:nnet3-merge-egs 1.cegs ark:-|' 2.raw\n";

    bool binary_write = true;
    std::string use_gpu = "yes",
      prior_rspecifier;
    NnetChainTrainingOptions opts;

    BaseFloat prior_weight = 1.0;

    ParseOptions po(usage);
    po.Register("binary", &binary_write, "Write output in binary mode");
    po.Register("prior", &prior_rspecifier, "The name of file contains pdf-priors.");
    po.Register("prior-weight", &prior_weight, "The weight used as power on priors.");
    po.Register("use-gpu", &use_gpu,
                "yes|no|optional|wait, only has effect if compiled with CUDA");

    opts.Register(&po);

    po.Read(argc, argv);

    if (po.NumArgs() != 4) {
      po.PrintUsage();
      exit(1);
    }

#if HAVE_CUDA==1
    CuDevice::Instantiate().SelectGpuId(use_gpu);
#endif

    std::string nnet_rxfilename = po.GetArg(1),
        den_fst_rxfilename = po.GetArg(2),
        examples_rspecifier = po.GetArg(3),
        nnet_wxfilename = po.GetArg(4);

    Nnet nnet;
    Vector<BaseFloat> prior_vec;
    ReadKaldiObject(nnet_rxfilename, &nnet);
    if (!prior_rspecifier.empty()) {
      ReadKaldiObject(prior_rspecifier, &prior_vec); 
      KALDI_ASSERT(prior_vec.Sum() > 0.0);
      prior_vec.Scale(1.0 / prior_vec.Sum()); // renormalize priors
      if (prior_weight != 1.0)
        prior_vec.ApplyPowAbs(prior_weight);
    }
    const CuVector<BaseFloat> *cu_prior_vec = new CuVector<BaseFloat>(prior_vec);
    
    fst::StdVectorFst den_fst;
    ReadFstKaldi(den_fst_rxfilename, &den_fst);
    
    NnetChainTrainer trainer(opts, den_fst, &nnet, cu_prior_vec);

    SequentialNnetChainExampleReader example_reader(examples_rspecifier);

    for (; !example_reader.Done(); example_reader.Next())
      trainer.Train(example_reader.Value());

    bool ok = trainer.PrintTotalStats();

#if HAVE_CUDA==1
    CuDevice::Instantiate().PrintProfile();
#endif
    WriteKaldiObject(nnet, nnet_wxfilename, binary_write);
    KALDI_LOG << "Wrote raw model to " << nnet_wxfilename;
    return (ok ? 0 : 1);
  } catch(const std::exception &e) {
    std::cerr << e.what() << '\n';
    return -1;
  }
}
