/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <algorithm>
#include <fstream>
#include <iostream>
#include "paddle/fluid/inference/tests/api/tester_helper.h"

namespace paddle {
namespace inference {
namespace analysis {

struct OneSlotInBatch {
  std::string name;
  std::vector<std::vector<float>> data;
  std::vector<int> shape;
  std::vector<size_t> lod;
};

struct DataRecord {
  std::vector<std::vector<OneSlotInBatch>> batched_data;
  std::map<std::string, std::vector<std::vector<float>>> datasets;
  size_t batch_iter{0}, num_samples;  // total number of samples

  DataRecord() = default;
  explicit DataRecord(const std::string &path, int batch_size = 1) {
    Load(path);
    Prepare(batch_size);
  }

  void Load(const std::string &path) {
    std::ifstream file(path);
    constexpr int num_slots = 154;
    std::string line;
    int num_lines = 0;
    while (std::getline(file, line)) {
      num_lines++;
      std::vector<std::string> data;
      split(line, '\t', &data);
      std::vector<float> slot_data;
      split_to_float(data[1], ' ', &slot_data);
      std::string name = data[0];
      PADDLE_ENFORCE_EQ(slot_data.size() % 11, 0,
                        "line %d, %s should be divisible", num_lines, name);
      datasets[name].emplace_back(std::move(slot_data));
    }
    num_samples = num_lines / num_slots;
    PADDLE_ENFORCE_EQ(num_samples * num_slots, static_cast<size_t>(num_lines),
                      "num samples should be divisible");
    PADDLE_ENFORCE_GT(num_samples, 0);
  }

  void Prepare(int bs) {
    for (auto it = datasets.begin(); it != datasets.end(); ++it) {
      PADDLE_ENFORCE_EQ(it->second.size(), num_samples,
                        "size of each slot should be equal");
    }
    size_t num_batches = num_samples / bs;
    EXPECT_GT(num_batches, 0);
    batched_data.resize(num_batches);
    for (auto &one_batch : batched_data) {
      one_batch.resize(datasets.size());
      size_t i = 0;
      for (auto it = datasets.begin(); it != datasets.end(); ++it) {
        auto &slot = one_batch[i];
        slot.name = it->first;
        slot.data.resize(bs);
        slot.lod.resize(bs + 1);
        slot.lod[0] = 0;
        auto &lod = slot.lod;
        auto &datas = it->second;
        for (int k = 0; k < bs; ++k) {
          size_t id = k + batch_iter * bs;
          std::copy(datas[id].begin(), datas[id].end(),
                    std::back_inserter(slot.data[k]));
          size_t len = datas[id].size() / 11;
          PADDLE_ENFORCE_EQ(len * 11, datas[id].size(),
                            "%s %d size should be divisible", slot.name, id);
          lod[k + 1] = lod[k] + len;
        }
        slot.shape.assign({static_cast<int>(lod[bs]), 11});
        i++;
      }
    }
  }

  const std::vector<OneSlotInBatch> &NextBatch() {
    if (batch_iter >= batched_data.size() - 1) {
      batch_iter = -1;
    }
    return batched_data[++batch_iter];
  }
};

static void TensorAssignSlot(PaddleTensor *tensor, const OneSlotInBatch &slot) {
  tensor->name = slot.name + "_embed";
  tensor->shape = slot.shape;
  tensor->dtype = PaddleDType::FLOAT32;
  tensor->lod.clear();
  tensor->lod.emplace_back(slot.lod);
  TensorAssignData(tensor, slot.data);
}

void PrepareInputs(std::vector<PaddleTensor> *input_slots, DataRecord *data) {
  const auto &one_batch = data->NextBatch();
  input_slots->resize(one_batch.size());
  for (size_t i = 0; i < one_batch.size(); ++i) {
    auto &slot = one_batch[i];
    TensorAssignSlot(&((*input_slots)[i]), slot);
  }
}

void SetInput(std::vector<std::vector<PaddleTensor>> *inputs) {
  DataRecord data(FLAGS_infer_data, FLAGS_batch_size);
  std::vector<PaddleTensor> input_slots;
  int epoch = FLAGS_test_all_data ? data.batched_data.size() : 1;
  LOG(INFO) << "number of samples: "
            << data.batched_data.size() * FLAGS_batch_size;
  for (int bid = 0; bid < epoch; ++bid) {
    PrepareInputs(&input_slots, &data);
    (*inputs).emplace_back(input_slots);
  }
}

void SetConfig(AnalysisConfig *cfg, bool use_mkldnn = false) {
  cfg->SetModel(FLAGS_infer_model + "/model", FLAGS_infer_model + "/params");
  cfg->DisableGpu();
  cfg->SwitchSpecifyInputNames();
  cfg->pass_builder()->TurnOnDebug();
  cfg->SetCpuMathLibraryNumThreads(FLAGS_paddle_num_threads);
  if (use_mkldnn) {
    cfg->EnableMKLDNN();
  }
}

void profile(bool use_mkldnn = false) {
  AnalysisConfig cfg;
  SetConfig(&cfg, use_mkldnn);

  std::vector<PaddleTensor> outputs;
  std::vector<std::vector<PaddleTensor>> input_slots_all;
  SetInput(&input_slots_all);
  TestPrediction(reinterpret_cast<const PaddlePredictor::Config *>(&cfg),
                 input_slots_all, &outputs, FLAGS_num_threads);
}

TEST(Analyzer_seq_pool1, profile) { profile(); }

// Compare result of NativeConfig and AnalysisConfig
TEST(Analyzer_seq_pool1, compare) {
  AnalysisConfig cfg;
  SetConfig(&cfg);

  std::vector<std::vector<PaddleTensor>> input_slots_all;
  SetInput(&input_slots_all);
  CompareNativeAndAnalysis(
      reinterpret_cast<const PaddlePredictor::Config *>(&cfg), input_slots_all);
}

// Compare Deterministic result
TEST(Analyzer_seq_pool1, compare_determine) {
  AnalysisConfig cfg;
  SetConfig(&cfg);

  std::vector<std::vector<PaddleTensor>> input_slots_all;
  SetInput(&input_slots_all);
  CompareDeterministic(reinterpret_cast<const PaddlePredictor::Config *>(&cfg),
                       input_slots_all);
}

void analysis_fuse_statis(bool use_zerocopy) {
  AnalysisConfig cfg;
  SetConfig(&cfg);
  cfg.SwitchUseFeedFetchOps(!use_zerocopy);
  int num_ops;
  auto predictor = CreatePaddlePredictor<AnalysisConfig>(cfg);
  auto fuse_statis = GetFuseStatis(predictor.get(), &num_ops);
  ASSERT_TRUE(fuse_statis.count("fc_fuse"));
  ASSERT_EQ(fuse_statis.at("fc_fuse"), 10);
  ASSERT_TRUE(fuse_statis.count("seqpool_concat_fuse"));
  EXPECT_EQ(fuse_statis.at("seqpool_concat_fuse"), 2);
  LOG(INFO) << "num_ops: " << num_ops;
  EXPECT_EQ(num_ops, 195);
}

// Check the fuse status
TEST(Analyzer_seq_pool1, fuse_statis) { analysis_fuse_statis(false); }

void PrepareZeroCopyInputs(
    const std::unique_ptr<PaddlePredictor> &predictor,
    std::vector<std::unique_ptr<ZeroCopyTensor>> *inputs) {
  DataRecord data(FLAGS_infer_data, FLAGS_batch_size);
  // only feed one batch
  const auto &one_batch = data.NextBatch();
  inputs->clear();
  for (size_t i = 0; i < one_batch.size(); ++i) {
    auto &slot = one_batch[i];
    auto tensor = predictor->GetInputTensor(slot.name + "_embed");
    tensor->Reshape(slot.shape);
    tensor->SetLoD({slot.lod});
    ZeroCopyTensorAssignData<float>(tensor.get(), slot.data);
    inputs->emplace_back(std::move(tensor));
  }
}

// diff: similarity_norm.tmp_0, // speed: fc_4.tmp_1
static const char out_var_name[] = "reduce_sum_0.tmp_0";

// return the output values
std::vector<float> zerocopy_profile(int repeat_times) {
  AnalysisConfig config;
  SetConfig(&config);
  config.SwitchUseFeedFetchOps(false);
  auto predictor = CreatePaddlePredictor<AnalysisConfig>(config);
  std::vector<std::unique_ptr<ZeroCopyTensor>> inputs;
  PrepareZeroCopyInputs(predictor, &inputs);
  auto output_tensor = predictor->GetOutputTensor(out_var_name);
  Timer timer;
  LOG(INFO) << "Warm up run...";
  timer.tic();
  predictor->ZeroCopyRun();
  PrintTime(FLAGS_batch_size, 1, 1, 0, timer.toc(), 1);
  if (FLAGS_profile) {
    paddle::platform::ResetProfiler();
  }
  LOG(INFO) << "Run " << repeat_times << " times...";
  timer.tic();
  for (int i = 0; i < repeat_times; i++) {
    predictor->ZeroCopyRun();
  }
  PrintTime(FLAGS_batch_size, repeat_times, 1, 0, timer.toc() / repeat_times,
            1);

  LOG(INFO) << "ZeroCopy output: " << DescribeZeroCopyTensor(*output_tensor);
  PaddlePlace place;
  int output_size{0};
  auto *pdata = output_tensor->data<float>(&place, &output_size);
  std::vector<float> res(output_size);
  for (int i = 0; i < output_size; ++i) {
    res[i] = pdata[i];
  }
  return res;
}

TEST(Analyzer_seq_pool1, zerocopy_profile) { zerocopy_profile(FLAGS_repeat); }

TEST(Analyzer_seq_pool1, zerocopy_profile_threads) {
  AnalysisConfig config;
  SetConfig(&config);
  config.SwitchUseFeedFetchOps(false);

  auto base_predictor = CreatePaddlePredictor<AnalysisConfig>(config);
  double total_time_of_threads{0};
  std::vector<std::thread> threads;

  for (int tid = 0; tid < FLAGS_num_threads; tid++) {
    threads.emplace_back([&, tid] {
      // To ensure the thread binding correctly,
      // please clone inside the threadpool.
      auto predictor = base_predictor->Clone();
      std::vector<std::unique_ptr<ZeroCopyTensor>> inputs;
      PrepareZeroCopyInputs(predictor, &inputs);
      auto output_tensor = predictor->GetOutputTensor(out_var_name);
      Timer timer;
      double total_time{0};

      LOG(INFO) << "Warm up run...";
      timer.tic();
      predictor->ZeroCopyRun();
      PrintTime(FLAGS_batch_size, 1, FLAGS_num_threads, tid, timer.toc(), 1);
      if (FLAGS_profile) {
        paddle::platform::ResetProfiler();
      }
      int repeat_times = FLAGS_repeat;
      LOG(INFO) << "Run " << repeat_times << " times...";
      timer.tic();

      for (int i = 0; i < repeat_times; i++) {
        predictor->ZeroCopyRun();
      }
      total_time += timer.toc();
      total_time_of_threads += total_time;

      LOG(INFO) << "thread time: " << total_time / repeat_times;
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  LOG(INFO) << "average time: "
            << total_time_of_threads / FLAGS_num_threads / FLAGS_repeat;
}

TEST(Analyzer_seq_pool1, zerocopy_fuse_statis) { analysis_fuse_statis(true); }

TEST(Analyzer_seq_pool1, zerocopy_compare_native) {
  AnalysisConfig config;
  SetConfig(&config);
  config.SwitchUseFeedFetchOps(true);
  auto predictor = CreatePaddlePredictor<NativeConfig>(config.ToNativeConfig());
  std::vector<PaddleTensor> native_outputs;
  std::vector<std::vector<PaddleTensor>> input_slots_all;
  SetInput(&input_slots_all);
  ASSERT_TRUE(predictor->Run(input_slots_all[0], &native_outputs));
  EXPECT_EQ(native_outputs.size(), 1UL);

  auto zerocopy_output = zerocopy_profile(1);
  EXPECT_EQ(zerocopy_output.size() * sizeof(float),
            native_outputs.front().data.length());
  auto *native_data = static_cast<float *>(native_outputs.front().data.data());
  for (size_t i = 0; i < zerocopy_output.size(); ++i) {
    EXPECT_NEAR(zerocopy_output[i], native_data[i], 1e-3);
  }
}

}  // namespace analysis
}  // namespace inference
}  // namespace paddle
