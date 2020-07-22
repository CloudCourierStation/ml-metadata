/* Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/tools/mlmd_bench/thread_runner.h"

#include <vector>

#include "ml_metadata/metadata_store/metadata_store.h"
#include "ml_metadata/metadata_store/metadata_store_factory.h"
#include "ml_metadata/metadata_store/types.h"
#include "ml_metadata/proto/metadata_store.pb.h"
#include "ml_metadata/tools/mlmd_bench/benchmark.h"
#include "ml_metadata/tools/mlmd_bench/proto/mlmd_bench.pb.h"
#include "ml_metadata/tools/mlmd_bench/stats.h"
#include "ml_metadata/tools/mlmd_bench/workload.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/threadpool.h"

namespace ml_metadata {
namespace {

// Prepares a list of MLMD client instance(`stores`) for each thread.
tensorflow::Status PrepareStoresForThreads(
    const ConnectionConfig mlmd_config, const int64 num_threads,
    std::vector<std::unique_ptr<MetadataStore>>& stores) {
  stores.resize(num_threads);
  // Each thread uses a different MLMD client instance to talk to
  // the same back-end.
  for (int64 i = 0; i < num_threads; ++i) {
    std::unique_ptr<MetadataStore> store;
    TF_RETURN_IF_ERROR(CreateMetadataStore(mlmd_config, &store));
    stores[i] = std::move(store);
  }
  return tensorflow::Status::OK();
}

// Sets up the current workload.
tensorflow::Status SetUpWorkload(const ConnectionConfig mlmd_config,
                                 WorkloadBase* workload) {
  std::unique_ptr<MetadataStore> set_up_store;
  TF_RETURN_IF_ERROR(CreateMetadataStore(mlmd_config, &set_up_store));
  TF_RETURN_IF_ERROR(workload->SetUp(set_up_store.get()));
  return tensorflow::Status::OK();
}

// Executes the current workload and updates `curr_thread_stats` with `op_stats`
// along the way.
tensorflow::Status ExecuteWorkload(const int64 work_items_start_index,
                                   const int64 op_per_thread,
                                   MetadataStore* curr_store,
                                   WorkloadBase* workload,
                                   int64& approx_total_done,
                                   ThreadStats& curr_thread_stats) {
  int64 work_items_index = work_items_start_index;
  while (work_items_index < work_items_start_index + op_per_thread) {
    // Each operation has a op_stats.
    OpStats op_stats;
    tensorflow::Status status =
        workload->RunOp(work_items_index, curr_store, op_stats);
    // If the error is not Abort error, break the current process.
    if (!status.ok() && status.code() != tensorflow::error::ABORTED) {
      TF_RETURN_IF_ERROR(status);
    }
    // Handles abort issues for concurrent writing to the db.
    if (!status.ok()) {
      continue;
    }
    work_items_index++;
    approx_total_done++;
    // Updates the current thread stats using the `op_stats`.
    curr_thread_stats.Update(op_stats, approx_total_done);
  }
  return tensorflow::Status::OK();
}

// Merges all the thread stats inside `thread_stats_list` into a workload stats
// and reports the workload's performance according to that. Also, store the
// performance inside `workload_summary`.
void MergeThreadStatsAndReport(const std::string workload_name,
                               ThreadStats thread_stats_list[], int64 size,
                               WorkloadConfigResult* workload_summary) {
  for (int64 i = 1; i < size; ++i) {
    thread_stats_list[0].Merge(thread_stats_list[i]);
  }
  // Reports the metrics of interests.
  std::pair<double, double> report = thread_stats_list[0].Report(workload_name);
  // Stores the performance result.
  workload_summary->set_bytes_per_second(report.first);
  workload_summary->set_microseconds_per_operation(report.second);
}

}  // namespace

ThreadRunner::ThreadRunner(const ConnectionConfig& mlmd_config,
                           const int64 num_threads)
    : mlmd_config_(mlmd_config), num_threads_(num_threads) {}

// The thread runner will first loops over all the executable workloads in
// benchmark and executes them one by one. Each workload will have a
// `thread_stats_list` to record the stats of each thread when executing the
// current workload.
// During the execution, each operation will has a `op_stats` to record current
// operation statistic. Each `op_stats` will be used to update the
// `thread_stats`.
// After the each thread has finished the execution, the workload stats will be
// generated by merging all the thread stats inside the `thread_stats_list`. The
// performance of the workload will be reported according to the workload stats.
tensorflow::Status ThreadRunner::Run(Benchmark& benchmark,
                                     MLMDBenchReport& mlmd_bench_report) {
  for (int i = 0; i < benchmark.num_workloads(); ++i) {
    WorkloadBase* workload = benchmark.workload(i);
    ThreadStats thread_stats_list[num_threads_];
    tensorflow::Status thread_status_list[num_threads_];
    TF_RETURN_IF_ERROR(SetUpWorkload(mlmd_config_, workload));
    const int64 op_per_thread = workload->num_operations() / num_threads_;
    std::vector<std::unique_ptr<MetadataStore>> stores;
    TF_RETURN_IF_ERROR(
        PrepareStoresForThreads(mlmd_config_, num_threads_, stores));
    WorkloadConfigResult* workload_summary =
        mlmd_bench_report.mutable_summaries(i);
    {
      // Create a thread pool for multi-thread execution.
      tensorflow::thread::ThreadPool pool(tensorflow::Env::Default(),
                                          "mlmd_bench", num_threads_);
      // `approx_total_done` is used for reporting progress along the way.
      int64 approx_total_done = 0;
      for (int64 t = 0; t < num_threads_; ++t) {
        const int64 work_items_start_index = op_per_thread * t;
        ThreadStats& curr_thread_stats = thread_stats_list[t];
        MetadataStore* curr_store = stores[t].get();
        tensorflow::Status& curr_status = thread_status_list[t];
        pool.Schedule([this, op_per_thread, workload, work_items_start_index,
                       curr_store, &curr_thread_stats, &curr_status,
                       &approx_total_done]() {
          curr_thread_stats.Start();
          curr_status.Update(
              ExecuteWorkload(work_items_start_index, op_per_thread, curr_store,
                              workload, approx_total_done, curr_thread_stats));
          curr_thread_stats.Stop();
        });
        TF_RETURN_IF_ERROR(curr_status);
      }
    }
    TF_RETURN_IF_ERROR(workload->TearDown());
    MergeThreadStatsAndReport(workload->GetName(), thread_stats_list,
                              num_threads_, workload_summary);
  }
  return tensorflow::Status::OK();
}

}  // namespace ml_metadata
