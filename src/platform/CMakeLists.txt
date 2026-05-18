add_library(mm_platform STATIC
    desktop/AllocDesktop.cpp
    desktop/TimingDesktop.cpp
    desktop/UdpSocketDesktop.cpp
    desktop/TcpServerDesktop.cpp
)
target_include_directories(mm_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
