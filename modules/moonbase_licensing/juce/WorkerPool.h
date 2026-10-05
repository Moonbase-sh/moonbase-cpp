#pragma once

// The controller's background workers: a few std::threads draining a queue.
//
// Deliberately not a juce::ThreadPool. When one is destroyed, JUCE gives each
// idle thread 500 ms to notice and exit, then kills it by force. On JUCE 6.1.3
// an idle pool thread now and then misses that window, and a killed thread can
// leave a lock held that hangs a later teardown in the same process. These
// workers sleep on a condition variable with no timeout and are always joined:
// never killed, and never left running.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration::detail {

class WorkerPool
{
public:
    explicit WorkerPool(int numThreads)
    {
        for (int i = 0; i < numThreads; ++i)
        {
            try
            {
                threads_.emplace_back([this] { run(); });
            }
            catch (const std::system_error&)
            {
                break; // out of threads: make do with the ones we have
            }
        }
        runInline_ = threads_.empty();
    }

    ~WorkerPool() { stop(); }

    // Runs the job on a worker. With no worker at all (the system refused to
    // start one) it runs right here instead, so the caller still gets its answer.
    void addJob(std::function<void()> job)
    {
        if (runInline_)
        {
            runJob(job);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
                return;
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

    // Runs on each worker as it exits, e.g. to detach it from the Java VM on
    // Android. Set it once, right after construction.
    void setWorkerExitHook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onExit_ = std::move(hook);
    }

    // Drops the jobs that haven't started, then waits for the running ones and
    // joins the workers. Callers cancel what they can first (the controller
    // cancels its HTTP requests), so this is normally instant. It never gives up
    // on a worker, because one left running could still be executing module code
    // after the plugin binary is unloaded. The longest it waits is a job blocked
    // on the license lock while another process holds it for its own network
    // check, which that process's request timeouts bound. Safe to call twice.
    void stop()
    {
        if (threads_.empty())
            return; // already stopped (or never had a worker)

        std::deque<std::function<void()>> dropped; // destroyed below, outside the lock
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            dropped.swap(jobs_);
        }
        wake_.notify_all();

        for (auto& thread : threads_)
            thread.join();
        threads_.clear();
    }

private:
    void run()
    {
        juce::Thread::setCurrentThreadName("Moonbase worker");

        std::unique_lock<std::mutex> lock(mutex_);
        for (;;)
        {
            wake_.wait(lock, [this] { return stopping_ || ! jobs_.empty(); });
            if (stopping_)
                break;

            auto job = std::move(jobs_.front());
            jobs_.pop_front();
            lock.unlock();
            runJob(job);
            lock.lock();
        }

        const auto onExit = onExit_;
        lock.unlock();
        if (onExit)
            onExit();
    }

    static void runJob(std::function<void()>& job)
    {
        // JUCE's networking creates Objective-C objects on Apple platforms, and
        // a plain std::thread has no autorelease pool of its own.
        JUCE_AUTORELEASEPOOL
        {
            try
            {
                job();
            }
            catch (...)
            {
                jassertfalse; // jobs handle their own errors; this one escaped
            }
            job = nullptr; // release the captures here, not under the queue lock
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_; // a job arrived, or stop() was called
    std::deque<std::function<void()>> jobs_;
    std::function<void()> onExit_;
    bool stopping_ = false;

    std::vector<std::thread> threads_;
    bool runInline_ = false;

    JUCE_DECLARE_NON_COPYABLE(WorkerPool)
};

} // namespace moonbase::juce_integration::detail
