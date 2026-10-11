"""Reports how loud given frequencies are in a WAV file, in dBFS.

Used with tools/play_tone.ps1 to prove a per-process recording contains the captured
program's tone and not another program's. Reads 16-bit PCM and 32-bit float WAV files,
including the WAVE_FORMAT_EXTENSIBLE ones the app writes, without any third-party modules.

usage: python -I tools/check_tones.py recording.wav 440 1320
exit code 0 always; read the printed levels (a tone that is really there sits far above -60 dBFS).
"""

import math
import struct
import sys


def read_wav(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit(f'{path}: not a RIFF/WAVE file')
    fmt = None
    pcm = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk_id == b'fmt ':
            fmt = body
        elif chunk_id == b'data':
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise SystemExit(f'{path}: missing fmt or data chunk')

    tag, channels, rate, _, block_align, bits = struct.unpack('<HHIIHH', fmt[:16])
    if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: the real tag is the first word of the SubFormat GUID
        tag = struct.unpack('<H', fmt[24:26])[0]
    frames = len(pcm) // block_align
    if tag == 3 and bits == 32:
        samples = struct.unpack(f'<{frames * channels}f', pcm[:frames * block_align])
        scale = 1.0
    elif tag == 1 and bits == 16:
        samples = struct.unpack(f'<{frames * channels}h', pcm[:frames * block_align])
        scale = 32768.0
    else:
        raise SystemExit(f'{path}: unsupported format tag {tag} with {bits} bits')
    mono = [samples[i * channels] / scale for i in range(frames)]  # first channel is enough
    return rate, channels, bits, mono


def tone_amplitude(x, rate, freq):
    """Goertzel amplitude estimate of one frequency over the whole signal."""
    k = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2 = s1
        s1 = s0
    power = s1 * s1 + s2 * s2 - k * s1 * s2
    return math.sqrt(max(power, 0.0)) * 2.0 / len(x)


def dbfs(amplitude):
    return 20.0 * math.log10(amplitude) if amplitude > 1e-9 else -180.0


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    freqs = [float(f) for f in sys.argv[2:]]
    rate, channels, bits, mono = read_wav(path)
    seconds = len(mono) / rate if rate else 0.0
    rms = math.sqrt(sum(v * v for v in mono) / len(mono)) if mono else 0.0
    print(f'{path}: {rate} Hz, {channels} ch, {bits} bit, {seconds:.2f} s, rms {dbfs(rms):.1f} dBFS')
    if not mono:
        return
    for freq in freqs:
        level = dbfs(tone_amplitude(mono, rate, freq))
        verdict = 'present' if level > -40.0 else ('absent' if level < -60.0 else 'faint')
        print(f'  {freq:7.1f} Hz: {level:7.1f} dBFS  {verdict}')


if __name__ == '__main__':
    main()
