find_package(Threads REQUIRED)

add_executable(audio_demo audio_demo.cpp)
target_link_libraries(audio_demo PRIVATE spsc::queue Threads::Threads)
target_compile_options(audio_demo PRIVATE -O2 -Wall -Wextra)

add_executable(marketdata_demo marketdata_demo.cpp)
target_link_libraries(marketdata_demo PRIVATE spsc::queue Threads::Threads)
target_compile_options(marketdata_demo PRIVATE -O2 -Wall -Wextra)
