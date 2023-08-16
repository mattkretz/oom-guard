oom-guard: main.cpp
	$(CXX) -O2 -std=gnu++20 -g0 -static-libstdc++ -static-libgcc -march=native $< -o $@

exec: oom-guard
	./oom-guard
