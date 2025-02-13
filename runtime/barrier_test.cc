/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "barrier.h"

#include <string>

#include "base/atomic.h"
#include "common_runtime_test.h"
#include "mirror/object_array-inl.h"
#include "thread-current-inl.h"
#include "thread_pool.h"

namespace art {
class CheckWaitTask : public Task {
 public:
  CheckWaitTask(Barrier* barrier, AtomicInteger* count1, AtomicInteger* count2)
      : barrier_(barrier),
        count1_(count1),
        count2_(count2) {}

  void Run(Thread* self) override {
    LOG(INFO) << "Before barrier" << *self;
    ++*count1_;
    barrier_->Wait(self);
    ++*count2_;
    LOG(INFO) << "After barrier" << *self;
  }

  void Finalize() override {
    delete this;
  }

 private:
  Barrier* const barrier_;
  AtomicInteger* const count1_;
  AtomicInteger* const count2_;
};

class BarrierTest : public CommonRuntimeTest {
 public:
  BarrierTest() {
    use_boot_image_ = true;  // Make the Runtime creation cheaper.
  }

  static int32_t num_threads;
};

int32_t BarrierTest::num_threads = 4;

// Check that barrier wait and barrier increment work.
TEST_F(BarrierTest, CheckWait) {
  Thread* self = Thread::Current();
  std::unique_ptr<ThreadPool> thread_pool(
      ThreadPool::Create("Barrier test thread pool", num_threads));
  Barrier barrier(num_threads + 1);  // One extra Wait() in main thread.
  Barrier timeout_barrier(0);  // Only used for sleeping on timeout.
  AtomicInteger count1(0);
  AtomicInteger count2(0);
  for (int32_t i = 0; i < num_threads; ++i) {
    thread_pool->AddTask(self, new CheckWaitTask(&barrier, &count1, &count2));
  }
  thread_pool->StartWorkers(self);
  while (count1.load(std::memory_order_relaxed) != num_threads) {
    timeout_barrier.Increment(self, 1, 100);  // sleep 100 msecs
  }
  // Count 2 should still be zero since no thread should have gone past the barrier.
  EXPECT_EQ(0, count2.load(std::memory_order_relaxed));
  // Perform one additional Wait(), allowing pool threads to proceed.
  barrier.Wait(self);
  // Wait for all the threads to finish.
  thread_pool->Wait(self, true, false);
  // Both counts should be equal to num_threads now.
  EXPECT_EQ(count1.load(std::memory_order_relaxed), num_threads);
  EXPECT_EQ(count2.load(std::memory_order_relaxed), num_threads);
  timeout_barrier.Init(self, 0);  // Reset to zero for destruction.
}

class CheckPassTask : public Task {
 public:
  CheckPassTask(Barrier* barrier, AtomicInteger* count, size_t subtasks)
      : barrier_(barrier),
        count_(count),
        subtasks_(subtasks) {}

  void Run(Thread* self) override {
    for (size_t i = 0; i < subtasks_; ++i) {
      ++*count_;
      // Pass through to next subtask.
      barrier_->Pass(self);
    }
  }

  void Finalize() override {
    delete this;
  }
 private:
  Barrier* const barrier_;
  AtomicInteger* const count_;
  const size_t subtasks_;
};

// Check that barrier pass through works.
TEST_F(BarrierTest, CheckPass) {
  Thread* self = Thread::Current();
  std::unique_ptr<ThreadPool> thread_pool(
      ThreadPool::Create("Barrier test thread pool", num_threads));
  Barrier barrier(0);
  AtomicInteger count(0);
  const int32_t num_tasks = num_threads * 4;
  const int32_t num_sub_tasks = 128;
  for (int32_t i = 0; i < num_tasks; ++i) {
    thread_pool->AddTask(self, new CheckPassTask(&barrier, &count, num_sub_tasks));
  }
  thread_pool->StartWorkers(self);
  const int32_t expected_total_tasks = num_sub_tasks * num_tasks;
  // Wait for all the tasks to complete using the barrier.
  barrier.Increment(self, expected_total_tasks);
  // The total number of completed tasks should be equal to expected_total_tasks.
  EXPECT_EQ(count.load(std::memory_order_relaxed), expected_total_tasks);
}

}  // namespace art
