# Common sources/headers
SRC            := prague_cc.cpp
HEADERS        := prague_cc.h

# Default flags (Unix-like); overridden or unused on Windows/MSVC
CPPFLAGS       := -std=c++11 -O3
WARN           := -Wall -Wextra
LDFLAGS        :=
LDLIBS         :=

# Detect MUSL (Unix-like only)
MUSL_DETECTED  := $(shell ldd --version 2>&1 | grep -i musl > /dev/null && echo yes || echo no)
ifeq ($(OS),Windows_NT)
  # Toolchain: MSVC
  CXX          := cl
  EXE_EXT      := .exe
  OBJ_EXT      := .obj
  # MSVC compile flags from recent makefile
  CXXFLAGS     := /nologo /EHsc /W4 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=NTDDI_WIN10_CO /DWIN32_LEAN_AND_MEAN
  # MSVC link libraries
  LDLIBS       := ws2_32.lib mswsock.lib
  # Archiver: MSVC librarian
  AR           := lib
  RM           := rm
else
  # Toolchain: GNU/Clang
  EXE_EXT      :=
  OBJ_EXT      := .o
  ifeq ($(MUSL_DETECTED),yes)
    CPPFLAGS   += -D__MUSL__
  endif
  UNAME        := $(shell uname -s)
  ifeq ($(UNAME),Linux)
    CXX        := g++
  else ifeq ($(UNAME),FreeBSD)
    CXX        := clang++
  else ifeq ($(UNAME),Darwin)
    CXX        := clang++
  else
    CXX        := g++
  endif
  # Unix-like threading library
  LDLIBS       += -pthread
  AR           := ar
  RM           := rm -f
endif

# Original targets
ALL_TARGETS    := udp_prague_receiver$(EXE_EXT) udp_prague_sender$(EXE_EXT)

all: $(ALL_TARGETS)

# Library build
lib_prague: $(SRC) $(HEADERS) Makefile
ifeq ($(OS),Windows_NT)
	$(CXX) $(CXXFLAGS) /c $(SRC) /Fo:libprague$(OBJ_EXT)
	$(AR) /nologo /OUT:libprague.lib libprague$(OBJ_EXT)
else
	$(CXX) $(CPPFLAGS) $(WARN) -c $(SRC) -o libprague$(OBJ_EXT)
	$(AR) rcs libprague.a libprague$(OBJ_EXT)
endif

# Receiver build
udp_prague_receiver$(EXE_EXT): udp_prague_receiver.cpp $(HEADERS) Makefile lib_prague
ifeq ($(OS),Windows_NT)
	$(CXX) $(CXXFLAGS) /c udpsocket.cpp /Fo:udpsocket$(OBJ_EXT)
	$(CXX) $(CXXFLAGS) /c udp_prague_receiver.cpp /Fo:udp_prague_receiver$(OBJ_EXT)
	$(CXX) udpsocket$(OBJ_EXT) udp_prague_receiver$(OBJ_EXT) libprague.lib $(LDLIBS) /Fe:$@
else
	$(CXX) $(CPPFLAGS) $(WARN) udpsocket.cpp udp_prague_receiver.cpp -L. -lprague $(LDFLAGS) $(LDLIBS) -o $@
endif

# Sender build
udp_prague_sender$(EXE_EXT): udp_prague_sender.cpp $(HEADERS) Makefile lib_prague
ifeq ($(OS),Windows_NT)
	$(CXX) $(CXXFLAGS) /c udpsocket.cpp /Fo:udpsocket$(OBJ_EXT)
	$(CXX) $(CXXFLAGS) /c udp_prague_sender.cpp /Fo:udp_prague_sender$(OBJ_EXT)
	$(CXX) udpsocket$(OBJ_EXT) udp_prague_sender$(OBJ_EXT) libprague.lib $(LDLIBS) /Fe:$@
else
	$(CXX) $(CPPFLAGS) $(WARN) udpsocket.cpp udp_prague_sender.cpp -L. -lprague $(LDFLAGS) $(LDLIBS) -o $@
endif

# Pattern rules
ifeq ($(OS),Windows_NT)
# MSVC compile rule
%.obj: %.cpp
	$(CXX) $(CXXFLAGS) /c $< /Fo:$@
else
# GNU/Clang compile rule
%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(WARN) -c $< -o $@
endif

clean:
ifeq ($(OS),Windows_NT)
	-$(RM) *.obj *.exe *.lib
else
	$(RM) udp_prague_receiver udp_prague_sender *.a *.o
endif
