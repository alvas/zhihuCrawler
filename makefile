
CC=g++
CFLAGS=-g -std=c++11 -Wall -Wextra
LIBS=-lpthread

SOURCES=$(wildcard src/*.cc)
OBJECTS=$(patsubst %.cc, objs/%.o, $(SOURCES))

TARGET=zhihuCrawler

all: $(TARGET)

$(TARGET): build $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

# $@ The file name of the target of the rule.
# $< The name of the first prerequisite.
objs/%.o: %.cc
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	@mkdir -p objs/src

tags:
	ctags -R .

cscope:
	find . -name "*.cc" -o -name "*.h" > cscope.files
	cscope -b -q -k

clean:
	@rm -rf objs
	@rm $(TARGET)
