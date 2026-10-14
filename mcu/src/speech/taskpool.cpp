#include "speech/taskpool.h"

TaskPool::TaskPool(int threads)
{
	for (int i = 0; i < threads; ++i)
		this->threads.emplace_back([this] { Run(); });
}

TaskPool::~TaskPool()
{
	Stop();
}

void TaskPool::Post(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping)
			return;
		tasks.push_back(std::move(task));
	}
	wakeup.notify_one();
}

void TaskPool::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
	}
	wakeup.notify_all();
	for (std::thread& thread : threads)
		if (thread.joinable())
			thread.join();
}

void TaskPool::Run()
{
	for (;;)
	{
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wakeup.wait(lock, [this] { return stopping || !tasks.empty(); });
			if (tasks.empty())
				return;
			task = std::move(tasks.front());
			tasks.pop_front();
		}
		task();
	}
}
