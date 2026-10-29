find_package(Git QUIET)
set(VCACHE_REVISION "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE VCACHE_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE VCACHE_GIT_RESULT)
    if(VCACHE_GIT_RESULT EQUAL 0)
        set(VCACHE_REVISION "${VCACHE_GIT_REVISION}")
    endif()
endif()

add_executable(vcache_bench vcache_bench.cpp)
target_link_libraries(vcache_bench PRIVATE vcache::vcache)
target_compile_definitions(vcache_bench PRIVATE
    VCACHE_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    VCACHE_BUILD_TYPE="$<CONFIG>"
    VCACHE_REVISION="${VCACHE_REVISION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vcache_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(vcache_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/benchmark)
