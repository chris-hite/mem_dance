#pragma once
#include <memory>
#include <stdlib.h> // posix_memalign


struct FreeingDeleter{
	
	template <typename U>
	void operator()(U* ptr) const {
		ptr->~U();
		::free((void*)ptr);
	}
};

template<typename T>
using freed_ptr = std::unique_ptr<T, FreeingDeleter>;



void foo(){
	freed_ptr<int> p((int*)malloc(sizeof(int)));
}


// TODO parameter forwarding and all the pain of unque_ptr to support arrays
template<typename T>
freed_ptr<T> makeSuperAligned(size_t alignment){
	void* memptr = nullptr;
	const int r = posix_memalign(&memptr, alignment, sizeof(T));
	//assert(!r);  // TODO error handling
	return freed_ptr<T>{ new(memptr) T{} };
}
