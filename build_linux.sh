cmake -Bbuild -H. -DCMAKE_INSTALL_PREFIX="/home/hexu/opt/paho-cpp" -DPAHO_BUILD_SAMPLES=ON -DPAHO_WITH_SSL=ON -DCMAKE_PREFIX_PATH="/home/hexu/opt/paho-c"
cmake --build build/ --target install

