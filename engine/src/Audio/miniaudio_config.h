// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE ONE miniaudio CONFIGURATION (vendored 0.11.26, external/miniaudio).
// miniaudio is a single-header library whose FEATURE MACROS change the
// layout of its public types — every TU in this tree that includes
// miniaudio.h MUST go through this wrapper so the whole build sees one
// consistent configuration (the same discipline as the impl TU: engine/
// src/Audio/miniaudio_impl.cpp defines MINIAUDIO_IMPLEMENTATION and
// includes THIS header, never miniaudio.h directly).
//
// Feature configuration — the OWNERSHIP rule, not miniaudio defaults:
//   MA_NO_DEVICE_IO        SDL3 owns the audio device (AudioEngine's
//                          SDL_AudioStream push path); miniaudio runs
//                          its engine + node graph DEVICE-LESS and is
//                          rendered manually per chunk
//                          (ma_engine_read_pcm_frames).
//   MA_NO_RESOURCE_MANAGER content identity/loading is TOURMALINE's
//                          (paks + the content registry); sounds are
//                          created from in-memory data sources only
//                          (ma_audio_buffer over decoded clip bytes).
//   MA_NO_DECODING         WAV decoding is SDL3's (SDL_LoadWAV +
//                          SDL_CreateAudioStream conversion, the
//                          existing tested path).
//   MA_NO_ENCODING         No audio encoding anywhere in the engine.
//
// miniaudio's role in this tree is EXACTLY the DSP + graph execution
// core (docs/audio_graph.md): the node graph, the per-voice
// source/pan/pitch/resample machinery, the biquad + delay effect
// nodes, and the sound-group submix buses.
#define MA_NO_DEVICE_IO
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_DECODING
#define MA_NO_ENCODING

#include "miniaudio.h"
