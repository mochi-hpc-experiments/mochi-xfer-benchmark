#include <mpi.h>
#include <tclap/CmdLine.h>
#include <spdlog/spdlog.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <iomanip>

namespace tl = thallium;

struct Context {
    std::string          server_address;
    tl::endpoint         server_endpoint;
    tl::engine           engine;
    tl::remote_procedure rpc_recv_data_via_args;
    tl::remote_procedure rpc_recv_data_via_rdma;
    tl::remote_procedure rpc_send_data_via_resp;
    tl::remote_procedure rpc_send_data_via_rdma;
    std::vector<char>    preallocated_wo_buffer;
    tl::bulk             preallocated_wo_bulk;
    std::vector<char>    preallocated_ro_buffer;
    tl::bulk             preallocated_ro_bulk;
    std::ofstream        ostream;
};

struct Options {
    std::string protocol;
    std::string output;
    bool client_use_progress_thread;
    bool server_use_progress_thread;
    int  server_num_handler_threads;
    unsigned iterations;
    std::vector<size_t> xfer_sizes;
};

static void setup_rpc_handlers(Context& context);
static Options parse_command_line(int argc, char **argv);
static void run_client(Context& context, const Options& options);
static void analyze_throughputs(Context& context, const std::string& op, size_t xfer_size,
                            const std::vector<double> throughputs);

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if(rank != 0) spdlog::set_level(spdlog::level::critical);
    if(size != 2) {
        spdlog::critical("This program is meant to run on exactly two processes");
        MPI_Finalize();
        exit(-1);
    }

    auto options = parse_command_line(argc, argv);
    auto context = Context{};

    if(rank == 0) {

        context.ostream.open(options.output);
        if(!context.ostream.good()) {
            spdlog::critical("Could not open {}", options.output);
            exit(-1);
        }

        context.engine = tl::engine(options.protocol, THALLIUM_CLIENT_MODE,
                            options.client_use_progress_thread);
        setup_rpc_handlers(context);
        std::array<char, 256> buffer;
        buffer.fill(0);
        MPI_Recv(buffer.data(), buffer.size(), MPI_BYTE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        context.server_address = std::string(buffer.data());
        context.server_endpoint = context.engine.lookup(context.server_address);

        context.preallocated_wo_buffer.resize(options.xfer_sizes.back());
        context.preallocated_wo_bulk = context.engine.expose(
            {{context.preallocated_wo_buffer.data(), context.preallocated_wo_buffer.size()}},
            tl::bulk_mode::write_only);
        context.preallocated_ro_buffer.resize(options.xfer_sizes.back());
        context.preallocated_ro_bulk = context.engine.expose(
            {{context.preallocated_ro_buffer.data(), context.preallocated_ro_buffer.size()}},
            tl::bulk_mode::read_only);

        run_client(context, options);

        context.preallocated_wo_bulk = tl::bulk();
        context.preallocated_ro_bulk = tl::bulk();
        context.engine.shutdown_remote_engine(context.server_endpoint);
        context.server_endpoint = tl::endpoint();
        context.engine.finalize();

    } else {

        context.engine = tl::engine(options.protocol, THALLIUM_SERVER_MODE,
                            options.server_use_progress_thread,
                            options.server_num_handler_threads);
        context.engine.push_finalize_callback([&context]() {
            context.preallocated_wo_bulk = tl::bulk();
            context.preallocated_ro_bulk = tl::bulk();
        });

        context.preallocated_wo_buffer.resize(options.xfer_sizes.back());
        context.preallocated_wo_bulk = context.engine.expose(
            {{context.preallocated_wo_buffer.data(), context.preallocated_wo_buffer.size()}},
            tl::bulk_mode::write_only);
        context.preallocated_ro_buffer.resize(options.xfer_sizes.back());
        context.preallocated_ro_bulk = context.engine.expose(
            {{context.preallocated_ro_buffer.data(), context.preallocated_ro_buffer.size()}},
            tl::bulk_mode::read_only);

        setup_rpc_handlers(context);
        context.server_address = context.engine.self();
        std::array<char, 256> buffer;
        buffer.fill(0);
        std::memcpy(buffer.data(), context.server_address.data(), context.server_address.size());
        MPI_Send(buffer.data(), buffer.size(), MPI_BYTE, 0, 0, MPI_COMM_WORLD);

        context.engine.enable_remote_shutdown();
        context.engine.wait_for_finalize();
    }

    MPI_Finalize();
}

Options parse_command_line(int argc, char **argv) {
    Options options;;
    try {
        TCLAP::CmdLine cmd("Mochi Xfer Benchmark", ' ', "0.1");
        TCLAP::ValueArg<std::string> protocol(
            "p", "protocol", "Address or protocol (e.g. ofi+tcp)", true, "", "string");
        TCLAP::ValueArg<std::string> output(
            "o", "output", "Output file name (CSV)", true, "", "string");
        TCLAP::ValueArg<std::string> logLevel("v", "verbose",
            "Log level (trace, debug, info, warning, error, critical, off)", false,
            "info", "string");
        TCLAP::SwitchArg clientUseProgressThread("", "client-use-progress-thread",
            "Use a Mercury progress thread in the client");
        TCLAP::SwitchArg serverUseProgressThread("", "server-use-progress-thread",
            "Use a Mercury progress thread in the server");
        TCLAP::ValueArg<int> serverNumHandlerThreads("", "server-num-handler-threads",
            "Number of handler threads to use (-1, 0, 1 are the only relevant values)",
            false, 0, "int");
        TCLAP::ValueArg<unsigned> iterations("i", "iterations",
            "Number of iterations to perform for each transfer",
            false, 1, "int");
        TCLAP::UnlabeledMultiArg<size_t> xferSizes("s", "sizes",
            true, "Sizes of the transfers to execute");

        cmd.add(protocol);
        cmd.add(output);
        cmd.add(logLevel);
        cmd.add(clientUseProgressThread);
        cmd.add(serverUseProgressThread);
        cmd.add(serverNumHandlerThreads);
        cmd.add(iterations);
        cmd.add(xferSizes);

        cmd.parse(argc, argv);

        options.protocol = protocol.getValue();
        options.output = output.getValue();
        options.client_use_progress_thread = clientUseProgressThread.getValue();
        options.server_use_progress_thread = serverUseProgressThread.getValue();
        options.server_num_handler_threads = serverNumHandlerThreads.getValue();
        options.iterations = iterations.getValue();
        options.xfer_sizes = xferSizes.getValue();
        std::sort(options.xfer_sizes.begin(), options.xfer_sizes.end());

        spdlog::set_level(spdlog::level::from_str(logLevel.getValue()));

    } catch(TCLAP::ArgException &e) {
        spdlog::critical("{} for arg {}", e.what(), e.argId());
        exit(-1);
    }
    return options;
}

void setup_rpc_handlers(Context& context) {
    context.rpc_recv_data_via_args = context.engine.define(
        "rpc_recv_data_via_args",
        [](const tl::request& req, const std::vector<char>& data) {
            (void)data;
            req.respond();
        });
    context.rpc_recv_data_via_rdma = context.engine.define(
        "rpc_recv_data_via_rdma",
        [&context](const tl::request& req, const tl::bulk& source,
                            size_t size, bool use_preallocated) {
            if(!use_preallocated) {
                std::vector<char> buffer(size);
                auto dest = context.engine.expose({{buffer.data(), buffer.size()}}, tl::bulk_mode::write_only);
                dest << source.on(req.get_endpoint());
            } else {
                auto& dest = context.preallocated_wo_bulk;
                dest(0,size) << source.on(req.get_endpoint());
            }
            req.respond();
        });
    context.rpc_send_data_via_resp = context.engine.define(
        "rpc_send_data_via_resp",
        [](const tl::request& req, size_t size) {
            std::vector<char> data(size);
            req.respond(data);
        });
    context.rpc_send_data_via_rdma = context.engine.define(
        "rpc_send_data_via_rdma",
        [&context](const tl::request& req, tl::bulk& dest,
                            size_t size, bool use_preallocated) {
            if(!use_preallocated) {
                std::vector<char> buffer(size);
                auto source = context.engine.expose({{buffer.data(), buffer.size()}}, tl::bulk_mode::read_only);
                dest.on(req.get_endpoint()) << source;
            } else {
                auto& source = context.preallocated_ro_bulk;
                dest.on(req.get_endpoint()) << source(0,size);
            }
            req.respond();
        });
}

static double do_rpc_recv_data_via_args(Context& context, size_t size);
static double do_rpc_recv_data_via_rdma(Context& context, size_t size, bool client_prealloc, bool server_prealloc);
static double rpc_send_data_via_resp(Context& context, size_t size);
static double rpc_send_data_via_rdma(Context& context, size_t size, bool client_prealloc, bool server_prealloc);

void run_client(Context& context, const Options& options) {
    context.ostream << "xfer_size, operation, avg(MB/s), var,  min, max" << std::endl;
    context.ostream << std::fixed << std::setprecision(9);
    spdlog::info("Starting benchmark");
    for(auto& xfer_size : options.xfer_sizes) {
        spdlog::info("=> xfer_size = {}", xfer_size);
        std::vector<double> throughputs(options.iterations);

        spdlog::info("==> operation = recv-args");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = do_rpc_recv_data_via_args(context, xfer_size);
        analyze_throughputs(context, "recv-args", xfer_size, throughputs);

        spdlog::info("==> operation = recv-rdma-(new,new)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = do_rpc_recv_data_via_rdma(context, xfer_size, false, false);
        analyze_throughputs(context,"recv-rdma-(new,new)", xfer_size, throughputs);

        spdlog::info("==> operation = recv-rdma-(pre,new)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = do_rpc_recv_data_via_rdma(context, xfer_size, true, false);
        analyze_throughputs(context,"recv-rdma-(pre,new)", xfer_size, throughputs);

        spdlog::info("==> operation = recv-rdma-(new,pre)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = do_rpc_recv_data_via_rdma(context, xfer_size, false, true);
        analyze_throughputs(context,"recv-rdma-(new,pre)", xfer_size, throughputs);

        spdlog::info("==> operation = recv-rdma-(pre,pre)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = do_rpc_recv_data_via_rdma(context, xfer_size, true, true);
        analyze_throughputs(context,"recv-rdma-(pre,pre)", xfer_size, throughputs);

        spdlog::info("==> operation = ");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = rpc_send_data_via_resp(context, xfer_size);
        analyze_throughputs(context,"send-resp", xfer_size, throughputs);

        spdlog::info("==> operation = send-rdma-(new,new)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = rpc_send_data_via_rdma(context, xfer_size, false, false);
        analyze_throughputs(context,"send-rdma-(new,new)", xfer_size, throughputs);

        spdlog::info("==> operation = send-rdma-(pre,new)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = rpc_send_data_via_rdma(context, xfer_size, true, false);
        analyze_throughputs(context,"send-rdma-(pre,new)", xfer_size, throughputs);

        spdlog::info("==> operation = send-rdma-(new,pre)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = rpc_send_data_via_rdma(context, xfer_size, false, true);
        analyze_throughputs(context,"send-rdma-(new,pre)", xfer_size, throughputs);

        spdlog::info("==> operation = send-rdma-(pre,pre)");
        for(unsigned i=0; i < options.iterations; i++)
            throughputs[i] = rpc_send_data_via_rdma(context, xfer_size, true, true);
        analyze_throughputs(context,"send-rdma-(pre,pre)", xfer_size, throughputs);
    }
}

double do_rpc_recv_data_via_args(Context& context, size_t xfer_size) {
    std::vector<char> data(xfer_size);
    double t1 = MPI_Wtime();
    context.rpc_recv_data_via_args.on(context.server_endpoint)(data);
    double t2 = MPI_Wtime();
    return xfer_size/((t2-t1)*1024*1024);
}

double do_rpc_recv_data_via_rdma(Context& context, size_t xfer_size, bool client_prealloc, bool server_prealloc) {
    std::vector<char> data(xfer_size);
    double t1 = MPI_Wtime();
    if(client_prealloc) {
        auto& bulk = context.preallocated_ro_bulk;
        std::memcpy(context.preallocated_ro_buffer.data(), data.data(), data.size());
        context.rpc_recv_data_via_rdma.on(context.server_endpoint)(bulk, xfer_size, server_prealloc);
    } else {
        auto bulk = context.engine.expose({{data.data(), data.size()}}, tl::bulk_mode::read_only);
        context.rpc_recv_data_via_rdma.on(context.server_endpoint)(bulk, xfer_size, server_prealloc);
    }
    double t2 = MPI_Wtime();
    return xfer_size/((t2-t1)*1024*1024);
}

double rpc_send_data_via_resp(Context& context, size_t xfer_size) {
    std::vector<char> data;
    double t1 = MPI_Wtime();
    data = context.rpc_send_data_via_resp.on(context.server_endpoint)(xfer_size).as<decltype(data)>();
    double t2 = MPI_Wtime();
    return xfer_size/((t2-t1)*1024*1024);
}

double rpc_send_data_via_rdma(Context& context, size_t xfer_size, bool client_prealloc, bool server_prealloc) {
    std::vector<char> data(xfer_size);
    double t1 = MPI_Wtime();
    if(client_prealloc) {
        auto& bulk = context.preallocated_wo_bulk;
        context.rpc_send_data_via_rdma.on(context.server_endpoint)(bulk, xfer_size, server_prealloc);
        std::memcpy(data.data(), context.preallocated_wo_buffer.data(), data.size());
    } else {
        auto bulk = context.engine.expose({{data.data(), data.size()}}, tl::bulk_mode::write_only);
        context.rpc_send_data_via_rdma.on(context.server_endpoint)(bulk, xfer_size, server_prealloc);
    }
    double t2 = MPI_Wtime();
    return xfer_size/((t2-t1)*1024*1024);
}

void analyze_throughputs(Context& context, const std::string& op, size_t xfer_size, const std::vector<double> throughputs) {
    double sum = std::accumulate(throughputs.begin(), throughputs.end(), 0.0);
    double min = std::accumulate(throughputs.begin(), throughputs.end(), INT_MAX, [](auto x, auto y){ return x < y ? x : y; });
    double max = std::accumulate(throughputs.begin(), throughputs.end(), INT_MIN, [](auto x, auto y){ return x > y ? x : y; });
    double avg = sum/throughputs.size();
    double var = 0.0;
    std::for_each(throughputs.begin(), throughputs.end(), [&var](auto x){ var += x*x; });
    var /= throughputs.size();
    var -= avg*avg;
    context.ostream << xfer_size << ", " << op << ", " << avg << ", " << var << ", " << min << ", " << max << std::endl;
}
