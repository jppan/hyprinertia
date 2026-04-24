.PHONY: all clean

all:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j
	cp build/libhyprinertia.so hyprinertia.so.tmp
	mv -f hyprinertia.so.tmp hyprinertia.so

clean:
	rm -rf build hyprinertia.so
