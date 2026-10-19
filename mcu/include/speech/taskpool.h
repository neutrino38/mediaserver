#ifndef SPEECH_TASKPOOL_H
#define SPEECH_TASKPOOL_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * A fixed set of threads running posted tasks in order of arrival. What may
 * wait — a DNS lookup, an HTTPS request to Mistral — runs here, never on the
 * WebSocket reactor.
 */
class TaskPool
{
public:
	explicit TaskPool(int threads);
	~TaskPool();

	void Post(std::function<void()> task);

	// Runs the tasks already posted, then joins the threads. Idempotent.
	void Stop();

private:
	void Run();

	std::mutex				mutex;
	std::condition_variable			wakeup;
	std::deque<std::function<void()>>	tasks;
	bool					stopping = false;
	std::vector<std::thread>		threads;
};

#endif
