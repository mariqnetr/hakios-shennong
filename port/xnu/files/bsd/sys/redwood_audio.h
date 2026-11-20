/*
 * /dev/redwood-audio: the sound iOS plays, on its way to whatever can make it heard.
 *
 * iOS expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them --
 * and, if asked for, into a recording that outlives the kernel (see dev/redwood_audio.c).
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_REDWOOD_AUDIO_H_
#define _SYS_REDWOOD_AUDIO_H_

#include <stdint.h>

#define REDWOOD_AUDIO_DEVICE    "/dev/redwood-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define REDWOOD_AUDIO_RATE      48000
#define REDWOOD_AUDIO_CHANNELS  2
#define REDWOOD_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(REDWOOD_AUDIO_SET_VOLUME): the speakers' volume, as iOS sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (iOS's volume at 0). The loudest is the kernel's to say (on the phone: Android's own
 * level less sound_below, redwood_sound.c); the plug-in only says how far under it.
 */
#include <sys/ioccom.h>

struct redwood_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define REDWOOD_AUDIO_SET_VOLUME _IOW('r', 1, struct redwood_audio_volume)

#ifdef KERNEL
#include <stdbool.h>

/*
 * For drivers. A sink is offered frames as they are written and says how many it took (the
 * rest are lost); it is called from the writer's thread and must not wait.
 */
typedef uint32_t (*redwood_audio_sink_t)(const int16_t *frames, uint32_t count);

void redwood_audio_set_sink(const char *name, redwood_audio_sink_t sink);
/* The volume iOS has set last (REDWOOD_AUDIO_SET_VOLUME): false if it has set none; *generation counts the settings. */
bool redwood_audio_volume(uint32_t *below, bool *muted, uint32_t *generation);
void redwood_audio_init(void);
void redwood_virtio_sound_init(void);   /* boot-arg virtio_sound=1: QEMU's virtio sound device */
void redwood_speaker_init(void);        /* boot-arg speaker_probe=N: a look at the phone's speaker amplifiers */
void redwood_haptic_init(void);         /* boot-arg haptic=N: the phone's vibration motor (bsd/dev/redwood_haptic.c) */
void redwood_haptic_quiesce(void);      /* its chip back into reset: at the run's end and on a panic's way out */
void redwood_haptic_pretend(uint32_t peak); /* a test's stand-in for iOS's waveform: as if a write of this loudness came */
void shennong_haptic_init(void);        /* boot-arg haptic=N on the Xiaomi 14 Pro: the PM8550B's haptics (bsd/dev/shennong_haptic.c) */
void shennong_speaker_init(void);       /* boot-arg spk_probe=N on the Xiaomi 14 Pro: a look at its CS35L43 amplifiers */
void shennong_speaker_quiesce(void);
bool shennong_speaker_on(void);         /* both CS35L43 out of reset, configured as Android leaves them, started (sound=3) */
void shennong_speaker_off(void);
void shennong_speaker_start(void);      /* sound=4: power them up again (configured, out of reset) */
void shennong_speaker_stop(void);
void shennong_speaker_poll(void);       /* their interrupt status said (and cleared) when not zero */
extern bool shennong_speaker_protected;  /* both amplifiers run Cirrus's speaker protection (amp_fw) */       /* ... and down, in silence */        /* ... stopped and into reset again */     /* their resets low again: at the run's end and on a panic's way out */
void shennong_haptic_quiesce(void);     /* its play stopped: at the run's end and on a panic's way out */
void redwood_torch_init(void);          /* boot-arg torch=N: the phone's flashlight (bsd/dev/redwood_torch.c) */
void redwood_torch_quiesce(void);       /* the light off: at the run's end and on a panic's way out */
void shennong_torch_init(void);         /* boot-arg torch=N on the Xiaomi 14 Pro: the PM8550's flash LEDs (bsd/dev/shennong_torch.c) */
void shennong_torch_quiesce(void);      /* its light off: at the run's end and on a panic's way out */
void shennong_camera_init(void);        /* boot-arg cam=N on the Xiaomi 14 Pro: its four sensors (bsd/dev/shennong_camera.c) */
void shennong_camera_quiesce(void);     /* a sensor still powered powered down: at the run's end and on a panic's way out */
void redwood_camera_init(void);         /* boot-arg cam=N: the phone's cameras (bsd/dev/redwood_camera.c) */
void redwood_camera_quiesce(void);      /* whatever of them was switched on, off: at the run's end and on a panic's way out */

/*
 * The phone's speakers (bsd/dev/redwood_speaker.c): its two Awinic amplifiers, set up as
 * Android's driver sets them up, from Android's own configuration file (see there).
 */
bool redwood_speaker_prepare(const uint8_t *acf, uint64_t size);       /* found, reset, configured: powered down, muted */
bool redwood_speaker_start(void);       /* powered up and unmuted at the lowest volume: only while the I2S clock runs */
bool redwood_speaker_volume(uint32_t below, uint32_t step_ms); /* below Android's level, in eighths of a dB; 6 dB a step */
bool redwood_speaker_healthy(void);     /* no fault reported; on one, both are powered down at once */
void redwood_speaker_stop(void);        /* faded out, muted, powered down, in Android's order */
void redwood_speaker_quiesce(void);     /* powered down at once, without sleeping: on a panic's way out */
/* the amplifiers' own temperature and battery voltage read (every second while they play): the reduction they call for */
uint32_t redwood_speaker_monitor(bool tell);

/*
 * The speakers' guard (bsd/dev/redwood_guard.c): what the samples go through in the feeder on their
 * way to the amplifiers, for want of the DSP's speaker protection. Levels in eighths of a dB.
 */
#define REDWOOD_GUARD_SILENT    0xffffffffu     /* the amplifiers are off: nothing reaches the speakers */
struct redwood_guard_report {
	uint32_t blocks, rms_blocks, fast_blocks, slow_blocks;  /* blocks done, and in how many each average held the level down */
	uint32_t limited_frames;        /* frames in which the peak limiter turned the level down */
	uint32_t peak_in, peak_out;     /* the largest sample that came in and that went out */
	uint32_t least_peak_gain;       /* the peak limiter's least gain, in 1/65536 */
	uint64_t least_gain, gain;      /* the power guard's (with the monitor's) least gain and the latest, Q30 */
	uint64_t rms, fast, slow;       /* the averages now, the louder speaker's: Q30 of a full-scale square wave at Android's level */
};
#define REDWOOD_GUARD_MAX_GAIN  (12 * 8)        /* the most gain before the guard: 12 dB */
void redwood_guard_configure(bool hpf, uint32_t gain);         /* the high-pass filter; `gain' before everything */
void redwood_guard_attenuation(uint32_t below);                 /* the amplifiers' level under Android's (or REDWOOD_GUARD_SILENT):
                                                                 * what the limits are reckoned from */
void redwood_guard_monitor(uint32_t below);                     /* a reduction asked for by redwood_speaker_monitor() */
void redwood_guard_process(int16_t *frames, uint32_t count);    /* stereo frames, in place */
void redwood_guard_take_report(struct redwood_guard_report *report);   /* what happened since the last call */
int redwood_guard_decibels(uint64_t ratio);                     /* 10 log10 of a Q30 ratio, in tenths of a dB */
/* boot-arg rec_sound_fed=1: the recording (rec_sound=N) has what the guard let through instead of what iOS wrote */
bool redwood_audio_recording_fed(void);
void redwood_audio_record_fed(const int16_t *frames, uint32_t count);

/*
 * The audio DSP's services, for the sound's path (bsd/dev/redwood_adsp.c, bsd/dev/redwood_sound.c).
 * redwood_adsp_apr() sends one APR packet to a service of the DSP (AFE 4, ASM 7, ADM 8) with the
 * given port and token (0: one of its own) and, with wait_ms, waits for the packet that carries
 * that token back. What is not waited for goes to the handler of events, if there is one and it
 * takes it (returns true). The handler is called on the link's thread and must not wait.
 */
struct redwood_apr_answer {
	uint32_t opcode, token, length;         /* length: of the payload, in bytes */
	uint32_t words[64];
};
typedef bool (*redwood_apr_events_t)(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload,
    uint32_t length);

bool redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct redwood_apr_answer *answer);
void redwood_adsp_apr_events(redwood_apr_events_t handler);
const uint8_t *redwood_adsp_fw_file(const char *name, uint64_t *size);        /* /fw/name, read whole; give back with: */
void redwood_adsp_fw_free(const uint8_t *data, uint64_t size);

void redwood_sound_dsp_up(void);        /* the DSP's audio services are up (adsp=1): boot-arg sound=N begins */
void redwood_sound_dsp_down(void);      /* before the DSP is shut down at the run's end: everything of the sound's stopped */
void redwood_sound_quiesce(void);       /* on a panic's way out: the amplifiers off, without sleeping */
/* the microphones' part of the memory the DSP sees (redwood_sound.c): false if it cannot be had; given back with _put() */
bool redwood_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes);
void redwood_audio_dsp_memory_put(void);
/* a second handler of the DSP's events (the microphones'), asked when the first does not take one */
void redwood_adsp_apr_events_second(redwood_apr_events_t handler);
/* boot-arg rec_sound_mic=1: the recording (rec_sound=N) has what the microphone hears (given with redwood_audio_record_fed()) */
bool redwood_audio_recording_mic(void);
void redwood_mic_dsp_up(void);          /* the DSP's audio services are up (adsp=1): boot-arg mic=N begins */
void redwood_mic_dsp_down(void);        /* before the DSP is shut down at the run's end: what the microphones' path asked for given back */

/*
 * The Xiaomi 14 Pro's DSP speaks GPR (AudioReach) instead of APR (bsd/dev/redwood_adsp.c): redwood_adsp_gpr() sends
 * one packet to a port of the DSP's (a module's instance, the APM 1, the PRM 2) with the given token (0: one of its
 * own) and, with wait_ms, waits for the packet that carries that token back. What is not waited for goes to the
 * handler of events, if there is one and it takes it; it is called on the link's thread and must not wait.
 */
typedef bool (*redwood_gpr_events_t)(uint32_t from_port, uint32_t opcode, uint32_t token, const uint32_t *payload,
    uint32_t length);
bool redwood_adsp_gpr(uint32_t to_port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct redwood_apr_answer *answer);
void redwood_adsp_gpr_events(redwood_gpr_events_t handler);
void redwood_adsp_gpr_port(uint32_t port);      /* the port our packets are from from now on (0: ours, 3, spf_core's) */
#define REDWOOD_GPR_NOT_SENT    0xffffffffu     /* answer->opcode when the packet could not be sent (no room the DSP gave for it) */
/* FastRPC (bsd/dev/redwood_adsp.c): one invocation of `handle' in the DSP's domain `domain', its arguments at `address'
 * as the DSP sees it; true and the DSP's return value when it answers in `ms'. */
bool redwood_adsp_fastrpc(uint32_t domain, uint32_t handle, uint32_t scalars, uint64_t address, uint64_t size, uint32_t ms,
    int32_t *retval);
/* Memory handed by the hypervisor from the virtual machines `from' to `to' (read, write, execute): TrustZone's result. */
int redwood_adsp_assign(uint64_t physical, uint64_t size, const uint32_t *from, unsigned from_count, const uint32_t *to,
    unsigned to_count);
/* assign_mem's lists in an SHM bridge of their own (Linux's qcom_tzmem on SM8650); the last bridge's result, and the
 * enabling's (1: never asked) */
void redwood_adsp_assign_bridge(bool on);
extern int redwood_adsp_bridge_result, redwood_adsp_bridge_enable_result;
void shennong_sound_dsp_gone(void);     /* the DSP is shut down: what was handed to it is taken back (shennong_sound.c) */
void shennong_sound_dsp_up(void);       /* the DSP's audio framework is up (adsp_keep=1): boot-arg sound=N begins (shennong_sound.c) */
void shennong_sound_dsp_down(void);     /* before the DSP is shut down at the run's end: the graph, the clock and the pins put back */
void shennong_mic_dsp_up(void);         /* the DSP's audio framework is up: boot-arg mic=N begins (shennong_mic.c) */
void shennong_mic_dsp_down(void);       /* before the DSP is shut down: what the microphones asked of it given back */

/*
 * QRTR ports of our own with the audio DSP (bsd/dev/redwood_adsp.c), for a client of one of its QMI services: the
 * datagrams to `port' go to `input', on the link's thread (it copies and does not wait); the service's place as the
 * DSP told of it; one datagram sent (0, or why not).
 */
typedef void (*redwood_qrtr_input_t)(uint32_t to_port, uint32_t from_node, uint32_t from_port, const uint8_t *data,
    uint32_t length);
bool redwood_adsp_qrtr_port(uint32_t port, redwood_qrtr_input_t input);
bool redwood_adsp_qrtr_service(uint32_t service, uint32_t *node, uint32_t *port);
int redwood_adsp_qrtr_send(uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length);
void shennong_sensors_dsp_up(void);     /* the DSP is up and QRTR spoken (gyro=N): the motion sensors begin (shennong_sensors.c) */
void shennong_sensors_dsp_down(void);   /* before the DSP is shut down: the sensors' streams stopped */
/* FastRPC without waiting (bsd/dev/redwood_adsp.c): one invocation sent as process `pid', its context's low bits the
 * DSP's domain; the answers whose context has `domain' in its low bits go to `handler' (on the link's thread). */
typedef void (*redwood_fastrpc_answer_t)(uint64_t context, int32_t retval);
int redwood_adsp_fastrpc_send(int32_t pid, int32_t tid, uint64_t context, uint32_t handle, uint32_t scalars, uint64_t address,
    uint64_t size);
void redwood_adsp_fastrpc_answers(uint32_t domain, redwood_fastrpc_answer_t handler);
void shennong_snsrpc_dsp_up(void);      /* the DSP is up and linked: boot-arg snsrpc=N, its sensors' files over FastRPC (shennong_snsrpc.c) */
void shennong_snsrpc_wait(void);        /* ... before the sensors' lookups: until the files have been read */
int32_t shennong_audiorpc_start(uint64_t physical, uint64_t bytes);     /* "audiopd" made with that heap, its files served (shennong_audiorpc.c) */
void shennong_audiorpc_dsp_down(void);  /* ... before the DSP is shut down */
void shennong_snsrpc_dsp_down(void);    /* ... before the DSP is shut down */
uint32_t shennong_snsrpc_quiet_ms(void);        /* ... how long nothing has been asked for */
#endif

#endif /* _SYS_REDWOOD_AUDIO_H_ */
