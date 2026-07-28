cmake_minimum_required(VERSION 3.16)

# Modern C++ across all components (RAII wrappers, enum class, std::optional, ...).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(grohe_dial)
