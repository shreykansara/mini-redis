CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)
TARGET := miniredis

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

run: all
	./$(TARGET)

clean:
	rm -rf build $(TARGET) miniredis.aof
