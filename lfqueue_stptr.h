#include <iostream>
#include <array>
#include <vector>
#include <queue>
#include <random>
#include <atomic>
#include <thread>
#include <type_traits>

namespace hin
{

	template<typename T>
	class lf_queue
	{
	private:

		template<typename Ptr, int N> struct queue_entry;

		static constexpr int alignment  = 64; // cacheline alignment, 
		static constexpr int num_queues = 32;
		static constexpr int queue_mask = num_queues - 1;

		using underlying_type = std::queue<T>;
		using pointer  		  = underlying_type*;
		using type 			  = queue_entry<pointer, alignment>;
		using container 	  = std::array<type, num_queues>;

		template<typename Ptr, int N>
	    struct alignas(alignment) queue_entry
		{
			std::atomic<Ptr> atomic_ptr;
			alignas(alignment) std::atomic<bool> dirty_;

			queue_entry() : dirty_(false) {}

			template<typename U>
			void store(U&& data, std::memory_order tag = std::memory_order_seq_cst)
			{
				atomic_ptr.store(std::forward<U>(data), tag);
			}
			Ptr load(std::memory_order tag = std::memory_order_seq_cst)
			{
				return atomic_ptr.load(tag);
			}

			bool compare_exchange_weak(Ptr& expec, Ptr desired, std::memory_order success, std::memory_order failure)
			{
				return atomic_ptr.compare_exchange_weak(expec, desired, success, failure);
			}

			bool compare_exchange_strong(Ptr& expec, Ptr desired, std::memory_order success, std::memory_order failure)
			{
				return atomic_ptr.compare_exchange_strong(expec, desired, success, failure);
			}

		};

		container data_;
		std::atomic<int> thread_offset_;

	public:


		lf_queue() : thread_offset_(0) 
		{
			for(int i = 0 ; i < num_queues; ++i)
			 {
			 		data_[i].store( new underlying_type{}, std::memory_order_relaxed );
			 }
		}

		/* a queue_holder serves as an RAII wrapper for ownership of a single-threaded queue
		   within data_. It stores the index at which it acquired the queue as well as the 
		   original pointer value present in the index before it was exchanged for nullptr.
		   Upon destruction, queue_holder will restore the index of data_ from where it retrieved
		   the queue to its original value and additionally set a boolean flag indicating wether
		   or not this queue still has data to be potentially dequeued.
		*/
		struct queue_holder
		{
			container& data_;
			int index_;
			pointer ptr_;
		
			queue_holder(int idx, pointer p, container& init) : data_(init), index_(idx), ptr_(p) {}
			queue_holder(const queue_holder&) = delete;
			queue_holder(queue_holder&& other) : data_(other.data_), index_(other.index_), ptr_(other.ptr_)
			{
				other.ptr_ = nullptr;
			}
			queue_holder& operator=(queue_holder&& other)
			{
				
				data_[index_].store(ptr_, std::memory_order_release); // release current pointer
				index_ = other.index_;
				ptr_ = other.ptr_;
				other.ptr_ = nullptr;

				return *this;
			}
			queue_holder& operator=(const queue_holder&) = delete;

			underlying_type& queue()
			{
				return *ptr_;
			}
			~queue_holder()
			{
				if(ptr_)
				{
					data_[index_].dirty_.store(! queue().empty(), std::memory_order_relaxed);
					data_[index_].store(ptr_, std::memory_order_release);
				}
			}
		};

		unsigned int get_index()
		{	

			static thread_local unsigned int cpuid = sched_getcpu() * 4 ;
			static thread_local int countdown = 500;
			static thread_local int 		 local_offset = thread_offset_.fetch_add(1, std::memory_order_relaxed) & 3;
			

			if(countdown-- > 0)
				return cpuid + local_offset;

			countdown = 500;
			__rdtscp(&cpuid);
			cpuid *= 4;
			return cpuid + local_offset;
			
		}

	
		/* dequeue-ing threads need to acquire queues with data in them (empty() == false) to avoid
		   several compare_exchange_strong operations in an effort to find a properly filled queue.
		   To aid this process, the destructor of queue_holder ( which is used as an RAII wrapper)
		   sets a boolean flag within its respective index of data_ corresponding to the value
		   of .empty() of its holding queue. Dequeueing threads can now check the boolean entry
		   before proceeding to cmpxchg. Note, this does _not_ guarantee that the queue will necessarily 
		   have elements in it. 1) The dequeuing threads are loading the boolean flag via memory_order_relaxed
		   and have no prior synchronizes-with operation to guarantee they'll see any value of that flag. 2) There
		   exists the possibility that a thread reads the boolean value and determines there is data present
		   within the queue, another thread then goes and dequeues everything, yet the original thread
		   has no idea that this happened. Needless to say, the boolean flag isn't foolproof, but it does aid
		   the dequeuing threads.

		*/
		queue_holder acquire_queue_dequeue()
		{
			
			
			unsigned int starting_position = get_index();
			for(int i = 0; i < num_queues; ++i)
			{
				int index = (starting_position + i) & queue_mask;
				
				if(! data_[index].dirty_.load(std::memory_order_relaxed))
					continue;

				pointer ptr = data_[index].load(std::memory_order_relaxed);
				if(ptr &&  data_[index].compare_exchange_strong(ptr, nullptr, std::memory_order_acquire, std::memory_order_relaxed))
				{	
					return {index, ptr, data_};
				}
				
			}
			return {0, nullptr, data_};
		}
	 
	 
	 	/* Enqueuing threads do not have the same restrictions as dequeing threads, and can thus acquire
	 	   any queue, regardless of the underlying size(). The DEqueueing threads necessarily have to do 
	 	   more work when reading the boolean flag, so the functions to acquire a queue are split between
	 	   ENqueueing and DEqueueing to avoid putting excess work on the ENqueueing threads.
	 	*/
		queue_holder acquire_queue()
		{
			
			
			unsigned int starting_position = get_index();

			for(int i = 0;; ++i)
			{
				int index = (starting_position + i) & queue_mask;
				pointer ptr = data_[index].load(std::memory_order_relaxed);

				if( ptr && data_[index].compare_exchange_strong(ptr, nullptr, std::memory_order_acquire, std::memory_order_relaxed))
				{
					
					return {index, ptr, data_};
				}
				
			}
			return {0, nullptr, data_};

		}

		
		template<typename U>
		bool enqueue(U&& arg)
		{
			auto queue_guard =  acquire_queue();
			if(! queue_guard.ptr_)
				return false;

			auto& queue = queue_guard.queue(); 

			queue.emplace(std::forward<U>(arg));
			return true;
		}

		template<typename It>
		bool enqueue_bulk(It iter, std::size_t count)
		{
			auto queue_guard =  acquire_queue(); // atmically acquire queue
			if(! queue_guard.ptr_)
				return false;
			auto& queue = queue_guard.queue(); // get a reference to the queue


			for(std::size_t i = 0; i < count; ++i)
			{
				queue.push(*iter++);
			}
			return true;
		}

		bool try_dequeue(T& item)
		{
	
			auto guard = acquire_queue_dequeue();
			if(guard.ptr_ == nullptr)
				return false;
			auto& queue = guard.queue();
			if(!queue.empty())
			{
				item = std::move(queue.front());
				queue.pop();
				return true;	
			}
			
			return false;
			
		}

		template<typename It>
		size_t try_dequeue_bulk(It output, size_t items)
		{
			auto queue_guard = acquire_queue_dequeue();			
			size_t count = 0;
			int    iters = 0;
			while(queue_guard.ptr_ && count < items)
			{
				auto queue = queue_guard.queue();
				std::size_t add = std::min(queue.size(), items - count); 
				count += add;
				for(std::size_t i = 0; i < add; ++i)
				{
					*output++ = std::move(queue.front());
					queue.pop();
				}
				if(count == items || iters++ == num_queues) // searched through enough iterations or reached goal
					return count;
				queue_guard = acquire_queue_dequeue();

			}

			return count;

		}


		~lf_queue()
		{
			for(auto& st_queue : data_)
			{
				delete st_queue.load(std::memory_order_relaxed);
			}
		}





	};
}; // end namespace hin