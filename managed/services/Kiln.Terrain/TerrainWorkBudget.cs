// SPDX-License-Identifier: MIT
namespace Kiln;

// One instance in Terrain's process-wide session. Tick thread only. Admission is FIFO, and a unit is at most 8³
// cells. Native calls cannot be preempted: the time limit stops admitting further work after a completed unit.
internal sealed class TerrainWorkBudget
{
    internal const int ReadCells = 32768, WriteCells = 4096;
    internal const double Milliseconds = 2;
    private ulong? step;
    private long reads, writes;
    private double spent;
    private sealed record Request(string Mod);
    private readonly LinkedList<Request> queue = new();

    public async Task<bool> Acquire(ITerrainBackend backend, long read, long write)
    {
        if (backend.BudgetStep is null) return backend.WorldRunning; // legacy synchronous test adapters
        if (read > ReadCells || write > WriteCells) throw new InvalidOperationException("terrain work unit exceeds host budget");
        var ticket = queue.AddLast(new Request(backend.Mod));
        try
        {
            while (backend.WorldRunning && ticket.List is not null)
            {
                if (step != backend.BudgetStep) { step = backend.BudgetStep; reads = writes = 0; spent = 0; }
                if (queue.First == ticket && reads + read <= ReadCells && writes + write <= WriteCells && spent < Milliseconds && backend.TryReserveBudget(read, write))
                {
                    reads += read; writes += write;
                    return true;
                }
                await backend.NextTick();
            }
            return false;
        }
        finally { if (ticket.List is not null) queue.Remove(ticket); }
    }

    public void Charge(ITerrainBackend backend, double started)
    {
        if (backend.BudgetStep is not null && step == backend.BudgetStep) {
            var elapsed = Math.Max(0, backend.BudgetMilliseconds - started);
            spent += elapsed;
            backend.ChargeBudget(elapsed);
        }
    }
    public void Retire(string mod)
    {
        for (var n = queue.First; n is not null;)
        {
            var next = n.Next;
            if (n.Value.Mod == mod) queue.Remove(n);
            n = next;
        }
    }
    public void Clear() { queue.Clear(); step = null; reads = writes = 0; spent = 0; }
}
