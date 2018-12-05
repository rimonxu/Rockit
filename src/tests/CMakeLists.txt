cmake_minimum_required( VERSION 2.8.8 )

include_directories(rt_base)
include_directories(rt_node)
include_directories(rt_task)

add_subdirectory(rt_base)
add_subdirectory(rt_node)
add_subdirectory(rt_task)

message(STATUS "cmake version ${CMAKE_VERSION} [@@]config unit-testcases")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

include_directories(.)

set(ROCKIT_TEST_SRC
    main.cpp
    # Testcase for RT_BASE
    rt_base/test_base_array_list.cpp
    rt_base/test_base_deque.cpp
    rt_base/test_base_hash_table.cpp
    rt_base/test_base_linked_list.cpp
    rt_base/test_base_memory.cpp
    rt_base/test_base_mutex_thread.cpp
    # Testcase for RT_NODE
    rt_node/test_node_basic.cpp
    rt_node/test_node_bus.cpp
    rt_node/test_node_data_flow.cpp
    # Testcase for RT_TASK
    rt_task/test_task_taskpool.cpp
)

add_executable(rockit_test ${ROCKIT_TEST_SRC})
target_link_libraries(rockit_test ${RT_BASE_STATIC} ${RT_NODE_STATIC} ${RT_TASK_STATIC})

message(STATUS "cmake version ${CMAKE_VERSION} [@@]config rockit_test")