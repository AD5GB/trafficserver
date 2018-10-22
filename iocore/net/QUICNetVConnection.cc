/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */
#include <climits>
#include <string>

#include "tscore/ink_config.h"
#include "records/I_RecHttp.h"
#include "tscore/Diags.h"

#include "P_Net.h"
#include "InkAPIInternal.h" // Added to include the quic_hook definitions
#include "Log.h"

#include "P_SSLNextProtocolSet.h"
#include "QUICTLS.h"

#include "QUICStats.h"
#include "QUICGlobals.h"
#include "QUICDebugNames.h"
#include "QUICEvents.h"
#include "QUICConfig.h"

#define STATE_FROM_VIO(_x) ((NetState *)(((char *)(_x)) - STATE_VIO_OFFSET))
#define STATE_VIO_OFFSET ((uintptr_t) & ((NetState *)0)->vio)

using namespace std::literals;
static constexpr std::string_view QUIC_DEBUG_TAG = "quic_net"sv;

#define QUICConDebug(fmt, ...) Debug(QUIC_DEBUG_TAG.data(), "[%s] " fmt, this->cids().data(), ##__VA_ARGS__)

#define QUICConVDebug(fmt, ...) Debug("v_quic_net", "[%s] " fmt, this->cids().data(), ##__VA_ARGS__)
#define QUICConVVVDebug(fmt, ...) Debug("vvv_quic_net", "[%s] " fmt, this->cids().data(), ##__VA_ARGS__)

#define QUICFCDebug(fmt, ...) Debug("quic_flow_ctrl", "[%s] " fmt, this->cids().data(), ##__VA_ARGS__)

#define QUICError(fmt, ...)                                           \
  Debug("quic_net", "[%s] " fmt, this->cids().data(), ##__VA_ARGS__); \
  Error("quic_net [%s] " fmt, this->cids().data(), ##__VA_ARGS__)

static constexpr uint32_t IPV4_HEADER_SIZE          = 20;
static constexpr uint32_t IPV6_HEADER_SIZE          = 40;
static constexpr uint32_t UDP_HEADER_SIZE           = 8;
static constexpr uint32_t MAX_PACKET_OVERHEAD       = 62; // Max long header len without length of token field of Initial packet
static constexpr uint32_t MAX_STREAM_FRAME_OVERHEAD = 24;
// static constexpr uint32_t MAX_CRYPTO_FRAME_OVERHEAD   = 16;
static constexpr uint32_t MINIMUM_INITIAL_PACKET_SIZE = 1200;
static constexpr ink_hrtime WRITE_READY_INTERVAL      = HRTIME_MSECONDS(20);
static constexpr uint32_t PACKET_PER_EVENT            = 32;
static constexpr uint32_t MAX_CONSECUTIVE_STREAMS     = 8; //< Interrupt sending STREAM frames to send ACK frame

static constexpr uint32_t MAX_PACKETS_WITHOUT_SRC_ADDR_VARIDATION = 3;

static constexpr uint32_t STATE_CLOSING_MAX_SEND_PKT_NUM  = 8; // Max number of sending packets which contain a closing frame.
static constexpr uint32_t STATE_CLOSING_MAX_RECV_PKT_WIND = 1 << STATE_CLOSING_MAX_SEND_PKT_NUM;

ClassAllocator<QUICNetVConnection> quicNetVCAllocator("quicNetVCAllocator");

// XXX This might be called on ET_UDP thread
void
QUICNetVConnection::init(QUICConnectionId peer_cid, QUICConnectionId original_cid, UDPConnection *udp_con,
                         QUICPacketHandler *packet_handler, QUICConnectionTable *ctable)
{
  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::acceptEvent);
  this->_packet_transmitter_mutex    = new_ProxyMutex();
  this->_frame_transmitter_mutex     = new_ProxyMutex();
  this->_udp_con                     = udp_con;
  this->_packet_handler              = packet_handler;
  this->_peer_quic_connection_id     = peer_cid;
  this->_original_quic_connection_id = original_cid;
  this->_quic_connection_id.randomize();
  // PacketHandler for out going connection doesn't have connection table
  if (ctable) {
    this->_ctable = ctable;
    this->_ctable->insert(this->_quic_connection_id, this);
    this->_ctable->insert(this->_original_quic_connection_id, this);
  }

  this->_update_cids();

  if (is_debug_tag_set(QUIC_DEBUG_TAG.data())) {
    char dcid_hex_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    char scid_hex_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    this->_peer_quic_connection_id.hex(dcid_hex_str, QUICConnectionId::MAX_HEX_STR_LENGTH);
    this->_quic_connection_id.hex(scid_hex_str, QUICConnectionId::MAX_HEX_STR_LENGTH);

    QUICConDebug("dcid=%s scid=%s", dcid_hex_str, scid_hex_str);
  }
}

bool
QUICNetVConnection::shouldDestroy()
{
  return this->refcount() == 0;
}

VIO *
QUICNetVConnection::do_io_read(Continuation *c, int64_t nbytes, MIOBuffer *buf)
{
  ink_assert(false);
  return nullptr;
}

VIO *
QUICNetVConnection::do_io_write(Continuation *c, int64_t nbytes, IOBufferReader *buf, bool owner)
{
  ink_assert(false);
  return nullptr;
}

int
QUICNetVConnection::acceptEvent(int event, Event *e)
{
  EThread *t    = (e == nullptr) ? this_ethread() : e->ethread;
  NetHandler *h = get_NetHandler(t);

  MUTEX_TRY_LOCK(lock, h->mutex, t);
  if (!lock.is_locked()) {
    if (event == EVENT_NONE) {
      t->schedule_in(this, HRTIME_MSECONDS(net_retry_delay));
      return EVENT_DONE;
    } else {
      e->schedule_in(HRTIME_MSECONDS(net_retry_delay));
      return EVENT_CONT;
    }
  }

  thread = t;

  // Send this NetVC to NetHandler and start to polling read & write event.
  if (h->startIO(this) < 0) {
    free(t);
    return EVENT_DONE;
  }

  // FIXME: complete do_io_xxxx instead
  this->read.enabled = 1;

  // Handshake callback handler.
  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_pre_handshake);

  // Send this netvc to InactivityCop.
  nh->startCop(this);

  if (inactivity_timeout_in) {
    set_inactivity_timeout(inactivity_timeout_in);
  } else {
    set_inactivity_timeout(0);
  }

  if (active_timeout_in) {
    set_active_timeout(active_timeout_in);
  }

  action_.continuation->handleEvent(NET_EVENT_ACCEPT, this);
  this->_schedule_packet_write_ready();

  return EVENT_DONE;
}

int
QUICNetVConnection::startEvent(int event, Event *e)
{
  ink_assert(event == EVENT_IMMEDIATE);
  MUTEX_TRY_LOCK(lock, get_NetHandler(e->ethread)->mutex, e->ethread);
  if (!lock.is_locked()) {
    e->schedule_in(HRTIME_MSECONDS(net_retry_delay));
    return EVENT_CONT;
  }

  if (!action_.cancelled) {
    this->connectUp(e->ethread, NO_FD);
  } else {
    this->free(e->ethread);
  }

  return EVENT_DONE;
}

// XXX This might be called on ET_UDP thread
void
QUICNetVConnection::start()
{
  QUICConfig::scoped_config params;

  this->_five_tuple.update(this->local_addr, this->remote_addr, SOCK_DGRAM);
  // Version 0x00000001 uses stream 0 for cryptographic handshake with TLS 1.3, but newer version may not
  if (this->direction() == NET_VCONNECTION_IN) {
    this->_reset_token.generate(this->_quic_connection_id, params->server_id());
    this->_hs_protocol       = this->_setup_handshake_protocol(params->server_ssl_ctx());
    this->_handshake_handler = new QUICHandshake(this, this->_hs_protocol, this->_reset_token, params->stateless_retry());
  } else {
    this->_hs_protocol       = this->_setup_handshake_protocol(params->client_ssl_ctx());
    this->_handshake_handler = new QUICHandshake(this, this->_hs_protocol);
    this->_handshake_handler->start(&this->_packet_factory, params->vn_exercise_enabled());
    this->_handshake_handler->do_handshake();
  }

  this->_application_map = new QUICApplicationMap();

  this->_frame_dispatcher = new QUICFrameDispatcher(this);
  this->_packet_factory.set_hs_protocol(this->_hs_protocol);
  this->_pn_protector.set_hs_protocol(this->_hs_protocol);

  // Create frame handlers
  this->_congestion_controller = new QUICCongestionController(this);
  for (auto s : QUIC_PN_SPACES) {
    int index            = static_cast<int>(s);
    QUICLossDetector *ld = new QUICLossDetector(this, this, this->_congestion_controller, &this->_rtt_measure, index);
    this->_frame_dispatcher->add_handler(ld);
    this->_loss_detector[index] = ld;
  }
  this->_remote_flow_controller = new QUICRemoteConnectionFlowController(UINT64_MAX);
  this->_local_flow_controller  = new QUICLocalConnectionFlowController(&this->_rtt_measure, UINT64_MAX);
  this->_path_validator         = new QUICPathValidator();
  this->_stream_manager         = new QUICStreamManager(this, &this->_rtt_measure, this->_application_map);

  this->_frame_dispatcher->add_handler(this);
  this->_frame_dispatcher->add_handler(this->_stream_manager);
  this->_frame_dispatcher->add_handler(this->_path_validator);
  this->_frame_dispatcher->add_handler(this->_handshake_handler);
}

void
QUICNetVConnection::free(EThread *t)
{
  QUICConDebug("Free connection");

  /* TODO: Uncmment these blocks after refactoring read / write process
    this->_udp_con        = nullptr;
    this->_packet_handler = nullptr;

    _unschedule_packet_write_ready();

    delete this->_handshake_handler;
    delete this->_application_map;
    delete this->_hs_protocol;
    delete this->_loss_detector;
    delete this->_frame_dispatcher;
    delete this->_stream_manager;
    delete this->_congestion_controller;
    if (this->_alt_con_manager) {
      delete this->_alt_con_manager;
    }

    super::clear();
  */
  this->_packet_handler->close_conenction(this);
}

void
QUICNetVConnection::free()
{
  this->free(this_ethread());
}

// called by ET_UDP
void
QUICNetVConnection::remove_connection_ids()
{
  if (this->_ctable) {
    this->_ctable->erase(this->_original_quic_connection_id, this);
    this->_ctable->erase(this->_quic_connection_id, this);
  }

  if (this->_alt_con_manager) {
    this->_alt_con_manager->invalidate_alt_connections();
  }
}

// called by ET_UDP
void
QUICNetVConnection::destroy(EThread *t)
{
  QUICConDebug("Destroy connection");
  /*  TODO: Uncmment these blocks after refactoring read / write process
    if (from_accept_thread) {
      quicNetVCAllocator.free(this);
    } else {
      THREAD_FREE(this, quicNetVCAllocator, t);
    }
  */
}

void
QUICNetVConnection::reenable(VIO *vio)
{
  return;
}

int
QUICNetVConnection::connectUp(EThread *t, int fd)
{
  int res        = 0;
  NetHandler *nh = get_NetHandler(t);
  this->thread   = this_ethread();
  ink_assert(nh->mutex->thread_holding == this->thread);

  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_pre_handshake);

  if ((res = nh->startIO(this)) < 0) {
    // FIXME: startIO only return 0 now! what should we do if it failed ?
  }

  nh->startCop(this);

  // FIXME: complete do_io_xxxx instead
  this->read.enabled = 1;

  // start QUIC handshake
  this->_schedule_packet_write_ready();

  return CONNECT_SUCCESS;
}

QUICConnectionId
QUICNetVConnection::peer_connection_id() const
{
  return this->_peer_quic_connection_id;
}

QUICConnectionId
QUICNetVConnection::original_connection_id() const
{
  return this->_original_quic_connection_id;
}

QUICConnectionId
QUICNetVConnection::connection_id() const
{
  return this->_quic_connection_id;
}

/*
 Return combination of dst connection id and src connection id for debug log
 e.g. "aaaaaaaa-bbbbbbbb"
   - "aaaaaaaa" : high 32 bit of dst connection id
   - "bbbbbbbb" : high 32 bit of src connection id
 */
std::string_view
QUICNetVConnection::cids() const
{
  return this->_cids;
}

const QUICFiveTuple
QUICNetVConnection::five_tuple() const
{
  return this->_five_tuple;
}

uint32_t
QUICNetVConnection::pmtu() const
{
  return this->_pmtu;
}

NetVConnectionContext_t
QUICNetVConnection::direction() const
{
  return this->netvc_context;
}

uint32_t
QUICNetVConnection::minimum_quic_packet_size()
{
  if (netvc_context == NET_VCONNECTION_OUT) {
    // FIXME Only the first packet need to be 1200 bytes at least
    return MINIMUM_INITIAL_PACKET_SIZE;
  } else {
    // FIXME This size should be configurable and should have some randomness
    // This is just for providing protection against packet analysis for protected packets
    return 32 + (this->_rnd() & 0x3f); // 32 to 96
  }
}

uint32_t
QUICNetVConnection::maximum_quic_packet_size() const
{
  if (this->options.ip_family == PF_INET6) {
    return this->_pmtu - UDP_HEADER_SIZE - IPV6_HEADER_SIZE;
  } else {
    return this->_pmtu - UDP_HEADER_SIZE - IPV4_HEADER_SIZE;
  }
}

uint64_t
QUICNetVConnection::_maximum_stream_frame_data_size()
{
  return this->maximum_quic_packet_size() - MAX_STREAM_FRAME_OVERHEAD - MAX_PACKET_OVERHEAD;
}

QUICStreamManager *
QUICNetVConnection::stream_manager()
{
  return this->_stream_manager;
}

void
QUICNetVConnection::retransmit_packet(const QUICPacket &packet)
{
  QUICConDebug("Retransmit %s packet #%" PRIu64, QUICDebugNames::packet_type(packet.type()), packet.packet_number());

  this->_packet_retransmitter.retransmit_packet(packet);
}

Ptr<ProxyMutex>
QUICNetVConnection::get_packet_transmitter_mutex()
{
  return this->_packet_transmitter_mutex;
}

void
QUICNetVConnection::handle_received_packet(UDPPacket *packet)
{
  this->_packet_recv_queue.enqueue(packet);
}

void
QUICNetVConnection::close(QUICConnectionErrorUPtr error)
{
  if (this->handler == reinterpret_cast<ContinuationHandler>(&QUICNetVConnection::state_connection_closed) ||
      this->handler == reinterpret_cast<ContinuationHandler>(&QUICNetVConnection::state_connection_closing)) {
    // do nothing
  } else {
    this->_switch_to_closing_state(std::move(error));
  }
}

std::vector<QUICFrameType>
QUICNetVConnection::interests()
{
  return {QUICFrameType::APPLICATION_CLOSE, QUICFrameType::CONNECTION_CLOSE, QUICFrameType::BLOCKED, QUICFrameType::MAX_DATA,
          QUICFrameType::NEW_CONNECTION_ID};
}

QUICConnectionErrorUPtr
QUICNetVConnection::handle_frame(QUICEncryptionLevel level, const QUICFrame &frame)
{
  QUICConnectionErrorUPtr error = nullptr;

  switch (frame.type()) {
  case QUICFrameType::MAX_DATA:
    this->_remote_flow_controller->forward_limit(static_cast<const QUICMaxDataFrame &>(frame).maximum_data());
    QUICFCDebug("[REMOTE] %" PRIu64 "/%" PRIu64, this->_remote_flow_controller->current_offset(),
                this->_remote_flow_controller->current_limit());
    this->_schedule_packet_write_ready();

    break;
  case QUICFrameType::PING:
    // Nothing to do
    break;
  case QUICFrameType::BLOCKED:
    // BLOCKED frame is for debugging. Nothing to do here.
    break;
  case QUICFrameType::NEW_CONNECTION_ID:
    error = this->_handle_frame(static_cast<const QUICNewConnectionIdFrame &>(frame));
    break;
  case QUICFrameType::APPLICATION_CLOSE:
  case QUICFrameType::CONNECTION_CLOSE:
    if (this->handler == reinterpret_cast<NetVConnHandler>(&QUICNetVConnection::state_connection_closed) ||
        this->handler == reinterpret_cast<NetVConnHandler>(&QUICNetVConnection::state_connection_draining)) {
      return error;
    }

    // 7.9.1. Closing and Draining Connection States
    // An endpoint MAY transition from the closing period to the draining period if it can confirm that its peer is also closing or
    // draining. Receiving a closing frame is sufficient confirmation, as is receiving a stateless reset.
    if (frame.type() == QUICFrameType::APPLICATION_CLOSE) {
      this->_switch_to_draining_state(QUICConnectionErrorUPtr(std::make_unique<QUICConnectionError>(
        QUICErrorClass::APPLICATION, static_cast<const QUICApplicationCloseFrame &>(frame).error_code())));
    } else {
      uint16_t error_code = static_cast<const QUICConnectionCloseFrame &>(frame).error_code();
      this->_switch_to_draining_state(
        QUICConnectionErrorUPtr(std::make_unique<QUICConnectionError>(static_cast<QUICTransErrorCode>(error_code))));
    }
    break;
  default:
    QUICConDebug("Unexpected frame type: %02x", static_cast<unsigned int>(frame.type()));
    ink_assert(false);
    break;
  }

  return error;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_handle_frame(const QUICNewConnectionIdFrame &frame)
{
  QUICConnectionErrorUPtr error = nullptr;

  if (frame.connection_id() == QUICConnectionId::ZERO()) {
    return std::make_unique<QUICConnectionError>(QUICTransErrorCode::PROTOCOL_VIOLATION, "received zero-length cid",
                                                 QUICFrameType::NEW_CONNECTION_ID);
  }

  this->_remote_alt_cids.push(frame.connection_id());

  return error;
}

// XXX Setup QUICNetVConnection on regular EThread.
// QUICNetVConnection::init() and QUICNetVConnection::start() might be called on ET_UDP EThread.
int
QUICNetVConnection::state_pre_handshake(int event, Event *data)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());

  if (!this->thread) {
    this->thread = this_ethread();
  }

  if (!this->nh) {
    this->nh = get_NetHandler(this_ethread());
  }

  // FIXME: Should be accept_no_activity_timeout?
  QUICConfig::scoped_config params;

  if (this->get_context() == NET_VCONNECTION_IN) {
    this->set_inactivity_timeout(HRTIME_SECONDS(params->no_activity_timeout_in()));
  } else {
    this->set_inactivity_timeout(HRTIME_SECONDS(params->no_activity_timeout_out()));
  }

  this->add_to_active_queue();

  this->_switch_to_handshake_state();
  return this->handleEvent(event, data);
}

// TODO: Timeout by active_timeout
int
QUICNetVConnection::state_handshake(int event, Event *data)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());

  if (this->_handshake_handler && this->_handshake_handler->is_completed()) {
    this->_switch_to_established_state();
    return this->handleEvent(event, data);
  }

  QUICConnectionErrorUPtr error = nullptr;

  switch (event) {
  case QUIC_EVENT_PACKET_READ_READY: {
    QUICPacketCreationResult result;
    net_activity(this, this_ethread());
    do {
      QUICPacketUPtr packet = this->_dequeue_recv_packet(result);
      if (result == QUICPacketCreationResult::NOT_READY) {
        error = nullptr;
      } else if (result == QUICPacketCreationResult::FAILED) {
        error = std::make_unique<QUICConnectionError>(QUICTransErrorCode::INTERNAL_ERROR);
      } else if (result == QUICPacketCreationResult::SUCCESS || result == QUICPacketCreationResult::UNSUPPORTED) {
        error = this->_state_handshake_process_packet(std::move(packet));
      }

      // if we complete handshake, switch to establish state
      if (this->_handshake_handler && this->_handshake_handler->is_completed()) {
        this->_switch_to_established_state();
        return this->handleEvent(event, data);
      }

    } while (error == nullptr && (result == QUICPacketCreationResult::SUCCESS || result == QUICPacketCreationResult::IGNORED));
    break;
  }
  case QUIC_EVENT_PACKET_WRITE_READY: {
    this->_close_packet_write_ready(data);

    // TODO: support RETRY packet
    error = this->_state_common_send_packet();

    // Reschedule WRITE_READY
    this->_schedule_packet_write_ready(true);

    break;
  }
  case QUIC_EVENT_PATH_VALIDATION_TIMEOUT:
    this->_handle_path_validation_timeout(data);
    break;
  case EVENT_IMMEDIATE:
    // Start Immediate Close because of Idle Timeout
    this->_handle_idle_timeout();
    break;
  default:
    QUICConDebug("Unexpected event: %s (%d)", QUICDebugNames::quic_event(event), event);
  }

  if (error != nullptr) {
    this->_handle_error(std::move(error));
  }

  return EVENT_CONT;
}

int
QUICNetVConnection::state_connection_established(int event, Event *data)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  QUICConnectionErrorUPtr error = nullptr;
  switch (event) {
  case QUIC_EVENT_PACKET_READ_READY: {
    error = this->_state_connection_established_receive_packet();
    break;
  }
  case QUIC_EVENT_PACKET_WRITE_READY: {
    this->_close_packet_write_ready(data);
    error = this->_state_common_send_packet();
    // Reschedule WRITE_READY
    this->_schedule_packet_write_ready(true);
    break;
  }
  case QUIC_EVENT_PATH_VALIDATION_TIMEOUT:
    this->_handle_path_validation_timeout(data);
    break;
  case EVENT_IMMEDIATE: {
    // Start Immediate Close because of Idle Timeout
    this->_handle_idle_timeout();
    break;
  }
  default:
    QUICConDebug("Unexpected event: %s (%d)", QUICDebugNames::quic_event(event), event);
  }

  if (error != nullptr) {
    QUICConDebug("QUICError: cls=%u, code=0x%" PRIu16, static_cast<unsigned int>(error->cls), error->code);
    this->_handle_error(std::move(error));
  }

  return EVENT_CONT;
}

int
QUICNetVConnection::state_connection_closing(int event, Event *data)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());

  QUICConnectionErrorUPtr error = nullptr;
  switch (event) {
  case QUIC_EVENT_PACKET_READ_READY:
    error = this->_state_closing_receive_packet();
    break;
  case QUIC_EVENT_PACKET_WRITE_READY:
    this->_close_packet_write_ready(data);
    this->_state_closing_send_packet();
    break;
  case QUIC_EVENT_PATH_VALIDATION_TIMEOUT:
    this->_handle_path_validation_timeout(data);
    break;
  case QUIC_EVENT_CLOSING_TIMEOUT:
    this->_close_closing_timeout(data);
    this->_switch_to_close_state();
    break;
  default:
    QUICConDebug("Unexpected event: %s (%d)", QUICDebugNames::quic_event(event), event);
    ink_assert(false);
  }

  return EVENT_DONE;
}

int
QUICNetVConnection::state_connection_draining(int event, Event *data)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());

  QUICConnectionErrorUPtr error = nullptr;
  switch (event) {
  case QUIC_EVENT_PACKET_READ_READY:
    error = this->_state_draining_receive_packet();
    break;
  case QUIC_EVENT_PACKET_WRITE_READY:
    // Do not send any packets in this state.
    // This should be the only difference between this and closing_state.
    this->_close_packet_write_ready(data);
    break;
  case QUIC_EVENT_PATH_VALIDATION_TIMEOUT:
    this->_handle_path_validation_timeout(data);
    break;
  case QUIC_EVENT_CLOSING_TIMEOUT:
    this->_close_closing_timeout(data);
    this->_switch_to_close_state();
    break;
  default:
    QUICConDebug("Unexpected event: %s (%d)", QUICDebugNames::quic_event(event), event);
    ink_assert(false);
  }

  return EVENT_DONE;
}

int
QUICNetVConnection::state_connection_closed(int event, Event *data)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  switch (event) {
  case QUIC_EVENT_SHUTDOWN: {
    this->_unschedule_packet_write_ready();
    this->_unschedule_closing_timeout();
    this->_unschedule_path_validation_timeout();
    this->_close_closed_event(data);
    this->next_inactivity_timeout_at = 0;
    this->next_activity_timeout_at   = 0;

    this->inactivity_timeout_in = 0;
    this->active_timeout_in     = 0;

    // TODO: Drop record from Connection-ID - QUICNetVConnection table in QUICPacketHandler
    // Shutdown loss detector
    for (auto s : QUIC_PN_SPACES) {
      QUICLossDetector *ld = this->_loss_detector[static_cast<int>(s)];
      SCOPED_MUTEX_LOCK(lock, ld->mutex, this_ethread());
      ld->handleEvent(QUIC_EVENT_LD_SHUTDOWN, nullptr);
    }

    if (this->nh) {
      this->nh->free_netvc(this);
    } else {
      this->free(this->mutex->thread_holding);
    }
    break;
  }
  case QUIC_EVENT_PACKET_WRITE_READY: {
    this->_close_packet_write_ready(data);
    break;
  }
  default:
    QUICConDebug("Unexpected event: %s (%d)", QUICDebugNames::quic_event(event), event);
  }

  return EVENT_DONE;
}

UDPConnection *
QUICNetVConnection::get_udp_con()
{
  return this->_udp_con;
}

void
QUICNetVConnection::net_read_io(NetHandler *nh, EThread *lthread)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  this->handleEvent(QUIC_EVENT_PACKET_READ_READY, nullptr);

  return;
}

int64_t
QUICNetVConnection::load_buffer_and_write(int64_t towrite, MIOBufferAccessor &buf, int64_t &total_written, int &needs)
{
  ink_assert(false);

  return 0;
}

int
QUICNetVConnection::populate_protocol(std::string_view *results, int n) const
{
  int retval = 0;
  if (n > retval) {
    results[retval++] = IP_PROTO_TAG_QUIC;
    if (n > retval) {
      retval += super::populate_protocol(results + retval, n - retval);
    }
  }
  return retval;
}

const char *
QUICNetVConnection::protocol_contains(std::string_view prefix) const
{
  const char *retval   = nullptr;
  std::string_view tag = IP_PROTO_TAG_QUIC;
  if (prefix.size() <= tag.size() && strncmp(tag.data(), prefix.data(), prefix.size()) == 0) {
    retval = tag.data();
  } else {
    retval = super::protocol_contains(prefix);
  }
  return retval;
}

void
QUICNetVConnection::registerNextProtocolSet(SSLNextProtocolSet *s)
{
  this->_next_protocol_set = s;
}

bool
QUICNetVConnection::is_closed() const
{
  return this->handler == reinterpret_cast<NetVConnHandler>(&QUICNetVConnection::state_connection_closed);
}

SSLNextProtocolSet *
QUICNetVConnection::next_protocol_set() const
{
  return this->_next_protocol_set;
}

QUICPacketNumber
QUICNetVConnection::largest_acked_packet_number(QUICEncryptionLevel level) const
{
  int index = QUICTypeUtil::pn_space_index(level);

  return this->_loss_detector[index]->largest_acked_packet_number();
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_process_packet(QUICPacketUPtr packet)
{
  QUICConnectionErrorUPtr error = nullptr;
  switch (packet->type()) {
  case QUICPacketType::VERSION_NEGOTIATION:
    error = this->_state_handshake_process_version_negotiation_packet(std::move(packet));
    break;
  case QUICPacketType::INITIAL:
    error = this->_state_handshake_process_initial_packet(std::move(packet));
    break;
  case QUICPacketType::RETRY:
    error = this->_state_handshake_process_retry_packet(std::move(packet));
    break;
  case QUICPacketType::HANDSHAKE:
    error = this->_state_handshake_process_handshake_packet(std::move(packet));
    break;
  case QUICPacketType::ZERO_RTT_PROTECTED:
    error = this->_state_handshake_process_zero_rtt_protected_packet(std::move(packet));
    break;
  case QUICPacketType::PROTECTED:
  default:
    QUICConDebug("Ignore %s(%" PRIu8 ") packet", QUICDebugNames::packet_type(packet->type()), static_cast<uint8_t>(packet->type()));

    error = std::make_unique<QUICConnectionError>(QUICTransErrorCode::INTERNAL_ERROR);
    break;
  }
  return error;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_process_version_negotiation_packet(QUICPacketUPtr packet)
{
  QUICConnectionErrorUPtr error = nullptr;

  if (packet->destination_cid() != this->connection_id()) {
    QUICConDebug("Ignore Version Negotiation packet");
    return error;
  }

  if (this->_handshake_handler->is_version_negotiated()) {
    QUICConDebug("ignore VN - already negotiated");
  } else {
    error = this->_handshake_handler->negotiate_version(packet.get(), &this->_packet_factory);

    // discard all transport state except packet number
    for (auto s : QUIC_PN_SPACES) {
      this->_loss_detector[static_cast<int>(s)]->reset();
    }
    this->_congestion_controller->reset();
    SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
    this->_packet_retransmitter.reset();

    // start handshake over
    this->_handshake_handler->reset();
    this->_handshake_handler->do_handshake();
    this->_schedule_packet_write_ready();
  }

  return error;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_process_initial_packet(QUICPacketUPtr packet)
{
  // QUIC packet could be smaller than MINIMUM_INITIAL_PACKET_SIZE when coalescing packets
  // if (packet->size() < MINIMUM_INITIAL_PACKET_SIZE) {
  //   QUICConDebug("Packet size is smaller than the minimum initial packet size");
  //   // Ignore the packet
  //   return QUICErrorUPtr(new QUICNoError());
  // }

  QUICConnectionErrorUPtr error = nullptr;

  // Start handshake
  if (this->netvc_context == NET_VCONNECTION_IN) {
    error = this->_handshake_handler->start(packet.get(), &this->_packet_factory);

    // If version negotiation was failed and VERSION NEGOTIATION packet was sent, nothing to do.
    if (this->_handshake_handler->is_version_negotiated()) {
      error = this->_recv_and_ack(std::move(packet));

      if (error == nullptr && !this->_handshake_handler->has_remote_tp()) {
        error = std::make_unique<QUICConnectionError>(QUICTransErrorCode::TRANSPORT_PARAMETER_ERROR);
      }
    }
  } else {
    // on client side, _handshake_handler is already started. Just process packet like _state_handshake_process_handshake_packet()
    error = this->_recv_and_ack(std::move(packet));
  }

  return error;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_process_retry_packet(QUICPacketUPtr packet)
{
  // discard all transport state
  this->_handshake_handler->reset();
  for (auto s : QUIC_PN_SPACES) {
    this->_loss_detector[static_cast<int>(s)]->reset();
  }
  this->_congestion_controller->reset();
  SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  this->_packet_retransmitter.reset();

  QUICConnectionErrorUPtr error = this->_recv_and_ack(std::move(packet));

  // Packet number of RETRY packet is echo of INITIAL packet
  this->_packet_recv_queue.reset();

  // Generate new Connection ID
  this->_rerandomize_original_cid();

  this->_hs_protocol->initialize_key_materials(this->_original_quic_connection_id);

  return error;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_process_handshake_packet(QUICPacketUPtr packet)
{
  // Source address is verified by receiving any message from the client encrypted using the
  // Handshake keys.
  if (this->netvc_context == NET_VCONNECTION_IN && !this->_src_addr_verified) {
    this->_src_addr_verified = true;
  }
  return this->_recv_and_ack(std::move(packet));
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_process_zero_rtt_protected_packet(QUICPacketUPtr packet)
{
  this->_stream_manager->init_flow_control_params(this->_handshake_handler->local_transport_parameters(),
                                                  this->_handshake_handler->remote_transport_parameters());
  this->_start_application();
  return this->_recv_and_ack(std::move(packet));
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_connection_established_process_protected_packet(QUICPacketUPtr packet)
{
  return this->_recv_and_ack(std::move(packet));
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_connection_established_receive_packet()
{
  QUICConnectionErrorUPtr error = nullptr;
  QUICPacketCreationResult result;

  // Receive a QUIC packet
  net_activity(this, this_ethread());
  do {
    QUICPacketUPtr p = this->_dequeue_recv_packet(result);
    if (result == QUICPacketCreationResult::FAILED) {
      return QUICConnectionErrorUPtr(new QUICConnectionError(QUICTransErrorCode::INTERNAL_ERROR));
    } else if (result == QUICPacketCreationResult::NO_PACKET) {
      return error;
    } else if (result == QUICPacketCreationResult::NOT_READY) {
      return error;
    } else if (result == QUICPacketCreationResult::IGNORED) {
      continue;
    }

    // Process the packet
    switch (p->type()) {
    case QUICPacketType::PROTECTED:
      // Migrate connection if required
      error = this->_state_connection_established_migrate_connection(*p);
      if (error != nullptr) {
        break;
      }

      if (this->netvc_context == NET_VCONNECTION_OUT) {
        this->_state_connection_established_initiate_connection_migration();
      }

      error = this->_state_connection_established_process_protected_packet(std::move(p));
      break;
    case QUICPacketType::INITIAL:
    case QUICPacketType::HANDSHAKE:
    case QUICPacketType::ZERO_RTT_PROTECTED:
      // Pass packet to _recv_and_ack to send ack to the packet. Stream data will be discarded by offset mismatch.
      error = this->_recv_and_ack(std::move(p));
      break;
    default:
      QUICConDebug("Unknown packet type: %s(%" PRIu8 ")", QUICDebugNames::packet_type(p->type()), static_cast<uint8_t>(p->type()));

      error = std::make_unique<QUICConnectionError>(QUICTransErrorCode::INTERNAL_ERROR);
      break;
    }

  } while (error == nullptr && (result == QUICPacketCreationResult::SUCCESS || result == QUICPacketCreationResult::IGNORED));
  return error;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_closing_receive_packet()
{
  while (this->_packet_recv_queue.size() > 0) {
    QUICPacketCreationResult result;
    QUICPacketUPtr packet = this->_dequeue_recv_packet(result);
    if (result == QUICPacketCreationResult::SUCCESS) {
      switch (packet->type()) {
      case QUICPacketType::VERSION_NEGOTIATION:
        // Ignore VN packets on closing state
        break;
      default:
        this->_recv_and_ack(std::move(packet));
        break;
      }
    }
    ++this->_state_closing_recv_packet_count;

    if (this->_state_closing_recv_packet_window < STATE_CLOSING_MAX_RECV_PKT_WIND &&
        this->_state_closing_recv_packet_count >= this->_state_closing_recv_packet_window) {
      this->_state_closing_recv_packet_count = 0;
      this->_state_closing_recv_packet_window <<= 1;

      this->_schedule_packet_write_ready(true);
      break;
    }
  }

  return nullptr;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_draining_receive_packet()
{
  while (this->_packet_recv_queue.size() > 0) {
    QUICPacketCreationResult result;
    QUICPacketUPtr packet = this->_dequeue_recv_packet(result);
    if (result == QUICPacketCreationResult::SUCCESS) {
      this->_recv_and_ack(std::move(packet));
      // Do NOT schedule WRITE_READY event from this point.
      // An endpoint in the draining state MUST NOT send any packets.
    }
  }

  return nullptr;
}

/**
 * 1. Check congestion window
 * 2. Allocate buffer for UDP Payload
 * 3. Generate QUIC Packet
 * 4. Store data to the paylaod
 * 5. Send UDP Packet
 */
QUICConnectionErrorUPtr
QUICNetVConnection::_state_common_send_packet()
{
  uint32_t packet_count = 0;
  uint32_t error        = 0;
  while (error == 0 && packet_count < PACKET_PER_EVENT) {
    uint32_t window = this->_congestion_controller->open_window();

    if (window == 0) {
      break;
    }

    Ptr<IOBufferBlock> udp_payload(new_IOBufferBlock());
    uint32_t udp_payload_len = std::min(window, this->_pmtu);
    udp_payload->alloc(iobuffer_size_to_index(udp_payload_len));

    uint32_t written = 0;
    for (auto level : QUIC_ENCRYPTION_LEVELS) {
      if (this->netvc_context == NET_VCONNECTION_IN && !this->_is_src_addr_verified() &&
          this->_handshake_packets_sent >= MAX_PACKETS_WITHOUT_SRC_ADDR_VARIDATION) {
        error = 1;
        break;
      }

      uint32_t max_packet_size = udp_payload_len - written;
      QUICPacketUPtr packet    = this->_packetize_frames(level, max_packet_size);

      if (packet) {
        if (this->netvc_context == NET_VCONNECTION_IN &&
            (packet->type() == QUICPacketType::INITIAL || packet->type() == QUICPacketType::HANDSHAKE)) {
          ++this->_handshake_packets_sent;
        }

        // TODO: do not write two QUIC Short Header Packets
        uint8_t *buf = reinterpret_cast<uint8_t *>(udp_payload->end());
        size_t len   = 0;
        packet->store(buf, &len);
        udp_payload->fill(len);
        written += len;

        // TODO: Avoid static function. We don't need to parse buffer again. Get packet number offset from packet.
        QUICPacket::protect_packet_number(
          buf, len, &this->_pn_protector,
          (this->_peer_quic_connection_id == QUICConnectionId::ZERO()) ? 0 : this->_peer_quic_connection_id.length());

        QUICConDebug("[TX] %s packet #%" PRIu64 " size=%zu", QUICDebugNames::packet_type(packet->type()), packet->packet_number(),
                     len);

        int index = QUICTypeUtil::pn_space_index(level);
        this->_loss_detector[index]->on_packet_sent(std::move(packet));
        packet_count++;
      }
    }

    if (written) {
      this->_packet_handler->send_packet(this, udp_payload);
    } else {
      udp_payload->dealloc();
      break;
    }
  }

  if (packet_count) {
    QUIC_INCREMENT_DYN_STAT_EX(QUICStats::total_packets_sent_stat, packet_count);
    net_activity(this, this_ethread());
  }

  return nullptr;
}

// RETRY packet contains ONLY a single STREAM frame
QUICConnectionErrorUPtr
QUICNetVConnection::_state_handshake_send_retry_packet()
{
  // size_t len = 0;
  // ats_unique_buf buf(nullptr, [](void *p) { ats_free(p); });
  // QUICPacketType current_packet_type = QUICPacketType::UNINITIALIZED;

  // QUICFrameUPtr frame(nullptr, nullptr);
  // bool retransmittable = this->_handshake_handler->is_stateless_retry_enabled() ? false : true;

  // SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  // SCOPED_MUTEX_LOCK(frame_transmitter_lock, this->_frame_transmitter_mutex, this_ethread());

  // frame = this->_stream_manager->generate_frame(this->_remote_flow_controller->credit(),
  // this->_maximum_stream_frame_data_size()); ink_assert(frame); ink_assert(frame->type() == QUICFrameType::STREAM);
  // this->_store_frame(buf, len, retransmittable, current_packet_type, std::move(frame));
  // if (len == 0) {
  //   return QUICErrorUPtr(new QUICConnectionError(QUICTransErrorCode::INTERNAL_ERROR));
  // }

  // QUICPacketUPtr packet = this->_build_packet(std::move(buf), len, retransmittable, QUICPacketType::RETRY);
  // this->_packet_handler->send_packet(*packet, this, this->_pn_protector);
  // this->_loss_detector->on_packet_sent(std::move(packet));

  // QUIC_INCREMENT_DYN_STAT_EX(QUICStats::total_packets_sent_stat, 1);

  return nullptr;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_closing_send_packet()
{
  this->_packetize_closing_frame();

  // TODO: should credit of congestion controller be checked?

  // During the closing period, an endpoint that sends a
  // closing frame SHOULD respond to any packet that it receives with
  // another packet containing a closing frame.  To minimize the state
  // that an endpoint maintains for a closing connection, endpoints MAY
  // send the exact same packet.
  if (this->_the_final_packet) {
    this->_packet_handler->send_packet(*this->_the_final_packet, this, this->_pn_protector);
  }

  return nullptr;
}

void
QUICNetVConnection::_store_frame(ats_unique_buf &buf, size_t &offset, uint64_t &max_frame_size, QUICFrameUPtr frame)
{
  size_t l = 0;
  frame->store(buf.get() + offset, &l, max_frame_size);

  // frame should be stored because it's created with max_frame_size
  ink_assert(l != 0);

  offset += l;
  max_frame_size -= l;

  if (is_debug_tag_set(QUIC_DEBUG_TAG.data())) {
    char msg[1024];
    frame->debug_msg(msg, sizeof(msg));
    QUICConDebug("[TX] %s", msg);
  }
}

QUICPacketUPtr
QUICNetVConnection::_packetize_frames(QUICEncryptionLevel level, uint64_t max_packet_size)
{
  QUICPacketUPtr packet = QUICPacketFactory::create_null_packet();
  if (max_packet_size <= MAX_PACKET_OVERHEAD) {
    return packet;
  }

  // TODO: adjust MAX_PACKET_OVERHEAD for each encryption level
  uint64_t max_frame_size = max_packet_size - MAX_PACKET_OVERHEAD;
  max_frame_size          = std::min(max_frame_size, this->_maximum_stream_frame_data_size());

  bool probing       = false;
  int frame_count    = 0;
  size_t len         = 0;
  ats_unique_buf buf = ats_unique_malloc(max_packet_size);
  QUICFrameUPtr frame(nullptr, nullptr);

  SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  SCOPED_MUTEX_LOCK(frame_transmitter_lock, this->_frame_transmitter_mutex, this_ethread());

  // CRYPTO
  frame = this->_handshake_handler->generate_frame(level, UINT16_MAX, max_frame_size);
  while (frame) {
    ++frame_count;
    probing |= frame->is_probing_frame();
    this->_store_frame(buf, len, max_frame_size, std::move(frame));
    frame = this->_handshake_handler->generate_frame(level, UINT16_MAX, max_frame_size);
  }

  // PATH_CHALLENGE, PATH_RESPOSNE
  frame = this->_path_validator->generate_frame(level, UINT16_MAX, max_frame_size);
  if (frame) {
    ++frame_count;
    probing |= frame->is_probing_frame();
    this->_store_frame(buf, len, max_frame_size, std::move(frame));
  }

  // NEW_CONNECTION_ID
  if (this->_alt_con_manager) {
    frame = this->_alt_con_manager->generate_frame(level, UINT16_MAX, max_frame_size);
    while (frame) {
      ++frame_count;
      probing |= frame->is_probing_frame();
      this->_store_frame(buf, len, max_frame_size, std::move(frame));

      frame = this->_alt_con_manager->generate_frame(level, UINT16_MAX, max_frame_size);
    }
  }

  // Lost frames
  frame = this->_packet_retransmitter.generate_frame(level, UINT16_MAX, max_frame_size);
  while (frame) {
    ++frame_count;
    probing |= frame->is_probing_frame();
    this->_store_frame(buf, len, max_frame_size, std::move(frame));

    frame = this->_packet_retransmitter.generate_frame(level, UINT16_MAX, max_frame_size);
  }

  // MAX_DATA
  frame = this->_local_flow_controller->generate_frame(level, UINT16_MAX, max_frame_size);
  if (frame) {
    ++frame_count;
    probing |= frame->is_probing_frame();
    this->_store_frame(buf, len, max_frame_size, std::move(frame));
  }

  // BLOCKED
  if (this->_remote_flow_controller->credit() == 0 && this->_stream_manager->will_generate_frame(level)) {
    frame = this->_remote_flow_controller->generate_frame(level, UINT16_MAX, max_frame_size);
    if (frame) {
      ++frame_count;
      probing |= frame->is_probing_frame();
      this->_store_frame(buf, len, max_frame_size, std::move(frame));
    }
  }

  // STREAM, MAX_STREAM_DATA, STREAM_BLOCKED
  if (!this->_path_validator->is_validating()) {
    frame = this->_stream_manager->generate_frame(level, this->_remote_flow_controller->credit(), max_frame_size);
    while (frame) {
      ++frame_count;
      probing |= frame->is_probing_frame();
      if (frame->type() == QUICFrameType::STREAM) {
        int ret = this->_remote_flow_controller->update(this->_stream_manager->total_offset_sent());
        QUICFCDebug("[REMOTE] %" PRIu64 "/%" PRIu64, this->_remote_flow_controller->current_offset(),
                    this->_remote_flow_controller->current_limit());
        ink_assert(ret == 0);
      }
      this->_store_frame(buf, len, max_frame_size, std::move(frame));

      if (++this->_stream_frames_sent % MAX_CONSECUTIVE_STREAMS == 0) {
        break;
      }

      frame = this->_stream_manager->generate_frame(level, this->_remote_flow_controller->credit(), max_frame_size);
    }
  }

  // ACK
  if (frame_count == 0) {
    if (this->_ack_frame_creator.will_generate_frame(level)) {
      frame = this->_ack_frame_creator.generate_frame(level, UINT16_MAX, max_frame_size);
    }
  } else {
    frame = this->_ack_frame_creator.generate_frame(level, UINT16_MAX, max_frame_size);
  }

  bool ack_only = false;
  if (frame != nullptr) {
    if (frame_count == 0) {
      ack_only = true;
    }
    ++frame_count;
    probing |= frame->is_probing_frame();
    this->_store_frame(buf, len, max_frame_size, std::move(frame));
  }

  // Schedule a packet
  if (len != 0) {
    if (level == QUICEncryptionLevel::INITIAL && this->netvc_context == NET_VCONNECTION_OUT) {
      // Pad with PADDING frames
      uint64_t min_size = this->minimum_quic_packet_size();
      min_size          = std::min(min_size, max_packet_size);
      if (min_size > len) {
        // FIXME QUICNetVConnection should not know the actual type value of PADDING frame
        memset(buf.get() + len, 0, min_size - len);
        len += min_size - len;
      }
    }

    // Packet is retransmittable if it's not ack only packet
    packet = this->_build_packet(level, std::move(buf), len, !ack_only, probing);
  }

  return packet;
}

void
QUICNetVConnection::_packetize_closing_frame()
{
  SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  SCOPED_MUTEX_LOCK(frame_transmitter_lock, this->_frame_transmitter_mutex, this_ethread());

  if (this->_connection_error == nullptr) {
    return;
  }

  QUICFrameUPtr frame = QUICFrameFactory::create_null_frame();
  if (this->_connection_error->cls == QUICErrorClass::APPLICATION) {
    frame = QUICFrameFactory::create_application_close_frame(std::move(this->_connection_error));
  } else {
    frame = QUICFrameFactory::create_connection_close_frame(std::move(this->_connection_error));
  }

  uint32_t max_size  = this->maximum_quic_packet_size();
  ats_unique_buf buf = ats_unique_malloc(max_size);

  size_t len              = 0;
  uint64_t max_frame_size = static_cast<uint64_t>(max_size);
  this->_store_frame(buf, len, max_frame_size, std::move(frame));

  QUICEncryptionLevel level = this->_hs_protocol->current_encryption_level();
  ink_assert(level != QUICEncryptionLevel::ZERO_RTT);
  this->_the_final_packet = this->_build_packet(level, std::move(buf), len, false, false);
}

QUICConnectionErrorUPtr
QUICNetVConnection::_recv_and_ack(QUICPacketUPtr packet)
{
  const uint8_t *payload      = packet->payload();
  uint16_t size               = packet->payload_length();
  QUICPacketNumber packet_num = packet->packet_number();
  QUICEncryptionLevel level   = QUICTypeUtil::encryption_level(packet->type());

  bool should_send_ack;
  bool is_flow_controlled;

  QUICConnectionErrorUPtr error = nullptr;

  error = this->_frame_dispatcher->receive_frames(level, payload, size, should_send_ack, is_flow_controlled);
  if (error != nullptr) {
    return error;
  }

  if (packet->type() == QUICPacketType::RETRY) {
    should_send_ack = false;
  }

  if (is_flow_controlled) {
    int ret = this->_local_flow_controller->update(this->_stream_manager->total_offset_received());
    QUICFCDebug("[LOCAL] %" PRIu64 "/%" PRIu64, this->_local_flow_controller->current_offset(),
                this->_local_flow_controller->current_limit());

    if (ret != 0) {
      return std::make_unique<QUICConnectionError>(QUICTransErrorCode::FLOW_CONTROL_ERROR);
    }

    this->_local_flow_controller->forward_limit(this->_stream_manager->total_reordered_bytes() + this->_flow_control_buffer_size);
    QUICFCDebug("[LOCAL] %" PRIu64 "/%" PRIu64, this->_local_flow_controller->current_offset(),
                this->_local_flow_controller->current_limit());
  }

  this->_ack_frame_creator.update(level, packet_num, should_send_ack);

  return error;
}

QUICPacketUPtr
QUICNetVConnection::_build_packet(QUICEncryptionLevel level, ats_unique_buf buf, size_t len, bool retransmittable, bool probing)
{
  return this->_build_packet(std::move(buf), len, retransmittable, probing, QUICTypeUtil::packet_type(level));
}

QUICPacketUPtr
QUICNetVConnection::_build_packet(ats_unique_buf buf, size_t len, bool retransmittable, bool probing, QUICPacketType type)
{
  QUICPacketUPtr packet = QUICPacketFactory::create_null_packet();

  switch (type) {
  case QUICPacketType::INITIAL: {
    QUICConnectionId dcid =
      (this->netvc_context == NET_VCONNECTION_OUT) ? this->_original_quic_connection_id : this->_peer_quic_connection_id;
    packet = this->_packet_factory.create_initial_packet(dcid, this->_quic_connection_id,
                                                         this->largest_acked_packet_number(QUICEncryptionLevel::INITIAL),
                                                         std::move(buf), len, retransmittable, probing);
    break;
  }
  case QUICPacketType::RETRY: {
    // Echo "_largest_received_packet_number" as packet number. Probably this is the packet number from triggering client packet.
    packet = this->_packet_factory.create_retry_packet(this->_peer_quic_connection_id, this->_quic_connection_id, std::move(buf),
                                                       len, retransmittable, probing);
    break;
  }
  case QUICPacketType::HANDSHAKE: {
    packet = this->_packet_factory.create_handshake_packet(this->_peer_quic_connection_id, this->_quic_connection_id,
                                                           this->largest_acked_packet_number(QUICEncryptionLevel::HANDSHAKE),
                                                           std::move(buf), len, retransmittable, probing);
    break;
  }
  case QUICPacketType::PROTECTED: {
    packet = this->_packet_factory.create_protected_packet(this->_peer_quic_connection_id,
                                                           this->largest_acked_packet_number(QUICEncryptionLevel::ONE_RTT),
                                                           std::move(buf), len, retransmittable, probing);
    break;
  }
  default:
    // should not be here except zero_rtt
    ink_assert(false);
    break;
  }

  return packet;
}

void
QUICNetVConnection::_init_flow_control_params(const std::shared_ptr<const QUICTransportParameters> &local_tp,
                                              const std::shared_ptr<const QUICTransportParameters> &remote_tp)
{
  this->_stream_manager->init_flow_control_params(local_tp, remote_tp);

  uint32_t local_initial_max_data  = 0;
  uint32_t remote_initial_max_data = 0;
  if (local_tp) {
    local_initial_max_data          = local_tp->getAsUInt32(QUICTransportParameterId::INITIAL_MAX_DATA);
    this->_flow_control_buffer_size = local_initial_max_data;
  }
  if (remote_tp) {
    remote_initial_max_data = remote_tp->getAsUInt32(QUICTransportParameterId::INITIAL_MAX_DATA);
  }

  this->_local_flow_controller->set_limit(local_initial_max_data);
  this->_remote_flow_controller->set_limit(remote_initial_max_data);
  QUICFCDebug("[LOCAL] %" PRIu64 "/%" PRIu64, this->_local_flow_controller->current_offset(),
              this->_local_flow_controller->current_limit());
  QUICFCDebug("[REMOTE] %" PRIu64 "/%" PRIu64, this->_remote_flow_controller->current_offset(),
              this->_remote_flow_controller->current_limit());
}

void
QUICNetVConnection::_handle_error(QUICConnectionErrorUPtr error)
{
  if (error->cls == QUICErrorClass::APPLICATION) {
    QUICError("QUICError: %s (%u), APPLICATION ERROR (0x%" PRIu16 ")", QUICDebugNames::error_class(error->cls),
              static_cast<unsigned int>(error->cls), error->code);
  } else {
    QUICError("QUICError: %s (%u), %s (0x%" PRIu16 ")", QUICDebugNames::error_class(error->cls),
              static_cast<unsigned int>(error->cls), QUICDebugNames::error_code(error->code), error->code);
  }

  // Connection Error
  this->close(std::move(error));
}

QUICPacketUPtr
QUICNetVConnection::_dequeue_recv_packet(QUICPacketCreationResult &result)
{
  QUICPacketUPtr packet = this->_packet_recv_queue.dequeue(result);

  if (result == QUICPacketCreationResult::SUCCESS) {
    if (this->direction() == NET_VCONNECTION_OUT) {
      // Reset CID if a server sent back a new CID
      // FIXME This should happen only once
      QUICConnectionId src_cid = packet->source_cid();
      // FIXME src connection id could be zero ? if so, check packet header type.
      if (src_cid != QUICConnectionId::ZERO()) {
        if (this->_peer_quic_connection_id != src_cid) {
          this->_update_peer_cid(src_cid);
        }
      }
    }

    this->_last_received_packet_type = packet->type();
  }

  // Debug prints
  switch (result) {
  case QUICPacketCreationResult::NO_PACKET:
    break;
  case QUICPacketCreationResult::NOT_READY:
    QUICConDebug("Not ready to decrypt the packet");
    break;
  case QUICPacketCreationResult::IGNORED:
    QUICConDebug("Ignored");
    break;
  case QUICPacketCreationResult::UNSUPPORTED:
    QUICConDebug("Unsupported version");
    break;
  case QUICPacketCreationResult::SUCCESS:
    if (packet->type() == QUICPacketType::VERSION_NEGOTIATION) {
      QUICConDebug("[RX] %s packet size=%u", QUICDebugNames::packet_type(packet->type()), packet->size());
    } else {
      QUICConDebug("[RX] %s packet #%" PRIu64 " size=%u", QUICDebugNames::packet_type(packet->type()), packet->packet_number(),
                   packet->size());
    }
    break;
  default:
    QUICConDebug("Failed to decrypt the packet");
    break;
  }

  return packet;
}

void
QUICNetVConnection::_schedule_packet_write_ready(bool delay)
{
  SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  if (!this->_packet_write_ready) {
    QUICConVVVDebug("Schedule %s event", QUICDebugNames::quic_event(QUIC_EVENT_PACKET_WRITE_READY));
    if (delay) {
      this->_packet_write_ready = this->thread->schedule_in(this, WRITE_READY_INTERVAL, QUIC_EVENT_PACKET_WRITE_READY, nullptr);
    } else {
      this->_packet_write_ready = this->thread->schedule_imm(this, QUIC_EVENT_PACKET_WRITE_READY, nullptr);
    }
  }
}

void
QUICNetVConnection::_unschedule_packet_write_ready()
{
  SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  if (this->_packet_write_ready) {
    this->_packet_write_ready->cancel();
    this->_packet_write_ready = nullptr;
  }
}

void
QUICNetVConnection::_close_packet_write_ready(Event *data)
{
  SCOPED_MUTEX_LOCK(packet_transmitter_lock, this->_packet_transmitter_mutex, this_ethread());
  ink_assert(this->_packet_write_ready == data);
  this->_packet_write_ready = nullptr;
}

void
QUICNetVConnection::_schedule_closing_timeout(ink_hrtime interval)
{
  if (!this->_closing_timeout) {
    QUICConDebug("Schedule %s event %" PRIu64 "ms", QUICDebugNames::quic_event(QUIC_EVENT_CLOSING_TIMEOUT), interval);
    this->_closing_timeout = this->thread->schedule_in_local(this, interval, QUIC_EVENT_CLOSING_TIMEOUT);
  }
}

void
QUICNetVConnection::_unschedule_closing_timeout()
{
  if (this->_closing_timeout) {
    QUICConDebug("Unschedule %s event", QUICDebugNames::quic_event(QUIC_EVENT_CLOSING_TIMEOUT));
    this->_closing_timeout->cancel();
    this->_closing_timeout = nullptr;
  }
}

void
QUICNetVConnection::_close_closing_timeout(Event *data)
{
  ink_assert(this->_closing_timeout == data);
  this->_closing_timeout = nullptr;
}

void
QUICNetVConnection::_schedule_closed_event()
{
  if (!this->_closed_event) {
    QUICConDebug("Schedule %s event", QUICDebugNames::quic_event(QUIC_EVENT_SHUTDOWN));
    this->_closed_event = this->thread->schedule_imm(this, QUIC_EVENT_SHUTDOWN, nullptr);
  }
}

void
QUICNetVConnection::_unschedule_closed_event()
{
  if (!this->_closed_event) {
    QUICConDebug("Unschedule %s event", QUICDebugNames::quic_event(QUIC_EVENT_SHUTDOWN));
    this->_closed_event->cancel();
    this->_closed_event = nullptr;
  }
}

void
QUICNetVConnection::_close_closed_event(Event *data)
{
  ink_assert(this->_closed_event == data);
  this->_closed_event = nullptr;
}

int
QUICNetVConnection::_complete_handshake_if_possible()
{
  if (this->handler != reinterpret_cast<NetVConnHandler>(&QUICNetVConnection::state_handshake)) {
    return 0;
  }

  if (!(this->_handshake_handler && this->_handshake_handler->is_completed())) {
    return -1;
  }

  if (this->netvc_context == NET_VCONNECTION_OUT && !this->_handshake_handler->has_remote_tp()) {
    return -1;
  }

  this->_init_flow_control_params(this->_handshake_handler->local_transport_parameters(),
                                  this->_handshake_handler->remote_transport_parameters());

  this->_start_application();

  return 0;
}

void
QUICNetVConnection::_schedule_path_validation_timeout(ink_hrtime interval)
{
  if (!this->_path_validation_timeout) {
    QUICConDebug("Schedule %s event in %" PRIu64 "ms", QUICDebugNames::quic_event(QUIC_EVENT_PATH_VALIDATION_TIMEOUT), interval);
    this->_path_validation_timeout = this->thread->schedule_in_local(this, interval, QUIC_EVENT_PATH_VALIDATION_TIMEOUT);
  }
}

void
QUICNetVConnection::_unschedule_path_validation_timeout()
{
  if (this->_path_validation_timeout) {
    QUICConDebug("Unschedule %s event", QUICDebugNames::quic_event(QUIC_EVENT_PATH_VALIDATION_TIMEOUT));
    this->_path_validation_timeout->cancel();
    this->_path_validation_timeout = nullptr;
  }
}

void
QUICNetVConnection::_close_path_validation_timeout(Event *data)
{
  ink_assert(this->_path_validation_timeout == data);
  this->_path_validation_timeout = nullptr;
}

void
QUICNetVConnection::_start_application()
{
  if (!this->_application_started) {
    this->_application_started = true;

    const uint8_t *app_name;
    unsigned int app_name_len = 0;
    this->_handshake_handler->negotiated_application_name(&app_name, &app_name_len);
    if (app_name == nullptr) {
      app_name     = reinterpret_cast<const uint8_t *>(IP_PROTO_TAG_HTTP_QUIC.data());
      app_name_len = IP_PROTO_TAG_HTTP_QUIC.size();
    }

    if (netvc_context == NET_VCONNECTION_IN) {
      Continuation *endpoint = this->_next_protocol_set->findEndpoint(app_name, app_name_len);
      if (endpoint == nullptr) {
        this->_handle_error(std::make_unique<QUICConnectionError>(QUICTransErrorCode::VERSION_NEGOTIATION_ERROR));
      } else {
        endpoint->handleEvent(NET_EVENT_ACCEPT, this);
      }
    } else {
      this->action_.continuation->handleEvent(NET_EVENT_OPEN, this);
    }
  }
}

void
QUICNetVConnection::_switch_to_handshake_state()
{
  QUICConDebug("Enter state_handshake");
  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_handshake);
}

void
QUICNetVConnection::_switch_to_established_state()
{
  if (this->_complete_handshake_if_possible() == 0) {
    QUICConDebug("Enter state_connection_established");
    QUICConDebug("Negotiated cipher suite: %s", this->_handshake_handler->negotiated_cipher_suite());

    SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_connection_established);

    std::shared_ptr<const QUICTransportParameters> remote_tp = this->_handshake_handler->remote_transport_parameters();
    QUICConfig::scoped_config params;

    if (netvc_context == NET_VCONNECTION_IN || (netvc_context == NET_VCONNECTION_OUT && params->cm_exercise_enabled() &&
                                                !remote_tp->contains(QUICTransportParameterId::DISABLE_MIGRATION))) {
      this->_alt_con_manager = new QUICAltConnectionManager(this, *this->_ctable);
    }
  } else {
    // Illegal state change
    ink_assert(!"Handshake has to be completed");
  }
}

void
QUICNetVConnection::_switch_to_closing_state(QUICConnectionErrorUPtr error)
{
  if (this->_complete_handshake_if_possible() != 0) {
    QUICConDebug("Switching state without handshake completion");
  }
  if (error->msg) {
    QUICConDebug("Reason: %.*s", static_cast<int>(strlen(error->msg)), error->msg);
  }

  this->_connection_error = std::move(error);
  this->_schedule_packet_write_ready();

  this->remove_from_active_queue();
  this->set_inactivity_timeout(0);

  int index      = QUICTypeUtil::pn_space_index(this->_hs_protocol->current_encryption_level());
  ink_hrtime rto = this->_loss_detector[index]->current_rto_period();

  QUICConDebug("Enter state_connection_closing");
  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_connection_closing);

  // This states SHOULD persist for three times the
  // current Retransmission Timeout (RTO) interval as defined in
  // [QUIC-RECOVERY].
  this->_schedule_closing_timeout(3 * rto);
}

void
QUICNetVConnection::_switch_to_draining_state(QUICConnectionErrorUPtr error)
{
  if (this->_complete_handshake_if_possible() != 0) {
    QUICConDebug("Switching state without handshake completion");
  }
  if (error->msg) {
    QUICConDebug("Reason: %.*s", static_cast<int>(strlen(error->msg)), error->msg);
  }

  this->remove_from_active_queue();
  this->set_inactivity_timeout(0);

  int index      = QUICTypeUtil::pn_space_index(this->_hs_protocol->current_encryption_level());
  ink_hrtime rto = this->_loss_detector[index]->current_rto_period();

  QUICConDebug("Enter state_connection_draining");
  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_connection_draining);

  // This states SHOULD persist for three times the
  // current Retransmission Timeout (RTO) interval as defined in
  // [QUIC-RECOVERY].

  this->_schedule_closing_timeout(3 * rto);
}

void
QUICNetVConnection::_switch_to_close_state()
{
  this->_unschedule_closing_timeout();
  this->_unschedule_path_validation_timeout();

  if (this->_complete_handshake_if_possible() != 0) {
    QUICConDebug("Switching state without handshake completion");
  }
  QUICConDebug("Enter state_connection_closed");
  SET_HANDLER((NetVConnHandler)&QUICNetVConnection::state_connection_closed);
  this->_schedule_closed_event();
}

void
QUICNetVConnection::_handle_idle_timeout()
{
  this->remove_from_active_queue();
  this->_switch_to_draining_state(std::make_unique<QUICConnectionError>(QUICTransErrorCode::NO_ERROR, "Idle Timeout"));

  // TODO: signal VC_EVENT_ACTIVE_TIMEOUT/VC_EVENT_INACTIVITY_TIMEOUT to application
}

void
QUICNetVConnection::_validate_new_path()
{
  this->_path_validator->validate();
  // Not sure how long we should wait. The spec says just "enough time".
  // Use the same time amount as the closing timeout.
  int index      = QUICTypeUtil::pn_space_index(this->_hs_protocol->current_encryption_level());
  ink_hrtime rto = this->_loss_detector[index]->current_rto_period();
  this->_schedule_path_validation_timeout(3 * rto);
}

void
QUICNetVConnection::_update_cids()
{
  snprintf(this->_cids_data, sizeof(this->_cids_data), "%08" PRIx32 "-%08" PRIx32 "", this->_peer_quic_connection_id.h32(),
           this->_quic_connection_id.h32());

  this->_cids = {this->_cids_data, sizeof(this->_cids_data)};
}

void
QUICNetVConnection::_update_peer_cid(const QUICConnectionId &new_cid)
{
  if (is_debug_tag_set(QUIC_DEBUG_TAG.data())) {
    char old_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    char new_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    this->_peer_quic_connection_id.hex(old_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);
    new_cid.hex(new_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);

    QUICConDebug("dcid: %s -> %s", old_cid_str, new_cid_str);
  }

  this->_peer_quic_connection_id = new_cid;
  this->_update_cids();
}

void
QUICNetVConnection::_update_local_cid(const QUICConnectionId &new_cid)
{
  if (is_debug_tag_set(QUIC_DEBUG_TAG.data())) {
    char old_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    char new_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    this->_quic_connection_id.hex(old_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);
    new_cid.hex(new_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);

    QUICConDebug("scid: %s -> %s", old_cid_str, new_cid_str);
  }

  this->_quic_connection_id = new_cid;
  this->_update_cids();
}

void
QUICNetVConnection::_rerandomize_original_cid()
{
  QUICConnectionId tmp = this->_original_quic_connection_id;
  this->_original_quic_connection_id.randomize();

  if (is_debug_tag_set(QUIC_DEBUG_TAG.data())) {
    char old_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    char new_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    tmp.hex(old_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);
    this->_original_quic_connection_id.hex(new_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);

    QUICConDebug("original cid: %s -> %s", old_cid_str, new_cid_str);
  }
}

bool
QUICNetVConnection::_is_src_addr_verified()
{
  return this->_src_addr_verified;
}

QUICHandshakeProtocol *
QUICNetVConnection::_setup_handshake_protocol(SSL_CTX *ctx)
{
  // Initialize handshake protocol specific stuff
  // For QUICv1 TLS is the only option
  QUICTLS *tls = new QUICTLS(ctx, this->direction());
  SSL_set_ex_data(tls->ssl_handle(), QUIC::ssl_quic_qc_index, static_cast<QUICConnection *>(this));
  return tls;
}

QUICConnectionErrorUPtr
QUICNetVConnection::_state_connection_established_migrate_connection(const QUICPacket &p)
{
  ink_assert(this->_handshake_handler->is_completed());

  QUICConnectionErrorUPtr error = nullptr;
  QUICConnectionId dcid         = p.destination_cid();

  if (dcid == this->_quic_connection_id) {
    return error;
  }

  if (this->netvc_context == NET_VCONNECTION_IN) {
    if (this->_remote_alt_cids.empty()) {
      // TODO: Should endpoint send connection error when remote endpoint doesn't send NEW_CONNECTION_ID frames before initiating
      // connection migration ?
      QUICConDebug("Ignore connection migration - remote endpoint initiated CM before sending NEW_CONNECTION_ID frames");

      return error;
    } else {
      QUICConDebug("Connection migration is initiated by remote");
    }
  }

  if (this->_alt_con_manager->migrate_to(dcid, this->_reset_token)) {
    // DCID of received packet is local cid
    this->_update_local_cid(dcid);

    // On client side (NET_VCONNECTION_OUT), nothing to do any more
    if (this->netvc_context == NET_VCONNECTION_IN) {
      Connection con;
      con.setRemote(&(p.from().sa));
      this->con.move(con);

      this->_update_peer_cid(this->_remote_alt_cids.front());
      this->_remote_alt_cids.pop();
      this->_validate_new_path();
    }
  } else {
    char dcid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    dcid.hex(dcid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);
    QUICConDebug("Connection migration failed cid=%s", dcid_str);
  }

  return error;
}

/**
 * Connection Migration Excercise from client
 */
QUICConnectionErrorUPtr
QUICNetVConnection::_state_connection_established_initiate_connection_migration()
{
  ink_assert(this->_handshake_handler->is_completed());
  ink_assert(this->netvc_context == NET_VCONNECTION_OUT);

  QUICConnectionErrorUPtr error = nullptr;

  std::shared_ptr<const QUICTransportParameters> remote_tp = this->_handshake_handler->remote_transport_parameters();
  QUICConfig::scoped_config params;

  if (!params->cm_exercise_enabled() || this->_connection_migration_initiated ||
      remote_tp->contains(QUICTransportParameterId::DISABLE_MIGRATION) || this->_remote_alt_cids.empty() ||
      this->_alt_con_manager->will_generate_frame(QUICEncryptionLevel::ONE_RTT)) {
    return error;
  }

  QUICConDebug("Initiated connection migration");
  this->_connection_migration_initiated = true;

  this->_update_peer_cid(this->_remote_alt_cids.front());
  this->_remote_alt_cids.pop();

  this->_validate_new_path();

  return error;
}

void
QUICNetVConnection::_handle_path_validation_timeout(Event *data)
{
  this->_close_path_validation_timeout(data);
  if (!this->_path_validator->is_validated()) {
    this->_switch_to_close_state();
  }
}