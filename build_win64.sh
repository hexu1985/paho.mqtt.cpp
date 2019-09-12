cmake -G "Visual Studio 14 Win64" -Bbuild -H. -DCMAKE_INSTALL_PREFIX="D:\mqtt\x64\paho-cpp" -DPAHO_BUILD_SAMPLES=ON -DPAHO_WITH_SSL=ON -DCMAKE_PREFIX_PATH="D:\mqtt\x64\paho-c" 
cmake --build build/ --target install

