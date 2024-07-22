# run this like this:
# CPATH=/path/to/aqmt/parser make

SRC=prague_cc.cpp
HEADERS=prague_cc.h
CPPFLAGS=-std=c++11 -O3
WARN=

CPP=g++
AR=ar

all: lib_prague udp_prague_receiver udp_prague_sender

lib_prague: $(SRC) $(HEADERS) Makefile
	$(CPP) $(CPPFLAGS) -O3 $(WARN) -c $(SRC) -o libprague.o
	$(AR) rcs libprague.a libprague.o

udp_prague_receiver: udp_prague_receiver.cpp $(HEADERS) Makefile lib_prague
	$(CPP) $(CPPFLAGS) -c udp_prague_receiver.cpp -L. -lprague -o $@

udp_prague_sender: udp_prague_sender.cpp $(HEADERS) Makefile lib_prague
	$(CPP) $(CPPFLAGS) -c udp_prague_sender.cpp -L. -lprague -o $@

clean:
	rm -rf udp_prague_receiver udp_prague_sender *.a *.o
