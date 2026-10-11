"""Exercises the in-app updater end to end against the real GitHub release.

Copies the given AudioCapture.exe into a fresh temp folder, launches it there without focus,
presses Check for Updates, reads the message box and, if a newer build is offered, answers Yes.
It then waits for the swap and restart, checks that the file in the temp folder now matches the
signed SHA-256 from the release's build-info.json, presses Check for Updates on the new copy and
expects "Up to Date". The user's saved settings are restored afterwards.

usage: python -I tools/update_smoke.py <AudioCapture.exe> [--expect available|uptodate|older]
exit code 0 when the flow behaved as expected.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gui_smoke  # noqa: E402  (our own helpers: window lookup, dialogs, settings guard)

IDC_UPDATE_BTN = 1042
IDC_STATUS_TEXT = 1008
IDYES = 6
MANIFEST_URL = 'https://github.com/matalvernaz/AudioCapture/releases/download/latest/build-info.json'

user32 = gui_smoke.user32
kernel32 = gui_smoke.kernel32
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
kernel32.QueryFullProcessImageNameW.restype = wt.BOOL


def sha256_of(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def dialog_text(dlg):
    texts = []

    @gui_smoke.WNDENUMPROC
    def on_child(hwnd, _):
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        if name.value == 'Static':
            t = gui_smoke.window_text(hwnd)
            if t:
                texts.append(t)
        return True

    user32.EnumChildWindows(dlg, on_child, 0)
    return ' | '.join(texts)


def wait_for_dialog(pid, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        dialogs = gui_smoke.windows_of(pid, '#32770')
        if dialogs:
            return dialogs[0]
        time.sleep(0.25)
    return None


def process_path(pid):
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ''
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    ok = kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size))
    kernel32.CloseHandle(handle)
    return buf.value if ok else ''


def find_instance(exe_path, exclude_pid, timeout):
    """Returns (pid, hwnd) of a window belonging to a process running exe_path."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        seen = set()

        @gui_smoke.WNDENUMPROC
        def on_window(hwnd, _):
            owner = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value != exclude_pid and owner.value not in seen:
                seen.add(owner.value)
            return True

        user32.EnumWindows(on_window, 0)
        for pid in seen:
            if process_path(pid).lower() == exe_path.lower():
                for hwnd in gui_smoke.windows_of(pid):
                    if user32.GetDlgItem(hwnd, IDC_UPDATE_BTN):
                        return pid, hwnd
        time.sleep(0.5)
    return None, None


def launch(exe_path):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
    app = subprocess.Popen([exe_path], startupinfo=startup, cwd=os.path.dirname(exe_path))
    hmain = None
    for _ in range(100):
        time.sleep(0.1)
        for hwnd in gui_smoke.windows_of(app.pid):
            if user32.GetDlgItem(hwnd, IDC_UPDATE_BTN):
                hmain = hwnd
        if hmain:
            break
    return app, hmain


def press_check(hmain):
    hbutton = user32.GetDlgItem(hmain, IDC_UPDATE_BTN)
    gui_smoke.click(hmain, hbutton, IDC_UPDATE_BTN)


def close_window(hwnd):
    user32.PostMessageW(hwnd, gui_smoke.WM_CLOSE, 0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('--expect', choices=['available', 'uptodate', 'older'], default='available')
    args = ap.parse_args()

    manifest = json.loads(urllib.request.urlopen(MANIFEST_URL, timeout=30).read().decode('ascii'))
    print(f"release: commit {manifest['commit'][:7]} from {manifest['commit_date']}, exe sha {manifest['exe']['sha256'][:12]}...")

    workdir = tempfile.mkdtemp(prefix='audiocapture-update-')
    exe_path = os.path.join(workdir, 'AudioCapture.exe')
    shutil.copy2(args.exe, exe_path)
    print(f'running copy: {exe_path} sha {sha256_of(exe_path)[:12]}...')
    guard = gui_smoke.SettingsGuard()
    failures = 0

    try:
        app, hmain = launch(exe_path)
        if not hmain:
            raise SystemExit('main window not found')
        press_check(hmain)
        dlg = wait_for_dialog(app.pid, 60)
        if not dlg:
            raise SystemExit('no message box within 60 s')
        title = gui_smoke.window_text(dlg)
        print(f"dialog '{title}': {dialog_text(dlg)}")

        expected_title = {'available': 'Update Available', 'uptodate': 'Up to Date', 'older': 'No Newer Build'}[args.expect]
        if title != expected_title:
            failures += 1
            print(f"FAIL expected '{expected_title}'")
            user32.PostMessageW(dlg, gui_smoke.WM_CLOSE, 0, 0)
            time.sleep(0.5)
            close_window(hmain)
            app.wait(timeout=15)
            return failures

        if title != 'Update Available':
            user32.PostMessageW(dlg, gui_smoke.WM_CLOSE, 0, 0)
            time.sleep(0.5)
            close_window(hmain)
            app.wait(timeout=15)
            print('pass  outcome as expected')
            return failures

        # answer Yes and watch the swap
        user32.PostMessageW(dlg, gui_smoke.WM_COMMAND, IDYES, 0)
        try:
            app.wait(timeout=120)
            print(f'old instance exited with code {app.returncode}')
        except subprocess.TimeoutExpired:
            failures += 1
            print('FAIL old instance did not exit')
            for d in gui_smoke.windows_of(app.pid, '#32770'):
                print(f"  dialog '{gui_smoke.window_text(d)}': {dialog_text(d)}")
            app.kill()
            return failures

        new_sha = sha256_of(exe_path)
        old_exists = os.path.exists(os.path.join(workdir, 'AudioCapture.old.exe'))
        print(f'file now sha {new_sha[:12]}..., old copy kept: {old_exists}')
        if new_sha != manifest['exe']['sha256']:
            failures += 1
            print('FAIL installed file does not match the signed SHA-256')
        else:
            print('pass  installed file matches the signed SHA-256')

        pid, hwnd = find_instance(exe_path, app.pid, 30)
        if not hwnd:
            failures += 1
            print('FAIL the new build did not start')
            return failures
        print(f'new instance PID {pid} is running')
        press_check(hwnd)
        dlg = wait_for_dialog(pid, 60)
        title = gui_smoke.window_text(dlg) if dlg else '(none)'
        print(f"dialog '{title}': {dialog_text(dlg) if dlg else ''}")
        if title != 'Up to Date':
            failures += 1
            print("FAIL expected 'Up to Date' from the new build")
        else:
            print('pass  new build reports up to date')
        if dlg:
            user32.PostMessageW(dlg, gui_smoke.WM_CLOSE, 0, 0)
            time.sleep(0.5)
        close_window(hwnd)
        for _ in range(60):
            if not process_path(pid):
                break
            time.sleep(0.5)
        return failures
    finally:
        guard.restore()
        for _ in range(10):
            try:
                shutil.rmtree(workdir)
                break
            except OSError:
                time.sleep(1)


if __name__ == '__main__':
    code = main()
    print(f'{code} failure(s)')
    sys.exit(code)
