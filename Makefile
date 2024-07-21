# run this like this:
# CPATH=/path/to/aqmt/parser make

SRC=prague_cc.cpp
HEADERS=prague_cc.h
CPPFLAGS=-L. -std=c++11 -O3

CPP=g++
AR=ar

all: lib_prague_cc udp_prague_receiver udp_prague_sender

lib_prague_cc: $(SRC) $(HEADERS) Makefile
		$(CPP) -c $(SRC) -std=c++11 -O3 -o lib_prague_cc.o
		$(AR) rcs lib_prague_cc.a lib_prague_cc.o

udp_prague_receiver: udp_prague_receiver.cpp prague_cc.cpp prague_cc.h
		$(CPP) $(CPPFLAGS) -c udp_prague_receiver.cpp

udp_prague_sender: udp_prague_sender.cpp prague_cc.cpp prague_cc.h
		$(CPP) $(CPPFLAGS) -c udp_prague_sender.cpp

clean:
	rm -rf prague_cc *.a *.o
