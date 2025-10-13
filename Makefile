# run this like this:
# GNU make utility (make/gmake in Linux/FreeBSD)

SRC=prague_cc.cpp
HEADERS=prague_cc.h
CPPFLAGS=-std=c++11 -O3
WARN=-Wall -Wextra

ifeq ($(OS),Windows_NT)
	CPP=g++
else
	UNAME=$(shell uname -s)
	ifeq ($(UNAME),Linux)
		CPP=g++
	else ifeq ($(UNAME),FreeBSD)
		CPP=clang++
	else ifeq ($(UNAME),Darwin)
		CPP=clang++
	endif
endif
AR=ar

all: udp_prague_sender udp_prague_receiver rt_receiver

lib_prague: $(SRC) $(HEADERS) Makefile
	$(CPP) $(CPPFLAGS) $(WARN) -c $(SRC) -o libprague.o
	$(AR) rcs libprague.a libprague.o

udp_prague_receiver: prague_receiver.cpp $(HEADERS) receiver_base.h Makefile lib_prague
	$(CPP) prague_receiver.cpp -L. -lprague --std=c++11 -pthread -O3 -Wall -Wextra -o $@

rt_receiver: rt_receiver.cpp $(HEADERS) receiver_base.h Makefile lib_prague
	$(CPP) rt_receiver.cpp -L. -lprague --std=c++11 -pthread -O3 -Wall -Wextra -o $@

udp_prague_sender: udp_prague_sender.cpp $(HEADERS) Makefile lib_prague
	$(CPP) udp_prague_sender.cpp -L. -lprague --std=c++11 -pthread -O3 -Wall -Wextra -o $@

clean:
	rm -rf udp_prague_receiver udp_prague_sender *.a *.o
