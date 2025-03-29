# Create your own CMakeLists.txt file here that will build your project, based 
# on the directory structure you choose. You can use the CMakeLists.txt files
# we provided in the src directory of Project 1 and Project 2 as a starting point.

# You will also have to add a CMakelists.txt file in the common/ directory. 

# Running the standard CMake build sequence should result in the following executables:
#   wSender
#   wReceiver
#   wSenderOpt
#   wReceiverOpt

# src/CMakeLists.txt

# If you have a "common" subdirectory with shared code, include it:
add_subdirectory(common)

# -----------------------------
# wSender
# -----------------------------
add_executable(wSender
    sender.cpp
)
target_link_libraries(wSender
    PRIVATE
        common             # Link our common library
        spdlog::spdlog     # Link spdlog
        cxxopts::cxxopts
        Boost::regex
)

# -----------------------------
# wReceiver
# -----------------------------
add_executable(wReceiver
    receiver.cpp
)
target_link_libraries(wReceiver
    PRIVATE
        common
        spdlog::spdlog
        cxxopts::cxxopts
        Boost::regex
)

# -----------------------------
# wSenderOpt
# -----------------------------
add_executable(wSenderOpt
    senderOpt.cpp
)
target_link_libraries(wSenderOpt
    PRIVATE
        common
        spdlog::spdlog
        cxxopts::cxxopts
        Boost::regex
)

# -----------------------------
# wReceiverOpt
# -----------------------------
add_executable(wReceiverOpt
    receiverOpt.cpp
)
target_link_libraries(wReceiverOpt
    PRIVATE
        common
        spdlog::spdlog
        cxxopts::cxxopts
        Boost::regex
)