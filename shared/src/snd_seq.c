#include "snd_seq.h"

long snd_seq_frames_per_row(const snd_song_t SND_PTR *song, int rate) {
  long bpm, rpb, fpr;

  if (!song || rate <= 0)
    return 0;
  bpm = (song->bpm > 0) ? (long)song->bpm : 120L;
  rpb = (song->rows_per_beat > 0) ? (long)song->rows_per_beat : 4L;

  /* Integer division on purpose. A song's tempo is therefore quantized to
     whole frames per row, which is what keeps two targets at the same mix rate
     sample-identical instead of drifting apart over a few minutes. */
  fpr = ((long)rate * 60L) / (bpm * rpb);
  if (fpr < 1L)
    fpr = 1L;
  return fpr;
}

static const snd_pattern_t SND_PTR *
snd_player_pattern(const snd_player_t SND_PTR *p) {
  const snd_song_t SND_PTR *s = p->song;
  int idx;

  if (!s || !s->order || s->order_length <= 0 || !s->patterns)
    return 0;
  if (p->order_index < 0 || p->order_index >= s->order_length)
    return 0;
  idx = (int)s->order[p->order_index];
  if (idx < 0 || idx >= s->pattern_count)
    return 0;
  return &s->patterns[idx];
}

void snd_player_init(snd_player_t SND_PTR *p, const snd_song_t SND_PTR *song,
                     const snd_mixer_t SND_PTR *m, long start_frame, int loop) {
  int i;

  if (!p)
    return;

  p->song = song;
  p->order_index = 0;
  p->row = 0;
  p->loop = loop;
  p->finished = 0;
  p->rows_played = 0uL;
  p->next_row_frame = start_frame;
  p->frames_per_row = snd_seq_frames_per_row(song, m ? m->rate : 0);

  p->channel_count = SND_SEQ_MAX_CHANNELS;
  if (song && song->patterns && song->pattern_count > 0) {
    int c = song->patterns[0].channel_count;
    if (c > 0 && c < p->channel_count)
      p->channel_count = c;
  }

  for (i = 0; i < SND_SEQ_MAX_CHANNELS; ++i) {
    snd_tone_reset(&p->tones[i]);
    snd_voice_reset(&p->voices[i]);
  }

  if (!song || !song->order || song->order_length <= 0 || p->frames_per_row <= 0)
    p->finished = 1;
}

void snd_player_stop(snd_player_t SND_PTR *p) {
  int i;
  if (!p)
    return;
  p->finished = 1;
  for (i = 0; i < SND_SEQ_MAX_CHANNELS; ++i) {
    snd_tone_stop(&p->tones[i]);
    snd_voice_stop(&p->voices[i]);
  }
}

int snd_player_finished(const snd_player_t SND_PTR *p) {
  if (!p)
    return 1;
  if (!p->finished)
    return 0;
  /* A song is not really over until its last note has decayed. */
  {
    int i;
    for (i = 0; i < SND_SEQ_MAX_CHANNELS; ++i) {
      if (p->tones[i].active || p->voices[i].active)
        return 0;
    }
  }
  return 1;
}

/* Fire one channel's event at an exact absolute output frame. */
static void snd_player_trigger(snd_player_t SND_PTR *p, snd_mixer_t SND_PTR *m,
                               int ch, const snd_event_t SND_PTR *ev,
                               long at_frame) {
  const snd_song_t SND_PTR *s = p->song;
  const snd_instrument_t SND_PTR *ins;
  int16_t gain;
  int note;

  if (ev->note == SND_NOTE_NONE)
    return;

  if (ev->note == SND_NOTE_OFF) {
    snd_tone_release(&p->tones[ch], at_frame);
    snd_voice_stop(&p->voices[ch]);
    return;
  }

  if (!s->instruments || ev->instrument >= (uint8_t)s->instrument_count)
    return;
  ins = &s->instruments[ev->instrument];

  note = SND_NOTE_VALUE(ev->note);
  if (note < 0)
    return;

  gain = ins->gain;
  if (ev->volume != SND_VOL_KEEP) {
    long v = ((long)gain * (long)ev->volume) / 64L;
    gain = (int16_t)v;
  }

  /* Retire whatever this channel was doing. A tracker channel is monophonic;
     that is the whole reason it is called a channel. */
  snd_tone_stop(&p->tones[ch]);
  snd_voice_stop(&p->voices[ch]);

  if (ins->clip) {
    /* Pitch is the ratio between the requested note and the note the clip was
       sampled at, in 8.8. */
    int base_hz = snd_note_hz(ins->base_note > 0 ? ins->base_note : 60);
    int want_hz = snd_note_hz(note);
    long pitch;

    if (base_hz <= 0)
      base_hz = 1;
    pitch = ((long)want_hz << SND_GAIN_SHIFT) / (long)base_hz;
    if (pitch < 1L)
      pitch = 1L;
    if (pitch > 32767L)
      pitch = 32767L;

    snd_voice_start(&p->voices[ch], ins->clip, m, at_frame, (int16_t)pitch,
                    gain);
    p->voices[ch].id = (int16_t)ch;
    if (ev->pan != SND_VOL_KEEP)
      snd_voice_set_pan(&p->voices[ch], gain, (int16_t)((int)ev->pan * 4));
  } else {
    snd_tone_start(&p->tones[ch], m, (snd_wave_t)ins->wave, snd_note_hz(note),
                   gain, at_frame, 0);
    snd_tone_set_env(&p->tones[ch], &ins->env);
    p->tones[ch].id = (int16_t)ch;
  }
}

/* Step the cursor one row, advancing the order list and handling the end of
   the song. */
static void snd_player_next_row(snd_player_t SND_PTR *p) {
  const snd_pattern_t SND_PTR *pat = snd_player_pattern(p);
  int rows = pat ? pat->row_count : 0;

  ++p->rows_played;
  ++p->row;
  if (rows <= 0 || p->row < rows)
    return;

  p->row = 0;
  ++p->order_index;
  if (p->order_index < p->song->order_length)
    return;

  if (p->loop) {
    p->order_index = 0;
  } else {
    p->order_index = p->song->order_length; /* park past the end */
    p->finished = 1;
  }
}

/* Mix every channel over the frames currently selected by the mixer's span. */
static void snd_player_mix_segment(snd_player_t SND_PTR *p,
                                   snd_mixer_t SND_PTR *m,
                                   snd_mix_stats_t SND_PTR *stats) {
  int i;
  for (i = 0; i < p->channel_count; ++i) {
    if (p->voices[i].active)
      snd_mix_voice_counted(m, &p->voices[i], stats);
    if (p->tones[i].active)
      snd_mix_tone(m, &p->tones[i]);
  }
}

/* Fire one row across every channel. */
static void snd_player_fire_row(snd_player_t SND_PTR *p, snd_mixer_t SND_PTR *m,
                                long at_frame) {
  const snd_pattern_t SND_PTR *pat = snd_player_pattern(p);
  int chans, i;

  if (!pat || pat->row_count <= 0 || !pat->events) {
    p->finished = 1;
    return;
  }

  chans = pat->channel_count;
  if (chans > p->channel_count)
    chans = p->channel_count;

  for (i = 0; i < chans; ++i) {
    const snd_event_t SND_PTR *ev =
        &pat->events[(long)p->row * (long)pat->channel_count + (long)i];
    snd_player_trigger(p, m, i, ev, at_frame);
  }
}

void snd_player_mix_block(snd_player_t SND_PTR *p, snd_mixer_t SND_PTR *m,
                          snd_mix_stats_t SND_PTR *stats) {
  long block_start, limit, cursor;
  int saved_0, saved_1;
  int guard = 0;

  if (!p || !m || !p->song || m->block_frames <= 0)
    return;

  block_start = m->block_frame;
  saved_0 = m->span_0;
  saved_1 = m->span_1;

  cursor = block_start + (long)saved_0;
  limit = block_start + (long)saved_1;

  /* A block is mixed in segments split at row boundaries, never as one piece.
     Triggering a row replaces whatever the channel was playing, so a row that
     lands mid-block must not be applied until the outgoing note has been mixed
     up to that exact frame -- otherwise the first part of the block loses a
     note it should still have been sounding, and a target with a long buffer
     hears something different from a target with a short one.

     This is what the span exists for. It is the same manoeuvre the renderer
     makes when a sprite straddles a tile edge: subdivide, do not approximate. */
  while (cursor < limit) {
    long seg_end = limit;

    if (++guard > (SND_SEQ_MAX_CHANNELS * 512))
      break; /* refuse to spin on a malformed song */

    if (!p->finished) {
      if (p->next_row_frame <= cursor) {
        /* Row due now (or overdue, if the caller started mid-song). Fire it
           and re-evaluate; several rows can land on one frame at extreme
           tempos. */
        snd_player_fire_row(p, m, cursor);
        p->next_row_frame += p->frames_per_row;
        snd_player_next_row(p);
        continue;
      }
      if (p->next_row_frame < seg_end)
        seg_end = p->next_row_frame;
    }

    snd_set_span(m, (int)(cursor - block_start), (int)(seg_end - cursor));
    snd_player_mix_segment(p, m, stats);
    cursor = seg_end;
  }

  m->span_0 = saved_0;
  m->span_1 = saved_1;
}

long snd_song_length_frames(const snd_song_t SND_PTR *song,
                            const snd_mixer_t SND_PTR *m) {
  long fpr, rows = 0;
  int i;

  if (!song || !m || !song->order || !song->patterns)
    return 0;
  fpr = snd_seq_frames_per_row(song, m->rate);

  for (i = 0; i < song->order_length; ++i) {
    int idx = (int)song->order[i];
    if (idx >= 0 && idx < song->pattern_count)
      rows += (long)song->patterns[idx].row_count;
  }
  return rows * fpr;
}
