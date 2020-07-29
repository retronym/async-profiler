PROFILER_VERSION=1.7.1
JATTACH_VERSION=1.5
JAVAC_RELEASE_VERSION=6
LIB_PROFILER=libasyncProfiler.so
JATTACH=jattach
API_JAR=async-profiler.jar
CONVERTER_JAR=converter.jar
JMH_PROFILER_JAR=jmh-profiler.jar
CFLAGS=-O2
CXXFLAGS=-O2
INCLUDES=-I$(JAVA_HOME)/include
LIBS=-ldl -lpthread
JAVAC=$(JAVA_HOME)/bin/javac
JAR=$(JAVA_HOME)/bin/jar
SOURCES := $(wildcard src/*.cpp)
HEADERS := $(wildcard src/*.h)
API_SOURCES := $(wildcard src/api/one/profiler/*.java)
CONVERTER_SOURCES := $(shell find src/converter -name '*.java')
JMH_PROFILER_SOURCES := $(shell find src/jmh-profiler -name '*.java')

MAVEN_CENTRAL=https://repo1.maven.org/maven2
JMH_VERSION=1.23
JMH_CORE_PATH=org/openjdk/jmh/jmh-core/$(JMH_VERSION)
JMH_CORE_SHA1=eb242d3261f3795c8bf09818d17c3241191284a0
JMH_CORE_JAR=jmh-core-$(JMH_VERSION).jar
JOPT_SIMPLE_VERSION=4.6
JOPT_SIMPLE_PATH=net/sf/jopt-simple/jopt-simple/$(JOPT_SIMPLE_VERSION)
JOPT_SIMPLE_SHA1=306816fb57cf94f108a43c95731b08934dcae15c
JOPT_SIMPLE_JAR=jopt-simple-$(JOPT_SIMPLE_VERSION).jar

ifeq ($(JAVA_HOME),)
  export JAVA_HOME:=$(shell java -cp . JavaHome)
endif

OS:=$(shell uname -s)
ifeq ($(OS), Darwin)
  CXXFLAGS += -D_XOPEN_SOURCE -D_DARWIN_C_SOURCE
  INCLUDES += -I$(JAVA_HOME)/include/darwin
  RELEASE_TAG:=$(PROFILER_VERSION)-macos-x64
else
  LIBS += -lrt
  INCLUDES += -I$(JAVA_HOME)/include/linux
  RELEASE_TAG:=$(PROFILER_VERSION)-linux-x64
endif


.PHONY: all release test clean

all: build build/$(LIB_PROFILER) build/$(JATTACH) build/$(API_JAR) build/$(CONVERTER_JAR) build/$(JMH_PROFILER_JAR)

release: build async-profiler-$(RELEASE_TAG).tar.gz

async-profiler-$(RELEASE_TAG).tar.gz: build/$(LIB_PROFILER) build/$(JATTACH) \
                                      build/$(API_JAR) build/$(CONVERTER_JAR) \
                                      profiler.sh LICENSE NOTICE *.md
	chmod 755 build profiler.sh
	chmod 644 LICENSE NOTICE *.md
	tar cvzf $@ $^

build/$(JOPT_SIMPLE_JAR):
	curl --silent --fail -L $(MAVEN_CENTRAL)/$(JOPT_SIMPLE_PATH)/$(JOPT_SIMPLE_JAR) > build/download.jar
	printf "$(JOPT_SIMPLE_SHA1)  build/download.jar\n" > build/expected.sha
	shasum -c build/expected.sha
	mv build/download.jar build/$(JOPT_SIMPLE_JAR)

build/$(JMH_CORE_JAR):
	curl --silent --fail -L $(MAVEN_CENTRAL)/$(JMH_CORE_PATH)/$(JMH_CORE_JAR) > build/download.jar
	printf "$(JMH_CORE_SHA1)  build/download.jar\n" > build/expected.sha
	shasum -c build/expected.sha
	mv build/download.jar build/$(JMH_CORE_JAR)

build:
	mkdir -p build

build/$(LIB_PROFILER): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -DPROFILER_VERSION=\"$(PROFILER_VERSION)\" $(INCLUDES) -fPIC -shared -o $@ $(SOURCES) $(LIBS)

build/$(JATTACH): src/jattach/jattach.c
	$(CC) $(CFLAGS) -DJATTACH_VERSION=\"$(JATTACH_VERSION)\" -o $@ $^

build/$(API_JAR): $(API_SOURCES)
	mkdir -p build/api
	$(JAVAC) -source $(JAVAC_RELEASE_VERSION) -target $(JAVAC_RELEASE_VERSION) -d build/api $^
	$(JAR) cvf $@ -C build/api .
	$(RM) -r build/api

build/$(CONVERTER_JAR): $(CONVERTER_SOURCES) src/converter/MANIFEST.MF
	mkdir -p build/converter
	$(JAVAC) -source 7 -target 7 -d build/converter $(CONVERTER_SOURCES)
	$(JAR) cvfm $@ src/converter/MANIFEST.MF -C build/converter .
	$(RM) -r build/converter

build/$(JMH_PROFILER_JAR): $(JMH_PROFILER_SOURCES) build/$(API_JAR) build/$(JOPT_SIMPLE_JAR) build/$(JMH_CORE_JAR)
	mkdir -p build/jmh-profiler
	$(JAVAC) -source 7 -target 7 -d build/jmh-profiler -cp build/$(API_JAR):build/$(JOPT_SIMPLE_JAR):build/$(JMH_CORE_JAR) $(JMH_PROFILER_SOURCES)
	$(JAR) cvf $@ -C build/jmh-profiler .
	$(RM) -r build/jmh-profiler

test: all
	test/smoke-test.sh
	test/thread-smoke-test.sh
	test/alloc-smoke-test.sh
	test/load-library-test.sh
	echo "All tests passed"

clean:
	$(RM) -r build
