using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

// Loopback-only client. The scheduler never waits for an inference response.
public static class OpenLoopBenchmarkClient
{
    private const int FrameBytes = 48;
    private const int MaxOffers = 500000;
    private const int DispatchQuantumUs = 200;
    private const int LateUs = 1000;
    private const int MaintenanceUs = 5000;

    private enum Outcome
    {
        Scheduled, Outstanding, Success, ScheduleMiss, ClientFull, Timeout, Eof, SocketError, SendError, BadResponse, StreamDiscarded
    }

    private struct Offer
    {
        public long Due, Attempt, End;
        public int Phase, Error;
        public Outcome State;
    }

    public sealed class Phase
    {
        public string Name;
        public double TargetRate, Seconds;
        public int Offered, Attempted, Successful, Late, ScheduleMiss, ClientFull, Timeout, Eof, SocketError, SendError, BadResponse, StreamDiscarded;
        public int PeakOutstanding, WithinWindowSuccess;
        public Resources Begin, End;
        public List<Resources> Samples = new List<Resources>();
        public double? P50Ms, P95Ms, P99Ms, WireP50Ms, WireP95Ms, WireP99Ms, LagP50Ms, LagP95Ms, LagP99Ms;
        public double? FirstHalfSuccessRate, LastHalfSuccessRate;
        public Dictionary<int, int> SocketErrors = new Dictionary<int, int>();
    }

    public sealed class Resources
    {
        public double ServerCpuMs, ClientCpuMs;
        public long ServerPrivateBytes;
        public int ServerHandles;
        public double ElapsedMs;
    }

    public sealed class Result
    {
        public Phase[] Phases;
        public int Reconnects, ConnectFailures, UnexpectedFrames;
        public double InPhaseDrainMs;
        internal Action<string> Save;
        public void SaveTrace(string path) => Save(path);
    }

    private static long Ticks(double seconds) => (long)(seconds * Stopwatch.Frequency);
    private static double Ms(long ticks) => ticks * 1000.0 / Stopwatch.Frequency;

    private sealed class Lane
    {
        private readonly object gate = new object();
        private readonly Queue<int> pending;
        private readonly int depth;
        private readonly int responseTimeoutMs;
        private readonly byte[] transmit, request, expected;
        private readonly Offer[] offers;
        private readonly CancellationToken stop;
        private Socket socket;
        private Task receiver = Task.CompletedTask, connector = Task.CompletedTask;
        private long reconnectAt;
        public int Reconnects, ConnectFailures, UnexpectedFrames;

        public Lane(int depth, int responseTimeoutMs, byte[] request, byte[] expected, Offer[] offers, CancellationToken stop)
        {
            this.depth = depth;
            this.responseTimeoutMs = responseTimeoutMs;
            this.request = request;
            this.expected = expected;
            this.offers = offers;
            this.stop = stop;
            transmit = new byte[depth * FrameBytes];
            pending = new Queue<int>(depth);
        }

        private static Socket CreateSocket() => new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
        {
            NoDelay = true,
            SendTimeout = 20,
            // Benchmark lanes are persistent during the workload. Abortive test cleanup
            // avoids accumulating client TIME_WAIT across hundreds of independent trials.
            LingerState = new LingerOption(true, 0)
        };

        public async Task Start()
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stop);
            deadline.CancelAfter(5000);
            var next = CreateSocket();
            try { await next.ConnectAsync(IPAddress.Loopback, 8080, deadline.Token).ConfigureAwait(false); }
            catch { next.Dispose(); throw; }
            lock (gate) Install(next);
        }

        private void Install(Socket next)
        {
            socket = next;
            receiver = Receive(next);
        }

        private async Task ConnectAgain()
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stop);
            deadline.CancelAfter(1000);
            var next = CreateSocket();
            try
            {
                await next.ConnectAsync(IPAddress.Loopback, 8080, deadline.Token).ConfigureAwait(false);
                lock (gate)
                {
                    if (!stop.IsCancellationRequested)
                    {
                        ++Reconnects;
                        Install(next);
                        next = null;
                    }
                }
            }
            catch (SocketException) { lock (gate) ++ConnectFailures; }
            catch (OperationCanceledException) { if (!stop.IsCancellationRequested) lock (gate) ++ConnectFailures; }
            finally { next?.Dispose(); }
        }

        public int Send(int first, int count)
        {
            lock (gate)
            {
                if (socket == null) return 0;
                count = Math.Min(count, depth - pending.Count);
                if (count == 0) return 0;
                for (int i = 0; i < count; ++i)
                    Buffer.BlockCopy(request, (first + i) * FrameBytes, transmit, i * FrameBytes, FrameBytes);
                long now = Stopwatch.GetTimestamp();
                for (int i = 0; i < count; ++i)
                {
                    int id = first + i;
                    offers[id].Attempt = now;
                    offers[id].State = Outcome.Outstanding;
                    pending.Enqueue(id);
                }
                try
                {
                    if (socket.Send(transmit, 0, count * FrameBytes, SocketFlags.None) != count * FrameBytes)
                        Break(Outcome.SendError, 0);
                }
                catch (SocketException error) { Break(Outcome.SendError, error.ErrorCode); }
                return count;
            }
        }

        public int Maintain(long now)
        {
            lock (gate)
            {
                if (pending.Count != 0 && now - offers[pending.Peek()].Attempt >= Ticks(responseTimeoutMs / 1000.0))
                    Break(Outcome.Timeout, 0);
                if (socket == null && now >= reconnectAt && connector.IsCompleted && receiver.IsCompleted && !stop.IsCancellationRequested)
                {
                    reconnectAt = now + Ticks(0.010);
                    connector = ConnectAgain();
                }
                return pending.Count;
            }
        }

        private async Task Receive(Socket source)
        {
            var buffer = new byte[4096];
            int retained = 0;
            try
            {
                while (!stop.IsCancellationRequested)
                {
                    int count = await source.ReceiveAsync(buffer.AsMemory(retained), SocketFlags.None, stop).ConfigureAwait(false);
                    lock (gate)
                    {
                        if (source != socket) return;
                        if (count == 0)
                        {
                            if (retained != 0 && pending.Count == 0) ++UnexpectedFrames;
                            Break(Outcome.Eof, 0);
                            return;
                        }
                        int end = retained + count;
                        int offset = 0;
                        long seen = Stopwatch.GetTimestamp();
                        while (offset + FrameBytes <= end)
                        {
                            if (pending.Count == 0)
                            {
                                ++UnexpectedFrames;
                                Break(Outcome.BadResponse, 0);
                                return;
                            }
                            int id = pending.Dequeue();
                            bool exact = buffer.AsSpan(offset, FrameBytes).SequenceEqual(expected.AsSpan(id * FrameBytes, FrameBytes));
                            offers[id].End = seen;
                            offers[id].State = exact ? Outcome.Success : Outcome.BadResponse;
                            if (!exact) { Break(Outcome.BadResponse, 0); return; }
                            offset += FrameBytes;
                        }
                        retained = end - offset;
                        if (retained != 0) Buffer.BlockCopy(buffer, offset, buffer, 0, retained);
                    }
                }
            }
            catch (SocketException error) { lock (gate) { if (source == socket) Break(Outcome.SocketError, error.ErrorCode); } }
            catch (ObjectDisposedException) { }
            catch (OperationCanceledException) { }
        }

        private void Break(Outcome state, int error)
        {
            socket?.Dispose();
            socket = null;
            long ended = Stopwatch.GetTimestamp();
            reconnectAt = ended + Ticks(0.010);
            while (pending.Count != 0)
            {
                int id = pending.Dequeue();
                // Only the expired prefix has individually crossed the deadline. The other
                // requests lose their ordered stream when that connection must close.
                bool expired = state != Outcome.Timeout || ended - offers[id].Attempt >= Ticks(responseTimeoutMs / 1000.0);
                offers[id].State = expired ? state : Outcome.StreamDiscarded;
                offers[id].Error = expired ? error : 0;
                offers[id].End = ended;
            }
        }

        public Task Finish()
        {
            lock (gate)
            {
                Break(Outcome.Timeout, 0);
                return Task.WhenAll(receiver, connector);
            }
        }
    }

    private static Offer[] Plan(Phase[] phases, bool poisson, int seed)
    {
        var result = new List<Offer>();
        var random = new Random(seed);
        double baseSeconds = 0;
        for (int phase = 0; phase < phases.Length; ++phase)
        {
            var spec = phases[phase];
            double next = poisson ? -Math.Log(1 - random.NextDouble()) / spec.TargetRate : 0;
            int index = 0;
            while (next < spec.Seconds)
            {
                if (result.Count == MaxOffers) throw new ArgumentException("Trial exceeds its bounded offer limit.");
                result.Add(new Offer { Due = Ticks(baseSeconds + next), Phase = phase });
                next = poisson ? next - Math.Log(1 - random.NextDouble()) / spec.TargetRate : ++index / spec.TargetRate;
            }
            baseSeconds += spec.Seconds;
        }
        return result.ToArray();
    }

    private static Resources Sample(Process server, Process client, long start)
    {
        server.Refresh();
        client.Refresh();
        return new Resources
        {
            ServerCpuMs = server.TotalProcessorTime.TotalMilliseconds,
            ClientCpuMs = client.TotalProcessorTime.TotalMilliseconds,
            ServerPrivateBytes = server.PrivateMemorySize64,
            ServerHandles = server.HandleCount,
            ElapsedMs = Ms(Stopwatch.GetTimestamp() - start)
        };
    }

    private static void SampleResources(Process server, Phase[] phases, long start, CancellationToken stop)
    {
        using var client = Process.GetCurrentProcess();
        long regular = start + Ticks(1);
        double seconds = 0;
        for (int i = 0; i < phases.Length; ++i)
        {
            var phase = phases[i];
            seconds += phase.Seconds;
            long boundary = start + Ticks(seconds);
            while (regular < boundary)
            {
                if (!WaitForSample(regular, stop)) return;
                phase.Samples.Add(Sample(server, client, start));
                regular += Ticks(1);
            }
            if (!WaitForSample(boundary, stop)) return;
            phase.End = Sample(server, client, start);
            if (i + 1 < phases.Length) phases[i + 1].Begin = phase.End;
            if (regular == boundary) regular += Ticks(1);
        }
    }

    private static bool WaitForSample(long at, CancellationToken stop)
    {
        // Resource queries can take several milliseconds. Neither the wait nor the
        // query runs on the dispatcher thread.
        int wait = (int)Math.Ceiling(Math.Max(0, Ms(at - Stopwatch.GetTimestamp())));
        return !stop.WaitHandle.WaitOne(wait);
    }

    // Use distinct permutations across the trial. This detects response swaps even on
    // a busy lane and keeps the established per-frame JSONL statistics verifiable.
    private static (byte[] Requests, byte[] Responses) BuildFrames(int count)
    {
        var requests = new byte[count * FrameBytes];
        var responses = new byte[count * FrameBytes];
        var requestTemplate = ConnectionBenchmarkClient.BuildBurst(1);
        var responseTemplate = ConnectionBenchmarkClient.BuildBurst(2);
        int[] order = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        for (int id = 0; id < count; ++id)
        {
            int template = id % ConnectionBenchmarkClient.FramesPerBurst * FrameBytes;
            int frame = id * FrameBytes;
            Buffer.BlockCopy(requestTemplate, template, requests, frame, 8);
            Buffer.BlockCopy(responseTemplate, template, responses, frame, 8);
            for (int i = 0; i < order.Length; ++i)
            {
                int source = template + 8 + order[i] * 4;
                int destination = frame + 8 + i * 4;
                Buffer.BlockCopy(requestTemplate, source, requests, destination, 4);
                Buffer.BlockCopy(responseTemplate, source, responses, destination, 4);
            }
            int pivot = order.Length - 2;
            while (pivot >= 0 && order[pivot] > order[pivot + 1]) --pivot;
            if (pivot < 0) throw new InvalidOperationException("No more distinct valid frames.");
            int swap = order.Length - 1;
            while (order[swap] < order[pivot]) --swap;
            (order[pivot], order[swap]) = (order[swap], order[pivot]);
            Array.Reverse(order, pivot + 1, order.Length - pivot - 1);
        }
        return (requests, responses);
    }

    public static Result Run(Process server, double rate, double seconds, double warmRate, double warmSeconds,
        double recoveryRate, double recoverySeconds, bool poisson, int seed, int connections, int depth, int timeoutMs,
        int dispatchDeadlineMs)
    {
        var phases = new List<Phase>();
        if (warmSeconds > 0) phases.Add(new Phase { Name = "warmup", TargetRate = warmRate, Seconds = warmSeconds });
        phases.Add(new Phase { Name = "measurement", TargetRate = rate, Seconds = seconds });
        if (recoverySeconds > 0) phases.Add(new Phase { Name = "recovery", TargetRate = recoveryRate, Seconds = recoverySeconds });
        if (connections < 1 || connections > 128 || depth < 1 || depth > 32 || timeoutMs < 10 || timeoutMs > 5000 ||
            dispatchDeadlineMs < 2 || dispatchDeadlineMs > 100)
            throw new ArgumentException("Invalid client resource limits.");
        foreach (var p in phases)
            if (!double.IsFinite(p.TargetRate) || p.TargetRate < 1 || p.TargetRate > 100000 ||
                !double.IsFinite(p.Seconds) || p.Seconds <= 0 || p.Seconds > 120)
                throw new ArgumentException("Invalid arrival phase.");
        var result = new Result { Phases = phases.ToArray() };
        var offers = Plan(result.Phases, poisson, seed);
        using var stopped = new CancellationTokenSource();
        var (request, expected) = BuildFrames(offers.Length);
        var lanes = new Lane[connections];
        var startup = new Task[connections];
        for (int i = 0; i < lanes.Length; ++i)
        {
            lanes[i] = new Lane(depth, timeoutMs, request, expected, offers, stopped.Token);
            startup[i] = lanes[i].Start();
        }
        long start;
        Task sampler = Task.CompletedTask;
        using var stopSampling = new CancellationTokenSource();
        try
        {
            Task.WaitAll(startup);
            using var client = Process.GetCurrentProcess();
            var baseline = Sample(server, client, 0);
            start = Stopwatch.GetTimestamp();
            baseline.ElapsedMs = 0;
            result.Phases[0].Begin = baseline;
            sampler = Task.Factory.StartNew(() => SampleResources(server, result.Phases, start, stopSampling.Token),
                CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
            long quantum = Ticks(DispatchQuantumUs / 1e6);
            long miss = Ticks(dispatchDeadlineMs / 1000.0);
            long maintenanceAt = start;
            long tick = start;
            int cursor = 0, laneIndex = 0, boundary = 0;
            double boundarySeconds = result.Phases[0].Seconds;
            long stopAt = start + Ticks(boundarySeconds);
            while (boundary < result.Phases.Length || cursor < offers.Length)
            {
                WaitUntil(tick);
                long now = Stopwatch.GetTimestamp();
                while (boundary < result.Phases.Length && now >= stopAt)
                {
                    if (++boundary < result.Phases.Length)
                    {
                        boundarySeconds += result.Phases[boundary].Seconds;
                        stopAt = start + Ticks(boundarySeconds);
                    }
                }
                if (now >= maintenanceAt)
                {
                    int pending = 0;
                    foreach (var lane in lanes) pending += lane.Maintain(now);
                    var phase = result.Phases[Math.Min(boundary, result.Phases.Length - 1)];
                    phase.PeakOutstanding = Math.Max(phase.PeakOutstanding, pending);
                    maintenanceAt = now + Ticks(MaintenanceUs / 1e6);
                }
                while (cursor < offers.Length && offers[cursor].Due <= now - start)
                {
                    if (now - start - offers[cursor].Due > miss)
                    {
                        offers[cursor++].State = Outcome.ScheduleMiss;
                        continue;
                    }
                    int batch = 1;
                    while (batch < depth && cursor + batch < offers.Length && offers[cursor + batch].Due <= now - start) ++batch;
                    int sent = 0;
                    for (int attempt = 0; attempt < lanes.Length && sent == 0; ++attempt)
                    {
                        sent = lanes[laneIndex].Send(cursor, batch);
                        laneIndex = (laneIndex + 1) % lanes.Length;
                    }
                    if (sent == 0) offers[cursor++].State = Outcome.ClientFull;
                    else cursor += sent;
                    now = Stopwatch.GetTimestamp();
                }
                tick = Math.Max(tick + quantum, now);
            }

            long drainStart = Stopwatch.GetTimestamp();
            while (Stopwatch.GetTimestamp() - drainStart <= Ticks((timeoutMs + 100) / 1000.0))
            {
                int pending = 0;
                foreach (var lane in lanes) pending += lane.Maintain(Stopwatch.GetTimestamp());
                if (pending == 0) break;
                Thread.Sleep(1);
            }
            result.InPhaseDrainMs = Ms(Stopwatch.GetTimestamp() - drainStart);
            if (!sampler.Wait(5000)) throw new TimeoutException("Resource sampler exceeded its deadline.");
        }
        finally
        {
            stopSampling.Cancel();
            stopped.Cancel();
            var finishing = Array.ConvertAll(lanes, lane => lane.Finish());
            if (!Task.WaitAll(finishing, 5000)) throw new TimeoutException("Client receive or reconnect did not terminate.");
            foreach (var lane in lanes)
            {
                result.Reconnects += lane.Reconnects;
                result.ConnectFailures += lane.ConnectFailures;
                result.UnexpectedFrames += lane.UnexpectedFrames;
            }
            if (!sampler.IsCompleted && !sampler.Wait(5000)) throw new TimeoutException("Resource sampler did not terminate.");
        }
        Summarize(result.Phases, offers, start);
        result.Save = path => WriteTrace(path, offers, start);
        return result;
    }

    private static void WaitUntil(long target)
    {
        for (long remaining = target - Stopwatch.GetTimestamp(); remaining > 0; remaining = target - Stopwatch.GetTimestamp())
        {
            if (remaining > Ticks(0.002)) Thread.Sleep(1);
            else Thread.SpinWait(64);
        }
    }

    private static double? Quantile(List<double> sorted, double rank, int population)
    {
        int index = (int)Math.Ceiling(rank * population) - 1;
        return index >= 0 && index < sorted.Count ? sorted[index] : (double?)null;
    }

    private static void Summarize(Phase[] phases, Offer[] offers, long start)
    {
        double phaseStart = 0;
        for (int phase = 0; phase < phases.Length; ++phase)
        {
            var p = phases[phase];
            var scheduled = new List<double>();
            var wire = new List<double>();
            var lag = new List<double>();
            int first = 0, second = 0;
            foreach (var o in offers)
            {
                if (o.Phase != phase) continue;
                ++p.Offered;
                if (o.Attempt != 0)
                {
                    ++p.Attempted;
                    long delay = o.Attempt - start - o.Due;
                    lag.Add(Ms(delay));
                    if (delay > Ticks(LateUs / 1e6)) ++p.Late;
                }
                switch (o.State)
                {
                    case Outcome.Success:
                        ++p.Successful;
                        scheduled.Add(Ms(o.End - start - o.Due));
                        wire.Add(Ms(o.End - o.Attempt));
                        if (o.End - start < Ticks(phaseStart + p.Seconds / 2)) ++first;
                        else if (o.End - start < Ticks(phaseStart + p.Seconds)) ++second;
                        break;
                    case Outcome.ScheduleMiss: ++p.ScheduleMiss; break;
                    case Outcome.ClientFull: ++p.ClientFull; break;
                    case Outcome.Timeout: ++p.Timeout; break;
                    case Outcome.Eof: ++p.Eof; break;
                    case Outcome.SocketError: ++p.SocketError; break;
                    case Outcome.SendError: ++p.SendError; break;
                    case Outcome.BadResponse: ++p.BadResponse; break;
                    case Outcome.StreamDiscarded: ++p.StreamDiscarded; break;
                    default: throw new InvalidDataException("Offer lacks a terminal outcome.");
                }
                if (o.Error != 0)
                {
                    p.SocketErrors.TryGetValue(o.Error, out int found);
                    p.SocketErrors[o.Error] = found + 1;
                }
            }
            scheduled.Sort(); wire.Sort(); lag.Sort();
            p.WithinWindowSuccess = first + second;
            p.P50Ms = Quantile(scheduled, .50, p.Offered); p.P95Ms = Quantile(scheduled, .95, p.Offered); p.P99Ms = Quantile(scheduled, .99, p.Offered);
            p.WireP50Ms = Quantile(wire, .50, wire.Count); p.WireP95Ms = Quantile(wire, .95, wire.Count); p.WireP99Ms = Quantile(wire, .99, wire.Count);
            p.LagP50Ms = Quantile(lag, .50, lag.Count); p.LagP95Ms = Quantile(lag, .95, lag.Count); p.LagP99Ms = Quantile(lag, .99, lag.Count);
            p.FirstHalfSuccessRate = first / (p.Seconds / 2); p.LastHalfSuccessRate = second / (p.Seconds / 2);
            phaseStart += p.Seconds;
        }
    }

    private static void WriteTrace(string path, Offer[] offers, long start)
    {
        using var gzip = new GZipStream(File.Create(path), CompressionLevel.Fastest);
        using var writer = new StreamWriter(gzip);
        writer.WriteLine("id\tphase\tscheduled_ticks\tdispatch_ticks\tend_ticks\toutcome\tsocket_error");
        for (int i = 0; i < offers.Length; ++i)
        {
            var o = offers[i];
            writer.WriteLine(FormattableString.Invariant($"{i}\t{o.Phase}\t{o.Due}\t{(o.Attempt == 0 ? -1 : o.Attempt - start)}\t{(o.End == 0 ? -1 : o.End - start)}\t{o.State}\t{o.Error}"));
        }
    }
}
