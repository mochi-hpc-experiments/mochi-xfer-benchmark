# Mochi Xfer Benchmark

This benchmark is specifically designed to answer one question:
what is the best way to transfer N bytes from a client to a server
in Mochi, on a given platform? Its tests includes relying on RPC
arguments, registering a different buffer at every operation for
RDMA, or re-using a preregistered buffer (which for clients
means copying the payload to it).

## Installing

This program can be installed using spack, provided that you have
the [Mochi repository](https://github.com/mochi-hpc/mochi-spack-packages)
added to your spack installation.

```
$ spack install mochi-xfer-benchmark
```

## Using the benchmark

Once installed, the `mochi-xfer-benchmark` can be used as a two-process
MPI program. It provides the following options:

* `-p/--protocol` (required): protocol to use (`na+sm`, `ofi+tcp`, etc.).
* `-o/--output` (required): name of the output CSV file.
* `-i/--iterations`: number of repetitions of each operation.
* `-c/--client-protocol`: protocol for the client to use, if different
  from that of the server.
* `-v/--verbose`: logging level (trace, debug, info, warning, error, critical, off).
* `--client-use-progress-thread`: if provided, the client will use a dedicated
  progress thread.
* `--server-use-progress-thread`: if provided, the server will use a dedicated
  progress thread.
* `--server-num-handler-threads`: number of handler threads (0, 1, or -1).
  With 0, RPCs will execute in the primary ES. With 1, they will execute in
  a dedicated ES. With -1, they will execute in the ES in which the progress
  loop runs.
* The remaining, non-labelled arguments forms the list of transfer sizes.

The benchmark will output a CSV file containing the results of the following
operations. "Send" and "Receive" are understood from the point of view of the
server (e.g. `recv-args` means that the server will receive the payload via
RPC arguments).

### recv-args

The client sends the payload as an RPC argument.

### recv-rdma-(x,y)

The server uses RDMA PULL to get the payload from the client.

`x` may be `new` (new registration) or `pre` (pre-registered bulk).
In the former case, the client creates a bulk handle to expose its
payload at every iteration. In the latter case, the client uses a
pre-allocated buffer and pre-registered bulk handle, and does a
`memcpy` of its payload in this buffer prior to sending the RPC.

Similarly `y` may be `new` or `pre`. In the former case, at every
operation the server will allocate a new buffer and register it
before doing a PULL. In the latter case, the server will rely on
a pre-allocated buffer and a corresponding pre-registered bulk
handle. No memcpy is performed in this case.

### send-resp

The server sends its payload as an RPC response.

### send-rdma-(x,y)

The server uses RDMA PUSH to send its payload to the client.

`x` may be `new` or `pre`. In the former case, the client creates
a bulk handle to expose the buffer in which it wants the data to
be placed. In the latter case, it relies on a pre-allocated buffer
and associated pre-registered bulk handle, and does a memcpy once
the data has arrived.

Similarly, `y` may be `new` or `pre`. In the former case, at every
operation the server allocates a new buffer and registers it
before doing a PUSH. In the latter case, the server will rely on
a pre-allocated buffer and a corresponding pre-registered bulk
handle.

