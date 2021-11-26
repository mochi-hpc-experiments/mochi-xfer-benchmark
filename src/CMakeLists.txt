add_executable (mochi-xfer-benchmark benchmark.cpp)
target_link_libraries (mochi-xfer-benchmark
    thallium PkgConfig::tclap spdlog::spdlog MPI::MPI_C)
install (TARGETS mochi-xfer-benchmark DESTINATION bin)
