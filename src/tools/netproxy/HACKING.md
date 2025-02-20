# Hacking

This section covers quick notes in using and modifying the code-base. The main
point of interest should be the a12 state machine and its decode and encode
translation units.

## Exporting shmif

The default implementation for this is in 'a12\_helper\_srv.c'

With exporting shmif we have a local shmif-server that listens on a connection
point, translates into a12 and sends over some communication channel to an
a12-server.

This is initiated by the 'a12\_channel\_open call. This takes an optional
authentication key used for preauthenticated setup where both have
performed the key-exchange in advance.

The connection point management is outside of the scope here, see the
arcan\_shmif\_server.h API to the libarcan-shmif-srv library, or the
corresponding a12\_helper\_srv.c

## Importing shmif

The default implementation for this is in 'a12\_helper\_cl.c'.

With importing shmif we have an a12-server that listens for incoming
connections, unpacks and maps into shmif connections. From the perspective of a
local arcan instance, it is just another client.

This is initiated by the 'a12\_channel\_build. This takes an optional
authentication key used for preauthenticated setup where both have
performed the key exchange in advance.

## Unpacking

In both export and import you should have access to a shmif\_cont. This
should be bound to a channel id via:

    a12_set_destination(S, &shmif_cont, 0)

There can only be one context assigned to a channel number, trying to call it
multiple times with the channel ID will replace the context, likely breaking
the internal state of the shmif context.

When data has been received over the communication channel, it needs to be
unpacked into the a12 state machine:

    a12_channel_unpack(S, my_data, number_of_bytes, void_tag, on_event)

The state machine will take care of signalling and modifying the shmif context
as well, but you will want to prove an 'on\_event' handler to intercept event
delivery. This will look like the processing after arcan\_shmif\_dequeue.

    on_event(struct arcan_shmif_cont*, int channel, struct arcan_event*, void_tag)

Forward relevant events into the context by arcan\_shmif\_enqueue:ing into it.

## Output

When the communication is available for writing, check with:

    out_sz = a12_channel_flush(S, &buf);
		if (out_sz)
		   send(buf, out_sz)

Until it no-longer produces any output. The a12 state machine assumes you are
done with the buffer and its contents by the next time you call any a12
function.

## Events

Forwarding events work just like the normal processing of a shmif\_wait or
poll call. Send it to a12\_channel\_enqueue and it will take care of repacking
and forwarding. There are a few events that require special treatment, and that
are those that carry a descriptor pointing to other data as any one channel
can only have a single binary data stream transfer in flight. There is a helper
in arcan\_shmif\_descrevent(ev) that will tell you if it is such an event or not.

If the enqueue call fails, it is likely due to congestion from existing transfers.
When that happens, defer processing and focus on flushing out data.

Some are also order dependent, so you can't reliably forward other data in between:

* FONTHINT : needs to be completed before vframe contents will be correct again
* BCHUNK\_OUT, BCHUNK\_IN : can be interleaved with other non-descriptor events
* STORE, RESTORE : needs to be completed before anything else is accepted
* DEVICEHINT : does not work cross- network

## Audio / Video

Both audio and video may need to be provided by each side depending on segment
type, as the ENCODE/sharing scenario changes the directionality, though it is
decided at allocation time.

The structures for defining audio and video parameters actually come from
the shmif\_srv API, though is synched locally in a12\_int.h. Thus in order
to send a video frame:

    struct shmifsrv_vbuffer vb = shmifsrv_video(shmifsrv_video(client));
    a12_channel_vframe(S, channel, &vb, &(struct a12_vframe_opts){...});

With the vframe-opts carrying hints to the encoder stage, the typical pattern
is to select those based on some feedback from the communication combined with
the type of the segment itself.

## Multiple Channels

One or many communication can, and should, be multiplexed over the same
carrier. Thus, there is a 1:1 relationship between channel id and shmif
contents in use. Since this is a state within the A12 context, use

    a12_channel_setid(S, chid);

Before enqueueing events or video. On the importer side, every-time a
new segment gets allocated, it should be mapped via:

    a12_set_destination(S, shmif_context, chid);

The actual allocation of the channel ids is performed in the server itself
as part of the event unpack stage with a NEWSEGMENT. In the event handler
callback you can thus get an event, but a NULL segment and a channel ID.
That is where to setup the new destination.

Basically just use the tag field in the shmif context to remember chid
and there should not be much more work to it.

Internally, it is quite a headache, as we have '4' different views of
the same action.

1. (opt.) shmif-client sends SEGREQ
2. (opt.) local-server forwards to remote server.
3. (opt.) remote-server sends to remote-arcan.
4. (opt.) remote-arcan maps new subsegment (NEWSEGMENT event).
5. remote-server maps this subsegment, assigns channel-ID. sends command.
6. local-server gets command, converts into NEWSEGMENT event, maps into
   channel.

## Packet construction

Going into the a12 internals, the first the to follow is how a packet is
constructed.

For sending/providing output, first build the appropriate control command.
When such a buffer is finished, send it to the "a12int\_append\_out"
function.

    uint8_t hdr_buf[CONTROL_PACKET_SIZE];
    /* populate hdr_buf, see a12int_vframehdr_build as an example */
    a12int_append_out(S, STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

This will take care of buffering, encryption and updating the authentication
code.

Continue in a similar way with any subpacket types, make sure to chunk output
in reasonably sized chunks so that interleaving of other packet types is
possible. This is needed in order to prevent audio / video from stuttering or
saturating other events.

"a12\_channel\_vframe" is probably the best example of providing output and
sending, since it needs to treat many options, large data and different
encoding schemes.

# Notes / Flaws

* vpts, origo\_ll and alpha flags are not yet covered

* custom timers should be managed locally, so the proxy server will still
  tick etc. without forwarding it remote...

* should we allow session- resume with a timeout? (pair authk in HELO)

# Protocol

This section mostly covers a rough draft of things as they evolve. A more
'real' spec is to be written separately towards the end of the subproject
in an RFC like style and the a12 state machine will be decoupled from the
current shmif dependency.

Each arcan segment correlates to a 'channel' that can be multiplexed over
one of these transports, with a sequence number used as a synchronization
primitive for re-linearization. For each channel, a number of streams can
be defined, each with a unique 32-bit identifier. A stream corresponds to
one binary, audio or video transfer operation. One stream of each type
(i.e. binary, audio and video) can be in flight at the same time.

The first message has the structure of:

 |---------------------|
 | 16 byte MAC         |
 | 8 byte salt (random)|
 |---------------------|
 | 1 byte command-code | | encrypted block
 | command-code data   | |
 |----------------------

Where the cipher-key and authentication-key for the first message is derived
from H(Salt | Key), where key is user-provided low-entropy (password) or the
default 'SETECASTRONOMY'.The command part carries the data needed for actual
key exchange and derivation. Clearly little protection is offered using this
construction alone. Without a proper pre-shared key it only provides trivial
obfuscation, with a proper pre-shared key it reduces DoS effect on keys, but
optionally also allows for a short/first- time public key authentication for
quick setup/sharing without going through a PKI infrastructure or TLS- style
certificates or SSH style 'do you accept this key yes/no' queries.

Each message after the first has the outer structure of :

 |---------------------|
 | 16 byte MAC         |
 |---------------------|  |
 | 8 byte sequence     |  |
 | 1 byte command code |  | encrypted block
 | command-code data   |  |
 |---------------------|  |
 | command- variable   |  |

The MAC for all messages is encrypt-then-MAC using chacha20 as streamcipher
where half counter space is used for client to server (first 8b of the ctr)
and half to the server to client (last 8b of the ctr). A rekey command will
be issued at regular intervals where a new key exchange is issued, deleting
the old one.

The 8-byte LSB sequence number is incremented for each message, and is used
as clock for scheduled rekeying and for a course grained counter estimating
drift.

The command- code can be one out of the following types:

1. control (128b fixed size)
2. event, tied to the format of arcan\_shmif\_evpack()
3. vstream-data
4. astream-data
5. bstream-data

Starting with the control commands, these affect connection status, but is
also used for defining new video/audio and binary streams.

## Control (1)
- [0..7]    last-seen seqnr : uint64
- [8..15]   entropy : uint8[8]
- [16]      channel-id : uint8
- [17]      command : uint8

The last-seen are used both as a timing channel and to determine drift.  If the
two sides start drifting outside a certain window, measures to reduce bandwidth
should be taken, including increasing compression parameters, lowering sample-
and frame- rates, cancelling ongoing frames, merging / delaying input events,
scaling window sizes, ... If they still keep drifting, show a user notice and
destroy the channel. The drift window should also be used with a safety factor
to determine the sequence number at which rekeying occurs.

The entropy contents can be used as input to the mix pool of a local CSPRNG to
balance out the risc of a locally weak entroy pool. The channel ID will have a
zero- value for the first channel, and after negotiation via [define-channel],
specify the channel the command effects. Discard messages to an unused channel
(within reasonable tolerances) as asynch- races can happen with data in-flight
during channel tear-down from interleaving.

### command = 0, hello
- [18]      Version major : uint8 (shmif-version until 1.0)
- [19]      Version minor : uint8 (shmif-version until 1.0)
- [20..27]  IV            : uint64
- [28+ 32]  C25519 Kp     : blob
- [33]      Flags         : uint8

The hello message contains key-material for normal DH-25519, but this effect
is modulated with the flag. Accepted flag values:

1 : direct- exchange : client will wait on the matching hello reply with the
other key material (synchronous).

2: nested- exchange : the public key is ephemeral and generated to avoid
leaking the actual public key (as it might be reused and can be tracked for
identification by an active MiM). This adds another hello- command pair and
round-trip cost with the real key exchange being performed in the inner.
This idea is borrowed from minimaLT.

### command = 1, shutdown
- [18..n] : last\_words : UTF-8

Destroy the segment defined by the header command-channel.
Destroying the primary segment kills all others as well.

### command = 2, define-channel
- [18]     channel-id : uint8
- [19]     type       : uint8
- [20]     direction  : uint8 (0 = seg to srv, 1 = srv to seg)
- [21..24] cookie     : uint32

This corresponds to a slightly altered version of the NEWSEGMENT event,
which should be absorbed and translated in each proxy.

### command = 3, stream-cancel
- [18]     stream-id : uint32
- [19]     code      : uint8

This command carries a 4 byte stream ID, which is the counter shared by all
bstream, vstream and astreams. The code dictates if the cancel is due to the
information being dated or undesired (0), encoded in an unhandled format (1)
or data is already known (cached, 2).

### command - 4, define vstream
- [18..21] : stream-id: uint32
- [22    ] : format: uint8
- [23..24] : surfacew: uint16
- [25..26] : surfaceh: uint16
- [27..28] : startx: uint16 (0..outw-1)
- [29..30] : starty: uint16 (0..outh-1)
- [31..32] : framew: uint16 (outw-startx + framew < outw)
- [33..34] : frameh: uint16 (outh-starty + frameh < outh)
- [35    ] : dataflags: uint8
- [36..39] : length: uint32
- [40..43] : expanded length: uint32
- [44]     : commit: uint8

The format field defines the encoding method applied. Current values are:

 R8G8B8A8 = 0 : raw 8-bit red, green, blue and alpha values
 R8G8B8 = 1 : raw 8-bit red, green and blue values
 RGB565 = 2 : raw 5 bit red, 6 bit green, 5 bit red
 DMINIZ = 3 : DEFLATE packaged block, set as ^ delta from last
 MINIZ =  4 : DEFLATE packaged block

This defines a new video stream frame. The length- field covers how many bytes
that need to be buffered for the data to be decoded. This can be chunked up
into 1..n packages, depending on interleaving and so on.

Commit indicates if this is the final (1) update before the accumulation
buffer can be forwarded without tearing, or if there are more blocks to come.

The length field indicates the number of total bytes for all the payloads
in subsequent vstream-data packets.

### command - 5, define astream
- [18..21] stream-id  : uint32
- [22]     channels   : uint8
- [23]     encoding   : uint8
- [24..25] nsamples   : uint16
- [26..29] rate       : uint32

The format fields determine the size of each sample, multiplied over the
number of samples to get the size of the stream. The field in [22] follows
the table:

### command - 6, define bstream
- [18..21] stream-id   : uint32
- [22..29] total-size  : uint64 (0 on streaming source)
- [30]     stream-type : uint8 (0: state, 1:bchunk, 2: font, 3: font-secondary)
- [31..34] id-token    : uint32 (used for bchunk pairing on _out/_store)
- [35 +16] blake2-hash : blob (0 if unknown)

This defines a new or continued binary transfer stream. The block-size sets the
number of continuous bytes in the stream until the point where another transfer
can be interleaved. There can thus be multiple binary streams in flight in
order to interrupt an ongoing one with a higher priority one.

### command - 7, ping
No extra data needed in the control command, just used as a periodic carrier
to keep the connection alive and measure drift.

### command - 8, rekey
- [0...7] future-seqnr : uint64
- [8..23] new key      : uint8[16]

This command indicate a sequence number in the future outside of the expected
established drift range along with a safety factory. When this sequence number
has been seen, new message and cipher keys will be derived from the new key and
used instead of the old one which is discarded and safely erased.

##  Event (2), fixed length
- [0..7] sequence number : uint64
- [8   ] channel-id      : uint8
- [9+  ] event-data      : special

The event data does not currently have a fixed packing format as the model is
still being refined and thus we use the opaque format from
arcan\_shmif\_eventpack.

## Vstream-data (3), Astream-data (4), Bstream-data (5) (variable length)
- [0   ] channel-id : uint8
- [1..4] stream-id  : uint32
- [5..6] length     : uint16

The data messages themselves will make out the bulk of communication,
and ties to a pre-defined channel/stream.

# Compressions and Codecs

To get audio/video/data transfers bandwidth efficient, the contents must be
compressed in some way. This is a rats nest of issues, ranging from patents
in less civilized parts of the world to complete dependency and hardware hell.

The current approach to 'feature negotiation' is simply for the sender to
try the best one it can, and fall back to inefficient safe-defaults if the
recipient returns a remark of the frame as unsupported.

# Event Model

The more complicated interactions come from events and their ordering,
especially when trying to interleave or deal with data carrying events.

The only stage this 'should' require special treatment is for font transfers
for client side text rendering which typically only happen before activation.

For other kinds of transfers, state and clipboard, they act as a mask/block
for certain other events. If a state-restore is requested for instance, it
makes no sense trying to interleave input events that assumes state has been
restored.

Input events are the next complication in line as any input event that relies
on a 'press-hold-release' pattern interpreted on the other end may be held for
too long, or with clients that deal with raw codes and repeats, cause
extraneous key-repeats or 'shadow releases'. To minimize the harm here, a more
complex state machine will be needed that tries to determine if the channel
blocks or not by relying on a ping-stream.
