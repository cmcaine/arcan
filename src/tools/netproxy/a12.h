/*
 A12, Arcan Line Protocol implementation

 Copyright (c) 2017-2019, Bjorn Stahl
 All rights reserved.

 Redistribution and use in source and binary forms,
 with or without modification, are permitted provided that the
 following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
 this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 3. Neither the name of the copyright holder nor the names of its contributors
 may be used to endorse or promote products derived from this software without
 specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef HAVE_A12
#define HAVE_A12

struct a12_state;

/*
 * the encryption related options need to be the same for both server-
 * and client- side or the communication will fail regardless of key validity.
 */
struct pk_response {
	bool valid;
	uint8_t key[64];
};

struct a12_context_options {
/* Provide to enable asymetric key authentication, set valid in the return to
 * allow the key, otherwise the session may be continued for a random number of
 * time or bytes before being terminated. Keymaterial in response is used by
 * the server side and can be */
	struct pk_response (*pk_lookup)(uint8_t pk[static 32]);

/* default is to add a round-trip and use an ephemeral public key to transfer
 * the real one, forces active MiM in order for an attacker to track Pk
 * (re-)use. */
	bool disable_ephemeral_k;

/* filled in using a12_plain_kdf used for the cipher and MAC before asymmetric
 * exchange has been performed. If disable_authenticity is set and no pk_lookup
 * is provided, everything will be over plaintext for debugging and trusted
 * networks */
	uint8_t authk[64];
	bool disable_authenticity;
};

/*
 * Takes a low entropy secret and generate a salted authentication key used
 * for the first key- exchange upon connection. If no shared secret is provided
 * the default 'SETECASTRONOMY' is used. The key is only used to authenticate
 * the first public key in place of a preauthenticated public key from a
 * previous session or a UI based pk_lookup implementation.
 */
void a12_plain_kdf(const char* ssecret, struct a12_context_options* dst);

/*
 * Use in place of malloc/free on struct a12_context_options and other
 * structs that may act as temporary store for keymaterial.
 *
 * Will attempt to:
 * 1. allocate in memory marked as excluded from crash dumps
 * 2. overwrite on free
 *
 * This is not sufficient as the data might taint into registers and so on,
 * but there still are not any reliable mechanisms for achieving this.
 */
void* a12_sensitive_alloc(size_t nb);
void a12_sensitive_free(void*, size_t nb);

/*
 * begin a new session (connect)
 */
struct a12_state* a12_open(struct a12_context_options*);

/*
 * being a new session (accept)
 */
struct a12_state* a12_build(struct a12_context_options*);

/*
 * Free the state block
 * This will return false if there are still mapped/active channels
 */
bool
a12_free(struct a12_state*);

/*
 * Take an incoming byte buffer and append to the current state of
 * the channel. Any received events will be pushed via the callback.
 */
void a12_unpack(
	struct a12_state*, const uint8_t*, size_t, void* tag, void (*on_event)
		(struct arcan_shmif_cont* wnd, int chid, struct arcan_event*, void*));

/*
 * Set the specified context as the recipient of audio/video buffers
 * for a specific channel id.
 */
void a12_set_destination(
	struct a12_state*, struct arcan_shmif_cont* wnd, uint8_t chid);

/*
 * Set the active channel used for tagging outgoing packages
 */
void a12_set_channel(struct a12_state* S, uint8_t chid);

/*
 * Returns the number of bytes that are pending for output on the channel,
 * this needs to be rate-limited by the caller in order for events and data
 * streams to be interleaved and avoid a poor user experience.
 *
 * Unless flushed >often< in response to unpack/enqueue/signal, this will grow
 * and buffer until there's no more data to be had. Internally, a12 n-buffers
 * and a12_flush act as a buffer step. The typical use is therefore:
 *
 * 1. [build state machine, open or accept]
 * while active:
 * 2. [if output buffer set, write to network channel]
 * 3. [enqueue events, add audio/video buffers]
 * 4. [if output buffer empty, a12_flush] => buffer size
 *
 * [allow_blob] behavior depends on value:
 *
 *    A12_FLUSH_NOBLOB : ignore all queued binary blobs
 *    A12_FLUSH_CHONLY : blocking (font/state) transfers for the current channel
 *    A12_FLUSH_ALL    : any pending data blobs
 *
 * These should be set when there are no audio/video frames from the source that
 * should be prioritised, and when the segment on the channel is in the preroll
 * state.
 */
enum a12_blob_mode {
	A12_FLUSH_NOBLOB = 0,
	A12_FLUSH_CHONLY,
	A12_FLUSH_ALL
};
size_t
a12_flush(struct a12_state*, uint8_t**, int allow_blob);

/*
 * Add a data transfer object to the active outgoing channel. The state machine
 * will duplicate the descriptor in [fd]. These will not necessarily be
 * transfered in order or immediately, but subject to internal heuristics
 * depending on current buffer pressure and bandwidth.
 *
 * For streaming descriptor types (non-seekable), size can be 0 and the stream
 * will be continued until the data source dies or the other end cancels.
 *
 * A number of subtle rules determine which order the binary streams will be
 * forwarded, so that a blob transfer can be ongoing for a while without
 * blocking interactivity due to an updated font and so on.
 *
 * Therefore, the incoming order of descrevents() may be different from the
 * outgoing one. For the current and expected set of types, this behavior is
 * safe, but something to consider if the need arises to add additional ones.
 *
 * This function will not immediately cause any data to be flushed out, but
 * rather checked whenever buffers are being swapped and appended as size
 * permits. There might also be, for instance, a ramp-up feature for fonts so
 * that the initial blocks might have started to land on the other side, and if
 * the transfer is not cancelled due to a local cache, burst out.
 */
enum a12_bstream_type {
	A12_BTYPE_STATE = 0,
	A12_BTYPE_FONT = 1,
	A12_BTYPE_FONT_SUPPL = 2,
	A12_BTYPE_BLOB = 3
};
void
a12_enqueue_bstream(
	struct a12_state*, int fd, int type, bool streaming, size_t sz);

/*
 * Get a status code indicating the state of the connection.
 *
 * <0 = dead/erroneous state
 *  0 = inactive (waiting for data)
 *  1 = processing (more data needed)
 */
int
a12_poll(struct a12_state*);

/*
 * For sessions that support multiplexing operations for multiple
 * channels, switch the active encoded channel to the specified ID.
 */
void
a12_set_channel(struct a12_state*, uint8_t chid);

/*
 * Enable debug tracing out to a FILE, set mask to the bitmap of
 * message types you are interested in messages from.
 */
enum trace_groups {
/* video system state transitions */
	A12_TRACE_VIDEO = 1,

/* audio system state transitions / data */
	A12_TRACE_AUDIO = 2,

/* system/serious errors */
	A12_TRACE_SYSTEM = 4,

/* event transfers in/out */
	A12_TRACE_EVENT = 8,

/* data transfer statistics */
	A12_TRACE_TRANSFER = 16,

/* debug messages, when patching / developing  */
	A12_TRACE_DEBUG = 32,

/* missing feature warning */
	A12_TRACE_MISSING = 64,

/* memory allocation status */
	A12_TRACE_ALLOC = 128,

/* crypto- system state transition */
	A12_TRACE_CRYPTO = 256,

/* video frame compression/transfer details */
	A12_TRACE_VDETAIL = 512,

/* binary blob transfer state information */
	A12_TRACE_BTRANSFER = 1024,
};
void
a12_set_trace_level(int mask, FILE* dst);

/*
 * forward a vbuffer from shm
 */
enum a12_vframe_method {
	VFRAME_METHOD_NORMAL = 0,
	VFRAME_METHOD_RAW_NOALPHA,
	VFRAME_METHOD_RAW_RGB565,
	VFRAME_METHOD_DPNG,
	VFRAME_METHOD_H264,
	VFRAME_METHOD_FLIF,
	VFRAME_METHOD_AV1
};

enum a12_vframe_compression_bias {
	VFRAME_BIAS_LATENCY = 0,
	VFRAME_BIAS_BALANCED,
	VFRAME_BIAS_QUALITY
};

/*
 * Open ended question here is if it is worth it (practically speaking) to
 * allow caching of various blocks and subregions vs. just normal compression.
 * The case can be made for CURSOR-type subsegments and possibly first frame of
 * a POPUP and some other types.
 */
struct a12_vframe_opts {
	enum a12_vframe_method method;
	enum a12_vframe_compression_bias bias;

	bool variable;
	union {
		float bitrate; /* !variable, Mbit */
		int ratefactor; /* variable (ffmpeg scale) */
	};
};

enum a12_aframe_method {
	AFRAME_METHOD_RAW = 0,
};

struct a12_aframe_opts {
	enum a12_aframe_method method;
};

struct a12_aframe_cfg {
	uint8_t channels;
	uint32_t samplerate;
};

/*
 * Register a handler that deals with binary- transfer cache lookup and
 * storage allocation. The supplied [on_bevent] handler is invoked twice:
 *
 * 1. When the other side has initiated a binary transfer. The type, size
 *    and possible checksum (all may be unknown) is provided.
 *
 * 2. When the transfer has completed or been cancelled.
 *
 * Each channel can only have one transfer in- flight, so it is safe to
 * track the state per-channel and not try to pair multiple transfers.
 *
 * Cancellation will be triggered on DONTWANT / CACHED. Otherwise the
 * new file descriptor will be populated and when the transfer is
 * completed / cancelled, the handler will be invoked again. It is up
 * to the handler to close any descriptor.
 */
enum a12_bhandler_flag {
	A12_BHANDLER_CACHED = 0,
	A12_BHANDLER_NEWFD,
	A12_BHANDLER_DONTWANT
};

enum a12_bhandler_state {
	A12_BHANDLER_CANCELLED = 0,
	A12_BHANDLER_COMPLETED,
	A12_BHANDLER_INITIALIZE
};

struct a12_bhandler_meta {
	enum a12_bhandler_state state;
	enum a12_bstream_type type;
	uint8_t checksum[16];
	uint64_t known_size;
	bool streaming;
	uint8_t channel;
	uint64_t streamid;
	int fd;
	struct arcan_shmif_cont* dcont;
};
struct a12_bhandler_res {
	enum a12_bhandler_flag flag;
	int fd;
};
void
a12_set_bhandler(struct a12_state*,
	struct a12_bhandler_res (*on_bevent)(
		struct a12_state* S, struct a12_bhandler_meta, void* tag),
	void* tag
);

/*
 * The following functions provide data over a channel, each channel corresponds
 * to a subsegment, with the special ID(0) referring to the primary segment. To
 * modify which subsegment actually receives some data, use the a12_set_channel
 * function.
 */

/*
 * Forward an event. Any associated descriptors etc.  will be taken over by the
 * transfer, and it is responsible for closing them on completion.
 *
 * Return:
 * false - congestion, flush some data and try again.
 */
bool
a12_channel_enqueue(struct a12_state*, struct arcan_event*);

/* Encode and transfer an audio frame with a number of samples in the native
 * audio sample format and the samplerate etc. configuration that matches */
void
a12_channel_aframe(
	struct a12_state* S, shmif_asample* buf,
	size_t n_samples,
	struct a12_aframe_cfg cfg,
	struct a12_aframe_opts opts
);

/* Encode and transfer a video frame based on the video buffer structure
 * provided as part of arcan_shmif_server. */
void
a12_channel_vframe(
	struct a12_state* S,
	struct shmifsrv_vbuffer* vb,
	struct a12_vframe_opts opts
);

/*
 * Forward / start a new channel intended for the 'real' client. If this
 * comes as a NEWSEGMENT event from the 'real' arcan instance, make sure
 * to also tie this to the segment via 12_channel_set_destination.
 *
 * [chid] is the assigned channel ID for the connection.
 * [output] is set to true for an output segment (server populates buffer)
 *          and is commonly false. (ioev[1].iv)
 * [segkind] matches a possible SEGID (ioev[2].iv)
 * [cookie] is paired to a SEGREQ event from the other side (ioev[3].iv)
 */
void
a12_channel_new(struct a12_state* S,
	uint8_t chid, uint8_t segkind, uint32_t cookie);

/*
 * Send the 'shutdown' command with an optional 'last_words' message,
 * this should be done before the _close and typically matches either
 * the client _drop:ing a segment (shmifsrv- side) or an _EXIT event
 * (shmif-client) side.
 */
void
a12_channel_shutdown(struct a12_state* S, const char* last_words);

/* Close / destroy the active channel, if this is the primary (0) all
 * channels will be closed and the connection terminated */
void
a12_channel_close(struct a12_state*);

/*
 * Cancel the binary stream that is ongoing in a specific channel
 */
void a12_stream_cancel(struct a12_state* S, uint8_t chid);
#endif
