#include "snd_midi_fm.h"

#include <limits.h>
#include <string.h>

/* A mild organ-like patch: useful as a deterministic fallback, not an attempt
 * to stand in for General MIDI or Doom's GENMIDI bank. */
const snd_fm_instrument_t snd_fm_default_instrument = {
    {{{256u, 176, SND_WAVE_SINE, {2u, 140u, 180u, 180}},
      {256u, 256, SND_WAVE_SINE, {2u, 180u, 220u, 210}},
      36u, 4u, SND_FM_ALGORITHM_FM, 0, 0},
     {{256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
      {256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
      0u, 0u, SND_FM_ALGORITHM_FM, 0, 0}},
    1u, SND_FM_FIXED_NOTE_NONE, {0u, 0u}};

/* A short noise-rich fallback for MIDI channel 10 when no percussion bank is
 * installed. Real Doom playback will eventually supply GENMIDI-derived
 * percussion instruments instead. */
const snd_fm_instrument_t snd_fm_default_percussion_instrument = {
    {{{512u, 128, SND_WAVE_NOISE, {0u, 20u, 70u, 96}},
      {256u, 256, SND_WAVE_NOISE, {0u, 25u, 90u, 64}},
      10u, 0u, SND_FM_ALGORITHM_ADDITIVE, 0, 0},
     {{256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
      {256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
      0u, 0u, SND_FM_ALGORITHM_FM, 0, 0}},
    1u, 60u, {0u, 0u}};

static void snd_midi_fm_channel_default(snd_midi_fm_channel_t SND_PTR *c) {
  if (!c)
    return;
  c->pitch_bend = (uint16_t)SND_MIDI_PITCH_CENTER;
  c->program = 0u;
  c->volume = 127u;
  c->expression = 127u;
  c->pan = 64u;
  c->sustain = 0u;
}

static void snd_midi_fm_clear_runtime(snd_midi_fm_t SND_PTR *fm) {
  int i;
  if (!fm)
    return;
  for (i = 0; i < SND_MIDI_CHANNELS; ++i)
    snd_midi_fm_channel_default(&fm->channel[i]);
  memset(fm->voices, 0, sizeof(fm->voices));
  fm->event_head = 0;
  fm->event_count = 0;
  fm->next_serial = 0uL;
  fm->midi_notes_started = 0uL;
  fm->fm_voices_started = 0uL;
  fm->fm_voices_released = 0uL;
  fm->voices_stolen = 0uL;
  fm->dropped_events = 0uL;
}

void snd_midi_fm_init(snd_midi_fm_t SND_PTR *fm) {
  if (!fm)
    return;
  memset(fm, 0, sizeof(*fm));
  fm->bank.fallback = &snd_fm_default_instrument;
  fm->bank.percussion_fallback = &snd_fm_default_percussion_instrument;
  fm->voice_limit = 9; /* OPL2-shaped default; OPL3-style callers may select 18. */
  snd_midi_fm_clear_runtime(fm);
}

void snd_midi_fm_reset(snd_midi_fm_t SND_PTR *fm) {
  snd_midi_fm_bank_t bank;
  int voice_limit;
  if (!fm)
    return;
  bank = fm->bank;
  voice_limit = fm->voice_limit;
  snd_midi_fm_clear_runtime(fm);
  fm->bank = bank;
  fm->voice_limit = voice_limit;
}

void snd_midi_fm_set_bank(snd_midi_fm_t SND_PTR *fm,
                          const snd_midi_fm_bank_t SND_PTR *bank) {
  if (!fm)
    return;
  if (bank) {
    fm->bank = *bank;
  } else {
    memset(&fm->bank, 0, sizeof(fm->bank));
    fm->bank.fallback = &snd_fm_default_instrument;
    fm->bank.percussion_fallback = &snd_fm_default_percussion_instrument;
  }
  if (!fm->bank.fallback)
    fm->bank.fallback = &snd_fm_default_instrument;
  if (!fm->bank.percussion_fallback)
    fm->bank.percussion_fallback = &snd_fm_default_percussion_instrument;
}

void snd_midi_fm_set_voice_limit(snd_midi_fm_t SND_PTR *fm, int voices) {
  int i;
  if (!fm)
    return;
  if (voices < 1)
    voices = 1;
  if (voices > SND_MIDI_FM_MAX_VOICES)
    voices = SND_MIDI_FM_MAX_VOICES;
  fm->voice_limit = voices;
  for (i = voices; i < SND_MIDI_FM_MAX_VOICES; ++i)
    fm->voices[i].active = 0u;
}

void snd_midi_fm_bind(snd_midi_fm_t SND_PTR *fm, snd_midi_t SND_PTR *midi) {
  int i;
  if (!fm || !midi)
    return;

  /* Binding after a caller has already configured MIDI state is well-defined:
   * future notes begin with the program/controllers that snd_midi already
   * knows about rather than silently reverting to backend defaults. */
  for (i = 0; i < SND_MIDI_CHANNELS; ++i) {
    fm->channel[i].pitch_bend = midi->channel[i].pitch_bend;
    fm->channel[i].program = midi->channel[i].program;
    fm->channel[i].volume = midi->channel[i].volume;
    fm->channel[i].expression = midi->channel[i].expression;
    fm->channel[i].pan = midi->channel[i].pan;
    fm->channel[i].sustain = midi->channel[i].sustain;
  }
  snd_midi_set_sink(midi, snd_midi_fm_midi_emit, fm);
}

static int snd_midi_fm_event_index(const snd_midi_fm_t SND_PTR *fm, int n) {
  return (fm->event_head + n) % SND_MIDI_FM_QUEUE_EVENTS;
}

int snd_midi_fm_queue_message(snd_midi_fm_t SND_PTR *fm, long frame,
                              const snd_midi_message_t SND_PTR *msg) {
  int idx;
  if (!fm || !msg)
    return 0;
  if (fm->event_count >= SND_MIDI_FM_QUEUE_EVENTS) {
    ++fm->dropped_events;
    return 0;
  }
  if (fm->event_count > 0) {
    int last = snd_midi_fm_event_index(fm, fm->event_count - 1);
    if (frame < fm->events[last].frame) {
      ++fm->dropped_events;
      return 0;
    }
  }
  idx = snd_midi_fm_event_index(fm, fm->event_count);
  fm->events[idx].frame = frame;
  fm->events[idx].msg = *msg;
  ++fm->event_count;
  return 1;
}

void snd_midi_fm_midi_emit(void SND_PTR *user, long frame,
                           const snd_midi_message_t SND_PTR *msg,
                           const snd_midi_channel_t SND_PTR *channel) {
  snd_midi_fm_t SND_PTR *fm = (snd_midi_fm_t SND_PTR *)user;
  (void)channel;
  if (!fm || !msg)
    return;
  (void)snd_midi_fm_queue_message(fm, frame, msg);
}

static snd_midi_fm_event_t SND_PTR *snd_midi_fm_front_event(
    snd_midi_fm_t SND_PTR *fm) {
  if (!fm || fm->event_count <= 0)
    return 0;
  return &fm->events[fm->event_head];
}

static void snd_midi_fm_pop_event(snd_midi_fm_t SND_PTR *fm) {
  if (!fm || fm->event_count <= 0)
    return;
  fm->event_head = (fm->event_head + 1) % SND_MIDI_FM_QUEUE_EVENTS;
  --fm->event_count;
  if (fm->event_count == 0)
    fm->event_head = 0;
}

static const snd_fm_instrument_t SND_PTR *snd_midi_fm_instrument_for(
    const snd_midi_fm_t SND_PTR *fm, int channel, int key) {
  const snd_midi_fm_bank_t SND_PTR *b;
  if (!fm)
    return &snd_fm_default_instrument;
  b = &fm->bank;

  if (channel == (int)SND_MIDI_GM_PERCUSSION_CHANNEL) {
    int idx = key - b->percussion_first_note;
    if (b->percussion && idx >= 0 && idx < b->percussion_count)
      return &b->percussion[idx];
    return b->percussion_fallback ? b->percussion_fallback
                                  : &snd_fm_default_percussion_instrument;
  }

  if (b->programs && b->program_count > 0) {
    int program = (int)fm->channel[channel].program;
    if (program >= 0 && program < b->program_count)
      return &b->programs[program];
  }
  return b->fallback ? b->fallback : &snd_fm_default_instrument;
}

static long snd_midi_fm_ms_to_frames(int rate, uint16_t ms) {
  int64_t n;
  if (rate <= 0 || ms == 0u)
    return 0L;
  n = (int64_t)rate * (int64_t)ms + 999LL;
  n /= 1000LL;
  if (n > LONG_MAX)
    return LONG_MAX;
  return (long)n;
}

static snd_fixed_t snd_midi_fm_hz_step(const snd_mixer_t SND_PTR *m, int hz) {
  int64_t step;
  if (!m || m->rate <= 0 || hz <= 0)
    return 0;
  step = ((int64_t)hz * (int64_t)SND_FIXED_ONE) / (int64_t)m->rate;
  if (step < 0)
    step = 0;
#if SND_FIXED_SHIFT == 16
  if (step > INT32_MAX)
    step = INT32_MAX;
#endif
  return (snd_fixed_t)step;
}

/* MIDI bend range is deliberately fixed at +/-2 semitones for this first
 * backend. Interpolation is performed between the exact note endpoints when a
 * MIDI event is applied, never in the sample loop. */
static snd_fixed_t snd_midi_fm_pitch_step(const snd_mixer_t SND_PTR *m,
                                          int pitch_128, uint16_t bend) {
  int32_t bend_128;
  int32_t pitch;
  int note;
  int frac;
  snd_fixed_t lo, hi;
  int32_t delta;

  /* Default MIDI pitch-bend range for this deliberately small backend is
   * +/-2 semitones. Convert it into the same 1/128-semitone domain used by
   * per-layer detune before interpolating between note endpoints. */
  bend_128 = (int32_t)(((int64_t)((int32_t)bend -
                                  (int32_t)SND_MIDI_PITCH_CENTER) *
                        256LL) /
                       8192LL);
  pitch = (int32_t)pitch_128 + bend_128;
  if (pitch < 0)
    pitch = 0;
  if (pitch > (127 * 128))
    pitch = 127 * 128;

  note = (int)(pitch / 128);
  frac = (int)(pitch % 128);
  lo = snd_midi_fm_hz_step(m, snd_note_hz(note));
  if (note >= 127 || frac == 0)
    return lo;
  hi = snd_midi_fm_hz_step(m, snd_note_hz(note + 1));
  delta = (int32_t)hi - (int32_t)lo;
  return (snd_fixed_t)((int32_t)lo +
                       (int32_t)(((int64_t)delta * frac) / 128LL));
}

static snd_fixed_t snd_midi_fm_apply_multiple(snd_fixed_t step,
                                               uint16_t multiple) {
  int64_t v;
  if (multiple == 0u)
    multiple = 256u;
  v = ((int64_t)step * (int64_t)multiple) >> 8;
#if SND_FIXED_SHIFT == 16
  if (v > INT32_MAX)
    v = INT32_MAX;
#endif
  return (snd_fixed_t)v;
}

static int16_t snd_midi_fm_voice_pitch(
    const snd_fm_instrument_t SND_PTR *instrument,
    const snd_fm_patch_t SND_PTR *patch, int key) {
  int32_t pitch;
  if (!instrument || !patch)
    return (int16_t)(key * 128);
  if (instrument->fixed_note != SND_FM_FIXED_NOTE_NONE)
    pitch = (int32_t)instrument->fixed_note * 128;
  else
    pitch = ((int32_t)key + (int32_t)patch->transpose) * 128;
  pitch += (int32_t)patch->detune_128;
  if (pitch < INT16_MIN)
    pitch = INT16_MIN;
  if (pitch > INT16_MAX)
    pitch = INT16_MAX;
  return (int16_t)pitch;
}

static void snd_midi_fm_update_voice_pitch(snd_midi_fm_voice_t SND_PTR *v,
                                            const snd_midi_fm_t SND_PTR *fm,
                                            const snd_mixer_t SND_PTR *m) {
  snd_fixed_t base;
  if (!v || !v->active || !v->patch || !fm || !m)
    return;
  base = snd_midi_fm_pitch_step(m, (int)v->pitch_128,
                                fm->channel[v->channel].pitch_bend);
  v->mod_step = snd_midi_fm_apply_multiple(base, v->patch->modulator.multiple);
  v->car_step = snd_midi_fm_apply_multiple(base, v->patch->carrier.multiple);
}

static int snd_midi_fm_find_voice(snd_midi_fm_t SND_PTR *fm) {
  int i;
  int oldest = -1;
  unsigned long oldest_serial = ~0uL;

  for (i = 0; i < fm->voice_limit; ++i) {
    if (!fm->voices[i].active)
      return i;
  }

  /* Prefer a voice already in release, then the oldest note. */
  for (i = 0; i < fm->voice_limit; ++i) {
    if (fm->voices[i].release_frame >= 0 && fm->voices[i].serial < oldest_serial) {
      oldest = i;
      oldest_serial = fm->voices[i].serial;
    }
  }
  if (oldest >= 0)
    return oldest;

  oldest_serial = ~0uL;
  for (i = 0; i < fm->voice_limit; ++i) {
    if (fm->voices[i].serial < oldest_serial) {
      oldest = i;
      oldest_serial = fm->voices[i].serial;
    }
  }
  return (oldest >= 0) ? oldest : 0;
}

static void snd_midi_fm_start_layer(
    snd_midi_fm_t SND_PTR *fm, snd_mixer_t SND_PTR *m, long frame,
    int channel, int key, int velocity,
    const snd_fm_instrument_t SND_PTR *instrument,
    const snd_fm_patch_t SND_PTR *patch) {
  snd_midi_fm_voice_t SND_PTR *v;
  int idx;

  if (!fm || !m || !instrument || !patch || velocity <= 0)
    return;

  idx = snd_midi_fm_find_voice(fm);
  v = &fm->voices[idx];
  if (v->active)
    ++fm->voices_stolen;

  memset(v, 0, sizeof(*v));
  v->patch = patch;
  v->channel = (uint8_t)channel;
  v->key = (uint8_t)key;
  v->velocity = (uint8_t)velocity;
  v->pitch_128 = snd_midi_fm_voice_pitch(instrument, patch, key);
  v->start_frame = frame;
  v->release_frame = -1L;
  v->serial = ++fm->next_serial;
  v->active = 1u;
  v->mod_noise = 0x9E3779B9u ^ (uint32_t)v->serial ^ ((uint32_t)key << 8);
  v->car_noise = 0x85EBCA6Bu ^ ((uint32_t)v->serial << 1) ^ (uint32_t)channel;
  if (!v->mod_noise)
    v->mod_noise = 1u;
  if (!v->car_noise)
    v->car_noise = 1u;

  snd_env_init(&v->mod_env,
               snd_midi_fm_ms_to_frames(m->rate, patch->modulator.env.attack_ms),
               snd_midi_fm_ms_to_frames(m->rate, patch->modulator.env.decay_ms),
               patch->modulator.env.sustain,
               snd_midi_fm_ms_to_frames(m->rate, patch->modulator.env.release_ms));
  snd_env_init(&v->car_env,
               snd_midi_fm_ms_to_frames(m->rate, patch->carrier.env.attack_ms),
               snd_midi_fm_ms_to_frames(m->rate, patch->carrier.env.decay_ms),
               patch->carrier.env.sustain,
               snd_midi_fm_ms_to_frames(m->rate, patch->carrier.env.release_ms));
  snd_midi_fm_update_voice_pitch(v, fm, m);
  ++fm->fm_voices_started;
}

static void snd_midi_fm_note_on(snd_midi_fm_t SND_PTR *fm,
                                snd_mixer_t SND_PTR *m, long frame,
                                int channel, int key, int velocity) {
  const snd_fm_instrument_t SND_PTR *instrument;
  int layers, i;
  if (!fm || !m || velocity <= 0)
    return;
  instrument = snd_midi_fm_instrument_for(fm, channel, key);
  layers = instrument ? (int)instrument->layer_count : 0;
  if (layers < 1)
    layers = 1;
  if (layers > SND_FM_MAX_LAYERS)
    layers = SND_FM_MAX_LAYERS;
  ++fm->midi_notes_started;
  for (i = 0; i < layers; ++i)
    snd_midi_fm_start_layer(fm, m, frame, channel, key, velocity,
                            instrument, &instrument->layer[i]);
}

static void snd_midi_fm_release_voice(snd_midi_fm_t SND_PTR *fm,
                                      snd_midi_fm_voice_t SND_PTR *v,
                                      long frame) {
  if (!fm || !v || !v->active || v->release_frame >= 0)
    return;
  v->release_frame = frame;
  v->sustained = 0u;
  ++fm->fm_voices_released;
}

static void snd_midi_fm_note_off(snd_midi_fm_t SND_PTR *fm, long frame,
                                 int channel, int key) {
  int i;
  for (i = 0; i < fm->voice_limit; ++i) {
    snd_midi_fm_voice_t SND_PTR *v = &fm->voices[i];
    if (!v->active || v->channel != (uint8_t)channel || v->key != (uint8_t)key)
      continue;
    v->key_released = 1u;
    if (fm->channel[channel].sustain) {
      v->sustained = 1u;
    } else {
      snd_midi_fm_release_voice(fm, v, frame);
    }
  }
}

static void snd_midi_fm_release_sustained(snd_midi_fm_t SND_PTR *fm,
                                          long frame, int channel) {
  int i;
  for (i = 0; i < fm->voice_limit; ++i) {
    snd_midi_fm_voice_t SND_PTR *v = &fm->voices[i];
    if (v->active && v->channel == (uint8_t)channel && v->sustained)
      snd_midi_fm_release_voice(fm, v, frame);
  }
}

static void snd_midi_fm_all_notes_off_channel(snd_midi_fm_t SND_PTR *fm,
                                               long frame, int channel) {
  int i;
  for (i = 0; i < fm->voice_limit; ++i) {
    snd_midi_fm_voice_t SND_PTR *v = &fm->voices[i];
    if (!v->active || v->channel != (uint8_t)channel)
      continue;
    v->key_released = 1u;
    if (fm->channel[channel].sustain)
      v->sustained = 1u;
    else
      snd_midi_fm_release_voice(fm, v, frame);
  }
}

static void snd_midi_fm_kill_channel(snd_midi_fm_t SND_PTR *fm, int channel) {
  int i;
  for (i = 0; i < fm->voice_limit; ++i)
    if (fm->voices[i].active && fm->voices[i].channel == (uint8_t)channel)
      fm->voices[i].active = 0u;
}

static void snd_midi_fm_update_channel_pitch(snd_midi_fm_t SND_PTR *fm,
                                              snd_mixer_t SND_PTR *m,
                                              int channel) {
  int i;
  for (i = 0; i < fm->voice_limit; ++i)
    if (fm->voices[i].active && fm->voices[i].channel == (uint8_t)channel)
      snd_midi_fm_update_voice_pitch(&fm->voices[i], fm, m);
}

static void snd_midi_fm_apply_event(snd_midi_fm_t SND_PTR *fm,
                                    snd_mixer_t SND_PTR *m,
                                    const snd_midi_fm_event_t SND_PTR *ev,
                                    long apply_frame) {
  uint8_t status, type;
  int ch;
  snd_midi_fm_channel_t SND_PTR *c;

  if (!fm || !m || !ev)
    return;

  status = ev->msg.status;
  type = (uint8_t)(status & 0xF0u);
  ch = (int)(status & 0x0Fu);
  c = &fm->channel[ch];

  switch (type) {
  case SND_MIDI_NOTE_ON:
    snd_midi_fm_note_on(fm, m, apply_frame, ch, (int)ev->msg.data1,
                        (int)ev->msg.data2);
    break;

  case SND_MIDI_NOTE_OFF:
    snd_midi_fm_note_off(fm, apply_frame, ch, (int)ev->msg.data1);
    break;

  case SND_MIDI_PROGRAM_CHANGE:
    c->program = ev->msg.data1;
    break;

  case SND_MIDI_PITCH_BEND:
    c->pitch_bend = (uint16_t)(((uint16_t)ev->msg.data2 << 7) |
                               (uint16_t)ev->msg.data1);
    snd_midi_fm_update_channel_pitch(fm, m, ch);
    break;

  case SND_MIDI_CONTROL_CHANGE:
    switch (ev->msg.data1) {
    case SND_MIDI_CC_VOLUME:
      c->volume = ev->msg.data2;
      break;
    case SND_MIDI_CC_EXPRESSION:
      c->expression = ev->msg.data2;
      break;
    case SND_MIDI_CC_PAN:
      c->pan = ev->msg.data2;
      break;
    case SND_MIDI_CC_SUSTAIN: {
      uint8_t old = c->sustain;
      c->sustain = (uint8_t)(ev->msg.data2 >= 64u);
      if (old && !c->sustain)
        snd_midi_fm_release_sustained(fm, apply_frame, ch);
      break;
    }
    case SND_MIDI_CC_ALL_SOUND_OFF:
      snd_midi_fm_kill_channel(fm, ch);
      break;
    case SND_MIDI_CC_ALL_NOTES_OFF:
      snd_midi_fm_all_notes_off_channel(fm, apply_frame, ch);
      break;
    case SND_MIDI_CC_RESET_CONTROLLERS: {
      uint8_t had_sustain = c->sustain;
      c->pitch_bend = (uint16_t)SND_MIDI_PITCH_CENTER;
      c->expression = 127u;
      c->pan = 64u;
      c->sustain = 0u;
      if (had_sustain)
        snd_midi_fm_release_sustained(fm, apply_frame, ch);
      snd_midi_fm_update_channel_pitch(fm, m, ch);
      break;
    }
    default:
      break;
    }
    break;

  default:
    break;
  }
}

static long snd_midi_fm_note_gain(const snd_midi_fm_t SND_PTR *fm,
                                  const snd_midi_fm_voice_t SND_PTR *v) {
  long g;
  const snd_midi_fm_channel_t SND_PTR *c = &fm->channel[v->channel];
  g = ((long)v->velocity * (long)SND_GAIN_UNITY + 63L) / 127L;
  g = (g * (long)c->volume + 63L) / 127L;
  g = (g * (long)c->expression + 63L) / 127L;
  if (g < 0L)
    g = 0L;
  if (g > 32767L)
    g = 32767L;
  return g;
}

static snd_fixed_t snd_midi_fm_phase_mod(int sample, uint16_t amount) {
#if SND_FIXED_SHIFT == 16
  long v;
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
  v = (long)sample * (long)amount * 2L;
#else
  /* 32768 -> one normalized full-scale unit, so amount=256 produces one
   * 16.16 cycle of phase displacement. */
  v = ((long)sample * (long)amount) >> 7;
#endif
  return (snd_fixed_t)v;
#else
  /* Wide fixed point is a host-side option. It can afford the exact scaling;
   * the 16.16 Pico/DOS path above stays division-free in the inner loop. */
  int64_t numerator = (int64_t)sample * (int64_t)amount;
  numerator *= ((int64_t)1 << (SND_FIXED_SHIFT - 8));
  if (SND_FULL_SCALE <= 0)
    return 0;
  return (snd_fixed_t)(numerator / (int64_t)SND_FULL_SCALE);
#endif
}

static long snd_midi_fm_scale_operator(long sample, int16_t level,
                                       int16_t env) {
  long v = sample;
  if (level < 0)
    level = 0;
  if (env < 0)
    env = 0;
  v = (v * (long)level) >> SND_GAIN_SHIFT;
  v = (v * (long)env) >> SND_GAIN_SHIFT;
  return v;
}

static void snd_midi_fm_emit_sample(snd_mixer_t SND_PTR *m, int frame,
                                    long sample, long gain, uint8_t pan) {
  long out = (sample * gain) >> SND_GAIN_SHIFT;
  if (m->channels == 2) {
    long right = (gain * (long)pan) / 127L;
    long left = gain - right;
    long i = (long)frame * 2L;
    left *= 2L;
    right *= 2L;
    snd_block_add(m, i, (sample * left) >> SND_GAIN_SHIFT);
    snd_block_add(m, i + 1L, (sample * right) >> SND_GAIN_SHIFT);
  } else {
    snd_block_add(m, (long)frame, out);
  }
}

static void snd_midi_fm_mix_voice_segment(snd_midi_fm_t SND_PTR *fm,
                                           snd_mixer_t SND_PTR *m,
                                           snd_midi_fm_voice_t SND_PTR *v) {
  long block_start, seg_start, seg_end;
  long gain;
  long max_release;
  int f0, f1, f;

  if (!fm || !m || !v || !v->active || !v->patch)
    return;

  block_start = m->block_frame;
  seg_start = block_start + (long)m->span_0;
  seg_end = block_start + (long)m->span_1;
  if (seg_end <= v->start_frame)
    return;
  if (seg_start < v->start_frame)
    seg_start = v->start_frame;

  max_release = v->mod_env.release;
  if (v->car_env.release > max_release)
    max_release = v->car_env.release;
  if (v->release_frame >= 0 && seg_start >= v->release_frame + max_release) {
    v->active = 0u;
    return;
  }
  if (v->release_frame >= 0 && seg_end > v->release_frame + max_release)
    seg_end = v->release_frame + max_release;
  if (seg_end <= seg_start) {
    v->active = 0u;
    return;
  }

  f0 = (int)(seg_start - block_start);
  f1 = (int)(seg_end - block_start);
  gain = snd_midi_fm_note_gain(fm, v);
  snd_touch_block(m);

  for (f = f0; f < f1; ++f) {
    const snd_fm_patch_t SND_PTR *p = v->patch;
    long abs_frame = block_start + (long)f;
    long since_start = abs_frame - v->start_frame;
    long since_release = (v->release_frame >= 0)
                             ? abs_frame - v->release_frame
                             : -1L;
    int16_t mod_env = snd_env_level(&v->mod_env, since_start, since_release);
    int16_t car_env = snd_env_level(&v->car_env, since_start, since_release);
    snd_fixed_t fb_phase = snd_midi_fm_phase_mod((int)v->last_mod, p->feedback);
    snd_fixed_t mod_sample_phase =
        (snd_fixed_t)((v->mod_phase + fb_phase) & (SND_FIXED_ONE - 1));
    long mod = (long)snd_wave_sample((snd_wave_t)p->modulator.wave,
                                    mod_sample_phase, &v->mod_noise);
    snd_fixed_t car_mod;
    snd_fixed_t car_sample_phase;
    long car;
    long mixed;

    mod = snd_midi_fm_scale_operator(mod, p->modulator.level, mod_env);
    if (mod > SND_FULL_SCALE)
      mod = SND_FULL_SCALE;
    if (mod < -SND_FULL_SCALE)
      mod = -SND_FULL_SCALE;
    v->last_mod = (int16_t)mod;

    car_mod = snd_midi_fm_phase_mod((int)mod, p->modulation);
    car_sample_phase =
        (snd_fixed_t)((v->car_phase + car_mod) & (SND_FIXED_ONE - 1));
    car = (long)snd_wave_sample((snd_wave_t)p->carrier.wave, car_sample_phase,
                               &v->car_noise);
    car = snd_midi_fm_scale_operator(car, p->carrier.level, car_env);

    if (p->algorithm == SND_FM_ALGORITHM_ADDITIVE)
      mixed = (car + mod) / 2L;
    else
      mixed = car;

    snd_midi_fm_emit_sample(m, f, mixed, gain,
                            fm->channel[v->channel].pan);

    v->mod_phase = (snd_fixed_t)((v->mod_phase + v->mod_step) &
                                 (SND_FIXED_ONE - 1));
    v->car_phase = (snd_fixed_t)((v->car_phase + v->car_step) &
                                 (SND_FIXED_ONE - 1));
  }

  if (v->release_frame >= 0 &&
      block_start + (long)f1 >= v->release_frame + max_release)
    v->active = 0u;
}

static void snd_midi_fm_mix_segment(snd_midi_fm_t SND_PTR *fm,
                                    snd_mixer_t SND_PTR *m) {
  int i;
  for (i = 0; i < fm->voice_limit; ++i)
    if (fm->voices[i].active)
      snd_midi_fm_mix_voice_segment(fm, m, &fm->voices[i]);
}

void snd_midi_fm_mix_block(snd_midi_fm_t SND_PTR *fm,
                           snd_mixer_t SND_PTR *m) {
  long block_start, cursor, limit;
  int saved_0, saved_1;

  if (!fm || !m || m->block_frames <= 0)
    return;

  block_start = m->block_frame;
  saved_0 = m->span_0;
  saved_1 = m->span_1;
  cursor = block_start + (long)saved_0;
  limit = block_start + (long)saved_1;

  while (cursor < limit) {
    snd_midi_fm_event_t SND_PTR *ev = snd_midi_fm_front_event(fm);
    long seg_end = limit;

    /* Apply every event due at the current cursor. An event that arrived late
     * is clamped to cursor rather than rewinding oscillator state. */
    while (ev && ev->frame <= cursor) {
      snd_midi_fm_apply_event(fm, m, ev, cursor);
      snd_midi_fm_pop_event(fm);
      ev = snd_midi_fm_front_event(fm);
    }

    if (ev && ev->frame < seg_end)
      seg_end = ev->frame;

    if (seg_end > cursor) {
      m->span_0 = (int)(cursor - block_start);
      m->span_1 = (int)(seg_end - block_start);
      snd_midi_fm_mix_segment(fm, m);
      cursor = seg_end;
    } else {
      /* Defensive progress if a malformed/out-of-order queue somehow reaches
       * here despite queue_message() rejecting it. */
      ++cursor;
    }
  }

  m->span_0 = saved_0;
  m->span_1 = saved_1;
}

void snd_midi_fm_all_sound_off(snd_midi_fm_t SND_PTR *fm) {
  int i;
  if (!fm)
    return;
  for (i = 0; i < SND_MIDI_FM_MAX_VOICES; ++i)
    fm->voices[i].active = 0u;
  fm->event_head = 0;
  fm->event_count = 0;
}

int snd_midi_fm_active_voices(const snd_midi_fm_t SND_PTR *fm) {
  int i, n = 0;
  if (!fm)
    return 0;
  for (i = 0; i < fm->voice_limit; ++i)
    if (fm->voices[i].active)
      ++n;
  return n;
}

int snd_midi_fm_pending_events(const snd_midi_fm_t SND_PTR *fm) {
  return fm ? fm->event_count : 0;
}
