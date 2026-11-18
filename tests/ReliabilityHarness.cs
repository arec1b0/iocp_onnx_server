using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

// Windows-only, loopback-only validation support for ReliabilitySoak.ps1.
public static class ReliabilityHarness
{
    private const int Port = 8080;
    private const int FrameBytes = 48;

    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes
    {
        public int Length;
        public IntPtr Descriptor;
        public int Inherit;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfo
    {
        public int Size;
        public IntPtr Reserved, Desktop, Title;
        public int X, Y, XSize, YSize, XCount, YCount, Fill, Flags;
        public short ShowWindow, ReservedSize;
        public IntPtr ReservedData, Input, Output, Error;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInfo
    {
        public IntPtr Process, Thread;
        public int ProcessId, ThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFile(string name, uint access, uint share,
        ref SecurityAttributes attributes, uint mode, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcess(string application, IntPtr command, IntPtr processAttributes,
        IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string directory,
        ref StartupInfo startup, out ProcessInfo information);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeInfo(IntPtr pipe, out uint flags, out uint outbound, out uint inbound, out uint instances);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool PeekNamedPipe(IntPtr pipe, IntPtr buffer, uint size, IntPtr read, out uint available, IntPtr remaining);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(IntPtr file, byte[] buffer, uint size, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AttachConsole(int processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetConsoleProcessList([Out] int[] processes, uint count);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GenerateConsoleCtrlEvent(uint controlEvent, uint group);

    private static Exception NativeError(string operation)
    {
        var error = Marshal.GetLastWin32Error();
        return new System.ComponentModel.Win32Exception(error, operation + " failed with Win32 error " + error);
    }

    private static IntPtr OpenConsoleFile(string name, bool input)
    {
        var attributes = new SecurityAttributes { Length = Marshal.SizeOf<SecurityAttributes>(), Inherit = 1 };
        var handle = CreateFile(name, input ? 0x80000000u : 0x40000000u, 7, ref attributes,
            input ? 3u : 1u, 0x80, IntPtr.Zero);
        if (handle == new IntPtr(-1)) throw NativeError("Open console stream");
        return handle;
    }

    public static Process StartServer(string executable, string directory, string stdout, string stderr)
    {
        var output = OpenConsoleFile(stdout, false);
        try { return StartServer(executable, directory, output, stderr); }
        finally { CloseHandle(output); }
    }

    public static Process StartServer(string executable, string directory, IntPtr stdout, string stderr)
    {
        var startup = new StartupInfo { Size = Marshal.SizeOf<StartupInfo>(), Flags = 0x101, Output = stdout };
        try
        {
            startup.Input = OpenConsoleFile("NUL", true);
            startup.Error = OpenConsoleFile(stderr, false);
            if (!CreateProcess(executable, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, true, 0x10,
                    IntPtr.Zero, directory, ref startup, out var info))
                throw NativeError("Create isolated server console");
            try
            {
                var process = Process.GetProcessById(info.ProcessId);
                _ = process.Handle;
                return process;
            }
            finally { CloseHandle(info.Thread); CloseHandle(info.Process); }
        }
        finally
        {
            if (startup.Input != IntPtr.Zero) CloseHandle(startup.Input);
            if (startup.Error != IntPtr.Zero) CloseHandle(startup.Error);
        }
    }

    public static uint InterruptServer(Process server, int timeoutMs)
    {
        // The controller was launched without a console. Only broadcast in
        // the dedicated console if it contains exactly this controller and its server.
        // Detach only this dedicated controller if its host allocated a console.
        FreeConsole();
        if (!AttachConsole(server.Id)) throw NativeError("Attach to server console");
        try
        {
            var processes = new int[3];
            var count = GetConsoleProcessList(processes, (uint)processes.Length);
            if (count != 2 || Array.IndexOf(processes, server.Id, 0, 2) < 0 ||
                Array.IndexOf(processes, Environment.ProcessId, 0, 2) < 0)
                throw new InvalidOperationException("Server console is not isolated.");
            if (!SetConsoleCtrlHandler(IntPtr.Zero, true)) throw NativeError("Protect console controller");
            if (!GenerateConsoleCtrlEvent(0, 0)) throw NativeError("Send the server CTRL+C");
            if (!server.WaitForExit(timeoutMs)) throw new TimeoutException("Server shutdown exceeded its deadline.");
            return ReadExitCode(server);
        }
        finally
        {
            // A controller may launch another server. Do not pass on CTRL+C ignore state.
            SetConsoleCtrlHandler(IntPtr.Zero, false);
            FreeConsole();
        }
    }

    public static uint ReadExitCode(Process server)
    {
        if (!GetExitCodeProcess(server.Handle, out uint exitCode)) throw NativeError("Read server exit code");
        return exitCode;
    }

    public static Task<uint> FillStdoutPipe(IntPtr read, IntPtr write)
    {
        if (!GetNamedPipeInfo(read, out _, out uint outbound, out uint inbound, out _))
            throw NativeError("Read pipe capacity");
        uint capacity = Math.Max(outbound, inbound);
        if (capacity == 0 || capacity > 65536) throw new IOException("Unexpected test pipe capacity.");
        if (!PeekNamedPipe(read, IntPtr.Zero, 0, IntPtr.Zero, out uint available, IntPtr.Zero) || available != 0)
            throw new IOException("The test requires a drained pipe at startup.");

        // Fill exactly the kernel buffer without changing its mode or leaving a pending write.
        return Task.Run(() =>
        {
            if (!WriteFile(write, new byte[capacity], capacity, out uint written, IntPtr.Zero) || written != capacity ||
                !PeekNamedPipe(read, IntPtr.Zero, 0, IntPtr.Zero, out uint buffered, IntPtr.Zero) || buffered != capacity)
                throw new IOException("Could not verify a full stdout pipe.");
            return capacity;
        });
    }

    public static TcpClient OpenInferredClient(int timeout)
    {
        var client = Connect(timeout);
        try
        {
            const int seed = 17;
            client.GetStream().Write(Request(seed));
            var actual = new byte[FrameBytes];
            var expected = new byte[FrameBytes];
            Expected(expected, 1, seed);
            ReceiveExact(client, actual, timeout, Stopwatch.StartNew());
            if (!actual.AsSpan().SequenceEqual(expected)) throw new IOException("Incorrect test inference result.");
            return client;
        }
        catch { client.Dispose(); throw; }
    }

    // Write a minimal ONNX graph with Mul(input, 2) followed by Expand.
    // The larger static output exercises Winsock backpressure with real ONNX inference.
    private static byte[] Join(params byte[][] fields)
    {
        using var stream = new MemoryStream();
        foreach (var field in fields) stream.Write(field);
        return stream.ToArray();
    }

    private static byte[] Varint(ulong value)
    {
        var result = new List<byte>();
        while (value >= 128) { result.Add((byte)((value & 127) | 128)); value >>= 7; }
        result.Add((byte)value);
        return result.ToArray();
    }

    private static byte[] Number(int id, ulong value) => Join(Varint((ulong)id << 3), Varint(value));
    private static byte[] Bytes(int id, byte[] value) => Join(Varint(((ulong)id << 3) | 2), Varint((ulong)value.Length), value);
    private static byte[] Name(int id, string value) => Bytes(id, Encoding.UTF8.GetBytes(value));

    private static byte[] TensorInfo(string name, ulong rows)
    {
        var shape = Join(Bytes(1, Number(1, rows)), Bytes(1, Number(1, 10)));
        return Join(Name(1, name), Bytes(2, Bytes(1, Join(Number(1, 1), Bytes(2, shape)))));
    }

    public static void WriteSoakModel(string path, int rows)
    {
        var multiply = Join(Name(1, "input"), Name(1, "scale"), Name(2, "doubled"), Name(4, "Mul"));
        var expand = Join(Name(1, "doubled"), Name(1, "shape"), Name(2, "output"), Name(4, "Expand"));
        var scale = Join(Number(1, 1), Number(2, 1), Name(8, "scale"), Bytes(9, BitConverter.GetBytes(2.0f)));
        var dimensions = Join(BitConverter.GetBytes((long)rows), BitConverter.GetBytes(10L));
        var shape = Join(Number(1, 2), Number(2, 7), Name(8, "shape"), Bytes(9, dimensions));
        var graph = Join(Bytes(1, multiply), Bytes(1, expand), Name(2, "reliability_fixture"),
            Bytes(5, scale), Bytes(5, shape), Bytes(11, TensorInfo("input", 1)), Bytes(12, TensorInfo("output", (ulong)rows)));
        File.WriteAllBytes(path, Join(Number(1, 8), Bytes(7, graph), Bytes(8, Number(2, 13))));
    }

    public static long UtcMicroseconds() => (DateTime.UtcNow.Ticks - DateTime.UnixEpoch.Ticks) / 10;

    public sealed class ClientLog
    {
        private readonly Attempt[] entries = new Attempt[8192];
        private long issued, written;
        public long Issued => Interlocked.Read(ref issued);
        public long Recorded { get { lock (entries) return written; } }
        public Attempt Begin(string role) => new Attempt { client_id = Interlocked.Increment(ref issued), role = role, started_us = UtcMicroseconds() };

        public void End(Attempt attempt, string operation, Exception error = null)
        {
            if (attempt == null) return;
            attempt.operation = operation;
            attempt.ended_us = UtcMicroseconds();
            if (error != null)
            {
                var cause = error.GetBaseException();
                attempt.outcome = cause is EndOfStreamException ? "early_eof" :
                    cause is TimeoutException ? "harness_deadline" : cause is SocketException ? "socket_error" : "harness_error";
                attempt.code = cause is SocketException socket ? socket.NativeErrorCode : cause is EndOfStreamException ? 0 : null;
            }
            lock (entries) { entries[written++ % entries.Length] = attempt; }
        }

        public Attempt[] Snapshot()
        {
            lock (entries)
            {
                var result = new Attempt[Math.Min(written, entries.Length)];
                for (var i = 0; i < result.Length; ++i) result[i] = entries[(written - result.Length + i) % entries.Length];
                return result;
            }
        }
    }

    public sealed class Attempt
    {
        public long client_id { get; set; }
        public long started_us { get; set; }
        public long? connected_us { get; set; }
        public long ended_us { get; set; }
        public long? age_us => connected_us.HasValue ? ended_us - connected_us.Value : null;
        public string local { get; set; }
        public string remote { get; } = "127.0.0.1:8080";
        public string role { get; set; }
        public string operation { get; set; }
        public string outcome { get; set; } = "completed";
        public int? code { get; set; }
        public int? shutdown_code { get; set; }
        public int phase { get; set; }
        public int requests_started { get; set; }
        public int responses_verified { get; set; }
    }

    public static Dictionary<string, int> TcpStates(int firstPort, int count)
    {
        var states = new Dictionary<string, int> { ["TimeWait"] = 0, ["Established"] = 0, ["CloseWait"] = 0,
            ["target_TimeWait"] = 0, ["target_Established"] = 0, ["target_CloseWait"] = 0 };
        var ports = new HashSet<int>();
        foreach (var row in IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpConnections())
        {
            if (row.LocalEndPoint.AddressFamily != AddressFamily.InterNetwork) continue;
            var state = row.State.ToString();
            if (states.ContainsKey(state)) ++states[state];
            if (row.RemoteEndPoint.Port == Port && IPAddress.IsLoopback(row.RemoteEndPoint.Address) && states.ContainsKey("target_" + state))
                ++states["target_" + state];
            int port = row.LocalEndPoint.Port;
            if (port >= firstPort && port - firstPort < count) ports.Add(port);
        }
        states["observed_dynamic_ports"] = ports.Count;
        states["dynamic_ports_not_seen"] = count - ports.Count;
        return states;
    }

    private static TcpClient Connect(int timeout, int receiveBuffer = 0, Attempt attempt = null)
    {
        var socket = new TcpClient(AddressFamily.InterNetwork);
        try
        {
            socket.NoDelay = true;
            socket.SendTimeout = timeout;
            socket.ReceiveTimeout = timeout;
            if (receiveBuffer > 0) socket.ReceiveBufferSize = receiveBuffer;
            if (!socket.ConnectAsync(IPAddress.Loopback, Port).Wait(timeout)) throw new TimeoutException("Loopback connect");
            if (attempt != null)
            {
                attempt.local = socket.Client.LocalEndPoint?.ToString();
                attempt.connected_us = UtcMicroseconds();
            }
            return socket;
        }
        catch
        {
            if (attempt != null)
            {
                try { attempt.local = socket.Client.LocalEndPoint?.ToString(); }
                catch (SocketException) { }
            }
            socket.Dispose();
            throw;
        }
    }

    private static byte[] Request(int seed)
    {
        var request = new byte[FrameBytes];
        BitConverter.GetBytes(10UL).CopyTo(request, 0);
        for (int i = 0; i < 10; ++i) BitConverter.GetBytes((float)(seed + i)).CopyTo(request, 8 + 4 * i);
        return request;
    }

    private static void Expected(byte[] output, int rows, int seed)
    {
        BitConverter.GetBytes((ulong)rows * 10).CopyTo(output, 0);
        for (int i = 0; i < 10; ++i) BitConverter.GetBytes(2.0f * (seed + i)).CopyTo(output, 8 + 4 * i);
        for (int copied = 40; copied < output.Length - 8;)
        {
            int count = Math.Min(copied, output.Length - 8 - copied);
            Buffer.BlockCopy(output, 8, output, 8 + copied, count);
            copied += count;
        }
    }

    private static void ReceiveExact(TcpClient client, byte[] output, int timeout, Stopwatch elapsed)
    {
        var stream = client.GetStream();
        for (int offset = 0; offset < output.Length;)
        {
            var remaining = timeout - elapsed.ElapsedMilliseconds;
            if (remaining <= 0) throw new TimeoutException("Response deadline");
            client.ReceiveTimeout = (int)remaining;
            int count = stream.Read(output, offset, output.Length - offset);
            if (count == 0) throw new EndOfStreamException("Response ended early");
            offset += count;
        }
    }

    private static void PeerClosed(TcpClient client, int timeout, NetworkStream stream = null, Attempt attempt = null)
    {
        client.ReceiveTimeout = timeout;
        try
        {
            if ((stream ?? client.GetStream()).ReadByte() != -1) throw new IOException("Unexpected extra response data");
            if (attempt != null) { attempt.outcome = "peer_eof"; attempt.code = 0; }
        }
        catch (IOException e) when (e.InnerException is SocketException socket &&
            (socket.SocketErrorCode == SocketError.ConnectionReset || socket.SocketErrorCode == SocketError.ConnectionAborted))
        {
            if (attempt != null) { attempt.outcome = "peer_close_error"; attempt.code = socket.NativeErrorCode; }
        }
    }

    private static void Finish(TcpClient client, int timeout, Attempt attempt = null)
    {
        var stream = client.GetStream();
        try { client.Client.Shutdown(SocketShutdown.Send); }
        catch (SocketException e) when (e.SocketErrorCode == SocketError.NotConnected || e.SocketErrorCode == SocketError.ConnectionReset)
        {
            if (attempt != null) attempt.shutdown_code = e.NativeErrorCode;
        }
        PeerClosed(client, timeout, stream, attempt);
    }

    public static List<TcpClient> OpenSaturation(int capacity, int timeout)
    {
        var sockets = new List<TcpClient>();
        try
        {
            for (int i = 0; i < capacity + 4; ++i) sockets.Add(Connect(timeout));
            return sockets;
        }
        catch
        {
            foreach (var client in sockets) client.Dispose();
            throw;
        }
    }

    public sealed class Workload
    {
        private readonly int rows, capacity, timeout, idle, frame, gap, saturationInterval;
        private readonly long[] latency;
        private readonly ReaderWriterLockSlim admission = new ReaderWriterLockSlim();
        private readonly ConcurrentDictionary<string, long> episodes = new ConcurrentDictionary<string, long>();
        private readonly ConcurrentQueue<string> errors = new ConcurrentQueue<string>();
        private readonly CancellationTokenSource stop = new CancellationTokenSource();
        private long requests;
        public ClientLog Connections { get; }

        public Workload(int rows, int capacity, int timeout, int idle, int frame, int gap, int saturationInterval, bool diagnostics = false)
        {
            this.rows = rows; this.capacity = capacity; this.timeout = timeout; this.idle = idle;
            this.frame = frame; this.gap = gap; this.saturationInterval = saturationInterval;
            latency = new long[timeout + 1];
            if (diagnostics) Connections = new ClientLog();
        }

        public long Requests => Interlocked.Read(ref requests);
        public long[] Latency => latency;
        public Dictionary<string, long> Episodes => new Dictionary<string, long>(episodes);
        public string[] Errors => errors.ToArray();
        public void Stop() => stop.Cancel();
        private bool Delay(int ms) => stop.Token.WaitHandle.WaitOne(ms);

        public Task Run(string[] roles, int seed, int seconds)
        {
            stop.CancelAfter(TimeSpan.FromSeconds(seconds));
            var workers = new Task[roles.Length];
            for (int i = 0; i < roles.Length; ++i)
            {
                string role = roles[i];
                int workerSeed = unchecked(seed + i * 104729);
                workers[i] = Task.Factory.StartNew(() => RunWorker(role, workerSeed), TaskCreationOptions.LongRunning);
            }
            return Task.WhenAll(workers);
        }

        public Task RunControlled(int[] rates, bool reuse, bool policies, int seed) =>
            Task.Factory.StartNew(() =>
            {
                if (policies) { RunPolicyProbe(seed); return; }
                int phase = 0;
                foreach (int rate in rates)
                {
                    ++phase;
                    // A fixed 100 ordered responses per phase, without catch-up bursts.
                    var pacing = TimeSpan.FromSeconds(1.0 / rate);
                    var group = reuse ? 100 : 1;
                    for (int offset = 0; offset < 100 && !stop.IsCancellationRequested; offset += group)
                    {
                        var attempt = Connections.Begin((reuse ? "reuse_" : "churn_") + rate);
                        attempt.phase = phase;
                        var operation = "connect";
                        try
                        {
                            using var client = Connect(timeout, attempt: attempt);
                            for (int request = 0; request < group && !stop.IsCancellationRequested; ++request)
                            {
                                var start = Stopwatch.StartNew();
                                Exchange(client, unchecked(seed % 1024 + offset + request), attempt, ref operation);
                                if (request + offset + 1 != 100 && pacing > start.Elapsed)
                                    stop.Token.WaitHandle.WaitOne(pacing - start.Elapsed);
                            }
                            operation = "recv_after_half_close";
                            Finish(client, timeout, attempt);
                            Connections.End(attempt, operation);
                        }
                        catch (Exception error)
                        {
                            Connections.End(attempt, operation, error);
                            errors.Enqueue(operation + ": " + error.GetBaseException().Message);
                            return;
                        }
                    }
                }
            }, TaskCreationOptions.LongRunning);

        private void Exchange(TcpClient client, int seed, Attempt attempt, ref string operation)
        {
            var start = Stopwatch.StartNew();
            operation = "send";
            ++attempt.requests_started;
            client.GetStream().Write(Request(seed));
            operation = "recv";
            var actual = new byte[FrameBytes];
            var expected = new byte[FrameBytes];
            Expected(expected, 1, seed);
            ReceiveExact(client, actual, timeout, start);
            if (!actual.AsSpan().SequenceEqual(expected)) throw new IOException("Invalid or out-of-order inference response");
            ++attempt.responses_verified;
            ++latency[(int)Math.Min(timeout, Math.Ceiling(start.Elapsed.TotalMilliseconds))];
            Interlocked.Increment(ref requests);
        }

        private void RunPolicyProbe(int seed)
        {
            var retained = new List<(TcpClient client, Attempt attempt)>();
            try
            {
                for (int i = 0; i < capacity; ++i)
                {
                    var attempt = Connections.Begin("capacity_holder");
                    var operation = "connect";
                    try
                    {
                        var client = Connect(timeout, attempt: attempt);
                        retained.Add((client, attempt));
                        Exchange(client, seed % 1024 + i, attempt, ref operation);
                    }
                    catch (Exception error)
                    {
                        Connections.End(attempt, operation, error);
                        throw;
                    }
                }
                ProbeClose("capacity_excess", false);
                foreach (var item in retained)
                {
                    var operation = "send";
                    try
                    {
                        Exchange(item.client, seed % 1024 - 1, item.attempt, ref operation);
                        operation = "recv_after_half_close";
                        Finish(item.client, timeout, item.attempt);
                    }
                    catch (Exception error) { Connections.End(item.attempt, operation, error); throw; }
                }
            }
            catch (Exception error) { errors.Enqueue(error.GetBaseException().Message); }
            finally
            {
                foreach (var item in retained)
                {
                    item.client.Dispose();
                    if (item.attempt.ended_us == 0) Connections.End(item.attempt, "recv_after_half_close");
                }
            }
            if (!stop.IsCancellationRequested && errors.IsEmpty)
            {
                ProbeClose("idle_probe", false);
                ProbeClose("frame_probe", true);
            }
        }

        private void ProbeClose(string role, bool partial)
        {
            var attempt = Connections.Begin(role);
            var operation = "connect";
            try
            {
                using var client = Connect(timeout, attempt: attempt);
                if (partial)
                {
                    operation = "send";
                    client.GetStream().WriteByte(10);
                }
                operation = "recv";
                PeerClosed(client, timeout, attempt: attempt);
                Connections.End(attempt, operation);
            }
            catch (Exception error)
            {
                Connections.End(attempt, operation, error);
                errors.Enqueue(operation + ": " + error.GetBaseException().Message);
            }
        }

        private void RunSaturationBurst()
        {
            var clients = new List<(TcpClient client, Attempt attempt)>();
            try
            {
                for (int i = 0; i < capacity + 4; ++i)
                {
                    var attempt = Connections?.Begin("saturation");
                    try { clients.Add((Connect(timeout, attempt: attempt), attempt)); }
                    catch (Exception error) { Connections?.End(attempt, "connect", error); throw; }
                }
                foreach (var item in clients)
                {
                    try
                    {
                        Finish(item.client, timeout, item.attempt);
                        Connections?.End(item.attempt, "recv_after_half_close");
                    }
                    catch (Exception error) { Connections?.End(item.attempt, "recv_after_half_close", error); throw; }
                }
            }
            finally
            {
                foreach (var item in clients)
                {
                    item.client.Dispose();
                    if (item.attempt != null && item.attempt.ended_us == 0)
                        Connections.End(item.attempt, "cleanup");
                }
            }
        }

        private void RunWorker(string role, int seed)
        {
            var rng = new Random(seed);
            string operation = "worker setup";
            Attempt attempt = null;
            bool valid = role == "persistent" || role == "short" || role == "fragmented";
            var actual = valid ? new byte[8 + rows * 40] : Array.Empty<byte>();
            var expected = valid ? new byte[actual.Length] : Array.Empty<byte>();
            try
            {
                while (!stop.IsCancellationRequested)
                {
                    if (role == "saturation")
                    {
                        operation = "saturation burst";
                        if (!admission.TryEnterWriteLock(timeout)) throw new TimeoutException("Saturation barrier");
                        try { RunSaturationBurst(); }
                        finally { admission.ExitWriteLock(); }
                    }
                    else if (valid)
                    {
                        operation = "connect";
                        if (!admission.TryEnterReadLock(timeout)) throw new TimeoutException("Request admission barrier");
                        try
                        {
                            attempt = Connections?.Begin(role);
                            using var client = Connect(timeout, attempt: attempt);
                            for (int i = 0; i < (role == "persistent" ? 8 : 1) && !stop.IsCancellationRequested; ++i)
                            {
                                int value = rng.Next(-1024, 1024);
                                var request = Request(value);
                                Expected(expected, rows, value);
                                var stream = client.GetStream();
                                var elapsed = Stopwatch.StartNew();
                                operation = "send";
                                if (attempt != null) ++attempt.requests_started;
                                if (role == "fragmented")
                                {
                                    stream.Write(request, 0, 3);
                                    Delay(5 + rng.Next(8));
                                    stream.Write(request, 3, 10);
                                    Delay(5 + rng.Next(8));
                                    stream.Write(request, 13, request.Length - 13);
                                }
                                else { stream.Write(request); }
                                operation = "recv";
                                ReceiveExact(client, actual, timeout, elapsed);
                                var milliseconds = (int)Math.Min(timeout, Math.Ceiling(elapsed.Elapsed.TotalMilliseconds));
                                if (!actual.AsSpan().SequenceEqual(expected)) throw new IOException("Invalid inference response");
                                Interlocked.Increment(ref latency[milliseconds]);
                                Interlocked.Increment(ref requests);
                                if (attempt != null) ++attempt.responses_verified;
                                if (Delay(gap / 2 + rng.Next(gap))) break;
                            }
                            operation = "recv_after_half_close";
                            Finish(client, timeout, attempt);
                        }
                        finally { admission.ExitReadLock(); }
                    }
                    else
                    {
                        attempt = Connections?.Begin(role);
                        operation = "connect";
                        AdverseEpisode(role, rng, attempt, ref operation);
                    }
                    Connections?.End(attempt, operation);
                    attempt = null;
                    episodes.AddOrUpdate(role, 1, (_, count) => count + 1);
                    Delay(role == "saturation" ? saturationInterval : gap / 2 + rng.Next(gap));
                }
            }
            catch (Exception error)
            {
                Connections?.End(attempt, operation, error);
                var cause = error.GetBaseException();
                var socketError = cause is SocketException socket ? " (" + socket.SocketErrorCode + ")" : "";
                errors.Enqueue(role + ", " + operation + socketError + ": " + cause.Message);
                stop.Cancel();
            }
        }

        private void AdverseEpisode(string role, Random rng, Attempt attempt, ref string operation)
        {
            if (!admission.TryEnterReadLock(timeout)) throw new TimeoutException("Adverse client admission barrier");
            TcpClient client;
            try { client = Connect(timeout, role == "stalled" ? 1024 : 0, attempt); }
            finally { admission.ExitReadLock(); }
            using (client)
            {
                var stream = client.GetStream();
                switch (role)
                {
                    case "idle":
                        operation = "recv";
                        PeerClosed(client, timeout, attempt: attempt);
                        break;
                    case "slow":
                        var prefix = Request(1);
                        operation = "send";
                        stream.Write(prefix, 0, 1);
                        if (!Delay(frame / 3))
                        {
                            stream.Write(prefix, 1, 1);
                            operation = "recv";
                            PeerClosed(client, timeout, attempt: attempt);
                        }
                        break;
                    case "stalled":
                        var batch = new byte[8 * FrameBytes];
                        for (int i = 0; i < 8; ++i) Request(i).CopyTo(batch, i * FrameBytes);
                        operation = "send";
                        if (attempt != null) attempt.requests_started = 8;
                        stream.Write(batch);
                        Delay(2 * idle);
                        client.LingerState = new LingerOption(true, 0);
                        break;
                    case "reset":
                        var reset = Request(rng.Next(-1024, 1024));
                        operation = "send";
                        stream.Write(reset, 0, rng.Next(1, FrameBytes + 1));
                        client.LingerState = new LingerOption(true, 0);
                        break;
                }
            }
        }
    }
}
