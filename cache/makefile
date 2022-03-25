TARGET=libcache.a

CXX ?= g++

SRCS=$(wildcard *.cpp)
OBJS=$(patsubst %.cpp, %.o, $(SRCS))

CXXFLAGS=-O3 -g -std=c++11 -fPIC

.cpp.o:
	$(CXX) $(CXXFLAGS) $< -c -o $@

$(TARGET):$(OBJS)
	ar cr $@ $+

clean:
	rm -rf $(TARGET) $(OBJS)
