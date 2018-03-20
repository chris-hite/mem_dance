#include "freed_ptr.hpp"

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

void pauseCPU() {
	__builtin_ia32_pause();
}

inline void spinSleepUs(const uint64_t waitUs, const uint64_t ts=rdtsc()) {
	if (waitUs) {
		const auto tw = ts + waitUs * tscPerSecondC / 1000000;
		while (rdtsc() < tw)
			pauseCPU();
	}
}

inline void pinCPU(int cpu) {
	cpu_set_t s {};
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof(s), &s);
}



struct TwoWordSignaling {
	using This = TwoWordSignaling;

	using I = uint64_t;

	static constexpr size_t cacheLineSize = 128;
	freed_ptr<volatile I> si = makeSuperAligned<volatile I>(cacheLineSize);
	freed_ptr<volatile I> so = makeSuperAligned<volatile I>(cacheLineSize);

	// state for spinning testing
	struct Tester {
		volatile I* si;
		volatile I* so;
		I last;

		Tester(This* x) : si{ x->si.get() }, so{ x->so.get() }, last {} {}


		void prepare() {}  // prepare, possibly prefetch

		void signalAndWaitForResponse() {
			++last;
			*si = last;

			while(*so != last)
				pauseCPU();
		}
	};

	// state for spinning fast thread
	struct Reciever {
		volatile I* si;
		volatile I* so;
		I last;

		Reciever(This* x) : si{ x->si.get() }, so{ x->so.get() }, last{ *si } {}

		using Message = std::optional<I>;

		void prepare() {}  // prepare, possibly prefetch

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
};



#if 0
// going to put this one on ice
struct FastRing{
	static constexpr size_t bufSize = 4096;
	static constexpr size_t maxPayloadSize = 256;
	static constexpr size_t maxPayloadAlignment = 8;
	static constexpr size_t cacheLineSize = 64;  // could also be 128

	char * buf;
	size_t offset = 0;// published offset only used for resyncing a late Reader

	const char* bufAtOffset(size_t offset) const  { return buf + (offset % bufSize); }
	      char* bufAtOffset(size_t offset)        { return buf + (offset % bufSize); }


	struct MessageHeader {
		uint32_t size;
		uint32_t seq;  // semlock-y
	};

	struct Writer {
		FastRing* shared = nullptr;
		size_t offset = 0;

		char* prepareWrite(size_t s) {

		}

		void commmitWrite(size_t s) {

		}
	};

	struct Reader {
		FastRing* shared = nullptr;
		size_t offset = 0;

		Reader(FastRing& fr) : shared{&fr}, offset{fr.offset} {}

		// safer read
		size_t read(char* out, size_t bufSize) {
			shared->bufAtOffset(offset);

		}
	};

};
/*

 void sender() {
 	 writeMessageGuts
 	 clearNextControlWord()  // could be avoided if we prime it
 	 writeMessageControlWord()  // commit - could be message type or size
 }

 void reader() {
 	 if (nextControlWord() ) {
 	 	 handleMessage()
 	 }
 }

 can't decide if I want to entertain magic ring buffer
 can't decide on MessageHeader

 can I be optimistic and assume the reader keeps up?

 */
struct WordRingSignaling {
	using This = WordRingSignaling;

	using I = uint64_t;
	static constexpr unsigned N = 4096;

	volatile I si = 0;
	volatile I so = 0;  // bad to put in same cache line

	volatile I ring[N] {};

	// state for spinning testing
	struct Tester {
		volatile I* si;
		volatile I* so;
		I last;

		Tester(This* x) : si{ &x->si }, so{ &x->so }, last {} {}

		void signalAndWaitForResponse() {
			++last;
			*si = last;

			while(*so != last)
				pauseCPU();
		}

		void prepare() {}
	};

	// state for spinning fast thread
	struct Reciever {
		volatile I* si;
		volatile I* so;
		I last;

		Reciever(This* x) : si{ &x->si }, so{ &x->so }, last{ *si } {}

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
};
#endif


struct TestRig : TwoWordSignaling{
	volatile bool testLoopStarted = false;
	volatile bool done = false;

	void testLoop() {
		pinCPU(7);

		Reciever receiver{this};

		int i = 0;
		testLoopStarted = true;
		while (!done) {
			if (const auto message = receiver.recieve() ) {
				receiver.reply(message);
				// simulate logging something or blast cache
			} else {
				if( i++ & 0xff )
					pauseCPU();
				else
					receiver.prepare();
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
			tester.prepare();
			spinSleepUs(1);

			const auto t0 = rdtsc();
			tester.signalAndWaitForResponse();
			const auto t1 = rdtsc();
			totalTSC += t1 - t0;
			n++;

			if( t1 > te)
				break;

			// try waiting a bit here = slows it down some 20%
			constexpr uint64_t waitUs = 0;
			spinSleepUs(waitUs, t1);
		}

		done = true;
		t.join();


		std::cout << "tscPerSecond=" << tscPerSecondC
			<< " n=" << n
			<< " totalTSC=" << totalTSC
			<< " ave(ns)=" << (totalTSC * 1e9 /tscPerSecondC  /n)
			<< std::endl;
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
