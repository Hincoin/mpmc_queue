lfqueue : lfqueue_stptr.o
	g++-6 -o lfqueue lfqueue_stptr.o -Wall -O3 -latomic -lpthread 

lfqueue_stptr.o : lfqueue_stptr.cpp
	g++-6 -c lfqueue_stptr.cpp -O3 -std=c++1z

	
