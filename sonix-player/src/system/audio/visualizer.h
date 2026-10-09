#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <alsa/asoundlib.h>
#include <stdint.h>

// The playback side of the now-playing visualizer. The playback thread hands
// every finished block to visualizer_feed(); it only copies a mono mix into a
// ring buffer, and only while the visualizer asks for frames. The analysis
// runs on the UI thread, in audio_get_visualizer_frame() (audio.h).

// buf holds frames of channels interleaved samples, 16 or 32 bits signed, at
// rate Hz, after the DSP chain: what goes to the output, before it is written
// to pcm. pcm is asked how much it still holds in front of the block, so the
// picture follows what is heard; NULL counts as nothing.
void visualizer_feed(const void *buf, int frames, int channels, int bits, int rate, snd_pcm_t *pcm);

#endif
