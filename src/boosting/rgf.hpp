#ifndef LIGHTGBM_BOOSTING_RGF_H_
#define LIGHTGBM_BOOSTING_RGF_H_

#include <LightGBM/utils/array_args.h>
#include <LightGBM/utils/log.h>
#include <LightGBM/utils/openmp_wrapper.h>
#include <LightGBM/boosting.h>

#include "score_updater.hpp"
#include "gbdt.h"

#include <assert.h>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <algorithm>

namespace LightGBM {
/*!
* \brief RGF algorithm implementation. including Training, prediction, bagging.
*/
class RGF: public GBDT {
public:
  /*!
  * \brief Constructor
  */
  RGF() : GBDT() { }
  /*!
  * \brief Destructor
  */
  ~RGF() { }
  /*!
  * \brief Initialization logic
  * \param config Config for boosting
  * \param train_data Training data
  * \param objective_function Training objective function
  * \param training_metrics Training metrics
  * \param output_model_filename Filename of output model
  */
  void Init(const BoostingConfig* config, const Dataset* train_data, const ObjectiveFunction* objective_function,
            const std::vector<const Metric*>& training_metrics) override {
    GBDT::Init(config, train_data, objective_function, training_metrics);
  }

  void ResetConfig(const BoostingConfig* config) override {
    GBDT::ResetConfig(config);
  }

  /*!
  * \brief one training iteration
  */

  bool TrainOneIter(const score_t* gradients, const score_t* hessians) {
    double init_score = 0.0f;
    // boosting first
    if (gradients == nullptr || hessians == nullptr) {
      init_score = BoostFromAverage();
      #ifdef TIMETAG
      auto start_time = std::chrono::steady_clock::now();
      #endif

      Boosting();
      gradients = gradients_.data();
      hessians = hessians_.data();

      #ifdef TIMETAG
      boosting_time += std::chrono::steady_clock::now() - start_time;
      #endif
    }

    #ifdef TIMETAG
    auto start_time = std::chrono::steady_clock::now();
    #endif

    bool should_continue = false;
    for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {

      #ifdef TIMETAG
      start_time = std::chrono::steady_clock::now();
      #endif
      const size_t bias = static_cast<size_t>(cur_tree_id) * num_data_;
      std::unique_ptr<Tree> new_tree(new Tree(2));
      if (class_need_train_[cur_tree_id]) {
        auto grad = gradients + bias;
        auto hess = hessians + bias;
        new_tree.reset(tree_learner_->Train(grad, hess, is_constant_hessian_));
      }

      #ifdef TIMETAG
      tree_time += std::chrono::steady_clock::now() - start_time;
      #endif

      if (new_tree->num_leaves() > 1) {
        should_continue = true;
        tree_learner_->RenewTreeOutput(new_tree.get(), objective_function_, train_score_updater_->score() + bias,
                                       num_data_, bag_data_indices_.data(), bag_data_cnt_);
        // shrinkage by learning rate
        new_tree->Shrinkage(shrinkage_rate_);
        // update score
        UpdateScore(new_tree.get(), cur_tree_id);
        if (std::fabs(init_score) > kEpsilon) {
          new_tree->AddBias(init_score);
        }
      } else {
        // only add default score one-time
        if (!class_need_train_[cur_tree_id] && models_.size() < static_cast<size_t>(num_tree_per_iteration_)) {
          auto output = class_default_output_[cur_tree_id];
          new_tree->AsConstantTree(output);
          // updates scores
          train_score_updater_->AddScore(output, cur_tree_id);
          for (auto& score_updater : valid_score_updater_) {
            score_updater->AddScore(output, cur_tree_id);
          }
        }
      }
      // add model
      models_.push_back(std::move(new_tree));

      if (iter_ != 0 && iter_ % 100 == 0) {
        FullyCorrectiveUpdate();
        Log::Info("Fully corrective updated.");
      }
    }

    if (!should_continue) {
      Log::Warning("Stopped training because there are no more leaves that meet the split requirements.");
      for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
        models_.pop_back();
      }
      return true;
    }

    ++iter_;
    return false;
  }

private:
  bool FullyCorrectiveUpdate () {
    int num_iterations = static_cast<int>(models_.size() / num_tree_per_iteration_);
    for (int iter = 0; iter < num_iterations; ++iter) {
      for (int tree_id = 0; tree_id < num_tree_per_iteration_; ++tree_id) {
        int model_index = iter * num_tree_per_iteration_ + tree_id;
        int64_t num_score = 0;
        objective_function_->
          GetGradients(GetTrainingScore(&num_score), gradients_.data(), hessians_.data());
        // auto gradients = gradients_.data();
        // auto hessians = hessians_.data();
        // for (int i = 0; i < num_data_; ++i) {
        //   leaf_pred[i] = tree_leaf_prediction[i][model_index];
        // }
        size_t bias = static_cast<size_t>(tree_id) * num_data_;
        auto gradients = gradients_.data() + bias;
        auto hessians = hessians_.data() + bias;
        auto new_tree = tree_learner_->FitByExistingTree(models_[model_index].get(), gradients, hessians);
        assert(new_tree->num_leaves() == models_[model_index]->num_leaves());
        for (int i = 0; i < new_tree->num_leaves(); ++i) {
          std::cout << "new: " << new_tree->LeafOutput(i) << "old: " << models_[model_index]->LeafOutput(i)
                    << std::endl;
        }
        train_score_updater_->AddScore(tree_learner_.get(), new_tree, tree_id);
        models_[model_index].reset(new_tree);
      }
    }
    return true;
  }
};

}  // namespace LightGBM
#endif   // LightGBM_BOOSTING_RGF_H_
