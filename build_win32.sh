cmake -G "Visual Studio 14" -Bbuild -H. -DCMAKE_INSTALL_PREFIX="D:\mqtt\x86\paho-cpp" -DPAHO_BUILD_SAMPLES=ON -DPAHO_WITH_SSL=ON -DCMAKE_PREFIX_PATH="D:\mqtt\x86\paho-c" 
cmake --build build/ --target install

