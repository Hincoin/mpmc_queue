#include <iostream>
#include <array>
#include <vector>
#include <queue>
#include <random>
#include <atomic>
#include <thread>
#include <ctime>
#include "lfqueue_stptr.h"
#include <unistd.h>


using SystemTime = timespec;
using counter_t  = std::size_t;
void sleep(int milliseconds)
{
	::usleep(milliseconds * 1000);
}

SystemTime getSystemTime()
{
	timespec t;
	std::atomic_signal_fence(std::memory_order_seq_cst);
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t) != 0) {
		t.tv_sec = (time_t)-1;
		t.tv_nsec = -1;
	}
	std::atomic_signal_fence(std::memory_order_seq_cst);
	
	return t;
}

double getTimeDelta(SystemTime start)
{
	timespec t;
	std::atomic_signal_fence(std::memory_order_seq_cst);
	if ((start.tv_sec == (time_t)-1 && start.tv_nsec == -1) || clock_gettime(CLOCK_MONOTONIC_RAW, &t) != 0) {
		return -1;
	}
	std::atomic_signal_fence(std::memory_order_seq_cst);

	return static_cast<double>(static_cast<long>(t.tv_sec) - static_cast<long>(start.tv_sec)) * 1000 + double(t.tv_nsec - start.tv_nsec) / 1000000;
}

template<typename TFunc>
counter_t rampUpToMeasurableNumberOfMaxOps(TFunc const& func, counter_t startOps = 256)
{
	counter_t ops = startOps;
	double time;
	do {
		time = func(ops);
		ops *= 2;
	} while (time < 20);
#ifdef NDEBUG
	return ops / 2;
#else
	return ops / 4;
#endif
}

std::size_t adjustForThreads(std::size_t suggestedOps, int nthreads)
{
	return std::max((counter_t)(suggestedOps / std::pow(2, std::sqrt((nthreads - 1) * 3))), suggestedOps / 16);
}



void heavy_concurrent(int nthreads)
{
		using SimpleThread = std::thread;
		using TQueue  = lf_queue<int>;

		TQueue q;
		std::vector<SimpleThread> threads(nthreads);
		std::vector<double> timings(nthreads);
		std::atomic<int> ready(0);

		counter_t single_threaded_ops = adjustForThreads(rampUpToMeasurableNumberOfMaxOps([](counter_t ops) {
			TQueue q;
			int item;
			auto start = getSystemTime();
			for (counter_t i = 0; i != ops; ++i) {
				q.enqueue(i);
				q.try_dequeue(item);
			}
			return getTimeDelta(start);
		}), nthreads);

		counter_t maxOps = single_threaded_ops * nthreads;
		for (int tid = 0; tid != nthreads; ++tid) {
			threads[tid] = SimpleThread([&](int id) {
				ready.fetch_add(1, std::memory_order_relaxed);
				while (ready.load(std::memory_order_relaxed) != nthreads)
					continue;
				
				if (id < 2) {
					// Alternate
					int item;
				
					for (counter_t i = 0; i != maxOps / 2; ++i) {
						q.try_dequeue(item);
						q.enqueue(i);
					}
					
				}
				else {
					if ((id & 1) == 0) {
						// Enqueue
						
						for (counter_t i = 0; i != maxOps; ++i) {
							q.enqueue(i);
						}
						
					}
					else {
						// Dequeue
						int item;
					
							for (counter_t i = 0; i != maxOps; ++i) {
								q.try_dequeue(item);
							}
					}
				}
			}, tid);
		}

		for(auto& task : threads)
		{
			task.join();
		}

}

void custom_bm()
{

	constexpr int num_threads = 1;
	std::vector<std::thread> threads;
	std::vector<std::vector<std::string>> consumer_results(num_threads);
	std::vector<std::vector<int>> consumer_results_idx(num_threads);
	std::array<std::string, 3> msgs = { {"Start it up! num_threads defeat the small string optimization!",
										"Kush! Dr. Dre && Snoop Dogg exclusive, SSO defeat, making this string longer",
										 "I am on this plane, with just over an hour left of flight time. Please defeat the SSO!"} };

	lf_queue<std::string> lfqueue;
	


	for(int i = 0; i < 100; ++i)
	{
		lfqueue.enqueue(msgs[i % 3]);
	}

	
	for(int i = 0; i < num_threads; ++i)
	{
		 
		 
			for(int shit = 0; shit < 32768; ++shit)
			{
						
				std::string tmp;
				for(int j = 0; j < 1024; ++j)
				{
					lfqueue.enqueue(msgs[j % 3]);
					
				}
				for(int j = 0; j < 512; ++j)
				{
					lfqueue.try_dequeue(tmp);
				}
				
			}
			

		 
	}

	for(auto& task : threads)
		task.join();

}
int main()
{

	custom_bm();	
}
