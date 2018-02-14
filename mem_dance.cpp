
#include <thread>
#include <chrono>
#include <iostream>
#include <optional>
#include <unistd.h>
#include <sched.h>

inline uint64_t rdtsc(void) {
	uint32_t hi, lo;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long) lo) | (((unsigned long long) hi) << 32);
}


inline uint64_t tscPerSecond(void) {
	const auto t0 = std::chrono::high_resolution_clock::now();
	const auto x0 = rdtsc();
	usleep(100000);  // 0.1s
	const auto t1 = std::chrono::high_resolution_clock::now();
	const auto x1 = rdtsc();
	return (x1 - x0) * std::chrono::seconds(1) / (t1 - t0);
}

inline const uint64_t tscPerSecondC = tscPerSecond();


inline void pinCPU(int cpu) {
	cpu_set_t s {};
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof(s), &s);
}



void pauseCPU() {
	__builtin_ia32_pause();
}



struct TestRig{
	using I = uint64_t;
	volatile uint64_t si = 0;
	volatile uint64_t so = 0;  // bad to put in same cache line

	// state for spinning fast thread
	struct Tester {
		volatile I* si;
		volatile I* so;
		I last;

		Tester(TestRig* x) : si{ &x->si }, so{ &x->so }, last {} {}

		void signalAndWaitForResponse() {
			++last;
			*si = last;

			while(*so != last)
				pauseCPU();
		}

		void backGroundWork() {}
	};

	// state for
	struct Reciever {
		volatile I* si;
		volatile I* so;
		I last;

		Reciever(TestRig* x) : si{ &x->si }, so{ &x->so }, last{ *si } {}

		using Message = std::optional<I>;

		Message recieve() {
			auto v = *si;
			if ( last == v )
				return {};
			last = v;
			return {v};
		}

		void reply(const Message& m) {
			*so = *m;
		}
	};

	//// bits above will get factored out

	volatile bool testLoopStarted = false;
	volatile bool done = false;

	void testLoop() {
		pinCPU(7);

		Reciever receiver{this};

		testLoopStarted = true;
		while (!done) {
			if (const auto message = receiver.recieve() ) {
				receiver.reply(message);
			} else {
				pauseCPU();
			}
		}
	}

	void run(){
		std::thread t([this] {testLoop();});

		Tester tester{this};

		pinCPU(5);
		while (!testLoopStarted)
			__builtin_ia32_pause();

		const auto ts = rdtsc();
		const auto te = ts + tscPerSecondC * 5;

		int64_t n = 0;
		int64_t totalTSC = 0;
		while (true) {
			const auto t0 = rdtsc();
			tester.signalAndWaitForResponse();
			const auto t1 = rdtsc();
			totalTSC += t1 - t0;
			n++;

			if( t1 > te)
				break;

			tester.backGroundWork();
			// try waiting a bit here - slows it down some 20%
			constexpr uint64_t waitUs = 0;

			if (waitUs) {
				const auto tw = t1 + waitUs * tscPerSecondC / 1000000;
				while (rdtsc() < tw)
					pauseCPU();
			}
		}

		done = true;
		t.join();


		std::cout << "tscPerSecond=" << tscPerSecondC
			<< " n=" << n
			<< " totalTSC=" << totalTSC
			<< " ave(ns)=" << (totalTSC * 1e9 /tscPerSecondC  /n)
			<<std::endl;
	}
};




int main() {
	TestRig().run();
	return 0;
}


/*
g++ -g3 -O3 -std=c++1z  -pthread mem_dance.cpp   && sudo LD_LIBRARY_PATH=/home/chris/tools/gcc-7.3.0/lib64: chrt -f 1 ./a.out
tscPerSecond=2591987263 n=32948722 totalTSC=10461874562 ave(ns)=122.501





 */
