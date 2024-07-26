# run this like this:
# CPATH=/path/to/aqmt/parser make

SRC=prague_cc.cpp
HEADERS=prague_cc.h
CPPFLAGS=-std=c++11 -O3
WARN=-Wall -Wextra

CPP=g++
AR=ar

all: udp_prague_receiver udp_prague_sender

lib_prague: $(SRC) $(HEADERS) Makefile
	$(CPP) $(CPPFLAGS) -O3 $(WARN) -c $(SRC) -o libprague.o
	$(AR) rcs libprague.a libprague.o

udp_prague_receiver: udp_prague_receiver.cpp $(HEADERS) Makefile lib_prague
	$(CPP) udp_prague_receiver.cpp -L. -lprague --std=c++11 -pthread -O3 -Wall -Wextra -o $@

udp_prague_sender: udp_prague_sender.cpp $(HEADERS) Makefile lib_prague
	$(CPP) udp_prague_sender.cpp -L. -lprague --std=c++11 -pthread -O3 -Wall -Wextra -o $@

clean:
	rm -rf udp_prague_receiver udp_prague_sender *.a *.o
