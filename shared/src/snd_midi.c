#include "snd_midi.h"

static void snd_midi_channel_defaults(snd_midi_channel_t SND_PTR *c) {
  if (!c)
    return;
  c->pitch_bend = (uint16_t)SND_MIDI_PITCH_CENTER;
  c->program = 0u;
  c->bank_msb = 0u;
  c->bank_lsb = 0u;
  c->volume = 127u;
  c->expression = 127u;
  c->pan = 64u;
  c->sustain = 0u;
  c->pressure = 0u;
}

static void snd_midi_controller_defaults(snd_midi_channel_t SND_PTR *c) {
  if (!c)
    return;
  /* Reset All Controllers does not change program/bank and this layer leaves
   * channel volume alone as well.  It does return the expressive controllers
   * and pitch wheel to neutral. */
  c->pitch_bend = (uint16_t)SND_MIDI_PITCH_CENTER;
  c->expression = 127u;
  c->pan = 64u;
  c->sustain = 0u;
  c->pressure = 0u;
}

void snd_midi_init(snd_midi_t SND_PTR *m) {
  int i;
  if (!m)
    return;
  m->sink.emit = 0;
  m->sink.user = 0;
  for (i = 0; i < SND_MIDI_CHANNELS; ++i)
    snd_midi_channel_defaults(&m->channel[i]);
  m->running_status = 0u;
  m->pending_status = 0u;
  m->data[0] = 0u;
  m->data[1] = 0u;
  m->data_count = 0u;
  m->data_need = 0u;
  m->system_data_left = 0u;
  m->in_sysex = 0u;
}

void snd_midi_reset(snd_midi_t SND_PTR *m) {
  snd_midi_emit_fn emit;
  void SND_PTR *user;
  int i;

  if (!m)
    return;

  emit = m->sink.emit;
  user = m->sink.user;

  for (i = 0; i < SND_MIDI_CHANNELS; ++i)
    snd_midi_channel_defaults(&m->channel[i]);

  m->running_status = 0u;
  m->pending_status = 0u;
  m->data[0] = 0u;
  m->data[1] = 0u;
  m->data_count = 0u;
  m->data_need = 0u;
  m->system_data_left = 0u;
  m->in_sysex = 0u;

  m->sink.emit = emit;
  m->sink.user = user;
}

void snd_midi_set_sink(snd_midi_t SND_PTR *m, snd_midi_emit_fn emit,
                       void SND_PTR *user) {
  if (!m)
    return;
  m->sink.emit = emit;
  m->sink.user = user;
}

int snd_midi_data_bytes(uint8_t status) {
  uint8_t type;

  if (status < 0x80u || status >= 0xF0u)
    return 0;

  type = (uint8_t)(status & 0xF0u);
  if (type == SND_MIDI_PROGRAM_CHANGE || type == SND_MIDI_CHANNEL_PRESSURE)
    return 1;

  if (type >= SND_MIDI_NOTE_OFF && type <= SND_MIDI_PITCH_BEND)
    return 2;

  return 0;
}

int snd_midi_message(snd_midi_t SND_PTR *m, long frame, uint8_t status,
                     uint8_t data1, uint8_t data2) {
  snd_midi_message_t msg;
  snd_midi_channel_t SND_PTR *c;
  uint8_t type;
  uint8_t ch;
  int need;

  if (!m)
    return 0;

  need = snd_midi_data_bytes(status);
  if (need == 0)
    return 0;

  ch = (uint8_t)(status & 0x0Fu);
  type = (uint8_t)(status & 0xF0u);
  c = &m->channel[ch];

  data1 &= 0x7Fu;
  data2 &= 0x7Fu;

  /* MIDI defines Note-On velocity zero as Note-Off.  Normalize it here so
   * every backend only has one note-release case to implement. */
  if (type == SND_MIDI_NOTE_ON && data2 == 0u) {
    type = SND_MIDI_NOTE_OFF;
    status = (uint8_t)(SND_MIDI_NOTE_OFF | ch);
  }

  switch (type) {
  case SND_MIDI_CONTROL_CHANGE:
    switch (data1) {
    case SND_MIDI_CC_BANK_MSB:
      c->bank_msb = data2;
      break;
    case SND_MIDI_CC_BANK_LSB:
      c->bank_lsb = data2;
      break;
    case SND_MIDI_CC_VOLUME:
      c->volume = data2;
      break;
    case SND_MIDI_CC_PAN:
      c->pan = data2;
      break;
    case SND_MIDI_CC_EXPRESSION:
      c->expression = data2;
      break;
    case SND_MIDI_CC_SUSTAIN:
      c->sustain = (uint8_t)(data2 >= 64u);
      break;
    case SND_MIDI_CC_RESET_CONTROLLERS:
      snd_midi_controller_defaults(c);
      break;
    default:
      break;
    }
    break;

  case SND_MIDI_PROGRAM_CHANGE:
    c->program = data1;
    break;

  case SND_MIDI_CHANNEL_PRESSURE:
    c->pressure = data1;
    break;

  case SND_MIDI_PITCH_BEND:
    c->pitch_bend = (uint16_t)(((uint16_t)data2 << 7) | (uint16_t)data1);
    break;

  default:
    break;
  }

  msg.status = status;
  msg.data1 = data1;
  msg.data2 = (need == 2) ? data2 : 0u;
  msg.size = (uint8_t)(need + 1);

  if (m->sink.emit)
    m->sink.emit(m->sink.user, frame, &msg, c);

  return 1;
}

static uint8_t snd_midi_system_data_bytes(uint8_t status) {
  switch (status) {
  case 0xF1u: /* MTC quarter frame */
  case 0xF3u: /* song select */
    return 1u;
  case 0xF2u: /* song position */
    return 2u;
  default:
    return 0u;
  }
}

int snd_midi_feed_byte(snd_midi_t SND_PTR *m, long frame, uint8_t byte) {
  int dispatched;

  if (!m)
    return 0;

  /* System real-time may occur between any two bytes and never cancels
   * running status.  This foundation does not consume clock/transport yet. */
  if (byte >= 0xF8u)
    return 0;

  if (byte & 0x80u) {
    if (byte >= 0xF0u) {
      /* Any System Common or SysEx status cancels channel running status. */
      m->running_status = 0u;
      m->pending_status = 0u;
      m->data_count = 0u;
      m->data_need = 0u;
      m->system_data_left = 0u;

      if (byte == 0xF0u) {
        m->in_sysex = 1u;
      } else if (byte == 0xF7u) {
        m->in_sysex = 0u;
      } else {
        m->in_sysex = 0u;
        m->system_data_left = snd_midi_system_data_bytes(byte);
      }
      return 0;
    }

    m->in_sysex = 0u;
    m->system_data_left = 0u;
    m->running_status = byte;
    m->pending_status = byte;
    m->data_count = 0u;
    m->data_need = (uint8_t)snd_midi_data_bytes(byte);
    return 0;
  }

  if (m->in_sysex)
    return 0;

  if (m->system_data_left) {
    --m->system_data_left;
    return 0;
  }

  if (!m->pending_status) {
    if (!m->running_status)
      return 0;
    m->pending_status = m->running_status;
    m->data_need = (uint8_t)snd_midi_data_bytes(m->pending_status);
    m->data_count = 0u;
  }

  if (m->data_need == 0u)
    return 0;

  if (m->data_count < 2u)
    m->data[m->data_count] = (uint8_t)(byte & 0x7Fu);
  ++m->data_count;

  if (m->data_count < m->data_need)
    return 0;

  dispatched = snd_midi_message(m, frame, m->pending_status, m->data[0],
                                (m->data_need > 1u) ? m->data[1] : 0u);

  /* Channel messages continue under running status. */
  m->pending_status = m->running_status;
  m->data_need = (uint8_t)snd_midi_data_bytes(m->running_status);
  m->data_count = 0u;
  m->data[0] = 0u;
  m->data[1] = 0u;

  return dispatched;
}
