using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Threading.Tasks;

public static class ConnectionBenchmarkClient
{
    public const int FramesPerBurst = 8;
    private const int FrameBytes = 48;
    private const int TimeoutMs = 5000;

    internal static byte[] BuildBurst(int multiplier)
    {
        var bytes = new byte[FramesPerBurst * FrameBytes];
        for (var frame = 0; frame < FramesPerBurst; ++frame)
        {
            var offset = frame * FrameBytes;
            BitConverter.GetBytes(10UL).CopyTo(bytes, offset);
            for (var element = 0; element < 10; ++element)
            {
                float value = multiplier * (frame * 10 + element - 5);
                BitConverter.GetBytes(value).CopyTo(bytes, offset + 8 + 4 * element);
            }
        }
        return bytes;
    }

    public sealed class Trial
    {
        public double ElapsedMs;
        public double P50Ms;
        public double P95Ms;
        public double P99Ms;
        public int Completed;
        public int EarlyEof;
        public int Reset;
        public int Aborted;
    }

    public static Trial Run(int clients, int connectionsPerClient, int burstsPerConnection) =>
        Run(clients, connectionsPerClient, burstsPerConnection, false);

    public static Trial Run(int clients, int connectionsPerClient, int burstsPerConnection, bool allowQueueRejection)
    {
        var request = BuildBurst(1);
        var expected = BuildBurst(2);
        var requestsPerClient = connectionsPerClient * burstsPerConnection * FramesPerBurst;
        var latencies = new long[clients * requestsPerClient];
        var counts = new Trial[clients];
        var stopwatch = Stopwatch.StartNew();
        var options = new ParallelOptions { MaxDegreeOfParallelism = clients };
        Parallel.For(0, clients, options, worker =>
        {
            var response = new byte[expected.Length];
            var nextLatency = worker * requestsPerClient;
            var partial = new Trial();
            counts[worker] = partial;
            for (var connection = 0; connection < connectionsPerClient; ++connection)
            {
                try
                {
                    using var client = new TcpClient(AddressFamily.InterNetwork);
                    client.NoDelay = true;
                    client.SendTimeout = TimeoutMs;
                    client.ReceiveTimeout = TimeoutMs;
                    if (!client.ConnectAsync(IPAddress.Loopback, 8080).Wait(TimeoutMs))
                    {
                        throw new IOException("Benchmark connect timed out.");
                    }
                    var stream = client.GetStream();
                    for (var burst = 0; burst < burstsPerConnection; ++burst)
                    {
                        var issued = Stopwatch.GetTimestamp();
                        stream.Write(request, 0, request.Length);
                        var offset = 0;
                        while (offset < response.Length)
                        {
                            int read;
                            try { read = stream.Read(response, offset, response.Length - offset); }
                            catch (IOException) when (allowQueueRejection)
                            {
                                ValidatePrefix(response, expected, offset);
                                throw;
                            }
                            if (read == 0)
                            {
                                ValidatePrefix(response, expected, offset);
                                throw new EndOfStreamException("Benchmark response was truncated.");
                            }
                            var end = offset + read;
                            if (end / FrameBytes > offset / FrameBytes)
                            {
                                var latency = Stopwatch.GetTimestamp() - issued;
                                for (var frame = offset / FrameBytes; frame < end / FrameBytes; ++frame)
                                {
                                    latencies[nextLatency++] = latency;
                                }
                            }
                            offset = end;
                        }
                        if (!response.AsSpan().SequenceEqual(expected))
                        {
                            throw new IOException("Benchmark response was incorrect.");
                        }
                    }
                    client.Client.Shutdown(SocketShutdown.Send);
                    if (stream.ReadByte() != -1)
                    {
                        throw new IOException("Benchmark connection produced an extra response.");
                    }
                }
                catch (EndOfStreamException) when (allowQueueRejection) { ++partial.EarlyEof; }
                catch (IOException error) when (allowQueueRejection &&
                    error.InnerException is SocketException socket && socket.SocketErrorCode == SocketError.ConnectionReset)
                { ++partial.Reset; }
                catch (IOException error) when (allowQueueRejection &&
                    error.InnerException is SocketException socket && socket.SocketErrorCode == SocketError.ConnectionAborted)
                { ++partial.Aborted; }
            }
            partial.Completed = nextLatency - worker * requestsPerClient;
        });
        stopwatch.Stop();
        var total = new Trial { ElapsedMs = stopwatch.Elapsed.TotalMilliseconds };
        for (var worker = 0; worker < clients; ++worker)
        {
            var part = counts[worker];
            Array.Copy(latencies, worker * requestsPerClient, latencies, total.Completed, part.Completed);
            total.Completed += part.Completed;
            total.EarlyEof += part.EarlyEof;
            total.Reset += part.Reset;
            total.Aborted += part.Aborted;
        }
        Array.Sort(latencies, 0, total.Completed);
        double Percentile(double rank) => total.Completed == 0 ? 0 :
            latencies[(int)Math.Ceiling(rank * total.Completed) - 1] * 1000.0 / Stopwatch.Frequency;
        total.P50Ms = Percentile(0.50);
        total.P95Ms = Percentile(0.95);
        total.P99Ms = Percentile(0.99);
        return total;
    }

    private static void ValidatePrefix(byte[] response, byte[] expected, int count)
    {
        if (!response.AsSpan(0, count).SequenceEqual(expected.AsSpan(0, count)))
            throw new InvalidDataException("Benchmark response prefix was incorrect.");
    }

    public static void ValidateMetricsFile(string path, int expectedRecords, bool requireCompleteBursts = true)
    {
        if (expectedRecords < 0 || (requireCompleteBursts && (expectedRecords == 0 || expectedRecords % FramesPerBurst != 0)))
            throw new ArgumentException("Expected complete benchmark bursts for JSONL reconciliation.");
        using var file = File.OpenRead(path);
        if (file.Length == 0 && expectedRecords == 0) return;
        if (file.Length == 0 || file.Seek(-1, SeekOrigin.End) < 0 || file.ReadByte() != '\n')
            throw new InvalidDataException("Metrics file ends in an incomplete record.");
        file.Position = 0;
        using var reader = new StreamReader(file);
        var counts = new int[FramesPerBurst];
        int records = 0;
        while (reader.ReadLine() is string line)
        {
            using var document = JsonDocument.Parse(line);
            var record = document.RootElement;
            int fields = 0;
            foreach (var field in record.EnumerateObject()) ++fields;
            var latency = record.GetProperty("latency_ms").GetDouble();
            var mean = record.GetProperty("input_mean").GetDouble();
            var stddev = record.GetProperty("input_stddev").GetDouble();
            var frame = (int)((mean + 0.5) / 10);
            if (fields != 5 || record.GetProperty("timestamp").GetInt64() <= 0 ||
                !double.IsFinite(latency) || latency < 0 || record.GetProperty("element_count").GetInt32() != 10 ||
                frame < 0 || frame >= FramesPerBurst || mean != frame * 10 - 0.5 ||
                !double.IsFinite(stddev) || Math.Abs(stddev - Math.Sqrt(8.25)) > 0.00001)
                throw new InvalidDataException("Incorrect metrics record at line " + (records + 1));
            ++counts[frame];
            ++records;
        }
        if (records != expectedRecords) throw new InvalidDataException("Metrics record count did not reconcile.");
        if (requireCompleteBursts)
            foreach (int count in counts)
                if (count != expectedRecords / FramesPerBurst)
                    throw new InvalidDataException("Metrics input statistics did not reconcile with every benchmark frame.");
    }
}
