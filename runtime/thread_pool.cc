
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

#include "thread_pool.h"

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <pthread.h>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/stl_util.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "runtime.h"
#include "thread-current-inl.h"

namespace art {

using android::base::StringPrintf;

static constexpr bool kMeasureWaitTime = false;

#if defined(__BIONIC__)
static constexpr bool kUseCustomThreadPoolStack = false;
#else
static constexpr bool kUseCustomThreadPoolStack = true;
#endif

ThreadPoolWorker::ThreadPoolWorker(AbstractThreadPool* thread_pool,
                                   const std::string& name,
                                   size_t stack_size)
    : thread_pool_(thread_pool),
      name_(name) {
  std::string error_msg;
  // On Bionic, we know pthreads will give us a big-enough stack with
  // a guard page, so don't do anything special on Bionic libc.
  if (kUseCustomThreadPoolStack) {
    // Add an inaccessible page to catch stack overflow.
    stack_size += kPageSize;
    stack_ = MemMap::MapAnonymous(name.c_str(),
                                  stack_size,
                                  PROT_READ | PROT_WRITE,
                                  /*low_4gb=*/ false,
                                  &error_msg);
    CHECK(stack_.IsValid()) << error_msg;
    CHECK_ALIGNED(stack_.Begin(), kPageSize);
    CheckedCall(mprotect,
                "mprotect bottom page of thread pool worker stack",
                stack_.Begin(),
                kPageSize,
                PROT_NONE);
  }
  const char* reason = "new thread pool worker thread";
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), reason);
  if (kUseCustomThreadPoolStack) {
    CHECK_PTHREAD_CALL(pthread_attr_setstack, (&attr, stack_.Begin(), stack_.Size()), reason);
  } else {
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), reason);
  }
  CHECK_PTHREAD_CALL(pthread_create, (&pthread_, &attr, &Callback, this), reason);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), reason);
}

ThreadPoolWorker::~ThreadPoolWorker() {
  CHECK_PTHREAD_CALL(pthread_join, (pthread_, nullptr), "thread pool worker shutdown");
}

void ThreadPoolWorker::SetPthreadPriority(int priority) {
  CHECK_GE(priority, PRIO_MIN);
  CHECK_LE(priority, PRIO_MAX);
#if defined(ART_TARGET_ANDROID)
  int result = setpriority(PRIO_PROCESS, pthread_gettid_np(pthread_), priority);
  if (result != 0) {
    PLOG(ERROR) << "Failed to setpriority to :" << priority;
  }
#else
  UNUSED(priority);
#endif
}

int ThreadPoolWorker::GetPthreadPriority() {
#if defined(ART_TARGET_ANDROID)
  return getpriority(PRIO_PROCESS, pthread_gettid_np(pthread_));
#else
  return 0;
#endif
}

void ThreadPoolWorker::Run() {
  Thread* self = Thread::Current();
  Task* task = nullptr;
  thread_pool_->creation_barier_.Pass(self);
  while ((task = thread_pool_->GetTask(self)) != nullptr) {
    task->Run(self);
    task->Finalize();
  }
}

void* ThreadPoolWorker::Callback(void* arg) {
  ThreadPoolWorker* worker = reinterpret_cast<ThreadPoolWorker*>(arg);
  Runtime* runtime = Runtime::Current();
  // Don't run callbacks for ThreadPoolWorkers. These are created for JITThreadPool and
  // HeapThreadPool and are purely internal threads of the runtime and we don't need to run
  // callbacks for the thread attach / detach listeners.
  // (b/251163712) Calling callbacks for heap thread pool workers causes deadlocks in some libjdwp
  // tests. Deadlocks happen when a GC thread is attached while libjdwp holds the event handler
  // lock for an event that triggers an entrypoint update from deopt manager.
  CHECK(runtime->AttachCurrentThread(
      worker->name_.c_str(),
      true,
      // Thread-groups are only tracked by the peer j.l.Thread objects. If we aren't creating peers
      // we don't need to specify the thread group. We want to place these threads in the System
      // thread group because that thread group is where important threads that debuggers and
      // similar tools should not mess with are placed. As this is an internal-thread-pool we might
      // rely on being able to (for example) wait for all threads to finish some task. If debuggers
      // are suspending these threads that might not be possible.
      worker->thread_pool_->create_peers_ ? runtime->GetSystemThreadGroup() : nullptr,
      worker->thread_pool_->create_peers_,
      /* should_run_callbacks= */ false));
  worker->thread_ = Thread::Current();
  // Mark thread pool workers as runtime-threads.
  worker->thread_->SetIsRuntimeThread(true);
  // Do work until its time to shut down.
  worker->Run();
  runtime->DetachCurrentThread(/* should_run_callbacks= */ false);
  return nullptr;
}

void ThreadPool::AddTask(Thread* self, Task* task) {
  MutexLock mu(self, task_queue_lock_);
  tasks_.push_back(task);
  // If we have any waiters, signal one.
  if (started_ && waiting_count_ != 0) {
    task_queue_condition_.Signal(self);
  }
}

void ThreadPool::RemoveAllTasks(Thread* self) {
  // The ThreadPool is responsible for calling Finalize (which usually delete
  // the task memory) on all the tasks.
  Task* task = nullptr;
  do {
    {
      MutexLock mu(self, task_queue_lock_);
      if (tasks_.empty()) {
        return;
      }
      task = tasks_.front();
      tasks_.pop_front();
    }
    task->Finalize();
  } while (true);
}

ThreadPool::~ThreadPool() {
  DeleteThreads();
  RemoveAllTasks(Thread::Current());
}

AbstractThreadPool::AbstractThreadPool(const char* name,
                                       size_t num_threads,
                                       bool create_peers,
                                       size_t worker_stack_size)
  : name_(name),
    task_queue_lock_("task queue lock", kGenericBottomLock),
    task_queue_condition_("task queue condition", task_queue_lock_),
    completion_condition_("task completion condition", task_queue_lock_),
    started_(false),
    shutting_down_(false),
    waiting_count_(0),
    start_time_(0),
    total_wait_time_(0),
    creation_barier_(0),
    max_active_workers_(num_threads),
    create_peers_(create_peers),
    worker_stack_size_(worker_stack_size) {}

void AbstractThreadPool::CreateThreads() {
  CHECK(threads_.empty());
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, task_queue_lock_);
    shutting_down_ = false;
    // Add one since the caller of constructor waits on the barrier too.
    creation_barier_.Init(self, max_active_workers_);
    while (GetThreadCount() < max_active_workers_) {
      const std::string worker_name = StringPrintf("%s worker thread %zu", name_.c_str(),
                                                   GetThreadCount());
      threads_.push_back(
          new ThreadPoolWorker(this, worker_name, worker_stack_size_));
    }
  }
}

void AbstractThreadPool::WaitForWorkersToBeCreated() {
  creation_barier_.Increment(Thread::Current(), 0);
}

const std::vector<ThreadPoolWorker*>& AbstractThreadPool::GetWorkers() {
  // Wait for all the workers to be created before returning them.
  WaitForWorkersToBeCreated();
  return threads_;
}

void AbstractThreadPool::DeleteThreads() {
  {
    Thread* self = Thread::Current();
    MutexLock mu(self, task_queue_lock_);
    // Tell any remaining workers to shut down.
    shutting_down_ = true;
    // Broadcast to everyone waiting.
    task_queue_condition_.Broadcast(self);
    completion_condition_.Broadcast(self);
  }
  // Wait for the threads to finish. We expect the user of the pool
  // not to run multi-threaded calls to `CreateThreads` and `DeleteThreads`,
  // so we don't guard the field here.
  STLDeleteElements(&threads_);
}

void AbstractThreadPool::SetMaxActiveWorkers(size_t max_workers) {
  MutexLock mu(Thread::Current(), task_queue_lock_);
  CHECK_LE(max_workers, GetThreadCount());
  max_active_workers_ = max_workers;
}

void AbstractThreadPool::StartWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = true;
  task_queue_condition_.Broadcast(self);
  start_time_ = NanoTime();
  total_wait_time_ = 0;
}

void AbstractThreadPool::StopWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = false;
}

bool AbstractThreadPool::HasStarted(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return started_;
}

Task* AbstractThreadPool::GetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  while (!IsShuttingDown()) {
    const size_t thread_count = GetThreadCount();
    // Ensure that we don't use more threads than the maximum active workers.
    const size_t active_threads = thread_count - waiting_count_;
    // <= since self is considered an active worker.
    if (active_threads <= max_active_workers_) {
      Task* task = TryGetTaskLocked();
      if (task != nullptr) {
        return task;
      }
    }

    ++waiting_count_;
    if (waiting_count_ == GetThreadCount() && !HasOutstandingTasks()) {
      // We may be done, lets broadcast to the completion condition.
      completion_condition_.Broadcast(self);
    }
    const uint64_t wait_start = kMeasureWaitTime ? NanoTime() : 0;
    task_queue_condition_.Wait(self);
    if (kMeasureWaitTime) {
      const uint64_t wait_end = NanoTime();
      total_wait_time_ += wait_end - std::max(wait_start, start_time_);
    }
    --waiting_count_;
  }

  // We are shutting down, return null to tell the worker thread to stop looping.
  return nullptr;
}

Task* AbstractThreadPool::TryGetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return TryGetTaskLocked();
}

Task* ThreadPool::TryGetTaskLocked() {
  if (HasOutstandingTasks()) {
    Task* task = tasks_.front();
    tasks_.pop_front();
    return task;
  }
  return nullptr;
}

bool AbstractThreadPool::IsActive(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return waiting_count_ != GetThreadCount() || HasOutstandingTasks();
}

void AbstractThreadPool::Wait(Thread* self, bool do_work, bool may_hold_locks) {
  if (do_work) {
    CHECK(!create_peers_);
    Task* task = nullptr;
    while ((task = TryGetTask(self)) != nullptr) {
      task->Run(self);
      task->Finalize();
    }
  }
  // Wait until each thread is waiting and the task list is empty.
  MutexLock mu(self, task_queue_lock_);
  while (!shutting_down_ && (waiting_count_ != GetThreadCount() || HasOutstandingTasks())) {
    if (!may_hold_locks) {
      completion_condition_.Wait(self);
    } else {
      completion_condition_.WaitHoldingLocks(self);
    }
  }
}

size_t ThreadPool::GetTaskCount(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return tasks_.size();
}

void AbstractThreadPool::SetPthreadPriority(int priority) {
  for (ThreadPoolWorker* worker : threads_) {
    worker->SetPthreadPriority(priority);
  }
}

void AbstractThreadPool::CheckPthreadPriority(int priority) {
#if defined(ART_TARGET_ANDROID)
  for (ThreadPoolWorker* worker : threads_) {
    CHECK_EQ(worker->GetPthreadPriority(), priority);
  }
#else
  UNUSED(priority);
#endif
}

}  // namespace art
