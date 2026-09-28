# Read-only WinEvent observation. No keyboard/mouse hooks or input injection.
if (!('ProductDesktopEvidence' -as [type])) {
Add-Type @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class DesktopEvidenceState {
    public int session_id;
    public bool interactive, default_input_desktop, same_input_desktop, input_available;
    public uint foreground_pid, last_input_tick;
    public string utc;
}
public static class ProductDesktopEvidence {
    [StructLayout(LayoutKind.Sequential)] struct LastInput { public uint size, time; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LastInput value);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(uint thread);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder value, int bytes, out int needed);
    static string DesktopName(IntPtr handle) {
        var name = new StringBuilder(256); int needed;
        return handle != IntPtr.Zero && GetUserObjectInformation(handle, 2, name, 512, out needed) ? name.ToString() : null;
    }
    public static DesktopEvidenceState Snapshot() {
        var state = new DesktopEvidenceState();
        using (var process = Process.GetCurrentProcess()) state.session_id = process.SessionId;
        state.interactive = Environment.UserInteractive;
        state.utc = DateTime.UtcNow.ToString("o");
        uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); state.foreground_pid = pid;
        var input = new LastInput { size = (uint)Marshal.SizeOf(typeof(LastInput)) };
        state.input_available = GetLastInputInfo(ref input); state.last_input_tick = input.time;
        var desktop = OpenInputDesktop(0, false, 1);
        try {
            var inputName = DesktopName(desktop);
            state.default_input_desktop = inputName == "Default";
            state.same_input_desktop = inputName != null && inputName == DesktopName(GetThreadDesktop(GetCurrentThreadId()));
        } finally { if (desktop != IntPtr.Zero) CloseDesktop(desktop); }
        return state;
    }
}
public sealed class DesktopMenuEvent {
    public long sequence, window_token;
    public string kind;
    public DesktopEvidenceState desktop;
}
public sealed class ProductMenuObserver : IDisposable {
    delegate void WinEvent(IntPtr hook, uint kind, IntPtr window, int objectId, int childId, uint thread, uint time);
    [StructLayout(LayoutKind.Sequential)] struct Message {
        public IntPtr window; public uint kind; public UIntPtr wparam; public IntPtr lparam;
        public uint time; public int x, y; public uint reserved;
    }
    [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint first, uint last, IntPtr module, WinEvent callback, uint pid, uint thread, uint flags);
    [DllImport("user32.dll")] static extern bool UnhookWinEvent(IntPtr hook);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll")] static extern bool PeekMessage(out Message message, IntPtr window, uint first, uint last, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref Message message);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref Message message);
    readonly object sync = new object();
    readonly Queue<DesktopMenuEvent> events = new Queue<DesktopMenuEvent>();
    readonly Dictionary<IntPtr,long> popups = new Dictionary<IntPtr,long>();
    readonly uint pid;
    readonly Thread worker;
    readonly ManualResetEvent ready = new ManualResetEvent(false);
    volatile bool stop, healthy = true, seen, closed;
    long sequence, nextToken, lost;
    public bool Healthy { get { return healthy; } }
    public bool PopupSeen { get { return seen; } }
    public bool PopupClosed { get { return closed; } }
    public long LostCount { get { lock(sync) return lost; } }
    public ProductMenuObserver(int processId) {
        pid = (uint)processId;
        worker = new Thread(Run); worker.IsBackground = true; worker.Start();
        if (!ready.WaitOne(5000) || !healthy) {
            Dispose(); throw new InvalidOperationException("MENU_OBSERVER_START_FAILED");
        }
    }
    void Add(string kind, long token) {
        var item = new DesktopMenuEvent { kind=kind, window_token=token, desktop=ProductDesktopEvidence.Snapshot() };
        lock(sync) {
            item.sequence = ++sequence;
            if(events.Count == 1024) { ++lost; return; }
            events.Enqueue(item);
        }
    }
    void OnEvent(IntPtr hook, uint kind, IntPtr window, int objectId, int childId, uint thread, uint time) {
        try {
            if (kind == 3) { Add("foreground_changed", 0); return; }
            if (window == IntPtr.Zero || (kind >= 0x8000 && (objectId != 0 || childId != 0))) return;
            long token;
            if (!popups.TryGetValue(window, out token)) {
                var name = new StringBuilder(256); GetClassName(window, name, 256);
                if (name.ToString().IndexOf("QWindowPopup", StringComparison.Ordinal) < 0) return;
                token = ++nextToken; popups[window] = token;
            }
            if (kind == 6 || kind == 0x8002) { seen=true; Add("popup_shown", token); }
            if (kind == 7 || kind == 0x8003 || kind == 0x8001) {
                if (seen) closed=true;
                Add(kind == 0x8001 ? "popup_destroyed" : "popup_hidden", token);
                if(kind == 0x8001) popups.Remove(window);
            }
        } catch { healthy=false; }
    }
    void Run() {
        var hooks = new List<IntPtr>(); WinEvent callback = OnEvent;
        try {
            hooks.Add(SetWinEventHook(6, 7, IntPtr.Zero, callback, pid, 0, 0));
            hooks.Add(SetWinEventHook(0x8001, 0x8003, IntPtr.Zero, callback, pid, 0, 0));
            hooks.Add(SetWinEventHook(3, 3, IntPtr.Zero, callback, 0, 0, 0));
            foreach(var hook in hooks) if(hook == IntPtr.Zero) throw new InvalidOperationException();
            Add("observer_started", 0); ready.Set();
            var previous = ProductDesktopEvidence.Snapshot();
            while(!stop) {
                Message message;
                while(PeekMessage(out message, IntPtr.Zero, 0, 0, 1)) {
                    TranslateMessage(ref message); DispatchMessage(ref message);
                }
                var current = ProductDesktopEvidence.Snapshot();
                if(current.input_available != previous.input_available || current.last_input_tick != previous.last_input_tick) Add("input_activity", 0);
                if(current.default_input_desktop != previous.default_input_desktop || current.same_input_desktop != previous.same_input_desktop) Add("desktop_changed", 0);
                previous=current;
                Thread.Sleep(20);
            }
        } catch { healthy=false; }
        finally {
            foreach(var hook in hooks) if(hook != IntPtr.Zero) UnhookWinEvent(hook);
            GC.KeepAlive(callback); ready.Set();
        }
    }
    public DesktopMenuEvent[] Drain() {
        lock(sync) { var result=events.ToArray(); events.Clear(); return result; }
    }
    public void Dispose() {
        stop=true;
        if(!worker.Join(5000)) throw new InvalidOperationException("MENU_OBSERVER_STOP_TIMEOUT");
        ready.Dispose();
    }
}
'@
}

function Get-DesktopEvidenceState { [ProductDesktopEvidence]::Snapshot() }

function Assert-DedicatedDesktop([int]$ExpectedSessionId, $Baseline = $null) {
    if ($ExpectedSessionId -le 0) { throw 'DEDICATED_DESKTOP_SESSION_REQUIRED' }
    $state = Get-DesktopEvidenceState
    if ($state.session_id -ne $ExpectedSessionId) { throw 'DEDICATED_DESKTOP_SESSION_MISMATCH' }
    if (!$state.interactive -or !$state.default_input_desktop -or !$state.same_input_desktop) {
        throw 'DEDICATED_DESKTOP_NOT_INTERACTIVE'
    }
    if (!$state.input_available) { throw 'DEDICATED_DESKTOP_INPUT_EVIDENCE_UNAVAILABLE' }
    if ($null -ne $Baseline -and $state.last_input_tick -ne $Baseline.last_input_tick) {
        throw 'DEDICATED_DESKTOP_INPUT_ACTIVITY'
    }
    return $state
}

function Start-MenuObserver([int]$ProcessId) { [ProductMenuObserver]::new($ProcessId) }

function Save-MenuObservation($Observer, [string]$AttemptId) {
    foreach ($event in $Observer.Drain()) {
        $row = [ordered]@{schema=1;run_id=$script:runId;cycle=$script:cycle;
            operation_id=$script:operation;attempt_id=$AttemptId;product_pid=$script:child.Id;
            source='win32_read_only';event=$event}
        [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-menu-events.jsonl'),
            (($row | ConvertTo-Json -Depth 5 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
    }
    if (!$Observer.Healthy -or $Observer.LostCount) { throw 'MENU_OBSERVER_EVIDENCE_INCOMPLETE' }
}

function Write-MenuPhase([string]$AttemptId,[string]$Phase) {
    $row=[ordered]@{schema=1;run_id=$script:runId;cycle=$script:cycle;operation_id=$script:operation;
        attempt_id=$AttemptId;product_pid=$script:child.Id;source='uia_driver';phase=$Phase;
        desktop=(Get-DesktopEvidenceState)}
    [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-menu-events.jsonl'),
        (($row | ConvertTo-Json -Depth 4 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
}

function Invoke-AccountTelemetry([int]$Seconds=45) {
    $attempt=[guid]::NewGuid().ToString('N')
    $observer=Start-MenuObserver $script:child.Id
    try {
        Write-MenuPhase $attempt 'open_requested'
        Invoke 'mainAccountMenu'
        $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
        do {
            Save-MenuObservation $observer $attempt
            if ($observer.PopupClosed) { throw 'ACCOUNT_MENU_CLOSED_BEFORE_SELECTION' }
            Sample-Resource 'active'
            try {
                $node=Find-Node 'mainPostMeetingTelemetry' ([Windows.Automation.ControlType]::MenuItem) -Optional
                if ($node) {
                    $pattern=Require-Pattern $node ([Windows.Automation.InvokePattern]::Pattern)
                    Save-MenuObservation $observer $attempt
                    if ($observer.PopupClosed) { throw 'ACCOUNT_MENU_CLOSED_BEFORE_SELECTION' }
                    Write-MenuPhase $attempt 'selection_requested'
                    $pattern.Invoke()
                    Write-MenuPhase $attempt 'selection_submitted'
                    return
                }
            } catch [Windows.Automation.ElementNotAvailableException] {
                Save-MenuObservation $observer $attempt
                if ($observer.PopupClosed) { throw 'ACCOUNT_MENU_CLOSED_BEFORE_SELECTION' }
                throw 'ACCOUNT_MENU_ELEMENT_UNAVAILABLE'
            }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($observer.PopupSeen) { throw 'ACCOUNT_MENU_ITEM_NOT_AVAILABLE' }
        throw 'ACCOUNT_MENU_NOT_OBSERVED'
    } catch {
        Write-MenuPhase $attempt $_.Exception.Message
        throw
    } finally {
        $observer.Dispose()
        Save-MenuObservation $observer $attempt
    }
}
