#pragma once

#include "target_func.h"
#include "kernel.h"
#include "non_diag_target_der.h"
#include <catboost/libs/options/enums.h>
#include <catboost/libs/options/loss_description.h>
#include <catboost/libs/metrics/pfound.h>
#include <catboost/cuda/gpu_data/dataset_base.h>
#include <catboost/cuda/gpu_data/querywise_helper.h>
#include <catboost/cuda/methods/helpers.h>
#include <catboost/libs/options/bootstrap_options.h>

namespace NCatboostCuda {
    template <class TSamplesMapping, class TDataSet>
    class TPFoundF;

    template <class TDataSet>
    class TPFoundF<NCudaLib::TStripeMapping, TDataSet>: public TNonDiagQuerywiseTarget<NCudaLib::TStripeMapping, TDataSet> {
    public:
        using TSamplesMapping = NCudaLib::TStripeMapping;
        using TParent = TNonDiagQuerywiseTarget<TSamplesMapping, TDataSet>;
        using TStat = TAdditiveStatistic;
        using TMapping = TSamplesMapping;
        CB_DEFINE_CUDA_TARGET_BUFFERS();

        TPFoundF(const TDataSet& dataSet,
                 TGpuAwareRandom& random,
                 const NCatboostOptions::TLossDescription& targetOptions)
            : TParent(dataSet,
                      random) {
            Init(targetOptions);
        }

        TPFoundF(TPFoundF&& other)
            : TParent(std::move(other))
            , PermutationCount(other.PermutationCount)
        {
        }

        using TParent::GetTarget;

        TAdditiveStatistic ComputeStats(const TConstVec& point, const TMap<TString, TString> params = TMap<TString, TString>()) const {
            Y_UNUSED(point);
            Y_UNUSED(params);
            CB_ENSURE(false, "Unimplemented");
        }


        static double Score(const TAdditiveStatistic& score) {
            return score.Stats[0] / score.Stats[1];
        }

        double Score(const TConstVec& point) const {
            return Score(ComputeStats(point));
        }

        void StochasticGradient(const TConstVec& point,
                                const NCatboostOptions::TBootstrapConfig& config,
                                TNonDiagQuerywiseTargetDers* target) const {
            ApproximateStochastic(point, config, false, target);
        }

        void StochasticNewton(const TConstVec& point,
                              const NCatboostOptions::TBootstrapConfig& config,
                              TNonDiagQuerywiseTargetDers* target) const {
            ApproximateStochastic(point, config, true, target);
        }

        void ApproximateStochastic(const TConstVec& point,
                                   const NCatboostOptions::TBootstrapConfig& bootstrapConfig,
                                   bool,
                                   TNonDiagQuerywiseTargetDers* target) const {
            {
                auto& querywiseSampler = GetQueriesSampler();
                const auto& sampledGrouping = TParent::GetSamplesGrouping();
                auto& qids = querywiseSampler.GetPerDocQids(sampledGrouping);

                TCudaBuffer<uint2, TMapping> tempPairs;
                auto& weights = target->PairDer2OrWeights;

                auto& gradient = target->PointWeightedDer;
                auto& sampledDocs = target->Docs;

                target->PointDer2OrWeights.Clear();

                double queriesSampleRate = 1.0;
                if (bootstrapConfig.GetBootstrapType() == EBootstrapType::Bernoulli) {
                    queriesSampleRate = bootstrapConfig.GetTakenFraction();
                }

                if (bootstrapConfig.GetBootstrapType() == EBootstrapType::Poisson) {
                    ythrow TCatboostException() << "Poisson bootstrap is not supported for YetiRankPairwise";
                }

                {
                    auto guard = NCudaLib::GetProfiler().Profile("Queries sampler in -YetiRankPairwise");
                    querywiseSampler.SampleQueries(TParent::GetRandom(),
                                                   queriesSampleRate,
                                                   1.0,
                                                   GetMaxQuerySize(),
                                                   sampledGrouping,
                                                   &sampledDocs);
                }

                TCudaBuffer<ui32, TMapping> sampledQids;
                TCudaBuffer<ui32, TMapping> sampledQidOffsets;

                ComputeQueryOffsets(qids,
                                    sampledDocs,
                                    &sampledQids,
                                    &sampledQidOffsets);

                TCudaBuffer<ui32, TMapping> matrixOffsets;
                matrixOffsets.Reset(sampledQidOffsets.GetMapping());
                {
                    auto tmp = TCudaBuffer<ui32, TMapping>::CopyMapping(matrixOffsets);
                    ComputeMatrixSizes(sampledQidOffsets,
                                       &tmp);
                    ScanVector(tmp, matrixOffsets);
                }

                {
                    auto guard = NCudaLib::GetProfiler().Profile("Make pairs");

                    tempPairs.Reset(CreateMappingFromTail(matrixOffsets, 0));
                    MakePairs(sampledQidOffsets,
                              matrixOffsets,
                              &tempPairs);
                }

                weights.Reset(tempPairs.GetMapping());
                FillBuffer(weights, 0.0f);

                gradient.Reset(sampledDocs.GetMapping());
                FillBuffer(gradient, 0.0f);

                auto expApprox = TCudaBuffer<float, TMapping>::CopyMapping(sampledDocs);
                Gather(expApprox, point, sampledDocs);

                auto targets = TCudaBuffer<float, TMapping>::CopyMapping(sampledDocs);
                Gather(targets, GetTarget().GetTargets(), sampledDocs);

                RemoveQueryMeans(sampledQids,
                                 sampledQidOffsets,
                                 &expApprox);
                ExpVector(expApprox);

                {
                    auto guard = NCudaLib::GetProfiler().Profile("PFoundFWeights");
                    ComputePFoundFWeightsMatrix(DistributedSeed(TParent::GetRandom()),
                                                GetPFoundPermutationCount(),
                                                expApprox,
                                                targets,
                                                sampledQids,
                                                sampledQidOffsets,
                                                matrixOffsets,
                                                &weights);

                    if (bootstrapConfig.GetBootstrapType() == EBootstrapType::Bayesian) {
                        auto& seeds = TParent::GetRandom().template GetGpuSeeds<NCudaLib::TStripeMapping>();
                        BayesianBootstrap(seeds,
                                          weights,
                                          bootstrapConfig.GetBaggingTemperature());
                    }
                }

                TCudaBuffer<ui32, TMapping> nzWeightPairIndices;

                FilterZeroEntries(&weights,
                                  &nzWeightPairIndices);

                auto& pairs = target->Pairs;
                pairs.Reset(weights.GetMapping());
                Gather(pairs, tempPairs, nzWeightPairIndices);

                {
                    auto guard = NCudaLib::GetProfiler().Profile("PFoundFMakePairsAndPointwiseGradient");

                    MakeFinalPFoundGradients(sampledDocs,
                                             expApprox,
                                             targets,
                                             weights,
                                             &pairs,
                                             &gradient);
                }
            }

            //TODO(noxoomo): maybe force defragmentation
            //TODO(noxoomo): check gradients filtering profits
        }

        void FillPairsAndWeightsAtPoint(const TConstVec&,
                                        TStripeBuffer<uint2>*,
                                        TStripeBuffer<float>*) const {
            CB_ENSURE(false, "unimplement yet");
        }

        void ApproximateAt(const TConstVec& point,
                           const TStripeBuffer<uint2>& pairs,
                           const TStripeBuffer<float>& pairWeights,
                           const TStripeBuffer<ui32>& scatterDerIndices,
                           TStripeBuffer<float>* value,
                           TStripeBuffer<float>* der,
                           TStripeBuffer<float>* pairDer2) const {
            PairLogitPairwise(point,
                              pairs,
                              pairWeights,
                              scatterDerIndices,
                              value,
                              der,
                              pairDer2);
        }

        static constexpr bool IsMinOptimal() {
            return false;
        }

        static constexpr TStringBuf ScoreMetricName() {
            return "PFound";
        }

        ELossFunction GetScoreMetricType() const {
            return ELossFunction::PFound;
        }

        ui32 GetPFoundPermutationCount() const {
            return PermutationCount;
        }

        static constexpr ENonDiagonalOracleType NonDiagonalOracleType() {
            return ENonDiagonalOracleType::Pairwise;
        }

    private:
        ui32 GetMaxQuerySize() const {
            auto& queriesInfo = TParent::GetSamplesGrouping();
            const ui32 queryCount = queriesInfo.GetQueryCount();
            const double meanQuerySize = GetTarget().GetTargets().GetObjectsSlice().Size() * 1.0 / queryCount;
            const ui32 estimatedQuerySizeLimit = 2 * meanQuerySize + 8;
            return Min<ui32>(estimatedQuerySizeLimit, 1023);
        }

        void Init(const NCatboostOptions::TLossDescription& targetOptions) {
            CB_ENSURE(targetOptions.GetLossFunction() == ELossFunction::YetiRankPairwise);
            PermutationCount = NCatboostOptions::GetYetiRankPermutations(targetOptions);
        }


        TQuerywiseSampler& GetQueriesSampler() const {
            if (QueriesSampler == nullptr) {
                QueriesSampler = new TQuerywiseSampler();
            }
            return *QueriesSampler;
        }

    private:
        mutable THolder<TQuerywiseSampler> QueriesSampler;
        ui32 PermutationCount = 10;
    };

}
