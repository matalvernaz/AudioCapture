"""Drives AudioCapture's real window with plain Win32 messages to check that starting, stopping
and starting again a capture of one process keeps capturing that process.

The app is launched minimized and never activated, nothing is typed into the foreground window,
and every click is a WM_COMMAND posted straight to the app, so a screen reader user can run this
while doing something else. Works without UI Automation (which exposes this app's controls as
bare panes on some machines).

usage: python -I tools/gui_smoke.py <AudioCapture.exe> <pid-to-capture> [--rounds 2] [--seconds 2]
                                     [--outdir DIR] [--monitor off|file|only]
Prints the Active Recordings rows after every Start and Stop, plus the text of any message box,
and lists the files written. Pair with tools/play_tone.ps1 and tools/check_tones.py.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import tempfile
import time

user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

# control ids, from include/resource.h
IDC_PROCESS_LIST = 1001
IDC_START_BTN = 1003
IDC_OUTPUT_PATH = 1006
IDC_STATUS_TEXT = 1008
IDC_RECORDING_LIST = 1009
IDC_PASSTHROUGH_CHECKBOX = 1021
IDC_MONITOR_ONLY_CHECKBOX = 1024

WM_CLOSE = 0x0010
WM_GETTEXT = 0x000D
WM_SETTEXT = 0x000C
WM_COMMAND = 0x0111
BN_CLICKED = 0
BM_SETCHECK = 0x00F1
LVM_FIRST = 0x1000
LVM_GETITEMCOUNT = LVM_FIRST + 4
LVM_ENSUREVISIBLE = LVM_FIRST + 19
LVM_SETITEMSTATE = LVM_FIRST + 43
LVM_GETITEMTEXTW = LVM_FIRST + 115
LVIS_FOCUSED = 0x0001
LVIS_SELECTED = 0x0002
LVIS_STATEIMAGEMASK = 0xF000

for fn, res, args in [
    (user32.SendMessageW, ctypes.c_ssize_t, [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_ssize_t]),
    (user32.PostMessageW, wt.BOOL, [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_ssize_t]),
    (user32.GetDlgItem, wt.HWND, [wt.HWND, ctypes.c_int]),
    (user32.GetWindowThreadProcessId, wt.DWORD, [wt.HWND, ctypes.POINTER(wt.DWORD)]),
    (user32.GetClassNameW, ctypes.c_int, [wt.HWND, wt.LPWSTR, ctypes.c_int]),
    (user32.IsWindowVisible, wt.BOOL, [wt.HWND]),
    (kernel32.OpenProcess, wt.HANDLE, [wt.DWORD, wt.BOOL, wt.DWORD]),
    (kernel32.CloseHandle, wt.BOOL, [wt.HANDLE]),
    (kernel32.VirtualAllocEx, ctypes.c_void_p, [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]),
    (kernel32.VirtualFreeEx, wt.BOOL, [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]),
    (kernel32.WriteProcessMemory, wt.BOOL, [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]),
    (kernel32.ReadProcessMemory, wt.BOOL, [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]),
]:
    fn.restype = res
    fn.argtypes = args

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
user32.EnumChildWindows.argtypes = [wt.HWND, WNDENUMPROC, wt.LPARAM]


class LVITEMW(ctypes.Structure):
    _fields_ = [
        ('mask', wt.UINT), ('iItem', ctypes.c_int), ('iSubItem', ctypes.c_int),
        ('state', wt.UINT), ('stateMask', wt.UINT), ('pszText', ctypes.c_void_p),
        ('cchTextMax', ctypes.c_int), ('lParam', ctypes.c_ssize_t), ('iIndent', ctypes.c_int),
        ('iGroupId', ctypes.c_int), ('cColumns', wt.UINT), ('puColumns', ctypes.c_void_p),
        ('piColFmt', ctypes.c_void_p), ('iGroup', ctypes.c_int),
    ]


class RemoteMemory:
    """Scratch memory inside the app's process, needed for list-view messages that take pointers."""

    def __init__(self, pid):
        self.handle = kernel32.OpenProcess(0x0008 | 0x0010 | 0x0020, False, pid)  # vm operation/read/write
        if not self.handle:
            raise OSError(ctypes.get_last_error(), 'OpenProcess failed')

    def alloc(self, size):
        addr = kernel32.VirtualAllocEx(self.handle, None, size, 0x3000, 0x04)  # commit+reserve, read/write
        if not addr:
            raise OSError(ctypes.get_last_error(), 'VirtualAllocEx failed')
        return addr

    def write(self, addr, data):
        done = ctypes.c_size_t()
        kernel32.WriteProcessMemory(self.handle, addr, data, len(data), ctypes.byref(done))

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        done = ctypes.c_size_t()
        kernel32.ReadProcessMemory(self.handle, addr, buf, size, ctypes.byref(done))
        return buf.raw[:done.value]

    def free(self, addr):
        kernel32.VirtualFreeEx(self.handle, addr, 0, 0x8000)  # release

    def close(self):
        kernel32.CloseHandle(self.handle)


def windows_of(pid, class_name=None):
    found = []

    @WNDENUMPROC
    def on_window(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            name = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, name, 256)
            if class_name is None or name.value == class_name:
                found.append(hwnd)
        return True

    user32.EnumWindows(on_window, 0)
    return found


def window_text(hwnd):
    buf = ctypes.create_unicode_buffer(4096)
    user32.SendMessageW(hwnd, WM_GETTEXT, 4096, ctypes.addressof(buf))
    return buf.value


def set_text(hwnd, text):
    buf = ctypes.create_unicode_buffer(text)
    user32.SendMessageW(hwnd, WM_SETTEXT, 0, ctypes.addressof(buf))


def lv_count(hlist):
    return user32.SendMessageW(hlist, LVM_GETITEMCOUNT, 0, 0)


def lv_text(mem, hlist, row, col):
    text_buf = mem.alloc(2048)
    item = LVITEMW(iSubItem=col, pszText=text_buf, cchTextMax=1024)
    item_buf = mem.alloc(ctypes.sizeof(item))
    mem.write(item_buf, bytes(item))
    chars = user32.SendMessageW(hlist, LVM_GETITEMTEXTW, row, item_buf)
    raw = mem.read(text_buf, 2 * max(chars, 0))
    mem.free(item_buf)
    mem.free(text_buf)
    return raw.decode('utf-16-le', 'replace')


def lv_set_state(mem, hlist, row, state, mask):
    item = LVITEMW(state=state, stateMask=mask)
    item_buf = mem.alloc(ctypes.sizeof(item))
    mem.write(item_buf, bytes(item))
    user32.SendMessageW(hlist, LVM_SETITEMSTATE, row, item_buf)
    mem.free(item_buf)


def recordings(mem, hlist):
    rows = []
    for row in range(lv_count(hlist)):
        rows.append((lv_text(mem, hlist, row, 0), lv_text(mem, hlist, row, 1), lv_text(mem, hlist, row, 2)))
    return rows


def drain_dialogs(pid):
    """Print and close any message box the app has open."""
    for dlg in windows_of(pid, '#32770'):
        texts = []

        @WNDENUMPROC
        def on_child(hwnd, _):
            name = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, name, 64)
            if name.value == 'Static':
                t = window_text(hwnd)
                if t:
                    texts.append(t)
            return True

        user32.EnumChildWindows(dlg, on_child, 0)
        print(f"  DIALOG '{window_text(dlg)}': {' | '.join(texts)}")
        user32.PostMessageW(dlg, WM_CLOSE, 0, 0)
        time.sleep(0.3)


def click(hmain, hbutton, control_id):
    user32.PostMessageW(hmain, WM_COMMAND, (BN_CLICKED << 16) | control_id, hbutton)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('pid', type=int)
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--seconds', type=float, default=2.0)
    ap.add_argument('--outdir', default=os.path.join(tempfile.gettempdir(), 'audiocapture-guismoke'))
    ap.add_argument('--monitor', choices=['off', 'file', 'only'], default='off',
                    help="'file' ticks Monitor audio (record and pass through), 'only' also ticks Monitor only")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # SW_SHOWNOACTIVATE: shown but never given focus (minimized would send it to the tray)
    app = subprocess.Popen([args.exe], startupinfo=startup)

    hmain = None
    for _ in range(100):
        time.sleep(0.1)
        for hwnd in windows_of(app.pid):
            if user32.GetDlgItem(hwnd, IDC_PROCESS_LIST):
                hmain = hwnd
        if hmain:
            break
    if not hmain:
        app.kill()
        raise SystemExit('main window not found')

    mem = RemoteMemory(app.pid)
    hlist = user32.GetDlgItem(hmain, IDC_PROCESS_LIST)
    hstart = user32.GetDlgItem(hmain, IDC_START_BTN)
    hrec = user32.GetDlgItem(hmain, IDC_RECORDING_LIST)
    hstatus = user32.GetDlgItem(hmain, IDC_STATUS_TEXT)
    for _ in range(100):  # the first process-list refresh can take a moment
        if lv_count(hlist) > 1:
            break
        time.sleep(0.1)
    print(f'window {hmain:#x}, {lv_count(hlist)} rows in the process list')

    set_text(user32.GetDlgItem(hmain, IDC_OUTPUT_PATH), args.outdir)
    if args.monitor != 'off':
        user32.SendMessageW(user32.GetDlgItem(hmain, IDC_PASSTHROUGH_CHECKBOX), BM_SETCHECK, 1, 0)
        user32.SendMessageW(user32.GetDlgItem(hmain, IDC_MONITOR_ONLY_CHECKBOX), BM_SETCHECK, 1 if args.monitor == 'only' else 0, 0)
        print(f'monitor mode: {args.monitor}')

    target_row = None
    for row in range(lv_count(hlist)):
        if lv_text(mem, hlist, row, 1).strip() == str(args.pid):
            target_row = row
            break
    if target_row is None:
        user32.PostMessageW(hmain, WM_CLOSE, 0, 0)
        raise SystemExit(f'pid {args.pid} is not in the process list')
    name = lv_text(mem, hlist, target_row, 0)
    lv_set_state(mem, hlist, target_row, 2 << 12, LVIS_STATEIMAGEMASK)  # tick the checkbox
    lv_set_state(mem, hlist, target_row, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED)
    user32.SendMessageW(hlist, LVM_ENSUREVISIBLE, target_row, 0)
    print(f'ticked row {target_row}: {name} (PID {args.pid})')

    before = set(os.listdir(args.outdir))
    for round_no in range(1, args.rounds + 1):
        print(f"--- round {round_no}: button '{window_text(hstart)}' -> start")
        click(hmain, hstart, IDC_START_BTN)
        time.sleep(1.0)
        drain_dialogs(app.pid)
        print(f'  status: {window_text(hstatus)}')
        for r in recordings(mem, hrec):
            print(f'  recording: {r[0]} PID {r[1]} -> {r[2]}')
        time.sleep(args.seconds)
        print(f"  button '{window_text(hstart)}' -> stop")
        click(hmain, hstart, IDC_START_BTN)
        time.sleep(1.0)
        drain_dialogs(app.pid)
        print(f'  after stop: {len(recordings(mem, hrec))} active recording(s); status: {window_text(hstatus)}')

    user32.PostMessageW(hmain, WM_CLOSE, 0, 0)
    try:
        app.wait(timeout=10)
    except subprocess.TimeoutExpired:
        app.kill()
    mem.close()
    for f in sorted(set(os.listdir(args.outdir)) - before):
        print(f'file: {os.path.join(args.outdir, f)}')


if __name__ == '__main__':
    main()
