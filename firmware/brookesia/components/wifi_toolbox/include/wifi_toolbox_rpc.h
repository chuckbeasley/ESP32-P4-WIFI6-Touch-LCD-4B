/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — custom-RPC protocol shared by the P4 host and the C6
 * co-processor.
 *
 * Transport: esp-hosted peer-data transfer, which carries opaque
 * (msg_id, bytes) over the Custom RPC family. Custom_RPC IDs only exist in the
 * Rpc proto family, so both sides need their peer-data feature enabled:
 *
 *   host (P4)   CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA=y
 *               CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA_MAX_CUSTOM_MSG_HANDLERS=8
 *   slave (C6)  CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y
 *
 * Why a custom channel at all: the two operations this toolbox exists for are
 * not carried by esp-hosted's Wi-Fi RPC — measured, both
 * esp_wifi_set_promiscuous* and esp_wifi_80211_tx() return
 * ESP_ERR_NOT_SUPPORTED from the host (see M0-FINDINGS.md). They have to run on
 * the C6, and this is how the host drives and observes them.
 *
 * Message IDs are sent as uint32_t. 0xFFFFFFFF is reserved by esp-hosted and
 * must not be used. Host->slave uses the 0x7000 block, slave->host the 0x7100
 * block, so the two directions can never be confused for one another.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on every wire-format change. A v2 host and a v1 slave must refuse each
 * other rather than mis-parse; both sides check this at PING.
 *
 * v2 added the capture session. v3 collapsed the reply messages into a single
 * status report — see the note on WIFI_TOOLBOX_MSG_CMD_STATUS. v4 added the power
 * save lease and channel hopping. */
#define WIFI_TOOLBOX_RPC_VERSION   (4)

/* ---- Injection modes ----------------------------------------------------
 *
 * Part of the protocol, not of the frame builders: the host names a mode and the
 * co-processor builds it, so both sides must agree on the numbering. The enum
 * therefore lives here, in the shared header, and the C6-side frame builders
 * include this file rather than declaring their own copy.
 *
 * The Tactility UI offers ten modes; Karma is absent here because the original's
 * Karma only broadcasts probe responses from the inject timer and real
 * per-station Karma needs the receive path, which is live only during a capture.
 * It returns when capture and injection can run together.
 */
typedef enum {
    WIFI_TOOLBOX_INJECT_BEACON_SPAM = 0,
    WIFI_TOOLBOX_INJECT_PROBE_FLOOD,
    WIFI_TOOLBOX_INJECT_DEAUTH_BURST,
    WIFI_TOOLBOX_INJECT_AUTH_FLOOD,
    WIFI_TOOLBOX_INJECT_ASSOC_FLOOD,
    WIFI_TOOLBOX_INJECT_ASSOC_SLEEP,
    WIFI_TOOLBOX_INJECT_DISASSOC,
    WIFI_TOOLBOX_INJECT_CLIENT_DEAUTH,
    WIFI_TOOLBOX_INJECT_NAV_DURATION,
    WIFI_TOOLBOX_INJECT_MODE_MAX
} wifi_toolbox_inject_mode_t;

/* Modes the driver will drop without the libnet80211 raw-frame patch: the
 * sanity check rejects deauth/dissoc specifically ("unsupport frame type: 0c0")
 * while beacon and probe still transmit. The host uses this to refuse these
 * modes up front instead of failing at Start. */
static inline int wifi_toolbox_mode_needs_raw_patch(wifi_toolbox_inject_mode_t mode)
{
    return (mode == WIFI_TOOLBOX_INJECT_DEAUTH_BURST) ||
           (mode == WIFI_TOOLBOX_INJECT_DISASSOC) ||
           (mode == WIFI_TOOLBOX_INJECT_CLIENT_DEAUTH);
}

/* ---- Frame classification ----------------------------------------------
 *
 * What the capture classifier can say about one frame, as bit flags: one frame
 * can be several things at once (a data frame carrying EAPOL is also a data
 * frame, and the counters overlap by design).
 *
 * These bits live in the protocol header because both sides name them: the
 * co-processor computes them, the host builds a capture filter out of them and
 * reads the low byte back in every frame record's `flags`. Duplicating the
 * numbering on the host is exactly the drift this file exists to prevent.
 */
typedef enum {
    WIFI_TOOLBOX_FRAME_OTHER     = 0,
    WIFI_TOOLBOX_FRAME_EAPOL     = 1u << 0,
    WIFI_TOOLBOX_FRAME_PMKID     = 1u << 1,
    WIFI_TOOLBOX_FRAME_DEAUTH    = 1u << 2,
    WIFI_TOOLBOX_FRAME_DISASSOC  = 1u << 3,
    WIFI_TOOLBOX_FRAME_BEACON    = 1u << 4,
    WIFI_TOOLBOX_FRAME_PROBE_REQ = 1u << 5,
} wifi_toolbox_frame_class_t;

/* ---- Message IDs --------------------------------------------------------
 *
 * The command set is capped at EIGHT IDs, and that is a hardware-ish limit rather
 * than a style choice.
 *
 * esp-hosted's peer-data feature owns a fixed-size callback table
 * (EH_CP_FEAT_PEER_DATA_MAX_HANDLERS). In the shipped 3.0.9 component that table
 * is hard-coded to 8, because the Kconfig symbol meant to size it is not defined
 * anywhere in the component. A registration past the end fails at REGISTRATION
 * time, on the co-processor, with nothing but a log line on a console that is not
 * wired to the host. From the host the symptom is a command that never answers —
 * indistinguishable from a dropped link. That is exactly how the capture-stats and
 * radio-info messages were lost in round 4.
 *
 * So the surface is deliberately small: five commands that change state, and this
 * one that reports it. Where a command would have needed its own ID, the mode is a
 * FIELD instead — ARM carries the mode it opens the gate for, START carries the
 * session it starts, and both directions of the report carry one status block, so
 * a caller always learns state, radio and counters together.
 *
 * wifi_toolbox_rpc.c asserts the count at compile time, so adding a ninth fails
 * the build rather than silently dropping a command in the field.
 */
#define WIFI_TOOLBOX_MSG_CMD_PING        (0x7000u)  /* liveness + capability   */
#define WIFI_TOOLBOX_MSG_CMD_SELFTEST    (0x7001u)  /* build frames, return them */
#define WIFI_TOOLBOX_MSG_CMD_ARM         (0x7002u)  /* authorisation gate      */
#define WIFI_TOOLBOX_MSG_CMD_DISARM      (0x7003u)  /* clear gate, stop all    */
#define WIFI_TOOLBOX_MSG_CMD_START       (0x7004u)  /* open a capture or injection session */
#define WIFI_TOOLBOX_MSG_CMD_STOP        (0x7005u)  /* end whatever is running */
#define WIFI_TOOLBOX_MSG_CMD_STATUS      (0x7006u)  /* state + radio + counters */
#define WIFI_TOOLBOX_MSG_CMD_CONTROL     (0x7007u)  /* power-save lease and channel */

#define WIFI_TOOLBOX_CMD_COUNT           (8u)

/* Slave -> host (responses and events). */
#define WIFI_TOOLBOX_MSG_RSP_PING        (0x7100u)
#define WIFI_TOOLBOX_MSG_RSP_SELFTEST    (0x7101u)
#define WIFI_TOOLBOX_MSG_EVT_STATE       (0x7102u)  /* session state changes   */
#define WIFI_TOOLBOX_MSG_EVT_FRAME       (0x7104u)  /* a captured frame        */
#define WIFI_TOOLBOX_MSG_EVT_CMD_RESULT  (0x7105u)  /* outcome of a command    */
#define WIFI_TOOLBOX_MSG_EVT_STATUS      (0x7106u)  /* the status block, pushed */
#define WIFI_TOOLBOX_MSG_RSP_STATUS      (0x7107u)  /* the status block, asked  */
#define WIFI_TOOLBOX_MSG_RSP_CONTROL     (0x7108u)  /* power-save / channel    */

/*
 * Outcome of a command that has no richer reply. Without this the host cannot
 * tell "the co-processor refused, here is why" from "the co-processor never got
 * it" — both look like silence, and the slave's console is not visible from the
 * host (only SDIO is wired).
 */
typedef struct __attribute__((packed)) {
    uint32_t cmd_id;         /* the WIFI_TOOLBOX_MSG_CMD_* being answered     */
    uint8_t  result;         /* wifi_toolbox_result_t                         */
    uint8_t  reserved[3];
} wifi_toolbox_cmd_result_t;

/* ---- Result codes ------------------------------------------------------- */

typedef enum {
    WIFI_TOOLBOX_OK = 0,
    WIFI_TOOLBOX_ERR_UNKNOWN_CMD,
    WIFI_TOOLBOX_ERR_BAD_PARAM,
    WIFI_TOOLBOX_ERR_NOT_ARMED,
    WIFI_TOOLBOX_ERR_RADIO_BUSY,
    WIFI_TOOLBOX_ERR_UNSUPPORTED,
    WIFI_TOOLBOX_ERR_TX_FAILED,
    WIFI_TOOLBOX_ERR_INTERNAL,
} wifi_toolbox_result_t;

/* ---- Ping / capability -------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint16_t rpc_version;
    uint8_t  reserved[2];
} wifi_toolbox_ping_req_t;

/*
 * Capability report. The UI needs this before offering a mode: the spec requires
 * "this co-processor cannot inject" as a first-class answer rather than a
 * failure at Start.
 *
 * `injection_available` is false when the co-processor firmware was built
 * without the toolbox (spec section 10.5: the shipped image can compile
 * injection out, so a host-side flag alone would be a false sense of safety),
 * and `raw_frame_patch` reports whether the libnet80211 sanity check has been
 * relaxed — without it deauth/disassoc are dropped by the driver while beacon
 * and probe still transmit, which is exactly the sort of half-working state the
 * UI must not present as "working".
 */
typedef struct __attribute__((packed)) {
    uint16_t rpc_version;
    uint16_t fw_version;
    uint8_t  injection_available;
    uint8_t  capture_available;
    uint8_t  raw_frame_patch;
    uint8_t  mode_count;
    uint8_t  max_tx_power_dbm;
    uint8_t  country_code[3];
    uint8_t  reserved[5];
} wifi_toolbox_caps_t;

/* ---- Arming gate --------------------------------------------------------
 *
 * Spec section 10.2: injection and deauth elicitation require an explicit
 * arming step. The gate is enforced on the CO-PROCESSOR, not just in the host
 * UI — the host is the thing being guarded against, so a host-side check alone
 * would be decoration.
 *
 * `acknowledged` must be set by the caller to affirm it is authorised to test
 * the target network. The flag is RAM-only: nothing resumes after a reboot, and
 * a link re-establishment clears it (spec section 10.3).
 */
typedef struct __attribute__((packed)) {
    uint8_t acknowledged;
    uint8_t mode;            /* wifi_toolbox_arm_mode_t                       */
    uint8_t reserved[2];
} wifi_toolbox_arm_req_t;

typedef enum {
    WIFI_TOOLBOX_ARM_CAPTURE = 0,
    WIFI_TOOLBOX_ARM_INJECT,
} wifi_toolbox_arm_mode_t;

/* ---- Injection limits --------------------------------------------------- */

/* Hard ceiling the slave enforces itself. The spec requires the duration to be
 * mandatory and bounded, and the slave to stop on its own timer even if the host
 * stops answering — so this is clamped on the co-processor, not trusted from the
 * host. The request shape itself is wifi_toolbox_inject_params_t, a field of the
 * single START request. */
#define WIFI_TOOLBOX_INJECT_MAX_DURATION_MS   (600000u)  /* 10 minutes */
#define WIFI_TOOLBOX_INJECT_MIN_DURATION_MS   (1000u)

/* ---- One START request, one session kind --------------------------------
 *
 * Capture and injection are separate sessions on one radio, so the command that
 * opens one carries which. Keeping the two request shapes as fields of one struct
 * rather than two message IDs is what lets the command surface fit the peer-data
 * table (see the note on the message IDs).
 *
 * `req_version` must equal WIFI_TOOLBOX_START_REQ_VERSION. It exists because this
 * struct is an ABI: the host and the co-processor are flashed separately, and a
 * mismatch must be refused rather than reinterpreted as whatever the bytes happen
 * to mean in the other layout.
 */
#define WIFI_TOOLBOX_START_REQ_VERSION  (1u)

typedef enum {
    WIFI_TOOLBOX_SESSION_CAPTURE = 0,
    WIFI_TOOLBOX_SESSION_INJECT  = 1,
} wifi_toolbox_session_t;

/* Capture fields, used when session == CAPTURE. */
typedef struct __attribute__((packed)) {
    uint32_t filter_mask;
    uint32_t elicit_ms;      /* ask for a deauth burst this often; 0 = never   */
    uint8_t  hop;            /* 1 = hop the band, 0 = stay on `channel`        */
    uint8_t  channel;
    uint16_t hop_dwell_ms;
    uint8_t  target_bssid[6];/* elicitation target; all zero = none            */
    uint8_t  eliciting;      /* out: the slave is eliciting                    */
    uint8_t  reserved[1];
} wifi_toolbox_capture_params_t;

/* Injection fields, used when session == INJECT. */
typedef struct __attribute__((packed)) {
    uint16_t duration_ms_lo;
    uint16_t duration_ms_hi;
    uint16_t listen_interval;
    uint16_t nav_duration;
    uint8_t  mode;           /* wifi_toolbox_inject_mode_t                     */
    uint8_t  channel;
    uint8_t  bssid[6];
    uint8_t  client[6];
    uint8_t  ssid_len;
    uint8_t  reserved[1];
    char     ssid[32];
} wifi_toolbox_inject_params_t;

typedef struct __attribute__((packed)) {
    uint16_t req_version;
    uint8_t  session;        /* wifi_toolbox_session_t                         */
    uint8_t  reserved;
    union {
        wifi_toolbox_capture_params_t capture;
        wifi_toolbox_inject_params_t  inject;
    } u;
} wifi_toolbox_start_req_t;

static inline uint32_t wifi_toolbox_inject_req_duration_ms(const wifi_toolbox_inject_params_t *r)
{
    return ((uint32_t)r->duration_ms_hi << 16) | (uint32_t)r->duration_ms_lo;
}

/* ---- CONTROL: power save and channel, neither of which the host can do ---
 *
 * Power save: the spec puts save/restore in the host's session handler, but every
 * esp_wifi_remote_* entry point in this tree is a weak UNSUPPORTED stub —
 * esp_wifi_set_ps and esp_wifi_get_ps included. The host cannot read the current
 * value, let alone restore it, so the lease lives where the radio is.
 *
 * Channel: same story. The host cannot tune the radio; it can only ask, and the
 * answer depends on state (association) it cannot see either.
 */
typedef enum {
    WIFI_TOOLBOX_CONTROL_PS_TAKE    = 0,  /* remember power save, then disable  */
    WIFI_TOOLBOX_CONTROL_PS_RELEASE = 1,  /* put back what was remembered       */
    WIFI_TOOLBOX_CONTROL_SET_CHANNEL = 2, /* fix the channel                    */
    WIFI_TOOLBOX_CONTROL_HOP_START  = 3,  /* hand tuning to the slave's hopper  */
    WIFI_TOOLBOX_CONTROL_HOP_STOP   = 4,
    WIFI_TOOLBOX_CONTROL_ELICIT     = 5,  /* send the deauth burst now          */
} wifi_toolbox_control_op_t;

typedef struct __attribute__((packed)) {
    uint8_t  op;             /* wifi_toolbox_control_op_t                      */
    uint8_t  channel;        /* SET_CHANNEL                                    */
    uint16_t hop_dwell_ms;   /* HOP_START                                      */
    uint8_t  disable;        /* PS_TAKE: 1 = force power save off              */
    uint8_t  bssid[6];       /* ELICIT: target                                 */
    uint8_t  reserved[1];
} wifi_toolbox_control_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  result;         /* wifi_toolbox_result_t                          */
    uint8_t  ps_held;
    uint8_t  ps_holders;     /* reference count; the host is one of them        */
    uint8_t  ps_saved;       /* the remembered value is meaningful              */
    uint8_t  ps_saved_type;  /* wifi_ps_type_t                                 */
    uint8_t  ps_current_type;
    uint8_t  channel;        /* the channel now in force, 0 when hopping        */
    uint8_t  hopping;
} wifi_toolbox_control_rsp_t;

/* ---- Capture filter -----------------------------------------------------
 *
 * The host names a filter and a channel; the co-processor decides which frames
 * are worth sending. That split is deliberate (spec section 3.3): sending every
 * captured frame over SDIO would spend the transport and the host's cycles on
 * frames nobody asked for, and at 6 Mbps of promiscuous traffic it would also
 * starve the station link sharing the same SDIO bus.
 *
 * The filter therefore lives in the protocol rather than in the host: the bits
 * match wifi_toolbox_frame_class_t (WIFI_TOOLBOX_FRAME_*) plus the category bits
 * below. Every frame the callback classifies as interesting is queued on the
 * slave and streamed up as EVT_FRAME; everything else is counted and dropped
 * where it arrives.
 */

/* Category bits, OR-ed into the same 32-bit mask as the classification bits. */
#define WIFI_TOOLBOX_CAP_FILTER_ALL_MGMT   (1u << 16)  /* every management frame */
#define WIFI_TOOLBOX_CAP_FILTER_ALL_DATA   (1u << 17)  /* every data frame       */
#define WIFI_TOOLBOX_CAP_FILTER_ALL        (0xFFFFFFFFu)

/* EAPOL, PMKID candidates, deauth and disassoc: the frames the PMKID/EAPOL tool
 * exists to collect. Beacons and probe requests are excluded by default because
 * on a busy channel they are the bulk of the traffic and the host has no use for
 * them unless the monitor view is open. */
#define WIFI_TOOLBOX_CAP_FILTER_DEFAULT \
    ((uint32_t)(WIFI_TOOLBOX_FRAME_EAPOL | WIFI_TOOLBOX_FRAME_PMKID | \
                WIFI_TOOLBOX_FRAME_DEAUTH | WIFI_TOOLBOX_FRAME_DISASSOC))

/* Longest frame body accepted into the stream. Frames longer than this are
 * clipped (and counted as clipped) rather than dropped, so the host sees a
 * truncated record instead of a hole. */
#define WIFI_TOOLBOX_CAP_FRAME_MAX   (1600u)

/* Channels the hopper visits, in order. The regulatory list belongs to the
 * co-processor's country configuration (spec section 12.8), so the host names
 * neither the band nor the channels — it asks to hop and the slave decides where. */
#define WIFI_TOOLBOX_HOP_CHANNEL_MIN   (1u)
#define WIFI_TOOLBOX_HOP_CHANNEL_MAX   (13u)
#define WIFI_TOOLBOX_HOP_DWELL_MS_MIN  (100u)
#define WIFI_TOOLBOX_HOP_DWELL_MS_MAX  (2000u)
#define WIFI_TOOLBOX_HOP_DWELL_MS_DEF  (250u)

/* How many frames are waiting to cross SDIO, and how many will never arrive. A
 * host that shows a capture as healthy while `dropped_ring_full` climbs is
 * lying, so both counters are in the summary rather than in a log line. */
typedef struct __attribute__((packed)) {
    uint8_t  state;             /* wifi_toolbox_state_t                      */
    uint8_t  channel;
    uint16_t reserved16;
    uint32_t packets_seen;      /* everything the callback was handed        */
    uint32_t frames_matched;    /* classified as interesting                 */
    uint32_t frames_sent;       /* handed to the transport                   */
    uint32_t frames_dropped;    /* sum of the two reasons below              */
    uint32_t dropped_ring_full; /* matched, but the ring had no room         */
    uint32_t transport_failed;  /* handed to the transport, which refused    */
    uint32_t frames_clipped;    /* longer than CAP_FRAME_MAX                 */
    uint32_t bytes_sent;
    uint32_t queued;            /* in flight right now                       */
    uint32_t max_queued;
    uint32_t elicits;           /* deauth bursts sent to force a handshake    */
    uint32_t last_error;        /* esp_err_t of the last transport failure   */
} wifi_toolbox_capture_stats_t;

/* One captured frame, streamed as WIFI_TOOLBOX_MSG_EVT_FRAME.
 *
 * There is no struct wrapper on the wire: the bytes are this header followed
 * immediately by `sig_len` bytes of 802.11 frame. A variable-length record keeps
 * the transport bill proportional to what is actually captured, which is the
 * whole point of filtering on the slave. */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_us;   /* modem timestamp; see the precision caveat below */
    uint16_t sig_len;        /* frame length as reported, FCS included           */
    uint16_t payload_len;    /* bytes actually following (<= CAP_FRAME_MAX)      */
    int8_t   rssi;           /* dBm                                             */
    uint8_t  channel;
    uint16_t seq;            /* slave-side sequence number, gaps are losses      */
    uint8_t  flags;          /* wifi_toolbox_frame_class_t, low byte            */
    uint8_t  clipped;        /* payload_len < sig_len                           */
    uint8_t  pkt_type;       /* wifi_promiscuous_pkt_type_t                     */
    uint8_t  reserved;
} wifi_toolbox_frame_evt_t;

/* rx_ctrl.timestamp is only precise while modem sleep is disabled, and it is a
 * 32-bit microsecond counter that wraps roughly every 71 minutes. It is carried
 * through for ordering and for the self-capture test; the PCAP writer stamps
 * records from the host's own clock and must not present this as wall time. */

/* ---- Radio state --------------------------------------------------------
 *
 * What the co-processor's radio is actually doing, which the host cannot infer.
 *
 * This exists because of a measured trap. The C6 is not a passive radio: the same
 * driver also serves esp-hosted, so "which channel am I on" and "am I associated"
 * are properties of a link the host does not own. A capture that saw frames on a
 * different channel than the one the host had asked for, with no error on either
 * side, is not something the host can diagnose on its own.
 */
typedef struct __attribute__((packed)) {
    uint8_t  channel;           /* primary channel the radio is on            */
    uint8_t  second;            /* wifi_second_chan_t                         */
    uint8_t  sta_connected;     /* the co-processor has a link                */
    uint8_t  sta_channel;       /* the link's channel, 0 when not connected    */
    int8_t   sta_rssi;          /* dBm, 0 when not connected                  */
    uint8_t  inject_running;
    uint8_t  capture_running;
    uint8_t  armed_mode;        /* 0xFF when disarmed                         */
    uint8_t  hopping;           /* the hopper is running                      */
    uint8_t  reserved[3];
} wifi_toolbox_radio_info_t;

/* ---- Self-test (builder verification on the real target) ----------------
 *
 * Builds one frame per mode on the co-processor and returns the bytes, so the host
 * can byte-compare against the Tactility originals. The builders cannot be
 * validated on the host: the host has no radio to build for.
 */

typedef struct __attribute__((packed)) {
    uint8_t  mode;          /* wifi_toolbox_inject_mode_t to build           */
    uint8_t  reserved[3];
    uint8_t  ssid_len;
    uint8_t  bssid[6];
    uint8_t  client[6];
    char     ssid[32];
} wifi_toolbox_selftest_req_t;

/* One built frame, returned verbatim so the host can compare bytes against the
 * Tactility originals (spec section 11: golden-byte tests). */
typedef struct __attribute__((packed)) {
    uint8_t  mode;
    uint8_t  result;        /* wifi_toolbox_result_t                         */
    uint16_t frame_len;
    uint8_t  frame[128];    /* enough for the longest builder (beacon+32 SSID) */
} wifi_toolbox_selftest_rsp_t;

/* ---- Session state / stats (shared shape) ------------------------------- */

typedef enum {
    WIFI_TOOLBOX_STATE_IDLE = 0,
    WIFI_TOOLBOX_STATE_INJECTING,
    WIFI_TOOLBOX_STATE_CAPTURING,
    WIFI_TOOLBOX_STATE_STOPPED_BY_DURATION,
    WIFI_TOOLBOX_STATE_STOPPED_BY_ERROR,
} wifi_toolbox_state_t;

typedef struct __attribute__((packed)) {
    uint8_t  state;         /* wifi_toolbox_state_t                          */
    uint8_t  mode;
    uint16_t channel;
    uint32_t frames_sent;
    uint32_t frames_failed;
    uint32_t elapsed_ms;
    uint32_t duration_ms;   /* authoritative on the slave: it stops itself   */
    uint8_t  last_error;    /* wifi_toolbox_result_t                         */
    uint8_t  reserved[3];
} wifi_toolbox_stats_t;

/* ---- One status block for everything the host needs to know -------------
 *
 * Sent as the reply to CMD_STATUS and pushed as EVT_STATUS after any command that
 * could have changed it. The two uses carry the identical block, so there is no
 * "which stats message is this" guess at the host — which is what the previous
 * separate IDs forced, and why two of them silently never arrived.
 */
typedef struct __attribute__((packed)) {
    wifi_toolbox_stats_t         inject;
    wifi_toolbox_capture_stats_t capture;
    wifi_toolbox_radio_info_t    radio;
    uint8_t  armed_mode;        /* 0xFF when the gate is shut                  */
    uint8_t  rpc_version;
    uint8_t  fw_version;
    uint8_t  reserved;
    /* Capabilities, reported here as well as by PING.
     *
     * They belong in the status block because the host needs them to render the
     * main screen ("this co-processor cannot inject") and the status block is the
     * one message it already asks for. Making the host do a second round trip on
     * esp-hosted's own RPC-request path just to learn them would be a second way
     * to be wrong, for no benefit. */
    uint16_t caps_flags;        /* see WIFI_TOOLBOX_CAP_*                      */
    uint8_t  mode_count;
    uint8_t  max_tx_power_dbm;
    uint8_t  country_code[3];
    uint8_t  reserved2[5];
} wifi_toolbox_status_t;

/* caps_flags bits, mirroring wifi_toolbox_caps_t's fields. */
#define WIFI_TOOLBOX_CAP_INJECTION   (1u << 0)
#define WIFI_TOOLBOX_CAP_CAPTURE     (1u << 1)
#define WIFI_TOOLBOX_CAP_RAW_PATCH   (1u << 2)

#ifdef __cplusplus
}
#endif
