// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;

namespace Kiln;

/// <summary>
/// A task a mod started with <see cref="KilnTasks"/>: its state, its fault, a way to cancel it and to await it. It is its
/// own awaitable, not a <see cref="Task"/>.
/// </summary>
/// <remarks>
/// <para><c>await task</c> never blocks: the code after it continues where it was awaited (on the tick when awaited from
/// tick code, in the task when awaited from another task). Reading the outcome before the task has ended throws instead
/// of waiting, on every thread: <see cref="KilnTask{T}.Result"/>, and <c>GetAwaiter().GetResult()</c> called by hand.
/// <see cref="Wait"/> blocks, so it is refused on the tick and on Kiln's task workers.</para>
/// <para><see cref="AsTask"/> gives a <see cref="Task"/> for composition (<c>Task.WhenAll</c>). Kiln cannot stop
/// <c>Wait()</c> or <c>.Result</c> on that Task: never block on it on the tick.</para>
/// </remarks>
public class KilnTask
{
    private const int Terminal = (int)KilnTaskState.Completed;

    private readonly object _gate = new();
    private volatile int _state = (int)KilnTaskState.Queued;
    private Exception? _fault;
    private OperationCanceledException? _cancel;
    private List<Continuation>? _continuations;
    private ManualResetEventSlim? _ended;
    private object? _source;  // the TaskCompletionSource behind AsTask, made on first use
    private TaskRun? _run;

    internal KilnTask(string name) => Name = name;

    /// <summary>The name the task was started with. Any thread.</summary>
    [AnyThread]
    public string Name { get; }

    /// <summary>Where the task stands now. Any thread; never blocks.</summary>
    [AnyThread]
    public KilnTaskState State => (KilnTaskState)_state;

    /// <summary>The exception that ended the task when <see cref="State"/> is <see cref="KilnTaskState.Faulted"/>; null in
    /// every other state. Any thread.</summary>
    [AnyThread]
    public Exception? Fault => _state == (int)KilnTaskState.Faulted ? _fault : null;

    internal bool IsTerminal => _state >= Terminal;

    internal TaskRun Run => _run!;

    internal void Attach(TaskRun run) => _run = run;

    /// <summary>Asks the task to stop: a task that has not started never starts (<see cref="KilnTaskState.Cancelled"/>), a
    /// running one sees <see cref="KilnTaskContext.Cancellation"/> signalled, and what it posted to the tick and was not
    /// applied yet is dropped. Cancellation is cooperative: Kiln never aborts a thread. A task that then returns normally
    /// still ends <see cref="KilnTaskState.Completed"/>. Callbacks registered on the token run on a .NET thread-pool
    /// thread, never on the thread that calls this. Any thread; never blocks; does nothing once the task has ended.</summary>
    [AnyThread]
    public void Cancel()
    {
        if (!IsTerminal) _run?.RequestCancel();
    }

    /// <summary>Blocks until the task has ended, then throws its fault (or its <see cref="OperationCanceledException"/>
    /// when it was cancelled). Only for threads of your own: on the tick thread and on Kiln's task workers it throws
    /// <see cref="InvalidOperationException"/>, also when the task has already ended. Await the task there instead.</summary>
    /// <exception cref="InvalidOperationException">Called on the tick thread, on a Kiln task worker or inside a task.</exception>
    public void Wait()
    {
        if (TaskRun.BlockingRefused() is { } why) throw new InvalidOperationException(why);
        WaitEnded(Timeout.InfiniteTimeSpan);
        ThrowIfNotCompleted();
    }

    /// <summary>A <see cref="Task"/> that ends as this task does (the same one on every call), for composition. Kiln cannot
    /// intercept blocking on it (<c>Wait()</c>, <c>.Result</c>, <c>GetAwaiter().GetResult()</c>): never block on it on the
    /// tick. Any thread.</summary>
    [AnyThread]
    public Task AsTask() => SourceTask();

    /// <summary>The awaiter <c>await task</c> uses. Any thread.</summary>
    [AnyThread]
    public Awaiter GetAwaiter() => new(this);

    /// <summary>Awaits a <see cref="KilnTask"/>. The continuation runs where it was awaited (the awaiting code's
    /// synchronization context, else the .NET thread pool), never inline on the thread that ends the task.</summary>
    public readonly struct Awaiter : ICriticalNotifyCompletion
    {
        private readonly KilnTask _task;

        internal Awaiter(KilnTask task) => _task = task;

        /// <summary>True once the task has ended.</summary>
        public bool IsCompleted => Target.IsTerminal;

        /// <summary>Throws the task's fault or cancellation. Before the task has ended it throws
        /// <see cref="InvalidOperationException"/> instead of waiting.</summary>
        public void GetResult() => Target.ThrowIfNotCompleted();

        /// <summary>Runs <paramref name="continuation"/> once when the task has ended.</summary>
        public void OnCompleted(Action continuation) => Target.Register(continuation, flowContext: true);

        /// <summary>As <see cref="OnCompleted"/>, without flowing the execution context.</summary>
        public void UnsafeOnCompleted(Action continuation) => Target.Register(continuation, flowContext: false);

        private KilnTask Target => _task ?? throw new InvalidOperationException("a default KilnTask.Awaiter: await a KilnTask instead");
    }

    // ---- outcome ----

    /// <summary>Throws unless the task completed: its fault, its cancellation, or (not ended yet) a refusal to wait.</summary>
    internal void ThrowIfNotCompleted()
    {
        switch ((KilnTaskState)_state)
        {
            case KilnTaskState.Completed:
                return;
            case KilnTaskState.Faulted:
                ExceptionDispatchInfo.Throw(_fault!);
                return;
            case KilnTaskState.Cancelled:
                ExceptionDispatchInfo.Throw(_cancel!);
                return;
            default:
                throw new InvalidOperationException(
                    $"the task '{Name}' has not ended: await it (its result is never waited for here)");
        }
    }

    internal void MarkRunning() => Interlocked.CompareExchange(ref _state, (int)KilnTaskState.Running, (int)KilnTaskState.Queued);

    /// <summary>A Task&lt;T&gt; body's result, before <see cref="Publish"/>.</summary>
    internal virtual void TakeResult(Task body) { }

    /// <summary>Ends the task, once: later calls change nothing (false). Continuations then run where they were
    /// registered, never inline here.</summary>
    internal bool Publish(KilnTaskState state, Exception? fault, OperationCanceledException? cancel)
    {
        List<Continuation>? continuations;
        ManualResetEventSlim? ended;
        object? source;
        lock (_gate)
        {
            if (IsTerminal) return false;
            _fault = fault;
            _cancel = cancel;
            _state = (int)state;  // volatile: the outcome above is visible to whoever sees the state
            continuations = _continuations;
            _continuations = null;
            ended = _ended;
            source = _source;
        }
        ended?.Set();
        if (source is not null) CompleteSource(source);
        if (continuations is not null)
            foreach (var c in continuations)
                c.Schedule();
        return true;
    }

    internal void Register(Action continuation, bool flowContext)
    {
        ArgumentNullException.ThrowIfNull(continuation);
        var c = new Continuation(continuation, SynchronizationContext.Current, flowContext ? ExecutionContext.Capture() : null);
        lock (_gate)
        {
            if (!IsTerminal)
            {
                (_continuations ??= []).Add(c);
                return;
            }
        }
        c.Schedule();  // already ended: still never inline
    }

    /// <summary>Blocks until the task has ended or <paramref name="timeout"/> passed (infinite: -1 ms); true when it
    /// ended. Callers check <see cref="TaskRun.BlockingRefused"/> first.</summary>
    internal bool WaitEnded(TimeSpan timeout)
    {
        ManualResetEventSlim ended;
        lock (_gate)
        {
            if (IsTerminal) return true;
            ended = _ended ??= new ManualResetEventSlim(false);
        }
        return ended.Wait(timeout);
    }

    // ---- AsTask ----

    private protected Task SourceTask()
    {
        object source;
        lock (_gate)
        {
            source = _source ??= NewSource();
            if (!IsTerminal) return TaskOf(source);
        }
        CompleteSource(source);
        return TaskOf(source);
    }

    private protected virtual object NewSource() => new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

    private protected virtual Task TaskOf(object source) => ((TaskCompletionSource)source).Task;

    private protected virtual bool TrySetSourceResult(object source) => ((TaskCompletionSource)source).TrySetResult();

    private void CompleteSource(object source)
    {
        switch ((KilnTaskState)_state)
        {
            case KilnTaskState.Completed:
                TrySetSourceResult(source);
                break;
            case KilnTaskState.Faulted:
                var task = TaskOf(source);
                if (SetException(source, _fault!)) _ = task.Exception;  // observed: the fault is reported once, by Kiln
                break;
            case KilnTaskState.Cancelled:
                SetCanceled(source, _cancel!.CancellationToken);
                break;
        }
    }

    private protected virtual bool SetException(object source, Exception e) => ((TaskCompletionSource)source).TrySetException(e);

    private protected virtual bool SetCanceled(object source, CancellationToken token) => ((TaskCompletionSource)source).TrySetCanceled(token);

    /// <summary>One registered continuation: posted to the context it was registered on, else to the thread pool.</summary>
    private sealed class Continuation(Action action, SynchronizationContext? context, ExecutionContext? flow)
    {
        public void Schedule()
        {
            if (context is not null) context.Post(static s => ((Continuation)s!).Invoke(), this);
            else ThreadPool.UnsafeQueueUserWorkItem(static s => s.Invoke(), this, preferLocal: false);
        }

        private void Invoke()
        {
            if (flow is null) action();
            else ExecutionContext.Run(flow, static s => ((Action)s!)(), action);
        }
    }
}

/// <summary>A <see cref="KilnTask"/> whose work returns a value: <c>T value = await task;</c>.</summary>
/// <typeparam name="T">The work's result.</typeparam>
public sealed class KilnTask<T> : KilnTask
{
    private T _result = default!;

    internal KilnTask(string name) : base(name) { }

    /// <summary>The work's result once the task has <see cref="KilnTaskState.Completed"/>. Never blocks: before the task has
    /// ended it throws <see cref="InvalidOperationException"/>, on every thread; after a fault or a cancel it throws that.
    /// Any thread.</summary>
    [AnyThread]
    public T Result
    {
        get
        {
            ThrowIfNotCompleted();
            return _result;
        }
    }

    /// <summary>A <see cref="Task{TResult}"/> that ends as this task does (the same one on every call), for composition.
    /// Never block on it on the tick. Any thread.</summary>
    [AnyThread]
    public new Task<T> AsTask() => (Task<T>)SourceTask();

    /// <summary>The awaiter <c>await task</c> uses. Any thread.</summary>
    [AnyThread]
    public new Awaiter GetAwaiter() => new(this);

    /// <summary>Awaits a <see cref="KilnTask{T}"/> for its result, as <see cref="KilnTask.Awaiter"/> does.</summary>
    public new readonly struct Awaiter : ICriticalNotifyCompletion
    {
        private readonly KilnTask<T> _task;

        internal Awaiter(KilnTask<T> task) => _task = task;

        /// <summary>True once the task has ended.</summary>
        public bool IsCompleted => Target.IsTerminal;

        /// <summary>The result, or the task's fault or cancellation. Before the task has ended it throws
        /// <see cref="InvalidOperationException"/> instead of waiting.</summary>
        public T GetResult() => Target.Result;

        /// <summary>Runs <paramref name="continuation"/> once when the task has ended.</summary>
        public void OnCompleted(Action continuation) => Target.Register(continuation, flowContext: true);

        /// <summary>As <see cref="OnCompleted"/>, without flowing the execution context.</summary>
        public void UnsafeOnCompleted(Action continuation) => Target.Register(continuation, flowContext: false);

        private KilnTask<T> Target => _task ?? throw new InvalidOperationException("a default KilnTask<T>.Awaiter: await a KilnTask<T> instead");
    }

    internal override void TakeResult(Task body) => _result = ((Task<T>)body).Result;

    private protected override object NewSource() => new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);

    private protected override Task TaskOf(object source) => ((TaskCompletionSource<T>)source).Task;

    private protected override bool TrySetSourceResult(object source) => ((TaskCompletionSource<T>)source).TrySetResult(_result);

    private protected override bool SetException(object source, Exception e) => ((TaskCompletionSource<T>)source).TrySetException(e);

    private protected override bool SetCanceled(object source, CancellationToken token) => ((TaskCompletionSource<T>)source).TrySetCanceled(token);
}
