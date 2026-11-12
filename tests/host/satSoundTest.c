// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for satellite element sets by sound (core/sat/sat_sound.c, docs/spec/sound-transfer.md): the frame and its
 * refusals, the shared vectors (tools/sat_sound.py writes them, the page's encoder must agree), the transfer bookkeeping,
 * and whole transfers through the audio path: HDLC and AFSK modulation, a phone speaker's level, noise, clipping, hum
 * and a clock error, into the demodulator the APRS receiver uses, 32 satellites sent twice.
 *
 *   make -C tests/host run-sat      (run from tests/host: the vectors are read from ../data)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sat/sat_sound.h"
#include "aprs/hdlc.h"
#include "aprs/afsk.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- the vectors file: flat strings and numbers, found by key in order ---

static char *vec_text;

static const char *jfind(const char *from, const char *key)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\": ", key);
	const char *p = strstr(from, pat);
	return p != NULL ? p + strlen(pat) : NULL;
}

static const char *jstr(const char *from, const char *key, char *out, size_t max)
{
	const char *p = jfind(from, key);
	if (p == NULL || *p != '"') {
		return NULL;
	}
	p++;
	size_t n = 0;
	while (*p != '"' && *p != '\0' && n + 1 < max) {
		out[n++] = *p++;
	}
	out[n] = '\0';
	return *p == '"' ? p + 1 : NULL;
}

static const char *jint(const char *from, const char *key, long *v)
{
	const char *p = jfind(from, key);
	if (p == NULL) {
		return NULL;
	}
	char *end;
	*v = strtol(p, &end, 10);
	return end;
}

static size_t unhex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	while (s[0] != '\0' && s[1] != '\0' && n < max) {
		unsigned v;
		sscanf(s, "%2x", &v);
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static void load_vectors(void)
{
	FILE *f = fopen("../data/sat_sound_vectors.json", "rb");
	if (f == NULL) {
		printf("FAIL: tests/data/sat_sound_vectors.json not found (run from tests/host)\n");
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	vec_text = malloc((size_t)n + 1);
	if (fread(vec_text, 1, (size_t)n, f) != (size_t)n) {
		exit(1);
	}
	vec_text[n] = '\0';
	fclose(f);
}

static void test_vectors(void)
{
	const char *good = strstr(vec_text, "\"good\"");
	const char *refused = strstr(vec_text, "\"refused\"");
	CHECK(good != NULL && refused != NULL, "the vectors have no good and refused lists");
	const char *p = good;
	int cases = 0;
	while (p != NULL) {
		char name[64], l1[80], l2[80], fh[1024], th[1024];
		long id, index, count, bits;
		const char *q = jstr(p, "name", name, sizeof(name));
		if (q == NULL || q > refused) {
			break;
		}
		q = jstr(q, "line1", l1, sizeof(l1));
		q = q != NULL ? jstr(q, "line2", l2, sizeof(l2)) : NULL;
		q = q != NULL ? jint(q, "id", &id) : NULL;
		q = q != NULL ? jint(q, "index", &index) : NULL;
		q = q != NULL ? jint(q, "count", &count) : NULL;
		q = q != NULL ? jstr(q, "frame_hex", fh, sizeof(fh)) : NULL;
		q = q != NULL ? jstr(q, "tones_hex", th, sizeof(th)) : NULL;
		q = q != NULL ? jint(q, "tone_bits", &bits) : NULL;
		if (q == NULL) {
			CHECK(0, "vector %d is malformed", cases);
			break;
		}
		cases++;
		uint8_t want[SAT_SOUND_FRAME_MAX], got[SAT_SOUND_FRAME_MAX];
		size_t wn = unhex(fh, want, sizeof(want));
		size_t gn = sat_sound_build(got, sizeof(got), (uint8_t)id, (uint8_t)index, (uint8_t)count, name, l1, l2);
		CHECK(gn == wn && memcmp(got, want, wn) == 0, "vector %d: the frame differs from the Python encoder's", cases);

		static uint8_t tones[1024], want_tones[1024];
		hdlc_enc_t e;
		hdlc_enc_init(&e, tones, sizeof(tones));
		hdlc_enc_flags(&e, 4);
		hdlc_enc_frame(&e, want, wn);
		hdlc_enc_flags(&e, 2);
		size_t tn = unhex(th, want_tones, sizeof(want_tones));
		CHECK(e.nbits == (uint32_t)bits && tn == (e.nbits + 7u) / 8u && memcmp(tones, want_tones, tn) == 0,
		      "vector %d: the HDLC tone bits differ from the Python encoder's (%u bits, want %ld)", cases, e.nbits, bits);

		sat_sound_rec_t r;
		CHECK(sat_sound_parse(want, wn, &r) == SAT_SOUND_OK && r.id == id && r.index == index && r.count == count &&
		      strcmp(r.name, name) == 0 && strcmp(r.line1, l1) == 0 && strcmp(r.line2, l2) == 0,
		      "vector %d: the parse differs", cases);
		p = q;
	}
	CHECK(cases == 3, "%d good vectors read, 3 expected", cases);

	p = refused;
	int refusals = 0;
	for (;;) {
		char why[64], name[64], l1[80], l2[80];
		long id, index, count;
		const char *q = jstr(p, "why", why, sizeof(why));
		if (q == NULL) {
			break;
		}
		q = jstr(q, "name", name, sizeof(name));
		q = q != NULL ? jstr(q, "line1", l1, sizeof(l1)) : NULL;
		q = q != NULL ? jstr(q, "line2", l2, sizeof(l2)) : NULL;
		q = q != NULL ? jint(q, "id", &id) : NULL;
		q = q != NULL ? jint(q, "index", &index) : NULL;
		q = q != NULL ? jint(q, "count", &count) : NULL;
		if (q == NULL) {
			CHECK(0, "refusal %d is malformed", refusals);
			break;
		}
		uint8_t buf[SAT_SOUND_FRAME_MAX + 8];
		CHECK(sat_sound_build(buf, sizeof(buf), (uint8_t)id, (uint8_t)index, (uint8_t)count, name, l1, l2) == 0,
		      "a frame was built for: %s", why);
		refusals++;
		p = q;
	}
	CHECK(refusals == 5, "%d refusals read, 5 expected", refusals);
}

// --- a TLE family: the ISS's lines with another catalog number, the check digits redone ---

static void make_tle(unsigned norad, char *l1, char *l2)
{
	strcpy(l1, "1 25544U 98067A   26300.51782528  .00016717  00000-0  30144-3 0  9990");
	strcpy(l2, "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49815361474550");
	char num[8];
	snprintf(num, sizeof(num), "%05u", norad);
	memcpy(l1 + 2, num, 5);
	memcpy(l2 + 2, num, 5);
	l1[68] = (char)('0' + tle_checksum(l1));
	l2[68] = (char)('0' + tle_checksum(l2));
}

static size_t make_frame_n(uint8_t id, unsigned index, unsigned count, uint8_t *out, size_t max)
{
	char l1[80], l2[80], name[32];
	make_tle(40000u + index, l1, l2);
	snprintf(name, sizeof(name), "SAT-%02u", index);
	return sat_sound_build(out, max, id, (uint8_t)index, (uint8_t)count, name, l1, l2);
}

static void test_frames(void)
{
	uint8_t f[SAT_SOUND_FRAME_MAX + 4];
	sat_sound_rec_t r;
	size_t n = make_frame_n(5, 3, 8, f, sizeof(f));
	CHECK(n == SAT_SOUND_HEAD + 6 + 2 * TLE_LINE_LEN, "frame length %zu", n);
	CHECK(sat_sound_parse(f, n, &r) == SAT_SOUND_OK && r.id == 5 && r.index == 3 && r.count == 8 &&
	      strcmp(r.name, "SAT-03") == 0 && r.el.norad == 40003u && r.el.inclo > 51.6 && r.el.inclo < 51.7, "a good frame parses");
	CHECK(sat_sound_build(f, n - 1, 5, 3, 8, "SAT-03", r.line1, r.line2) == 0, "a buffer one byte short is refused");

	// the longest frame: a name of 24 characters
	char l1[80], l2[80];
	make_tle(25544, l1, l2);
	n = sat_sound_build(f, sizeof(f), 1, 0, 1, "ABCDEFGHIJKLMNOPQRSTUVWX", l1, l2);
	CHECK(n == SAT_SOUND_FRAME_MAX, "the longest frame is %zu, expected %u", n, (unsigned)SAT_SOUND_FRAME_MAX);
	CHECK(sat_sound_parse(f, n, &r) == SAT_SOUND_OK && strlen(r.name) == 24, "a 24 character name parses");
	n = sat_sound_build(f, sizeof(f), 1, 0, 1, "", l1, l2);
	CHECK(n == SAT_SOUND_HEAD + 2 * TLE_LINE_LEN && sat_sound_parse(f, n, &r) == SAT_SOUND_OK && r.name[0] == '\0',
	      "no name parses");

	// every way a received frame can be wrong
	uint8_t g[SAT_SOUND_FRAME_MAX + 4];
	n = make_frame_n(5, 3, 8, f, sizeof(f));
	CHECK(sat_sound_parse(f, 3, &r) == SAT_SOUND_ERR_SHORT, "short");
	memcpy(g, f, n); g[0] = 0x55;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_MAGIC, "magic");
	memcpy(g, f, n); g[1] = 2;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_VERSION, "version");
	memcpy(g, f, n); g[3] = 8;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_INDEX, "index at the count");
	memcpy(g, f, n); g[4] = 0;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_INDEX, "count 0");
	memcpy(g, f, n); g[4] = 33; g[3] = 0;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_INDEX, "count 33");
	memcpy(g, f, n); g[5] = 25;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_NAME, "name length 25");
	memcpy(g, f, n); g[SAT_SOUND_HEAD] = 0x01;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_NAME, "a control character in the name");
	memcpy(g, f, n); g[SAT_SOUND_HEAD] = 0x80;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_NAME, "a non-ASCII byte in the name");
	memcpy(g, f, n); g[5] = 200;
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_NAME, "a name length past the frame");
	CHECK(sat_sound_parse(f, n - 1, &r) == SAT_SOUND_ERR_LENGTH, "one byte short");
	memcpy(g, f, n); g[n] = ' ';
	CHECK(sat_sound_parse(g, n + 1, &r) == SAT_SOUND_ERR_LENGTH, "one byte long");
	memcpy(g, f, n); g[SAT_SOUND_HEAD + 6 + 20] ^= 1;	// a digit of line 1
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_TLE, "a changed digit fails the check digit");
	memcpy(g, f, n); g[SAT_SOUND_HEAD + 6] = '2';
	CHECK(sat_sound_parse(g, n, &r) == SAT_SOUND_ERR_TLE, "line 1 doesn't start with 1");
	CHECK(sat_sound_parse(f, 0, &r) == SAT_SOUND_ERR_SHORT, "empty");

	// no input may crash the parser: every length of a good frame, and a spray of changed bytes
	unsigned seed = 1;
	for (size_t k = 0; k <= n + 2; k++) {
		sat_sound_parse(f, k, &r);
	}
	for (int k = 0; k < 20000; k++) {
		memcpy(g, f, n);
		seed = seed * 1103515245u + 12345u;
		g[(seed >> 8) % n] = (uint8_t)(seed >> 20);
		sat_sound_parse(g, (seed >> 4) % 4 == 0 ? (seed >> 12) % (n + 3) : n, &r);
	}
}

static void test_rx(void)
{
	sat_sound_rx_t rx;
	sat_sound_rec_t r;
	uint8_t f[SAT_SOUND_FRAME_MAX + 4];
	sat_sound_rx_init(&rx);
	CHECK(!sat_sound_rx_done(&rx), "nothing heard is not done");

	size_t n = make_frame_n(9, 2, 3, f, sizeof(f));
	CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_NEW && r.index == 2 && rx.heard == 1, "the first frame is new");
	CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_DUP && rx.heard == 1 && rx.dups == 1, "its copy is a duplicate");
	n = make_frame_n(9, 0, 3, f, sizeof(f));
	CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_NEW && rx.heard == 2 && !sat_sound_rx_done(&rx), "out of order");
	n = make_frame_n(9, 1, 3, f, sizeof(f));
	CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_NEW && rx.heard == 3 && sat_sound_rx_done(&rx), "all three");

	CHECK(sat_sound_rx_frame(&rx, f, 10, &r) == SAT_SOUND_RX_BAD && rx.bad == 1 && rx.heard == 3, "a bad frame is counted, nothing lost");

	// another transfer id starts again; so does another count
	n = make_frame_n(10, 0, 3, f, sizeof(f));
	CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_NEW && rx.id == 10 && rx.heard == 1 && rx.bad == 1 &&
	      !sat_sound_rx_done(&rx), "a new transfer id starts over, the refused count stays");
	n = make_frame_n(10, 0, 4, f, sizeof(f));
	CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_NEW && rx.count == 4 && rx.heard == 1, "a new count starts over");

	// a full 32 satellite transfer
	sat_sound_rx_init(&rx);
	for (unsigned i = 0; i < 32; i++) {
		n = make_frame_n(77, 31 - i, 32, f, sizeof(f));
		CHECK(sat_sound_rx_frame(&rx, f, n, &r) == SAT_SOUND_RX_NEW, "satellite %u of 32", 31 - i);
	}
	CHECK(sat_sound_rx_done(&rx) && rx.seen == 0xFFFFFFFFu, "32 of 32");
}

// --- whole transfers through the audio ---

typedef struct {
	sat_sound_rx_t rx;
	unsigned frames, new_sats;
} run_t;

static void on_frame(const uint8_t *frame, size_t n, void *ctx)
{
	run_t *run = ctx;
	sat_sound_rec_t r;
	run->frames++;
	if (sat_sound_rx_frame(&run->rx, frame, n, &r) == SAT_SOUND_RX_NEW) {
		run->new_sats++;
	}
}

static uint32_t rng = 12345;

static float uni(void)
{
	rng = rng * 1664525u + 1013904223u;
	return ((float)(rng >> 8) + 0.5f) / 16777216.0f;
}

static float gauss(void)
{
	return sqrtf(-2.0f * logf(uni())) * cosf(6.2831853f * uni());
}

typedef struct {
	unsigned sats, passes;
	float level;		// the tone peak, 1.0 = full scale at the microphone's ADC
	float snr_db;		// tone RMS over white noise RMS; 99 = none
	float clip;		// the signal is multiplied by this before the ADC clips it at full scale; 1 = no clipping beyond level
	float hum_db;		// a 50 Hz hum this many dB under the tones, 0 = none
	float dc;		// a DC offset, in full scale units
	uint32_t mod_rate;	// the sender's rate: 48000 (a phone) or a rate near 8000 (a clock error)
} cfg_t;

// Modulate at the sender's rate, bring it to the radio's 8 kHz the way a codec does (average, drop), add what the room
// adds, demodulate. Returns the distinct satellites received
static unsigned run_transfer(const cfg_t *c, run_t *run)
{
	static uint8_t tones[1024], frame[SAT_SOUND_FRAME_MAX + 4];
	static int16_t pcm[110000], out[20000];
	afsk_demod_t d;
	memset(run, 0, sizeof(*run));
	sat_sound_rx_init(&run->rx);
	if (!afsk_demod_init(&d, 8000, on_frame, run)) {
		return 0;
	}
	float tone_rms = c->level * 0.7071f;
	float sigma = c->snr_db >= 99.0f ? 0.0f : tone_rms * powf(10.0f, -c->snr_db / 20.0f);
	uint32_t ratio = (c->mod_rate + 4000u) / 8000u;	// 6 for a phone at 48 kHz; 1 near 8 kHz (a clock error)
	unsigned long t = 0;
	for (unsigned p = 0; p < c->passes; p++) {
		for (unsigned i = 0; i < c->sats; i++) {
			size_t n = make_frame_n(42, i, c->sats, frame, sizeof(frame));
			hdlc_enc_t e;
			hdlc_enc_init(&e, tones, sizeof(tones));
			hdlc_enc_flags(&e, 30);
			hdlc_enc_frame(&e, frame, n);
			hdlc_enc_flags(&e, 3);
			afsk_mod_t m;
			afsk_mod_init(&m, c->mod_rate);
			size_t k = afsk_mod(&m, tones, e.nbits, (int16_t)(c->level * 32767.0f), pcm, sizeof(pcm) / sizeof(pcm[0]));
			size_t gap = 800u + (uint32_t)(uni() * 4000.0f);	// the room before the frame: 0.1 to 0.6 s
			size_t o = 0;
			for (size_t j = 0; j < gap * ratio + k + 800u * ratio; j += ratio) {
				float acc = 0.0f;
				for (uint32_t q = 0; q < ratio; q++) {
					size_t at = j + q;
					acc += (at >= gap * ratio && at - gap * ratio < k) ? (float)pcm[at - gap * ratio] / 32768.0f : 0.0f;
				}
				float x = (ratio > 1 ? acc / (float)ratio : acc) * c->clip;
				float hum = c->hum_db > 0.0f ? tone_rms * powf(10.0f, -c->hum_db / 20.0f) * 1.4142f *
					    sinf(6.2831853f * 50.0f * (float)t / 8000.0f) : 0.0f;
				x += hum + c->dc + sigma * gauss();
				x = x > 0.999f ? 0.999f : x < -1.0f ? -1.0f : x;
				out[o++] = (int16_t)(x * 32768.0f);
				t++;
				if (o == sizeof(out) / sizeof(out[0])) {
					afsk_demod(&d, out, o);
					o = 0;
				}
			}
			afsk_demod(&d, out, o);
		}
	}
	return run->rx.heard;
}

static void test_audio(void)
{
	run_t run;
	cfg_t c = { .sats = 32, .passes = 2, .level = 0.5f, .snr_db = 99.0f, .clip = 1.0f, .mod_rate = 48000 };

	// a clean channel: every frame, once each pass
	unsigned heard = run_transfer(&c, &run);
	CHECK(heard == 32 && sat_sound_rx_done(&run.rx) && run.frames == 64 && run.new_sats == 32 && run.rx.dups == 32 &&
	      run.rx.bad == 0, "clean: %u of 32 heard, %u frames, %u dups, %u bad", heard, run.frames, run.rx.dups, run.rx.bad);

	// what the radio measured at the microphone: a quiet tone (P 50 of 32767 is 0.0015) is already far above the codec's
	// floor (P 4 to 8); the loudest tried was P 450 (0.014). Model the weakest at 20 dB over the room's noise
	c.level = 0.0015f;
	c.snr_db = 20.0f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 32, "the quietest tone tried, 20 dB over the noise: %u of 32", heard);
	c.level = 0.014f;
	c.snr_db = 32.0f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 32, "the medium tone, 32 dB over the noise: %u of 32", heard);

	// a loud phone clipped hard by the codec
	c.level = 0.5f;
	c.snr_db = 99.0f;
	c.clip = 8.0f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 32, "clipped to a square wave: %u of 32", heard);
	c.clip = 1.0f;

	// room noise
	c.snr_db = 15.0f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 32, "15 dB over the noise: %u of 32", heard);
	c.snr_db = 10.0f;
	heard = run_transfer(&c, &run);
	printf("# 10 dB over the noise: %u of 32 heard (%u frames of 64 decoded)\n", heard, run.frames);
	CHECK(heard >= 24, "10 dB over the noise: only %u of 32", heard);
	c.snr_db = 6.0f;
	heard = run_transfer(&c, &run);
	printf("# 6 dB over the noise: %u of 32 heard (%u frames of 64 decoded)\n", heard, run.frames);

	// a hum and a DC offset
	c.snr_db = 99.0f;
	c.hum_db = 6.0f;
	c.dc = 0.05f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 32, "a 50 Hz hum 6 dB under the tones and a DC offset: %u of 32", heard);
	c.hum_db = 0.0f;
	c.dc = 0.0f;

	// the sender's clock 0.5 % fast and slow, far more than a phone's audio clock is off (tens of ppm); the demodulator
	// is tested to 0.5 % in aprsTest. The sender at 8 kHz so the rates differ
	c.mod_rate = 8040;
	c.sats = 8;
	heard = run_transfer(&c, &run);
	CHECK(heard == 8, "a sender 0.5 %% fast: %u of 8", heard);
	c.mod_rate = 7960;
	heard = run_transfer(&c, &run);
	CHECK(heard == 8, "a sender 0.5 %% slow: %u of 8", heard);
	c.mod_rate = 48000;

	// one pass is enough on a clean channel; the second pass is for the frames noise takes
	c.sats = 32;
	c.passes = 1;
	c.snr_db = 99.0f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 32 && run.rx.dups == 0, "one clean pass: %u of 32, %u dups", heard, run.rx.dups);

	// noise alone, and a short burst of garbage: nothing is taken
	c.level = 0.0f;
	c.snr_db = 99.0f;
	heard = run_transfer(&c, &run);
	CHECK(heard == 0 && run.frames == 0, "silence decodes nothing: %u frames", run.frames);
}

// satSoundTest --wav FILE: the WAV tools/sat_sound.py made (16 bit mono, a rate that is a multiple of 8000) through the same
// receiver: how many satellites it carries
static int decode_wav(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t hdr[44];
	if (f == NULL || fread(hdr, 1, 44, f) != 44 || memcmp(hdr, "RIFF", 4) != 0) {
		printf("not a WAV: %s\n", path);
		return 2;
	}
	uint32_t rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
	if (rate % 8000u != 0 || hdr[22] != 1 || hdr[34] != 16) {
		printf("%s: need 16 bit mono at a multiple of 8000 Hz (got %u Hz)\n", path, rate);
		return 2;
	}
	uint32_t ratio = rate / 8000u;
	run_t run;
	afsk_demod_t d;
	memset(&run, 0, sizeof(run));
	sat_sound_rx_init(&run.rx);
	afsk_demod_init(&d, 8000, on_frame, &run);
	static int16_t out[4096];
	size_t o = 0;
	int16_t s;
	int32_t acc = 0;
	uint32_t k = 0;
	while (fread(&s, 2, 1, f) == 1) {
		acc += s;
		if (++k == ratio) {
			out[o++] = (int16_t)(acc / (int32_t)ratio);
			acc = 0;
			k = 0;
			if (o == sizeof(out) / sizeof(out[0])) {
				afsk_demod(&d, out, o);
				o = 0;
			}
		}
	}
	afsk_demod(&d, out, o);
	fclose(f);
	printf("%s: %u satellites heard of %u, %u frames, %u copies, %u refused%s\n", path, run.rx.heard, run.rx.count, run.frames,
	       run.rx.dups, run.rx.bad, sat_sound_rx_done(&run.rx) ? ", complete" : "");
	return sat_sound_rx_done(&run.rx) ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--wav") == 0) {
		return decode_wav(argv[2]);
	}
	load_vectors();
	test_vectors();
	test_frames();
	test_rx();
	test_audio();
	free(vec_text);
	if (failures != 0) {
		printf("# FAILED %d\n", failures);
		return 1;
	}
	printf("# PASSED\n");
	return 0;
}
