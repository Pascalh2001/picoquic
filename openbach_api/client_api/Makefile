BUILD_FLAGS=-pthread -std=c++11 -pedantic -Wall -Wextra -Werror -Wno-unused-variable -fPIC $(CXXFLAGS) -DGENERATE_LIB
ASIO_FLAGS=-DASIO_STANDALONE -I./dependencies/asio-1.10.6/include
LIBS_FLAGS=-pthread $(LDLIBS)

PREFIX?=/usr/

all: build/libcollectagent.so

build/collectagent.o: ./src/collectagent.cpp ./src/collectagent.h ./src/syslog.h
	@mkdir -p build
	g++ $(CPPFLAGS) ${BUILD_FLAGS} ${ASIO_FLAGS} -c $< -o $@

build/libcollectagent.so: ./build/collectagent.o
	@mkdir -p build
	g++ -o $@ $< $(LDFLAGS) -shared ${LIBS_FLAGS}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/lib/
	mkdir -p ${DESTDIR}${PREFIX}/include/
	cp build/libcollectagent.so ${DESTDIR}${PREFIX}/lib/
	cp src/collectagent.h ${DESTDIR}${PREFIX}/include/

clean:
	@rm -rf build/

uninstall:
	@rm -f ${DESTDIR}${PREFIX}/lib/libcollectagent.so
	@rm -f ${DESTDIR}${PREFIX}/include/collectagent.h
