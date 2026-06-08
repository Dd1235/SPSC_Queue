find_package(Threads REQUIRED)

add_executable(test_basic test_basic.cpp)
target_link_libraries(test_basic PRIVATE spsc::queue spsc_sanitizers Threads::Threads)
target_compile_options(test_basic PRIVATE -Wall -Wextra)
add_test(NAME basic COMMAND test_basic)

add_executable(test_stress test_stress.cpp)
target_link_libraries(test_stress PRIVATE spsc::queue spsc_sanitizers Threads::Threads)
target_compile_options(test_stress PRIVATE -Wall -Wextra)
add_test(NAME stress COMMAND test_stress)
set_tests_properties(stress PROPERTIES TIMEOUT 120)
