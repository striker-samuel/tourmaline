#!/usr/bin/env python3
"""Original CC0 arcade theme and SFX. Pure stdlib; emits editable MIDI + WAV.
No soundfont, runtime synth, network or third-party audio downloads needed.
"""
import argparse
from array import array
import math
from pathlib import Path
import random
import struct
import tempfile
import wave

RATE = 22050
BPM = 120
BEATS = 32
SECONDS = BEATS * 60.0 / BPM
TICKS = 480


def notes():
    # Original 8-bar D-minor-flavoured arcade ostinato, 120 BPM.
    lead = [
        [(74, 0, .42), (77, .5, .42), (81, 1, .66),
         (79, 1.75, .42), (77, 2.25, .6), (74, 3, .65)],
        [(72, 0, .42), (76, .5, .42), (79, 1, .75),
         (76, 2, .4), (74, 2.5, .4), (72, 3, .7)],
        [(70, 0, .6), (74, .75, .4), (77, 1.25, .5),
         (81, 2, .45), (77, 2.5, .45), (74, 3, .7)],
        [(69, 0, .45), (72, .5, .45), (76, 1, .5),
         (79, 1.75, .4), (76, 2.25, .4), (72, 3, .7)],
    ]
    roots = [50, 48, 46, 45, 50, 53, 46, 45]
    out = []
    for bar in range(8):
        base = bar * 4
        for pitch, offset, duration in lead[bar % len(lead)]:
            out.append((base + offset, duration, pitch, 0, .23))
        for beat in (0, 1, 2, 3):
            out.append((base + beat, .75, roots[bar] +
                        (12 if beat == 2 else 0), 1, .16))
        for step in range(8):
            out.append((base + step*.5, .31,
                        roots[bar]+24+[0, 7, 12, 7][step % 4], 2, .055))
    return out


def vlq(number):
    result = [number & 127]
    number >>= 7
    while number:
        result.insert(0, 128 | (number & 127))
        number >>= 7
    return bytes(result)


def write_midi(path, score):
    events = [(0, -2, b'\xff\x51\x03\x07\xa1\x20'),
              (0, -1, bytes((0xc0, 80))),
              (0, -1, bytes((0xc1, 38))),
              (0, -1, bytes((0xc2, 10)))]
    for start, duration, pitch, voice, strength in score:
        tick = round(start*TICKS)
        end = round((start+duration)*TICKS)
        vel = max(1, min(127, round(strength*320)))
        events.append((tick, 1, bytes((0x90+voice, pitch, vel))))
        events.append((end, 0, bytes((0x80+voice, pitch, 0))))
    events.sort(key=lambda e: (e[0], e[1]))
    track = bytearray()
    previous = 0
    for tick, _, payload in events:
        track += vlq(tick-previous) + payload
        previous = tick
    track += vlq(BEATS*TICKS-previous) + b'\xff\x2f\x00'
    path.write_bytes(b'MThd'+struct.pack('>IHHH',6,0,1,TICKS)
                     +b'MTrk'+struct.pack('>I',len(track))+track)


def tone(freq, time, voice):
    phase = (freq*time) % 1.0
    if voice == 0:
        return .82 if phase < .24 else -.82
    if voice == 1:
        return 4.0*abs(phase-.5)-1.0
    return math.sin(2*math.pi*freq*time)


def music_samples(score):
    length = round(SECONDS*RATE)
    buf = array('f', [0.0]) * length
    for beat, duration, pitch, voice, gain in score:
        begin = round(beat*60.0/BPM*RATE)
        count = min(round(duration*60.0/BPM*RATE), length-begin)
        freq = 440.0 * (2.0 ** ((pitch-69)/12.0))
        for i in range(count):
            age = i/RATE
            envelope = min(1.0, age/.008, (count-i)/(RATE*.055))
            buf[begin+i] += tone(freq,age,voice)*gain*envelope
    # Music loop uses the exact MIDI length; fade edge to avoid discontinuity.
    ramp = int(.04*RATE)
    for i in range(ramp):
        buf[i] *= i/ramp
        buf[-1-i] *= i/ramp
    return buf


def sound_samples(kind):
    rng = random.Random(0xA57E001)
    duration = {'laser':.22, 'hit':.48, 'wave':.65, 'gameover':.9}[kind]
    out = array('f', [0.0])*round(duration*RATE)
    for i in range(len(out)):
        t = i/RATE
        a = t/duration
        if kind == 'laser':
            freq = 1150*(1-a)**2+120
            sample = math.sin(2*math.pi*(120*t + 1030*(t-t*t/(3*duration))))
            sample += .15*math.sin(2*math.pi*freq*t)
            envelope = (1-a)**2 * min(1.0,t/.005)
        elif kind == 'hit':
            sample = .85*(rng.random()*2-1)+.25*math.sin(2*math.pi*90*t)
            envelope = (1-a)**3 * min(1.0,t/.004)
        elif kind == 'wave':
            pitch = [523,659,784,1046][min(3,int(a*4))]
            sample = math.sin(2*math.pi*pitch*t)
            envelope = min(1.0,t/.01)*(1-a)*.7
        else:
            pitch = [494,392,294,220][min(3,int(a*4))]
            sample = .7*math.sin(2*math.pi*pitch*t)
            envelope = min(1.0,t/.01)*(1-a)
        out[i] = sample * envelope * .6
    return out


def save_wav(path, data):
    peak = max(.0001, max(abs(x) for x in data))
    scale = min(1.0, .89/peak)
    with wave.open(str(path), 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(struct.pack('<%dh'%len(data), *(
            int(max(-1.0,min(1.0,x*scale))*32760) for x in data)))


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    score = notes()
    write_midi(directory/'music.mid', score)
    save_wav(directory/'music.wav', music_samples(score))
    for name in ('laser', 'hit', 'wave', 'gameover'):
        save_wav(directory/(name+'.wav'), sound_samples(name))


def self_test():
    with tempfile.TemporaryDirectory() as temp:
        target = Path(temp)
        generate(target)
        midi = (target/'music.mid').read_bytes()
        assert midi.startswith(b'MThd') and b'MTrk' in midi
        for name in ('music','laser','hit','wave','gameover'):
            with wave.open(str(target/(name+'.wav')), 'rb') as wav:
                assert wav.getnchannels() == 1 and wav.getsampwidth() == 2
                assert wav.getframerate() == RATE and wav.getnframes() > RATE//10
        assert (target/'music.wav').stat().st_size > 2*RATE*10
    print('Asteroids generated MIDI/WAV smoke tests passed')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path)
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
    else:
        if not args.output:
            ap.error('--output is required')
        generate(args.output)
