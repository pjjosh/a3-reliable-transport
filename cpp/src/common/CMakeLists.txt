# src/common/CMakeLists.txt

# Declare a header-only library
add_library(common INTERFACE)

# Ensure that any target linking to 'common' can see this directory
target_include_directories(common
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
