/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/lib/http/session/HQSession.h>

#include <proxygen/lib/http/codec/HQControlCodec.h>
#include <proxygen/lib/http/codec/HQUtils.h>
#include <proxygen/lib/http/codec/HTTP1xCodec.h>
#include <proxygen/lib/http/codec/HTTPCodec.h>
#include <proxygen/lib/http/codec/HTTPCodecFilter.h>
#include <proxygen/lib/http/session/HTTPSession.h>
#include <proxygen/lib/http/session/HTTPSessionController.h>
#include <proxygen/lib/http/session/HTTPSessionStats.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>

#include <folly/CppAttributes.h>
#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/DelayedDestructionBase.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/HHWheelTimer.h>
#include <quic/QuicConstants.h>
#include <quic/logging/QLoggerConstants.h>
#include <wangle/acceptor/ConnectionManager.h>

namespace {
static const uint16_t kMaxReadsPerLoop = 16;
static const std::string kNoProtocolString("");
static const std::string kH1QV1ProtocolString("h1q-fb");
static const std::string kH1QLigerProtocolString("h1q");
static const std::string kH1QV2ProtocolString("h1q-fb-v2");
static const std::string kQUICProtocolName("QUIC");

// handleSessionError is mostly setup to process application error codes
// that we want to send.  If we receive an application error code, convert to
// HTTP_CLOSED_CRITICAL_STREAM
quic::QuicErrorCode quicControlStreamError(quic::QuicErrorCode error) {
  switch (error.type()) {
    case quic::QuicErrorCode::Type::ApplicationErrorCode_E:
      return quic::QuicErrorCode(
          proxygen::HTTP3::ErrorCode::HTTP_CLOSED_CRITICAL_STREAM);
    case quic::QuicErrorCode::Type::LocalErrorCode_E:
    case quic::QuicErrorCode::Type::TransportErrorCode_E:
      return error;
  }
  folly::assume_unreachable();
}
} // namespace

using namespace proxygen::hq;

namespace proxygen {

const std::string kH3FBCurrentDraft("h3-fb-05");
const std::string kH3CurrentDraft("h3-27");
const std::string kHQCurrentDraft("hq-27");

const http2::PriorityUpdate hqDefaultPriority{kSessionStreamId, false, 15};

HQSession::~HQSession() {
  VLOG(3) << *this << " closing";
  runDestroyCallbacks();
}

void HQSession::setSessionStats(HTTPSessionStats* stats) {
  HTTPSessionBase::setSessionStats(stats);
  invokeOnAllStreams([&stats](HQStreamTransportBase* stream) {
    stream->byteEventTracker_.setTTLBAStats(stats);
  });
}

void HQSession::setPartiallyReliableCallbacks(quic::StreamId id) {
  sock_->setDataExpiredCallback(id, &unidirectionalReadDispatcher_);
  sock_->setDataRejectedCallback(id, &unidirectionalReadDispatcher_);
}

void HQSession::onNewBidirectionalStream(quic::StreamId id) noexcept {
  VLOG(4) << __func__ << " sess=" << *this << ": new streamID=" << id;
  // The transport should never call onNewBidirectionalStream before
  // onTransportReady
  DCHECK(versionUtils_) << "The transport should never call " << __func__
                        << " before onTransportReady";
  if (!versionUtils_->checkNewStream(id)) {
    return;
  }
  auto hqStream = findNonDetachedStream(id);
  DCHECK(!hqStream);
  hqStream = createStreamTransport(id);
  DCHECK(hqStream);
  sock_->setReadCallback(id, this);
  maxIncomingStreamId_ = std::max(maxIncomingStreamId_, id);
}

void HQSession::onNewUnidirectionalStream(quic::StreamId id) noexcept {
  // This is where a new unidirectional ingress stream is available
  // Try to check whether this is a push
  // if yes, register this as a push
  VLOG(4) << __func__ << " sess=" << *this << ": new streamID=" << id;
  // The transport should never call onNewUnidirectionalStream
  // before onTransportReady
  DCHECK(versionUtils_) << "The transport should never call " << __func__
                        << " before onTransportReady";
  if (!versionUtils_->checkNewStream(id)) {
    return;
  }

  // The new stream should not exist yet.
  auto existingStream = findStream(id);
  DCHECK(!existingStream) << "duplicate " << __func__ << " for streamID=" << id;
  // This has to be a new control or push stream, but we haven't read the
  // preface yet
  // Assign the stream to the dispatcher
  unidirectionalReadDispatcher_.takeTemporaryOwnership(id);
  sock_->setPeekCallback(id, &unidirectionalReadDispatcher_);
}

void HQSession::onStopSending(quic::StreamId id,
                              quic::ApplicationErrorCode error) noexcept {
  isInOnStopSending_ = true;
  SCOPE_EXIT {
    isInOnStopSending_ = false;
  };
  auto errorCode = static_cast<HTTP3::ErrorCode>(error);
  VLOG(3) << __func__ << " sess=" << *this << ": new streamID=" << id
          << " error=" << toString(errorCode);
  auto stream = findStream(id);
  if (stream) {
    handleWriteError(stream, error);
  }
}

bool HQSession::H1QFBV1VersionUtils::checkNewStream(quic::StreamId id) {
  // Reject all unidirectional streams and all server-initiated streams
  if (session_.sock_->isUnidirectionalStream(id) ||
      session_.sock_->isServerStream(id)) {
    session_.abortStream(HTTPException::Direction::INGRESS_AND_EGRESS,
                         id,
                         HTTP3::ErrorCode::HTTP_WRONG_STREAM);
    return false;
  }
  return true;
}

bool HQSession::GoawayUtils::checkNewStream(HQSession& session,
                                            quic::StreamId id) {
  // Reject all bidirectional, server-initiated streams
  if (session.sock_->isBidirectionalStream(id) &&
      session.sock_->isServerStream(id)) {
    session.abortStream(HTTPException::Direction::INGRESS_AND_EGRESS,
                        id,
                        HTTP3::ErrorCode::HTTP_WRONG_STREAM);
    return false;
  }
  // Cancel any stream that is out of the range allowed by GOAWAY
  if (session.drainState_ != DrainState::NONE) {
    // TODO: change this in (id >= maxAllowedStreamId_)
    // (see https://github.com/quicwg/base-drafts/issues/1717)
    // NOTE: need to consider the downstream case as well, since streams may
    // come out of order and we may get a new stream with lower id than
    // advertised in the goaway, and we need to accept that
    if ((session.direction_ == TransportDirection::UPSTREAM &&
         id > session.maxAllowedStreamId_) ||
        (session.direction_ == TransportDirection::DOWNSTREAM &&
         session.sock_->isBidirectionalStream(id) &&
         id > session.maxIncomingStreamId_)) {
      session.abortStream(HTTPException::Direction::INGRESS_AND_EGRESS,
                          id,
                          HTTP3::ErrorCode::HTTP_REQUEST_REJECTED);
      return false;
    }
  }

  return true;
}

bool HQSession::onTransportReadyCommon() noexcept {
  localAddr_ = sock_->getLocalAddress();
  peerAddr_ = sock_->getPeerAddress();
  quicInfo_->clientConnectionId = sock_->getClientConnectionId();
  quicInfo_->serverConnectionId = sock_->getServerConnectionId();
  // NOTE: this can drop the connection if the next protocol is not supported
  if (!getAndCheckApplicationProtocol()) {
    return false;
  }
  transportInfo_.acceptTime = getCurrentTime();
  getCurrentTransportInfoWithoutUpdate(&transportInfo_);
  transportInfo_.setupTime = millisecondsSince(transportStart_);
  transportInfo_.connectLatency = millisecondsSince(transportStart_).count();
  transportInfo_.protocolInfo = quicInfo_;
  if (!versionUtils_->createEgressControlStreams()) {
    return false;
  }
  // Apply the default settings
  // TODO: 0-RTT settings
  versionUtils_->applySettings({});
  // notifyPendingShutdown may be invoked before onTransportReady,
  // so we need to address that here by kicking the GOAWAY logic if needed
  if (drainState_ == DrainState::PENDING) {
    versionUtils_->sendGoaway();
  }
  return true;
}

bool HQSession::H1QFBV2VersionUtils::createEgressControlStreams() {
  if (!session_.createEgressControlStream(
          UnidirectionalStreamType::H1Q_CONTROL)) {
    return false;
  }
  session_.scheduleWrite();
  return true;
}

bool HQSession::HQVersionUtils::createEgressControlStreams() {
  if (!session_.createEgressControlStream(UnidirectionalStreamType::CONTROL) ||
      !session_.createEgressControlStream(
          UnidirectionalStreamType::QPACK_ENCODER) ||
      !session_.createEgressControlStream(
          UnidirectionalStreamType::QPACK_DECODER)) {
    return false;
  }

  session_.sendSettings();
  session_.scheduleWrite();
  return true;
}

bool HQSession::createEgressControlStream(UnidirectionalStreamType streamType) {
  auto id = sock_->createUnidirectionalStream();
  if (id.hasError()) {
    LOG(ERROR) << "Failed to create " << streamType
               << " unidirectional stream. error='" << id.error() << "'";
    onConnectionError(std::make_pair(quic::LocalErrorCode::CONNECT_FAILED,
                                     "Failed to create unidirectional stream"));
    return false;
  }

  auto matchPair = controlStreams_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(streamType),
      std::forward_as_tuple(*this, id.value(), streamType));
  CHECK(matchPair.second) << "Emplacement failed";
  sock_->setControlStream(id.value());
  matchPair.first->second.generateStreamPreface();
  return true;
}

HQSession::HQControlStream* FOLLY_NULLABLE
HQSession::createIngressControlStream(quic::StreamId id,
                                      UnidirectionalStreamType streamType) {

  auto ctrlStream = findControlStream(streamType);
  // this is an error in the use of the API, egress control streams must get
  // created at the very beginning
  if (!ctrlStream) {
    LOG(FATAL) << "Cannot create ingress control stream without an egress "
                  "stream streamID="
               << id << " sess=" << *this;
    return nullptr;
  }

  if (ctrlStream->ingressCodec_) {
    LOG(ERROR) << "Too many " << streamType << " streams for sess=" << *this;
    dropConnectionAsync(
        std::make_pair(HTTP3::ErrorCode::HTTP_WRONG_STREAM_COUNT,
                       "HTTP wrong stream count"),
        kErrorConnection);
    return nullptr;
  }

  ctrlStream->setIngressStreamId(id);
  ctrlStream->setIngressCodec(
      versionUtils_->createControlCodec(streamType, *ctrlStream));
  return ctrlStream;
}

std::unique_ptr<hq::HQUnidirectionalCodec>
HQSession::H1QFBV2VersionUtils::createControlCodec(
    hq::UnidirectionalStreamType type, HQControlStream& controlStream) {
  switch (type) {
    case hq::UnidirectionalStreamType::H1Q_CONTROL: {
      auto codec = std::make_unique<hq::HQControlCodec>(
          controlStream.getIngressStreamId(),
          session_.direction_,
          hq::StreamDirection::INGRESS,
          session_.ingressSettings_,
          type);
      codec->setCallback(&controlStream);
      return codec;
    }
    default:
      CHECK(false);
      return nullptr;
  }
}

std::unique_ptr<hq::HQUnidirectionalCodec>
HQSession::HQVersionUtils::createControlCodec(hq::UnidirectionalStreamType type,
                                              HQControlStream& controlStream) {
  switch (type) {
    case hq::UnidirectionalStreamType::CONTROL: {
      auto codec = std::make_unique<hq::HQControlCodec>(
          controlStream.getIngressStreamId(),
          session_.direction_,
          hq::StreamDirection::INGRESS,
          session_.ingressSettings_,
          type);
      codec->setCallback(&controlStream);
      return codec;
    }
    // This is quite weird for now. The stream types are defined  based on
    // the component that initiates them, so the ingress stream from the
    // QPACK Encoder is linked to the local QPACKDecoder, and viceversa
    case hq::UnidirectionalStreamType::QPACK_ENCODER:
      return std::make_unique<hq::QPACKEncoderCodec>(qpackCodec_,
                                                     controlStream);
    case hq::UnidirectionalStreamType::QPACK_DECODER:
      return std::make_unique<hq::QPACKDecoderCodec>(qpackCodec_,
                                                     controlStream);
    default:
      LOG(FATAL) << "Failed to create ingress codec";
      return nullptr;
  }
}

bool HQSession::getAndCheckApplicationProtocol() {
  CHECK(sock_);
  auto alpn = sock_->getAppProtocol();
  if (alpn) {
    if (alpn == kH1QV1ProtocolString || alpn == kH1QLigerProtocolString ||
        alpn == kHQCurrentDraft) {
      version_ = HQVersion::H1Q_FB_V1;
    } else if (alpn == kH1QV2ProtocolString) {
      version_ = HQVersion::H1Q_FB_V2;
    } else if (alpn == kH3FBCurrentDraft || alpn == kH3CurrentDraft) {
      version_ = HQVersion::HQ;
    }
  }
  if (!alpn || !version_) {
    // next protocol not specified or version not supported, close connection
    // with error
    LOG(ERROR) << "next protocol not supported: "
               << (alpn ? *alpn : "no protocol") << " sess=" << *this;

    onConnectionError(std::make_pair(quic::LocalErrorCode::CONNECT_FAILED,
                                     "ALPN not supported"));
    return false;
  }
  alpn_ = *alpn;
  setVersionUtils();
  return true;
}

void HQSession::setVersionUtils() {
  DCHECK(version_);
  switch (*version_) {
    case HQVersion::H1Q_FB_V1:
      versionUtils_ = std::make_unique<H1QFBV1VersionUtils>(*this);
      break;
    case HQVersion::H1Q_FB_V2:
      versionUtils_ = std::make_unique<H1QFBV2VersionUtils>(*this);
      break;
    case HQVersion::HQ:
      versionUtils_ = std::make_unique<HQVersionUtils>(*this);
      break;
    default:
      LOG(FATAL) << "No Version Utils for version " << alpn_;
  }
  versionUtilsReady_.set();
}

void HQSession::onReplaySafe() noexcept {
  // We might have got onTransportReady with 0-rtt in which case we only get the
  // server connection id after replay safe.
  quicInfo_->serverConnectionId = sock_->getServerConnectionId();
  if (infoCallback_) {
    infoCallback_->onFullHandshakeCompletion(*this);
  }

  for (auto callback : waitingForReplaySafety_) {
    callback->onReplaySafe();
  }
  waitingForReplaySafety_.clear();
}

void HQSession::onConnectionError(
    std::pair<quic::QuicErrorCode, std::string> code) noexcept {
  // the connector will drop the connection in case of connect error
  CHECK(!isInOnStopSending_)
      << "got connection error during onStopSending: " << code.second;
  HQSession::DestructorGuard dg(this);
  VLOG(4) << __func__ << " sess=" << *this
          << ": connection error=" << code.second;

  // Map application errors here to kErrorConnectionReset: eg, the peer tore
  // down the connection
  auto proxygenErr = toProxygenError(code.first, /*fromPeer=*/true);
  if (infoCallback_) {
    infoCallback_->onIngressError(*this, proxygenErr);
  }

  onConnectionErrorHandler(code);

  // force close all streams.
  // close with error won't invoke any connection callback, reentrancy safe
  dropConnectionSync(std::move(code), proxygenErr);
}

bool HQSession::getCurrentTransportInfo(wangle::TransportInfo* tinfo) {
  getCurrentTransportInfoWithoutUpdate(tinfo);
  tinfo->setupTime = transportInfo_.setupTime;
  tinfo->secure = transportInfo_.secure;
  tinfo->appProtocol = transportInfo_.appProtocol;
  tinfo->connectLatency = transportInfo_.connectLatency;
  // Copy props from the transport info.
  transportInfo_.rtt = tinfo->rtt;
  transportInfo_.rtt_var = tinfo->rtt_var;
  if (sock_) {
    auto quicInfo = sock_->getTransportInfo();
    quicInfo_->ptoCount = quicInfo.ptoCount;
    quicInfo_->totalPTOCount = quicInfo.totalPTOCount;
    quicInfo_->totalTransportBytesSent = quicInfo.bytesSent;
    quicInfo_->totalTransportBytesRecvd = quicInfo.bytesRecvd;
    quicInfo_->transportSettings = sock_->getTransportSettings();
    tinfo->protocolInfo = quicInfo_;
  }
  return true;
}

bool HQSession::getCurrentTransportInfoWithoutUpdate(
    wangle::TransportInfo* tinfo) const {
  tinfo->validTcpinfo = true;
  tinfo->appProtocol = std::make_shared<std::string>(alpn_);
  tinfo->securityType = kQUICProtocolName;
  tinfo->protocolInfo = quicInfo_;
  if (sock_) {
    auto quicInfo = sock_->getTransportInfo();
    tinfo->rtt = quicInfo.srtt;
    tinfo->rtt_var = static_cast<int64_t>(quicInfo.rttvar.count());
    tinfo->caAlgo = std::string(
        congestionControlTypeToString(quicInfo.congestionControlType));
    // Cwnd is logged in terms of MSS.
    tinfo->cwnd = static_cast<int64_t>(quicInfo.congestionWindow /
                                       quicInfo.mss);
    tinfo->mss = quicInfo.mss;
    tinfo->cwndBytes = static_cast<int64_t>(quicInfo.congestionWindow);
    tinfo->rtx = static_cast<int64_t>(quicInfo.packetsRetransmitted);
    tinfo->rtx_tm = static_cast<int64_t>(quicInfo.timeoutBasedLoss);
    tinfo->rto = static_cast<int64_t>(quicInfo.pto.count());
    tinfo->totalBytes = static_cast<int64_t>(quicInfo.bytesSent);
  }
  // TODO: fill up other properties.
  return true;
}

bool HQSession::getCurrentStreamTransportInfo(QuicStreamProtocolInfo* qspinfo,
                                              quic::StreamId streamId) {
  if (sock_) {
    auto streamTransportInfo = sock_->getStreamTransportInfo(streamId);
    if (streamTransportInfo) {
      qspinfo->streamTransportInfo = streamTransportInfo.value();
      return true;
    }
  }
  return false;
}

void HQSession::HQStreamTransportBase::generateGoaway() {
  folly::IOBufQueue dummyBuf{folly::IOBufQueue::cacheChainLength()};
  if (!codecStreamId_) {
    codecStreamId_ = 0;
  }
  auto g = folly::makeGuard(setActiveCodec(__func__));
  if (codecFilterChain->isReusable() || codecFilterChain->isWaitingToDrain()) {
    codecFilterChain->generateGoaway(
        dummyBuf, *codecStreamId_, ErrorCode::NO_ERROR);
  }
}

bool HQSession::HQStreamTransportBase::hasPendingBody() const {
  return writeBuf_.chainLength() != 0 ||
         (queueHandle_.isTransactionEnqueued() && txn_.hasPendingBody());
}

bool HQSession::HQStreamTransportBase::hasPendingEOM() const {
  return pendingEOM_ ||
         (queueHandle_.isTransactionEnqueued() && txn_.isEgressEOMQueued());
}

bool HQSession::HQStreamTransportBase::hasPendingEgress() const {
  return writeBuf_.chainLength() > 0 || pendingEOM_ ||
         queueHandle_.isTransactionEnqueued();
}

bool HQSession::HQStreamTransportBase::wantsOnWriteReady(size_t canSend) const {
  // The txn wants onWriteReady if it's enqueued AND
  //   a) There is available flow control and it has body OR
  //   b) All body is egressed and it has only pending EOM
  return queueHandle_.isTransactionEnqueued() &&
         ((canSend > writeBuf_.chainLength() && txn_.hasPendingBody()) ||
          (!txn_.hasPendingBody() && txn_.isEgressEOMQueued()));
}

void HQSession::drainImpl() {
  if (drainState_ != DrainState::NONE) {
    // no op
    VLOG(5) << "Already draining sess=" << *this;
    return;
  }
  drainState_ = DrainState::PENDING;
  if (versionUtils_) {
    versionUtils_->sendGoaway();
  }
  setCloseReason(ConnectionCloseReason::SHUTDOWN);
}

void HQSession::H1QFBV1VersionUtils::sendGoaway() {
  session_.invokeOnAllStreams(
      [](HQStreamTransportBase* stream) { stream->generateGoaway(); });
}

void HQSession::GoawayUtils::sendGoaway(HQSession& session) {
  if (session.direction_ == TransportDirection::UPSTREAM) {
    return;
  }
  if (session.drainState_ == DrainState::DONE) {
    return;
  }
  // send GOAWAY frame on the control stream
  DCHECK(session.drainState_ == DrainState::PENDING ||
         session.drainState_ == DrainState::FIRST_GOAWAY);

  auto connCtrlStream =
      session.findControlStream((session.version_ == HQVersion::H1Q_FB_V2)
                                    ? UnidirectionalStreamType::H1Q_CONTROL
                                    : UnidirectionalStreamType::CONTROL);
  auto g = folly::makeGuard(connCtrlStream->setActiveCodec(__func__));
  // cannot get here before onTransportReady, since the VersionUtils are being
  // set after ALPN is available
  DCHECK(connCtrlStream);
  auto goawayStreamId = session.getGoawayStreamId();
  auto generated = connCtrlStream->codecFilterChain->generateGoaway(
      connCtrlStream->writeBuf_, goawayStreamId, ErrorCode::NO_ERROR);
  auto writeOffset =
      session.sock_->getStreamWriteOffset(connCtrlStream->getEgressStreamId());
  auto writeBufferedBytes = session.sock_->getStreamWriteBufferedBytes(
      connCtrlStream->getEgressStreamId());
  if (generated == 0 || writeOffset.hasError() ||
      writeBufferedBytes.hasError()) {
    // shortcut to shutdown
    LOG(ERROR) << " error generating GOAWAY sess=" << session;
    session.drainState_ = DrainState::DONE;
    return;
  }
  VLOG(3) << "generated GOAWAY maxStreamID=" << goawayStreamId
          << " sess=" << session;

  auto res = session.sock_->registerDeliveryCallback(
      connCtrlStream->getEgressStreamId(),
      *writeOffset + *writeBufferedBytes +
          connCtrlStream->writeBuf_.chainLength(),
      connCtrlStream);
  if (res.hasError()) {
    // shortcut to shutdown
    LOG(ERROR) << " error generating GOAWAY sess=" << session;
    session.drainState_ = DrainState::DONE;
    return;
  }
  session.scheduleWrite();
  if (session.drainState_ == DrainState::PENDING) {
    session.drainState_ = DrainState::FIRST_GOAWAY;
  } else {
    DCHECK_EQ(session.drainState_, DrainState::FIRST_GOAWAY);
    session.drainState_ = DrainState::SECOND_GOAWAY;
  }
}

quic::StreamId HQSession::getGoawayStreamId() {
  if (drainState_ == DrainState::NONE || drainState_ == DrainState::PENDING) {
    // The maximum representable stream id in a quic varint
    return quic::kEightByteLimit;
  }
  return maxIncomingStreamId_;
}

size_t HQSession::sendSettings() {
  DCHECK(versionUtils_) << "The transport should never call " << __func__
                        << " before onTransportReady";
  return versionUtils_->sendSettings();
}

size_t HQSession::HQVersionUtils::sendSettings() {
  for (auto& setting : session_.egressSettings_.getAllSettings()) {
    auto id = httpToHqSettingsId(setting.id);
    if (id) {
      switch (*id) {
        case hq::SettingId::HEADER_TABLE_SIZE:
          qpackCodec_.setDecoderHeaderTableMaxSize(setting.value);
          break;
        case hq::SettingId::QPACK_BLOCKED_STREAMS:
          qpackCodec_.setMaxBlocking(setting.value);
          break;
        case hq::SettingId::MAX_HEADER_LIST_SIZE:
          break;
      }
    }
  }

  auto connCtrlStream =
      session_.findControlStream(UnidirectionalStreamType::CONTROL);
  auto g = folly::makeGuard(connCtrlStream->setActiveCodec(__func__));
  DCHECK(connCtrlStream);
  auto generated = connCtrlStream->codecFilterChain->generateSettings(
      connCtrlStream->writeBuf_);
  session_.scheduleWrite();
  return generated;
}

void HQSession::notifyPendingShutdown() {
  VLOG(4) << __func__ << " sess=" << *this;
  drainImpl();
}

void HQSession::closeWhenIdle() {
  VLOG(4) << __func__ << " sess=" << *this;
  drainImpl();
  if (version_ == HQVersion::H1Q_FB_V1) {
    drainState_ = DrainState::DONE;
  }
  cleanupPendingStreams();
  checkForShutdown();
}

void HQSession::dropConnection(const std::string& errorMsg) {
  auto msg = errorMsg.empty() ? "Stopping" : errorMsg;
  dropConnectionSync(std::make_pair(HTTP3::ErrorCode::HTTP_NO_ERROR, msg),
                     kErrorDropped);
}

void HQSession::dropConnectionAsync(
    std::pair<quic::QuicErrorCode, std::string> errorCode,
    ProxygenError proxygenError) {
  if (!dropInNextLoop_.has_value()) {
    dropInNextLoop_ = std::make_pair(errorCode, proxygenError);
    scheduleLoopCallback(true);
  } else {
    VLOG(4) << "Session already scheduled to be dropped: sess=" << *this;
  }
}

void HQSession::dropConnectionSync(
    std::pair<quic::QuicErrorCode, std::string> errorCode,
    ProxygenError proxygenError) {
  VLOG(4) << __func__ << " sess=" << *this;
  HQSession::DestructorGuard dg(this);
  // dropping_ is used to guard against dropConnection->onError->dropConnection
  // re-entrancy. instead drainState_ = DONE means the connection can only be
  // deleted naturally in checkForShutdown.
  // We can get here with drainState_ == DONE, if somthing is holding a
  // DestructorGuardon the session when it gets dropped.
  if (dropping_) {
    VLOG(5) << "Already dropping sess=" << *this;
    return;
  }
  dropping_ = true;
  if (getNumStreams() > 0) {
    // should deliver errors to all open streams, they will all detach-
    sock_->close(std::move(errorCode));
    sock_.reset();
    setCloseReason(ConnectionCloseReason::SHUTDOWN);
    // If the txn had no registered cbs, there could be streams left
    // But we are not supposed to unregister the read callback, so this really
    // shouldn't happen
    invokeOnAllStreams([proxygenError](HQStreamTransportBase* stream) {
      stream->errorOnTransaction(proxygenError, "Dropped connection");
    });
  } else {
    // Can only be here if this wasn't fully drained. Cases like
    //  notify + drop  (PENDING)
    //  notify + CLOSE_SENT (in last request) + reset (no response) + drop
    //  CLOSE_RECEIVED (in last response) + drop
    // In any of these cases, it's ok to just close the socket.
    // Note that the socket could already be deleted in case multiple calls
    // happen, under a destructod guard.
    if (sock_) {
      // this should be closeNow()
      sock_->close(folly::none);
      sock_.reset();
    }
  }
  drainState_ = DrainState::DONE;
  cancelLoopCallback();
  checkForShutdown();
  if (VLOG_IS_ON(5)) {
    unidirectionalReadDispatcher_.invokeOnPendingStreamIDs(
        [&](quic::StreamId pendingStreamId) {
          VLOG(5) << __func__ << " pendingStreamStillOpen: " << pendingStreamId;
        });
  }
  CHECK_EQ(getNumStreams(), 0);
}

void HQSession::checkForShutdown() {
  // For HQ upstream connections with a control stream, if the client wants to
  // go away, it can just stop creating new connections and set drainining
  // state to DONE, so that it will just shut down the socket when all the
  // request streams are done. In the process it will still be able to receive
  // and process GOAWAYs from the server
  // NOTE: this cannot be moved in VersionUtils, since we need to be able to
  // shutdown even before versionUtils is set in onTransportReady
  if (version_ != HQVersion::H1Q_FB_V1 &&
      direction_ == TransportDirection::UPSTREAM &&
      drainState_ == DrainState::PENDING) {
    if (VLOG_IS_ON(5)) {
      unidirectionalReadDispatcher_.invokeOnPendingStreamIDs(
          [&](quic::StreamId pendingStreamId) {
            VLOG(5) << __func__
                    << " pendingStreamStillOpen: " << pendingStreamId;
          });
    }
    drainState_ = DrainState::DONE;
  }

  // This is somewhat inefficient, checking every stream for possible detach
  // when we know explicitly earlier which ones are ready.  This is here to
  // minimize issues with iterator invalidation.
  invokeOnAllStreams(
      [](HQStreamTransportBase* stream) { stream->checkForDetach(); });
  if (drainState_ == DrainState::DONE && (getNumStreams() == 0) &&
      !isLoopCallbackScheduled()) {
    if (sock_) {
      sock_->close(folly::none);
      sock_.reset();
    }

    destroy();
  }
}

void HQSession::errorOnTransactionId(quic::StreamId id, HTTPException ex) {
  auto stream = findStream(id);
  if (stream) {
    stream->errorOnTransaction(std::move(ex));
  }
}

void HQSession::HQStreamTransportBase::errorOnTransaction(
    ProxygenError err, const std::string& errorMsg) {
  std::string extraErrorMsg;
  if (!errorMsg.empty()) {
    extraErrorMsg = folly::to<std::string>(". ", errorMsg);
  }

  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS,
                   folly::to<std::string>(getErrorString(err),
                                          " on transaction id: ",
                                          getStreamId(),
                                          extraErrorMsg));
  ex.setProxygenError(err);
  errorOnTransaction(std::move(ex));
}

void HQSession::HQStreamTransportBase::errorOnTransaction(HTTPException ex) {
  if (!detached_) {
    txn_.onError(std::move(ex));
  }
  if (ex.isIngressException()) {
    abortIngress();
  }
  if (ex.isEgressException()) {
    abortEgress(true);
  }
}

HQSession::HQStreamTransportBase* FOLLY_NULLABLE
HQSession::findNonDetachedStream(quic::StreamId streamId) {
  return findStreamImpl(streamId,
                        true /* includeEgress */,
                        true /* includeIngress */,
                        false /* includeDetached */);
}

HQSession::HQStreamTransportBase* FOLLY_NULLABLE
HQSession::findStream(quic::StreamId streamId) {
  return findStreamImpl(streamId,
                        true /* includeEgress */,
                        true /* includeIngress */,
                        true /* includeDetached */);
}

HQSession::HQStreamTransportBase* FOLLY_NULLABLE
HQSession::findIngressStream(quic::StreamId streamId, bool includeDetached) {
  return findStreamImpl(streamId,
                        false /* includeEgress */,
                        true /* includeIngress */,
                        includeDetached);
}

HQSession::HQStreamTransportBase* FOLLY_NULLABLE
HQSession::findEgressStream(quic::StreamId streamId, bool includeDetached) {
  return findStreamImpl(streamId,
                        true /* includeEgress */,
                        false /* includeIngress */,
                        includeDetached);
}

HQSession::HQStreamTransportBase* FOLLY_NULLABLE
HQSession::findStreamImpl(quic::StreamId streamId,
                          bool includeEgress,
                          bool includeIngress,
                          bool includeDetached) {
  HQStreamTransportBase* pstream{nullptr};
  auto it = streams_.find(streamId);
  if (it != streams_.end()) {
    pstream = &it->second;
  }
  if (!pstream && (includeIngress || includeEgress)) {
    pstream = findPushStream(streamId);
  }
  if (!pstream) {
    return nullptr;
  }
  CHECK(pstream);
  DCHECK(pstream->isUsing(streamId));
  if (!includeDetached) {
    if (pstream->detached_) {
      return nullptr;
    }
  }
  return pstream;
}

HQSession::HQControlStream* FOLLY_NULLABLE
HQSession::findControlStream(UnidirectionalStreamType streamType) {
  auto it = controlStreams_.find(streamType);
  if (it == controlStreams_.end()) {
    return nullptr;
  } else {
    return &it->second;
  }
}

HQSession::HQControlStream* FOLLY_NULLABLE
HQSession::findControlStream(quic::StreamId streamId) {
  auto it = std::find_if(
      controlStreams_.begin(),
      controlStreams_.end(),
      [&](const std::pair<const UnidirectionalStreamType, HQControlStream>&
              entry) { return entry.second.isUsing(streamId); });
  if (it == controlStreams_.end()) {
    return nullptr;
  } else {
    return &it->second;
  }
}

bool HQSession::eraseStream(quic::StreamId streamId) {
  // Try different possible locations and remove the
  // stream
  bool erased = false;
  if (streams_.erase(streamId)) {
    erased = true;
  }

  // TODO: only do this when stream is server-uni
  erased |= erasePushStream(streamId);

  return erased;
}

void HQSession::injectTraceEventIntoAllTransactions(TraceEvent& event) {
  invokeOnAllStreams([event](HQStreamTransportBase* stream) {
    HTTPTransactionHandler *handler = stream->txn_.getHandler();
    if (handler != nullptr) {
      handler->traceEventAvailable(event);
    }
  });
}

void HQSession::runLoopCallback() noexcept {
  // We schedule this callback to run at the end of an event
  // loop iteration if either of two conditions has happened:
  //   * The session has generated some egress data (see scheduleWrite())
  //   * Reads have become unpaused (see resumeReads())

  inLoopCallback_ = true;
  HQSession::DestructorGuard dg(this);
  auto scopeg = folly::makeGuard([this, toSend=maxToSend_] {
    // This ScopeGuard needs to be under the above DestructorGuard
    updatePendingWrites();
    if (toSend > 0) {
      if (txnEgressQueue_.empty()) {
        // We wrote out everything
        resumeTransactions();
      } else {
        // We couldn't write everything, the socket is backpressuring
        pauseTransactions();
      }
    }
    checkForShutdown();
    inLoopCallback_ = false;
  });

  if (dropInNextLoop_.has_value()) {
    dropConnectionSync(dropInNextLoop_->first, dropInNextLoop_->second);
    return;
  }

  readsPerLoop_ = 0;

  // First process the read data
  //   - and maybe resume reads on the stream
  processReadData();

  versionUtils_->readDataProcessed();

  // Then handle the writes
  // Write all the control streams first
  maxToSend_ -= writeControlStreams(maxToSend_);
  // Then write the request streams
  if (!txnEgressQueue_.empty() && maxToSend_ > 0) {
    // TODO: we could send FIN only?
    maxToSend_ = writeRequestStreams(maxToSend_);
  }
  // Zero out maxToSend_ here.  We won't egress anything else until the next
  // onWriteReady call
  maxToSend_ = 0;

  if (!txnEgressQueue_.empty()) {
    scheduleWrite();
  }

  // Maybe schedule the next loop callback
  VLOG(4) << "sess=" << *this << " maybe schedule the next loop callback. "
          << " pending writes: " << !txnEgressQueue_.empty()
          << " pending processing reads: " << pendingProcessReadSet_.size();
  if (!pendingProcessReadSet_.empty()) {
    scheduleLoopCallback(false);
  }
  // checkForShutdown is now in ScopeGuard
}

void HQSession::HQVersionUtils::readDataProcessed() {
  auto ici = qpackCodec_.encodeInsertCountInc();
  if (ici) {
    auto QPACKDecoderStream =
        session_.findControlStream(UnidirectionalStreamType::QPACK_DECODER);
    DCHECK(QPACKDecoderStream);
    QPACKDecoderStream->writeBuf_.append(std::move(ici));
    // don't need to explicitly schedule write because this is called in the
    // loop before control streams are written
  }
}

folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
HQSession::HQVersionUtils::onIngressPeekDataAvailable(uint64_t streamOffset) {
  CHECK(hqStreamCodecPtr_);
  return hqStreamCodecPtr_->onIngressDataAvailable(streamOffset);
}

folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
HQSession::HQVersionUtils::onIngressDataExpired(uint64_t streamOffset) {
  CHECK(hqStreamCodecPtr_);
  return hqStreamCodecPtr_->onIngressDataExpired(streamOffset);
}

folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
HQSession::HQVersionUtils::onIngressDataRejected(uint64_t streamOffset) {
  CHECK(hqStreamCodecPtr_);
  return hqStreamCodecPtr_->onIngressDataRejected(streamOffset);
}

folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
HQSession::HQVersionUtils::onEgressBodySkip(uint64_t bodyOffset) {
  CHECK(hqStreamCodecPtr_);
  return hqStreamCodecPtr_->onEgressBodySkip(bodyOffset);
}

folly::Expected<uint64_t, hq::UnframedBodyOffsetTrackerError>
HQSession::HQVersionUtils::onEgressBodyReject(uint64_t bodyOffset) {
  CHECK(hqStreamCodecPtr_);
  return hqStreamCodecPtr_->onEgressBodyReject(bodyOffset);
}

void HQSession::scheduleWrite() {
  // always call for the whole connection and iterate trough all the streams
  // in onWriteReady
  if (scheduledWrite_) {
    return;
  }

  scheduledWrite_ = true;
  sock_->notifyPendingWriteOnConnection(this);
}

void HQSession::scheduleLoopCallback(bool thisIteration) {
  if (!isLoopCallbackScheduled()) {
    auto evb = getEventBase();
    if (evb) {
      evb->runInLoop(this, thisIteration);
    }
  }
}

void HQSession::resumeReads(quic::StreamId streamId) {
  VLOG(4) << __func__ << " sess=" << *this << ": resuming reads id="
          << streamId;
  sock_->resumeRead(streamId);
  scheduleLoopCallback(true);
  // TODO: ideally we should cancel the managed timeout when all the streams are
  // paused and then restart it when the timeouts are unpaused
}

void HQSession::resumeReads() {
  VLOG(4) << __func__ << " sess=" << *this << ": resuming reads";
  invokeOnIngressStreams([this] (HQStreamTransportBase* hqStream) {
    if (sock_) {
      sock_->resumeRead(hqStream->getIngressStreamId());
    }
  });
}


void HQSession::pauseReads(quic::StreamId streamId) {
  VLOG(4) << __func__ << " sess=" << *this << ": pausing reads id=" << streamId;
  sock_->pauseRead(streamId);
}

void HQSession::pauseReads() {
  VLOG(4) << __func__ << " sess=" << *this << ": pausing reads";
  invokeOnIngressStreams([this] (HQStreamTransportBase* hqStream) {
      sock_->pauseRead(hqStream->getIngressStreamId());
    });
}

void HQSession::readAvailable(quic::StreamId id) noexcept {
  // this is the bidirectional callback
  VLOG(4) << __func__ << " sess=" << *this
          << ": readAvailable on streamID=" << id;
  if (readsPerLoop_ >= kMaxReadsPerLoop) {
    VLOG(2) << __func__ << " sess=" << *this
            << ":skipping read for streamID=" << id
            << " maximum reads per loop reached";
    return;
  }
  readsPerLoop_++;
  readRequestStream(id);

  scheduleLoopCallback(true);
}

void HQSession::readError(
    quic::StreamId id,
    std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
        error) noexcept {
  VLOG(4) << __func__ << " sess=" << *this << ": readError streamID=" << id
          << " error: " << error;

  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS,
                   folly::to<std::string>("Got error=", quic::toString(error)));

  switch (error.first.type()) {
    case quic::QuicErrorCode::Type::ApplicationErrorCode_E: {
      auto errorCode =
          static_cast<HTTP3::ErrorCode>(*error.first.asApplicationErrorCode());
      VLOG(3) << "readError: QUIC Application Error: " << toString(errorCode)
              << " streamID=" << id << " sess=" << *this;
      auto stream = findNonDetachedStream(id);
      if (stream) {
        stream->onResetStream(errorCode, std::move(ex));
      } else {
        // When a stream is erased, it's callback is cancelled, so it really
        // should be here
        VLOG(3)
            << "readError: received application error=" << toString(errorCode)
            << " for detached streamID=" << id << " sess=" << *this;
      }
      break;
    }
    case quic::QuicErrorCode::Type::LocalErrorCode_E: {
      quic::LocalErrorCode& errorCode = *error.first.asLocalErrorCode();
      VLOG(3) << "readError: QUIC Local Error: " << errorCode
              << " streamID=" << id << " sess=" << *this;
      if (errorCode == quic::LocalErrorCode::CONNECT_FAILED) {
        ex.setProxygenError(kErrorConnect);
      } else {
        ex.setProxygenError(kErrorShutdown);
      }
      errorOnTransactionId(id, std::move(ex));
      break;
    }
    case quic::QuicErrorCode::Type::TransportErrorCode_E: {
      quic::TransportErrorCode& errorCode = *error.first.asTransportErrorCode();
      VLOG(3) << "readError: QUIC Transport Error: " << errorCode
              << " streamID=" << id << " sess=" << *this;
      ex.setProxygenError(kErrorConnectionReset);
      // TODO: set Quic error when quic is OSS
      ex.setErrno(uint32_t(errorCode));
      errorOnTransactionId(id, std::move(ex));
      break;
    }
  }
}

bool HQSession::isPartialReliabilityEnabled(quic::StreamId id) {
  if (!isPartialReliabilityEnabled()) {
    VLOG(10) << __func__ << " PR disabled for the session streamID=" << id;
    return false;
  }
  auto hqStream = findNonDetachedStream(id);
  if (!hqStream) {
    VLOG(10) << __func__ << " stream possibly detached streamID=" << id;
    return false;
  }
  if (!sock_->isBidirectionalStream(id)) {
    VLOG(10) << __func__ << " PR disabled for unidirectional streamID=" << id;
    return false;
  }

  VLOG(10) << __func__ << " PR enabled for streamID=" << id;
  return true;
}

HQSession::HQStreamTransportBase* FOLLY_NULLABLE
HQSession::getPRStream(quic::StreamId id, const char *event) {
  DCHECK(isPartialReliabilityEnabled(id))
    << "PR not enabled prior to " << event;
  auto hqStream = findStream(id);
  if (hqStream) {
    if (hqStream->detached_) {
      LOG(ERROR) << event << " event received for detached stream " << id;
      return nullptr;
    }
  }
  return hqStream;
}

void HQSession::onPartialDataAvailable(
    quic::StreamId id,
    const HQUnidirStreamDispatcher::Callback::PeekData& partialData) {
  auto hqStream = getPRStream(id, "data");
  if (hqStream) {
    hqStream->processPeekData(partialData);
  }
}

void HQSession::processExpiredData(quic::StreamId id, uint64_t offset) {
  auto hqStream = getPRStream(id, "expired");
  if (hqStream) {
    hqStream->processDataExpired(offset);
  }
}

void HQSession::processRejectedData(quic::StreamId id, uint64_t offset) {
  auto hqStream = getPRStream(id, "rejected");
  if (hqStream) {
    hqStream->processDataRejected(offset);
  }
}

void HQSession::timeoutExpired() noexcept {
  VLOG(3) << "ManagedConnection timeoutExpired " << *this;
  if (getNumStreams() > 0) {
    VLOG(3) << "ignoring session timeout " << *this;
    resetTimeout();
    return;
  }
  VLOG(3) << "Timeout with nothing pending " << *this;
  setCloseReason(ConnectionCloseReason::TIMEOUT);
  closeWhenIdle();
}

HQSession::HQControlStream* FOLLY_NULLABLE
HQSession::tryCreateIngressControlStream(quic::StreamId id, uint64_t preface) {
  auto res = versionUtils_->parseStreamPreface(preface);
  if (!res) {
    LOG(ERROR) << "Got unidirectional stream with unknown preface "
               << static_cast<uint64_t>(preface) << " streamID=" << id
               << " sess=" << *this;
    return nullptr;
  }

  auto ctrlStream = createIngressControlStream(id, *res);
  if (!ctrlStream) {
    return nullptr;
  }

  sock_->setControlStream(id);
  return ctrlStream;
}

folly::Optional<UnidirectionalStreamType>
HQSession::H1QFBV2VersionUtils::parseStreamPreface(uint64_t preface) {
  hq::UnidirectionalTypeF parse = [](hq::UnidirectionalStreamType type)
      -> folly::Optional<UnidirectionalStreamType> {
    switch (type) {
      case UnidirectionalStreamType::H1Q_CONTROL:
        return type;
      default:
        return folly::none;
    }
  };
  return hq::withType(preface, parse);
}

folly::Optional<UnidirectionalStreamType>
HQSession::HQVersionUtils::parseStreamPreface(uint64_t preface) {
  hq::UnidirectionalTypeF parse = [](hq::UnidirectionalStreamType type)
      -> folly::Optional<UnidirectionalStreamType> {
    switch (type) {
      case UnidirectionalStreamType::CONTROL:
      case UnidirectionalStreamType::PUSH:
      case UnidirectionalStreamType::QPACK_ENCODER:
      case UnidirectionalStreamType::QPACK_DECODER:
        return type;
      default:
        return folly::none;
    }
  };
  return hq::withType(preface, parse);
}

void HQSession::readControlStream(HQControlStream* ctrlStream) {
  DCHECK(ctrlStream);
  auto readRes = sock_->read(ctrlStream->getIngressStreamId(), 0);
  if (readRes.hasError()) {
    LOG(ERROR) << "Got synchronous read error=" << readRes.error();
    readError(ctrlStream->getIngressStreamId(),
              {readRes.error(), folly::StringPiece("sync read error")});
    return;
  }
  resetTimeout();
  quic::Buf data = std::move(readRes.value().first);
  auto readSize = data ? data->computeChainDataLength() : 0;
  VLOG(4) << "Read " << readSize << " bytes from control stream";
  ctrlStream->readBuf_.append(std::move(data));
  ctrlStream->readEOF_ = readRes.value().second;

  if (infoCallback_) {
    infoCallback_->onRead(
      *this, readSize,
      static_cast<HTTPCodec::StreamID>(ctrlStream->getIngressStreamId()));
  }
  // GOAWAY may trigger session destroy, need a guard for that
  DestructorGuard dg(this);
  ctrlStream->processReadData();
}

// Dispatcher method implementation
void HQSession::assignReadCallback(quic::StreamId id,
                                   hq::UnidirectionalStreamType type,
                                   size_t toConsume,
                                   quic::QuicSocket::ReadCallback* const cb) {
  VLOG(4) << __func__ << " streamID=" << id << " type=" << type
          << " toConsume=" << toConsume << " cb=" << std::hex << cb;
  CHECK(cb) << "Bug in dispatcher - null callback passed";

  auto consumeRes = sock_->consume(id, toConsume);
  CHECK(!consumeRes.hasError()) << "Unexpected error consuming bytes";

  // Notify the read callback
  if (infoCallback_) {
    infoCallback_->onRead(
      *this, toConsume, static_cast<HTTPCodec::StreamID>(id));
  }

  auto ctrlStream =
      tryCreateIngressControlStream(id, static_cast<uint64_t>(type));

  if (!ctrlStream) {
    rejectStream(id);
    return;
  }

  // After reading the preface we can switch to the regular readCallback
  sock_->setPeekCallback(id, nullptr);
  sock_->setReadCallback(id, cb);

  // The transport will send notifications via the read callback
  // for the *future* events, but not for this one.
  // In case there is additional data on the control stream,
  // it can be not seen until the next read notification.
  // To mitigate that, we propagate the onReadAvailable to the control stream.
  controlStreamReadAvailable(id);
}

// Dispatcher method implementation
void HQSession::assignPeekCallback(quic::StreamId id,
                                   hq::UnidirectionalStreamType type,
                                   size_t toConsume,
                                   quic::QuicSocket::PeekCallback* const cb) {
  VLOG(4) << __func__ << " streamID=" << id << " type=" << type
          << " toConsume=" << toConsume << " cb=" << std::hex << cb;
  CHECK(cb) << "Bug in dispatcher - null callback passed";

  auto consumeRes = sock_->consume(id, toConsume);
  CHECK(!consumeRes.hasError()) << "Unexpected error consuming bytes";

  // Install the new peek callback
  sock_->setPeekCallback(id, cb);
}

void HQSession::rejectStream(quic::StreamId id) {
  // Do not read data for unknown unidirectional stream types.
  // Send STOP_SENDING and rely on the peer sending a RESET to clear the
  // stream in the transport.
  sock_->stopSending(id, HTTP3::ErrorCode::HTTP_UNKNOWN_STREAM_TYPE);
  // It is safe to stop reading from this stream.
  // The peer is supposed to reset it on receipt of a STOP_SENDING
  sock_->setPeekCallback(id, nullptr);
  sock_->setReadCallback(id, nullptr);
}

folly::Optional<hq::UnidirectionalStreamType> HQSession::parseStreamPreface(
    uint64_t preface) {
  return versionUtils_->parseStreamPreface(preface);
}

size_t HQSession::cleanupPendingStreams() {
  std::vector<quic::StreamId> streamsToCleanup;

  // Collect the pending stream ids from the dispatcher
  unidirectionalReadDispatcher_.invokeOnPendingStreamIDs(
      [&](quic::StreamId pendingStreamId) {
        streamsToCleanup.push_back(pendingStreamId);
      });

  cleanupUnboundPushStreams(streamsToCleanup);

  // Clean up the streams by detaching all callbacks
  for (auto pendingStreamId : streamsToCleanup) {
    clearStreamCallbacks(pendingStreamId);
  }

  return streamsToCleanup.size();
}

void HQSession::clearStreamCallbacks(quic::StreamId id) {
  if (sock_) {
    sock_->setReadCallback(id, nullptr);
    sock_->setPeekCallback(id, nullptr);

    if (isPartialReliabilityEnabled()) {
      sock_->setDataExpiredCallback(id, nullptr);
      sock_->setDataRejectedCallback(id, nullptr);
    }
  } else {
    VLOG(4) << "Attempt to clear callbacks on closed socket";
  }
}

void HQSession::controlStreamReadAvailable(quic::StreamId id) {
  VLOG(4) << __func__ << " sess=" << *this << ": streamID=" << id;
  auto ctrlStream = findControlStream(id);
  if (!ctrlStream) {
    LOG(ERROR) << "Got readAvailable on unknown stream id=" << id
               << " sess=" << *this;
    return;
  }
  readControlStream(ctrlStream);
}

void HQSession::controlStreamReadError(
    quic::StreamId id,
    const HQUnidirStreamDispatcher::Callback::ReadError& error) {
  VLOG(4) << __func__ << " sess=" << *this << ": readError streamID=" << id
          << " error: " << error;

  auto ctrlStream = findControlStream(id);

  if (!ctrlStream) {
    const quic::LocalErrorCode* err = error.first.asLocalErrorCode();
    bool shouldLog = !err || (*err != quic::LocalErrorCode::NO_ERROR);
    LOG_IF(ERROR, shouldLog)
        << __func__ << " received read error=" << error
        << " for unknown control streamID=" << id << " sess=" << *this;
    return;
  }

  handleSessionError(ctrlStream,
                     StreamDirection::INGRESS,
                     quicControlStreamError(error.first),
                     toProxygenError(error.first));
}

void HQSession::readRequestStream(quic::StreamId id) noexcept {
  auto hqStream = findIngressStream(id, false /* includeDetached */);
  if (!hqStream) {
    // can we even get readAvailable after a stream is marked for detach ?
    DCHECK(findStream(id));
    return;
  }
  // Read as much as you possibly can!
  auto readRes = sock_->read(id, 0);

  if (readRes.hasError()) {
    LOG(ERROR) << "Got synchronous read error=" << readRes.error();
    readError(id, {readRes.error(), folly::StringPiece("sync read error")});
    return;
  }

  resetTimeout();
  quic::Buf data = std::move(readRes.value().first);
  auto readSize = data ? data->computeChainDataLength() : 0;
  hqStream->readEOF_ = readRes.value().second;
  VLOG(3) << "Got streamID=" << hqStream->getStreamId() << " len=" << readSize
          << " eof=" << uint32_t(hqStream->readEOF_) << " sess=" << *this;
  if (hqStream->readEOF_) {
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hqStream->createdTime);
    if (sock_ && sock_->getState() && sock_->getState()->qLogger) {
      sock_->getState()->qLogger->addStreamStateUpdate(
        id, quic::kOnEOM, timeDiff);
    }
  } else if (readSize == 0) {
    VLOG(3) << "Got a blank read, ignoring sess=" << *this;
    return;
  }
  // Just buffer the data and postpone processing in the loop callback
  hqStream->readBuf_.append(std::move(data));

  if (infoCallback_) {
    infoCallback_->onRead(*this, readSize, hqStream->getStreamId());
  }

  pendingProcessReadSet_.insert(id);
}

void HQSession::processReadData() {
  for (auto it = pendingProcessReadSet_.begin();
       it != pendingProcessReadSet_.end();) {
    auto g = folly::makeGuard([&]() {
      // the codec may not have processed all the data, but we won't ask again
      // until we get more
      // TODO: set a timeout?
      it = pendingProcessReadSet_.erase(it);
    });

    HQStreamTransportBase* ingressStream =
        findIngressStream(*it, true /* includeDetached */);

    if (!ingressStream) {
      // ingress on a transaction may cause other transactions to get deleted
      continue;
    }

    // Check whether the stream has been detached
    if (ingressStream->detached_) {
      VLOG(4) << __func__ << " killing pending read data for detached txn="
              << ingressStream->txn_;
      ingressStream->readBuf_.move();
      ingressStream->readEOF_ = false;
      continue;
    }

    // Feed it to the codec
    auto blocked = ingressStream->processReadData();
    if (!blocked) {
      if (ingressStream->readEOF_) {
        ingressStream->onIngressEOF();
      }
      continue;
    }
  }
}

void HQSession::H1QFBV1VersionUtils::headersComplete(HTTPMessage* msg) {
  // for h1q-fb-v1 start draining on receipt of a Connection:: close header
  if (session_.drainState_ == DrainState::DONE) {
    return;
  }
  if (msg->checkForHeaderToken(HTTP_HEADER_CONNECTION, "close", false)) {
    if (session_.drainState_ == DrainState::CLOSE_SENT) {
      session_.drainState_ = DrainState::DONE;
    } else {
      if (session_.drainState_ == DrainState::NONE) {
        session_.drainImpl();
      }
      session_.drainState_ = DrainState::CLOSE_RECEIVED;
    }
  }
}

void HQSession::HQVersionUtils::headersComplete(HTTPMessage* /*msg*/) {
  auto QPACKDecoderStream =
      session_.findControlStream(UnidirectionalStreamType::QPACK_DECODER);

  if (QPACKDecoderStream && !QPACKDecoderStream->writeBuf_.empty()) {
    session_.scheduleWrite();
  }
}

void HQSession::H1QFBV1VersionUtils::checkSendingGoaway(
    const HTTPMessage& msg) {
  if (session_.drainState_ == DrainState::NONE && !msg.wantsKeepalive()) {
    // Initiate the drain if the message explicitly requires no keepalive
    // NOTE: this will set the state to PENDING
    session_.notifyPendingShutdown();
  }

  if (session_.drainState_ == DrainState::CLOSE_RECEIVED) {
    session_.drainState_ = DONE;
  } else if (session_.drainState_ == DrainState::PENDING) {
    session_.drainState_ = DrainState::CLOSE_SENT;
  }
}

void HQSession::onSettings(const SettingsList& settings) {
  CHECK(versionUtils_);
  versionUtils_->onSettings(settings);
  receivedSettings_ = true;
}

void HQSession::HQVersionUtils::applySettings(const SettingsList& settings) {
  DestructorGuard g(&session_);
  VLOG(3) << "Got SETTINGS sess=" << session_;

  uint32_t tableSize = kDefaultIngressHeaderTableSize;
  uint32_t blocked = kDefaultIngressQpackBlockedStream;
  FOLLY_MAYBE_UNUSED uint32_t numPlaceholders = kDefaultIngressNumPlaceHolders;
  for (auto& setting : settings) {
    auto id = httpToHqSettingsId(setting.id);
    if (id) {
      switch (*id) {
        case hq::SettingId::HEADER_TABLE_SIZE:
          tableSize = setting.value;
          break;
        case hq::SettingId::QPACK_BLOCKED_STREAMS:
          blocked = setting.value;
          break;
        case hq::SettingId::MAX_HEADER_LIST_SIZE:
          // this setting is stored in ingressSettings_ and enforced in the
          // StreamCodec
          break;
      }
    }
  }
  qpackCodec_.setEncoderHeaderTableSize(tableSize);
  qpackCodec_.setMaxVulnerable(blocked);
  VLOG(3) << "Applied SETTINGS sess=" << session_ << " size=" << tableSize
          << " blocked=" << blocked;
}

void HQSession::HQVersionUtils::onSettings(const SettingsList& settings) {
  applySettings(settings);
  if (session_.infoCallback_) {
    session_.infoCallback_->onSettings(session_, settings);
  }
}

void HQSession::onGoaway(uint64_t lastGoodStreamID,
                         ErrorCode code,
                         std::unique_ptr<folly::IOBuf> /* debugData */) {
  // NOTE: This function needs to be idempotent. i.e. be a no-op if invoked
  // twice with the same lastGoodStreamID
  DCHECK_EQ(direction_, TransportDirection::UPSTREAM);
  DCHECK(version_ != HQVersion::H1Q_FB_V1);
  VLOG(3) << "Got GOAWAY maxStreamID=" << lastGoodStreamID << " sess=" << *this;
  maxAllowedStreamId_ = std::min(maxAllowedStreamId_, lastGoodStreamID);
  setCloseReason(ConnectionCloseReason::GOAWAY);
  // drains existing streams and prevents new streams to be created
  drainImpl();

  invokeOnNonDetachedStreams([this, code](HQStreamTransportBase* stream) {
    // Invoke onGoaway on all transactions
    stream->txn_.onGoaway(code);
    // Abort transactions which have been initiated locally but not created
    // successfully at the remote end
    // TODO: change this in (stream->getStreamId() >= maxAllowedStreamId_)
    // (see https://github.com/quicwg/base-drafts/issues/1717)
    if (stream->getStreamId() > maxAllowedStreamId_) {
      stream->errorOnTransaction(kErrorStreamUnacknowledged, "");
    }
  });

  if (drainState_ == DrainState::NONE || drainState_ == DrainState::PENDING) {
    drainState_ = DrainState::FIRST_GOAWAY;
  } else if (drainState_ == DrainState::FIRST_GOAWAY) {
    drainState_ = DrainState::DONE;
  }
  checkForShutdown();
}

void HQSession::pauseTransactions() {
  writesPaused_ = true;
  invokeOnEgressStreams(
      [](HQStreamTransportBase* stream) { stream->txn_.pauseEgress(); });
}

void HQSession::resumeTransactions() {
  DestructorGuard g(this);
  auto resumeFn = [this](HTTP2PriorityQueue&,
                         HTTPCodec::StreamID id,
                         HTTPTransaction* txn,
                         double) {
    if (txn && !txn->isEgressComplete() && sock_) {
      auto flowControl = sock_->getStreamFlowControl(id);
      if (!flowControl.hasError() && flowControl->sendWindowAvailable > 0) {
        txn->resumeEgress();
      }
    }
    return false;
  };
  auto stopFn = [this] {
    return !hasActiveTransactions();
  };

  txnEgressQueue_.iterateBFS(resumeFn, stopFn, true /* all */);
  writesPaused_ = false;
}

void HQSession::setNewTransactionPauseState(HTTPTransaction* txn) {
  bool pauseNew = writesPaused_;
  if (!pauseNew && sock_) {
    if (sock_->getConnectionBufferAvailable() == 0) {
      pauseNew = true;
    } else {
      auto flowControl = sock_->getConnectionFlowControl();
      if (!flowControl.hasError() && flowControl->sendWindowAvailable == 0) {
        pauseNew = true;
      }
    }
  }
  if (pauseNew) {
    CHECK(txn);
    // If writes are paused, start this txn off in the egress paused state
    VLOG(4) << *this << " starting streamID=" << txn->getID()
            << " egress paused";
    txn->pauseEgress();
  }
}

void HQSession::notifyEgressBodyBuffered(int64_t bytes) {
  if (HTTPSessionBase::notifyEgressBodyBuffered(bytes, true) &&
      !inLoopCallback_ && !isLoopCallbackScheduled() && sock_) {
    sock_->getEventBase()->runInLoop(this);
  }
}

void HQSession::onFlowControlUpdate(quic::StreamId id) noexcept {
  VLOG(4) << __func__ << " sess=" << *this << ": streamID=" << id;

  auto flowControl = sock_->getStreamFlowControl(id);
  if (flowControl.hasError()) {
    LOG(ERROR) << "Got error=" << flowControl.error() << " streamID=" << id;
    return;
  }

  auto ctrlStream = findControlStream(id);
  if (ctrlStream && flowControl->sendWindowAvailable > 0) {
    if (sock_ && sock_->getState() && sock_->getState()->qLogger) {
      sock_->getState()->qLogger->addStreamStateUpdate(
          id,
          quic::getFlowControlWindowAvailable(flowControl->sendWindowAvailable),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - ctrlStream->createdTime));
    }
    scheduleWrite();
    return;
  }

  auto stream = findEgressStream(id);

  if (!stream) {
    LOG(ERROR) << "Got flow control update for unknown streamID=" << id
               << " sess=" << this;
    return;
  }

  auto& txn = stream->txn_;
  // Check if this stream has flow control, or has only EOM pending
  if (flowControl->sendWindowAvailable > 0 ||
      (!stream->hasPendingBody() && stream->hasPendingEOM())) {
    // TODO: are we intentionally piggyback the time value for flow control
    // window here?
    if (sock_ && sock_->getState() && sock_->getState()->qLogger) {
      sock_->getState()->qLogger->addStreamStateUpdate(
          id,
          quic::getFlowControlWindowAvailable(flowControl->sendWindowAvailable),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - stream->createdTime));
    }
    if (stream->hasPendingEgress()) {
      txnEgressQueue_.signalPendingEgress(stream->queueHandle_.getHandle());
    }
    if (!stream->detached_ && txn.isEgressPaused()) {
      // txn might be paused
      txn.resumeEgress();
    }
    scheduleWrite();
  }
}

void HQSession::onConnectionWriteReady(uint64_t maxToSend) noexcept {
  VLOG(4) << __func__ << " sess=" << *this << ": maxToSend=" << maxToSend;
  scheduledWrite_ = false;
  maxToSend_ = maxToSend;

  scheduleLoopCallback(true);
}

void HQSession::onConnectionWriteError(
    std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
        error) noexcept {
  scheduledWrite_ = false;
  VLOG(4) << __func__ << " sess=" << *this << ": writeError error=" << error;
  // Leave this as a no-op.  We will most likely get onConnectionError soon
}

uint64_t HQSession::writeControlStreams(uint64_t maxEgress) {
  uint64_t maxEgressOrig = maxEgress;
  // NOTE: process the control streams in the order they are stored
  // this could potentially lead to stream starvation
  for (auto& it : controlStreams_) {
    if (it.second.writeBuf_.empty()) {
      continue;
    }
    auto sent = controlStreamWriteImpl(&it.second, maxEgress);
    DCHECK_LE(sent, maxEgress);
    maxEgress -= sent;
    if (maxEgress == 0) {
      break;
    }
  }
  return maxEgressOrig - maxEgress;
}

uint64_t HQSession::controlStreamWriteImpl(HQControlStream* ctrlStream,
                                           uint64_t maxEgress) {
  auto egressStreamId = ctrlStream->getEgressStreamId();
  auto flowControl = sock_->getStreamFlowControl(egressStreamId);
  if (flowControl.hasError()) {
    LOG(ERROR)
        << "Got error=" << flowControl.error() << " streamID=" << egressStreamId
        << " bufLen=" << static_cast<int>(ctrlStream->writeBuf_.chainLength())
        << " readEOF=" << ctrlStream->readEOF_;
    handleSessionError(ctrlStream,
                       StreamDirection::EGRESS,
                       quicControlStreamError(flowControl.error()),
                       toProxygenError(flowControl.error()));
    return 0;
  }

  auto streamSendWindow = flowControl->sendWindowAvailable;
  size_t canSend = std::min(streamSendWindow, maxEgress);
  auto sendLen = std::min(canSend, ctrlStream->writeBuf_.chainLength());
  auto tryWriteBuf = ctrlStream->writeBuf_.splitAtMost(canSend);

  CHECK_GE(sendLen, 0);
  VLOG(4) << __func__ << " before write sess=" << *this
          << ": streamID=" << egressStreamId << " maxEgress=" << maxEgress
          << " sendWindow=" << streamSendWindow
          << " tryToSend=" << tryWriteBuf->computeChainDataLength();

  auto writeRes = writeBase(egressStreamId,
                            &ctrlStream->writeBuf_,
                            std::move(tryWriteBuf),
                            sendLen,
                            false,
                            nullptr);

  if (writeRes.hasError()) {
    // Going to call this a write error no matter what the underlying reason was
    handleSessionError(ctrlStream,
                       StreamDirection::EGRESS,
                       quicControlStreamError(writeRes.error()),
                       kErrorWrite);
    return 0;
  }

  size_t sent = *writeRes;
  VLOG(4)
      << __func__ << " after write sess=" << *this
      << ": streamID=" << ctrlStream->getEgressStreamId() << " sent=" << sent
      << " buflen=" << static_cast<int>(ctrlStream->writeBuf_.chainLength());
  if (infoCallback_) {
    infoCallback_->onWrite(*this, sent);
  }

  CHECK_GE(maxEgress, sent);
  return sent;
}

void HQSession::handleSessionError(HQStreamBase* stream,
                                   StreamDirection streamDir,
                                   quic::QuicErrorCode err,
                                   ProxygenError proxygenError) {
  // This is most likely a control stream
  std::string appErrorMsg;
  HTTP3::ErrorCode appError = HTTP3::ErrorCode::HTTP_NO_ERROR;
  auto ctrlStream = dynamic_cast<HQControlStream*>(stream);
  if (ctrlStream) {
    auto id = (streamDir == StreamDirection::EGRESS
                   ? ctrlStream->getEgressStreamId()
                   : ctrlStream->getIngressStreamId());
    // TODO: This happens for each control stream during shutdown, and that is
    // too much for a LOG(ERROR)
    VLOG(3) << "Got error on control stream error=" << err << " streamID=" << id
            << " Dropping connection. sess=" << *this;
    appErrorMsg = "HTTP error on control stream";
  } else {
    auto requestStream = dynamic_cast<HQStreamTransport*>(stream);
    CHECK(requestStream);
    auto id = requestStream->getEgressStreamId();
    LOG(ERROR) << "Got error on request stream error=" << err
               << " streamID=" << id << " Dropping connection. sess=" << *this;
    appErrorMsg = "HTTP error on request stream";
    // for request streams this function must be called with an ApplicationError
    DCHECK(err.asApplicationErrorCode());
  }
  // errors on a control stream means we must drop the entire connection,
  // but there are some errors that we expect during shutdown
  bool shouldDrop = false;
  switch (err.type()) {
    case quic::QuicErrorCode::Type::ApplicationErrorCode_E:
      // An ApplicationErrorCode is expected when
      //  1. The peer resets a control stream
      //  2. A control codec detects a connection error on a control stream
      //  3. A stream codec detects a connection level error (eg: compression)
      // We always want to drop the connection in these cases.
      appError = static_cast<HTTP3::ErrorCode>(*err.asApplicationErrorCode());
      shouldDrop = true;
      break;
    case quic::QuicErrorCode::Type::LocalErrorCode_E:
      // a LocalErrorCode::NO_ERROR is expected whenever the socket gets
      // closed without error
      shouldDrop = (*err.asLocalErrorCode() != quic::LocalErrorCode::NO_ERROR);
      break;
    case quic::QuicErrorCode::Type::TransportErrorCode_E:
      shouldDrop = true;
      break;
  }
  if (!shouldDrop) {
    return;
  }
  if (ctrlStream && appError == HTTP3::ErrorCode::HTTP_NO_ERROR) {
    // If we got a local or transport error reading or writing on a
    // control stream, send CLOSED_CRITICAL_STREAM.
    appError = HTTP3::ErrorCode::HTTP_CLOSED_CRITICAL_STREAM;
  }
  // we cannot just simply drop the connection here, since in case of a
  // close received from the remote, we may have other readError callbacks on
  // other streams after this one. So run in the next loop callback, in this
  // same loop
  dropConnectionAsync(std::make_pair(appError, appErrorMsg),
                           proxygenError);
}

uint64_t HQSession::writeRequestStreams(uint64_t maxEgress) noexcept {
  // requestStreamWriteImpl may call txn->onWriteReady
  txnEgressQueue_.nextEgress(nextEgressResults_);
  for (auto it = nextEgressResults_.begin(); it != nextEgressResults_.end();
       ++it) {
    auto& ratio = it->second;
    auto hqStream =
        static_cast<HQStreamTransportBase*>(&it->first->getTransport());
    // TODO: scale maxToSend by ratio?
    auto sent = requestStreamWriteImpl(hqStream, maxEgress, ratio);
    DCHECK_LE(sent, maxEgress);
    maxEgress -= sent;

    if (maxEgress == 0 && std::next(it) != nextEgressResults_.end()) {
      VLOG(3) << __func__ << " sess=" << *this
              << " got more to send than the transport could take";
      break;
    }
  }
  nextEgressResults_.clear();
  return maxEgress;
}

void HQSession::handleWriteError(HQStreamTransportBase* hqStream,
                                 quic::QuicErrorCode err) {
  // We call this INGRESS_AND_EGRESS so it fully terminates the
  // HTTPTransaction state machine.
  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS,
                   folly::to<std::string>("Got error=", quic::toString(err)));
  // TODO: set Quic error when quic is OSS
  switch (err.type()) {
    case quic::QuicErrorCode::Type::ApplicationErrorCode_E: {
      // If we have an application error code, it must have
      // come from the peer (most likely STOP_SENDING). This
      // is logically a stream abort, not a write error
      auto h3ErrorCode =
          static_cast<HTTP3::ErrorCode>(*err.asApplicationErrorCode());
      ex.setCodecStatusCode(hqToHttpErrorCode(h3ErrorCode));
      ex.setProxygenError(h3ErrorCode == HTTP3::ErrorCode::HTTP_REQUEST_REJECTED
                              ? kErrorStreamUnacknowledged
                              : kErrorStreamAbort);
      break;
    }
    case quic::QuicErrorCode::Type::LocalErrorCode_E: {
      ex.setErrno(uint32_t(*err.asLocalErrorCode()));
      ex.setProxygenError(kErrorWrite);
      break;
    }
    case quic::QuicErrorCode::Type::TransportErrorCode_E: {
      CHECK(false) << "Unexpected errorCode=" << *err.asTransportErrorCode();
      break;
    }
  }
  // Do I need a dguard here?
  abortStream(ex.getDirection(),
              hqStream->getStreamId(),
              HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED);
  hqStream->errorOnTransaction(std::move(ex));
}

folly::Expected<size_t, quic::LocalErrorCode> HQSession::writeBase(
    quic::StreamId id,
    folly::IOBufQueue* writeBuf,
    std::unique_ptr<folly::IOBuf> data,
    size_t tryToSend,
    bool sendEof,
    quic::QuicSocket::DeliveryCallback* deliveryCallback) {

  auto writeRes = sock_->writeChain(id,
                                    std::move(data),
                                    sendEof,
                                    false, // cork
                                    deliveryCallback);
  if (writeRes.hasError()) {
    LOG(ERROR) << " Got error=" << writeRes.error() << " streamID=" << id;
    return folly::makeUnexpected(writeRes.error());
  }

  auto notWrittenBuf = std::move(writeRes.value());
  auto sent = tryToSend;
  if (notWrittenBuf && !notWrittenBuf->empty()) {
    // The transport gave back some data, prepend to the write buffer.
    // According to the QuicSocket API this should never happen if we are
    // enforcing the flow control limits
    VLOG(4) << "stream " << id << " got "
            << notWrittenBuf->computeChainDataLength()
            << " bytes back from the transport";
    sent -= notWrittenBuf->computeChainDataLength();
    VLOG(4) << __func__ << " sess=" << *this << ": streamID=" << id
            << " tryToSend: " << tryToSend << " actual bytes sent: " << sent;
    auto tmpBuf = writeBuf->move();
    writeBuf->append(std::move(notWrittenBuf));
    writeBuf->append(std::move(tmpBuf));
  }
  return sent;
}

size_t HQSession::handleWrite(HQStreamTransportBase* hqStream,
                              std::unique_ptr<folly::IOBuf> data,
                              size_t tryToSend,
                              bool sendEof) {
  quic::QuicSocket::DeliveryCallback* deliveryCallback =
      sendEof ? this : nullptr;

  auto writeRes = writeBase(hqStream->getEgressStreamId(),
                            &hqStream->writeBuf_,
                            std::move(data),
                            tryToSend,
                            sendEof,
                            deliveryCallback);
  if (writeRes.hasError()) {
    handleWriteError(hqStream, writeRes.error());
    return 0;
  }

  auto sent = *writeRes;
  if (sent == tryToSend && sendEof) {
    // This will hold the transaction open until onDeliveryAck or onCanceled
    DCHECK(deliveryCallback);
    hqStream->txn_.incrementPendingByteEvents();
    // NOTE: This may not be necessary long term, once we properly implement
    // detach or when we enforce flow control for headers and EOM
    hqStream->pendingEOM_ = false;
  }
  hqStream->bytesWritten_ += sent;
  // hqStream's byteEventTracker cannot be changed, so no need to pass
  // shared_ptr or use in while loop
  hqStream->byteEventTracker_.processByteEvents(
      nullptr, hqStream->streamEgressCommittedByteOffset());
  return sent;
}

uint64_t HQSession::requestStreamWriteImpl(HQStreamTransportBase* hqStream,
                                           uint64_t maxEgress,
                                           double ratio) {
  CHECK(hqStream->queueHandle_.isStreamTransportEnqueued());
  HTTPTransaction::DestructorGuard dg(&hqStream->txn_);

  auto streamId = hqStream->getStreamId();
  auto flowControl = sock_->getStreamFlowControl(streamId);
  if (flowControl.hasError()) {
    LOG(ERROR)
        << "Got error=" << flowControl.error() << " streamID=" << streamId
        << " detached=" << hqStream->detached_
        << " readBufLen=" << static_cast<int>(hqStream->readBuf_.chainLength())
        << " writeBufLen="
        << static_cast<int>(hqStream->writeBuf_.chainLength())
        << " readEOF=" << hqStream->readEOF_
        << " ingressError_=" << static_cast<int>(hqStream->ingressError_)
        << " eomGate_=" << hqStream->eomGate_;
    handleWriteError(hqStream, flowControl.error());
    return 0;
  }

  auto streamSendWindow = flowControl->sendWindowAvailable;

  size_t canSend = std::min(streamSendWindow, maxEgress);

  // we may have already buffered more than the amount the transport can take,
  // or the txn may not have any more body bytes/EOM to add. In case, there is
  // no need to call txn->onWriteReady.
  if (hqStream->wantsOnWriteReady(canSend)) {
    // Populate the write buffer by telling the transaction how much
    // room is available for body data
    size_t maxBodySend = canSend - hqStream->writeBuf_.chainLength();
    VLOG(4) << __func__ << " asking txn for more bytes sess=" << *this
            << ": streamID=" << streamId << " canSend=" << canSend
            << " remain=" << hqStream->writeBuf_.chainLength()
            << " pendingEOM=" << hqStream->pendingEOM_
            << " maxBodySend=" << maxBodySend << " ratio=" << ratio;
    hqStream->txn_.onWriteReady(maxBodySend, ratio);
    // onWriteReady may not be able to detach any byte from the deferred egress
    // body bytes, in case it's getting rate limited.
    // In that case the txn will get removed from the egress queue from
    // onWriteReady
    if (hqStream->writeBuf_.empty() && !hqStream->pendingEOM_) {
      return 0;
    }
  }
  auto sendLen = std::min(canSend, hqStream->writeBuf_.chainLength());
  auto tryWriteBuf = hqStream->writeBuf_.splitAtMost(canSend);
  bool sendEof = (hqStream->pendingEOM_ && !hqStream->hasPendingBody());

  CHECK(sendLen > 0 || sendEof);
  VLOG(4) << __func__ << " before write sess=" << *this
          << ": streamID=" << streamId << " maxEgress=" << maxEgress
          << " sendWindow=" << streamSendWindow
          << " tryToSend=" << tryWriteBuf->computeChainDataLength()
          << " sendEof=" << sendEof;

  size_t sent = handleWrite(hqStream, std::move(tryWriteBuf), sendLen, sendEof);

  VLOG(4) << __func__ << " after write sess=" << *this
          << ": streamID=" << streamId << " sent=" << sent
          << " buflen=" << static_cast<int>(hqStream->writeBuf_.chainLength())
          << " hasPendingBody=" << hqStream->txn_.hasPendingBody()
          << " EOM=" << hqStream->pendingEOM_;
  if (infoCallback_) {
    infoCallback_->onWrite(*this, sent);
  }
  CHECK_GE(maxEgress, sent);


  bool flowControlBlocked = (sent == streamSendWindow && !sendEof);
  if (flowControlBlocked) {
    // TODO: this one doesn't create trouble, but it's certainly not logging the
    // extra params anyway.
    if (sock_ && sock_->getState() && sock_->getState()->qLogger) {
      sock_->getState()->qLogger->addStreamStateUpdate(
          streamId,
          quic::kStreamBlocked,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - hqStream->createdTime));
    }
  }
  // sendAbort can clear the egress queue, so this stream may no longer be
  // enqueued
  if (hqStream->queueHandle_.isStreamTransportEnqueued() &&
      (!hqStream->hasPendingEgress() || flowControlBlocked)) {
    VLOG(4) << "clearPendingEgress for " << hqStream->txn_;
    txnEgressQueue_.clearPendingEgress(hqStream->queueHandle_.getHandle());
  }
  if (flowControlBlocked && !hqStream->txn_.isEgressComplete()) {
    VLOG(4) << __func__ << " txn flow control blocked, txn=" << hqStream->txn_;
    hqStream->txn_.pauseEgress();
  }
  return sent;
}

void HQSession::onDeliveryAck(quic::StreamId id,
                              uint64_t offset,
                              std::chrono::microseconds rtt) {
  VLOG(4) << __func__ << " sess=" << *this << ": streamID=" << id
          << " offset=" << offset;
  auto pEgressStream = findEgressStream(id, true /* includeDetached */);
  DCHECK(pEgressStream);
  if (pEgressStream) {
    pEgressStream->txn_.onEgressLastByteAck(
        std::chrono::duration_cast<std::chrono::milliseconds>(rtt));
    pEgressStream->txn_.decrementPendingByteEvents();
  } else {
    LOG(ERROR) << " not expecting to receive delivery ack for erased stream";
  }
}

void HQSession::onCanceled(quic::StreamId id, uint64_t /*offset*/) {
  VLOG(3) << __func__ << " sess=" << *this << ": streamID=" << id;
  auto pEgressStream = findEgressStream(id);
  if (pEgressStream) {
    pEgressStream->txn_.decrementPendingByteEvents();
  } else {
    LOG(DFATAL) << __func__ << " sess=" << *this << ": streamID=" << id
                << " onCanceled but txn missing, aborted without reset?";
  }
}

void HQSession::HQControlStream::onDeliveryAck(
    quic::StreamId id, uint64_t /*offset*/, std::chrono::microseconds /*rtt*/) {
  // We set the delivery callback for the control stream to keep track of the
  // GOAWAY being delivered to the remote endpoint. When that happens we can
  // send a second GOAWAY. sendGoaway is a no-op after the second time
  VLOG(3) << "GOAWAY received by remote endpoint on streamID=" << id
          << " sess=" << session_;
  session_.onGoawayAck();
}

void HQSession::HQControlStream::onCanceled(quic::StreamId id,
                                            uint64_t /*offset*/) {
  // This shouldn't really happen, but in case it does let's accelerate draining
  VLOG(3) << "GOAWAY delivery callback canceled on streamID=" << id
          << " sess=" << session_;
  session_.drainState_ = DrainState::DONE;
  // if we are shutting down, do so in the loop callback
  session_.scheduleLoopCallback(false);
}

void HQSession::onGoawayAck() {
  if (drainState_ == DrainState::FIRST_GOAWAY) {
    versionUtils_->sendGoaway();
  } else if (drainState_ == DrainState::SECOND_GOAWAY) {
    drainState_ = DrainState::DONE;
  }
  // if we are shutting down, do so in the loop callback
  scheduleLoopCallback(false);
}

HQSession::HQStreamTransport* FOLLY_NULLABLE
HQSession::createStreamTransport(quic::StreamId streamId) {
  VLOG(3) << __func__ << " sess=" << *this;

  // Checking for egress and ingress streams as well
  auto streamAlreadyExists = findStream(streamId);
  if (!sock_->good() || streamAlreadyExists) {
    VLOG(3) << __func__ << " Refusing to add a transaction on a closing "
            << " session / existing transaction"
            << " sock good: " << sock_->good()
            << "; streams count: " << streams_.count(streamId) << "; streamId "
            << streamId;
    return nullptr;
  }

  // If this is the first transport, invoke the connection
  // activation callbacks.
  // NOTE: Should this be called when an an ingress push stream
  // is created ?
  if (getNumStreams() == 0) {
    if (infoCallback_) {
      infoCallback_->onActivateConnection(*this);
    }
    if (getConnectionManager()) {
      getConnectionManager()->onActivated(*this);
    }
  }

  // The transport should never call createStreamTransport before
  // onTransportReady
  DCHECK(versionUtils_) << "The transport should never call " << __func__
                        << " before onTransportReady";
  std::unique_ptr<HTTPCodec> codec = versionUtils_->createCodec(streamId);
  auto matchPair = streams_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(streamId),
      std::forward_as_tuple(
          *this,
          direction_,
          streamId,
          getNumTxnServed(),
          std::move(codec),
          WheelTimerInstance(transactionsTimeout_, getEventBase()),
          nullptr, /*   HTTPSessionStats* sessionStats_ */
          hqDefaultPriority,
          folly::none /* assocStreamId */));
  incrementSeqNo();

  CHECK(matchPair.second) << "Emplacement failed, despite earlier "
                             "existence check.";

  if (versionUtils_ && drainState_ != DrainState::NONE) {
    versionUtils_->sendGoawayOnRequestStream(matchPair.first->second);
  }

  // tracks max historical streams
  HTTPSessionBase::onNewOutgoingStream(getNumOutgoingStreams());

  return &matchPair.first->second;
}

std::unique_ptr<HTTPCodec> HQSession::H1QFBV1VersionUtils::createCodec(
    quic::StreamId /*streamId*/) {
  return std::make_unique<HTTP1xCodec>(session_.direction_,
                                       session_.forceUpstream1_1_);
}

std::unique_ptr<HTTPCodec> HQSession::HQVersionUtils::createCodec(
    quic::StreamId streamId) {
  auto QPACKEncoderStream =
      session_.findControlStream(UnidirectionalStreamType::QPACK_ENCODER);
  DCHECK(QPACKEncoderStream);
  auto QPACKDecoderStream =
      session_.findControlStream(UnidirectionalStreamType::QPACK_DECODER);
  DCHECK(QPACKDecoderStream);
  auto codec = std::make_unique<hq::HQStreamCodec>(
      streamId,
      session_.direction_,
      qpackCodec_,
      QPACKEncoderStream->writeBuf_,
      QPACKDecoderStream->writeBuf_,
      [this, id = QPACKEncoderStream->getEgressStreamId()] {
        if (!session_.sock_) {
          return uint64_t(0);
        }
        auto res = session_.sock_->getStreamFlowControl(id);
        if (res.hasError()) {
          return uint64_t(0);
        }
        return res->sendWindowAvailable;
      },
      session_.egressSettings_,
      session_.ingressSettings_,
      session_.isPartialReliabilityEnabled());
  hqStreamCodecPtr_ = codec.get();
  return std::move(codec);
}

void HQSession::H1QFBV1VersionUtils::sendGoawayOnRequestStream(
    HQSession::HQStreamTransport& stream) {
  stream.generateGoaway();
}

HQSession::HQStreamTransportBase::HQStreamTransportBase(
    HQSession& session,
    TransportDirection direction,
    HTTPCodec::StreamID txnId,
    uint32_t seqNo,
    const WheelTimerInstance& timeout,
    HTTPSessionStats* stats,
    http2::PriorityUpdate priority,
    folly::Optional<HTTPCodec::StreamID> parentTxnId,
    folly::Optional<hq::UnidirectionalStreamType> type)
    : HQStreamBase(session, session.codec_, type),
      HTTP2PriorityQueueBase(kSessionStreamId),
      txn_(direction,
           txnId,
           seqNo,
           *this,
           *this,
           timeout.getWheelTimer(),
           timeout.getDefaultTimeout(),
           stats,
           false, // useFlowControl
           0,     // receiveInitialWindowSize
           0,     // sendInitialWindowSize,
           priority,
           parentTxnId),
      byteEventTracker_(nullptr) {
  VLOG(4) << __func__ << " txn=" << txn_;
  byteEventTracker_.setTTLBAStats(session_.sessionStats_);
  quicStreamProtocolInfo_ = std::make_shared<QuicStreamProtocolInfo>();
}

void HQSession::HQStreamTransportBase::initCodec(
    std::unique_ptr<HTTPCodec> codec, const std::string& where) {
  VLOG(3) << where << " " << __func__ << " txn=" << txn_;
  CHECK(session_.sock_)
      << "Socket is null drainState=" << (int)session_.drainState_
      << " streams=" << session_.getNumStreams();
  realCodec_ = std::move(codec);
  if (session_.version_ == HQVersion::HQ) {
    auto c = dynamic_cast<hq::HQStreamCodec*>(realCodec_.get());
    CHECK(c) << "HQ should use HQStream codec";
    c->setActivationHook([this] { return setActiveCodec("self"); });
  }
  auto g = folly::makeGuard(setActiveCodec(__func__));
  if (session_.direction_ == TransportDirection::UPSTREAM || txn_.isPushed()) {
    codecStreamId_ = codecFilterChain->createStream();
  }
  hasCodec_ = true;
}

void HQSession::HQStreamTransportBase::initIngress(const std::string& where) {
  VLOG(3) << where << " " << __func__ << " txn=" << txn_;
  CHECK(session_.sock_)
      << "Socket is null drainState=" << (int)session_.drainState_
      << " streams=" << session_.getNumStreams();

  if (session_.receiveStreamWindowSize_.has_value()) {
    session_.sock_->setStreamFlowControlWindow(
        getIngressStreamId(), session_.receiveStreamWindowSize_.value());
  }

  auto g = folly::makeGuard(setActiveCodec(where));

  codecFilterChain->setCallback(this);
  eomGate_.then([this] { txn_.onIngressEOM(); });
  hasIngress_ = true;
}

HTTPTransaction* FOLLY_NULLABLE
HQSession::newTransaction(HTTPTransaction::Handler* handler) {
  VLOG(4) << __func__ << " sess=" << *this;

  if (drainState_ == DrainState::CLOSE_SENT ||
      drainState_ == DrainState::FIRST_GOAWAY ||
      drainState_ == DrainState::DONE) {
    VLOG(4) << __func__ << " newTransaction after drain: " << *this;
    return nullptr;
  }
  if (!sock_->good()) {
    VLOG(4) << __func__ << " newTransaction after sock went bad: " << this;
    return nullptr;
  }

  // TODO stream limit handling
  auto quicStreamId = sock_->createBidirectionalStream();
  if (!quicStreamId) {
    VLOG(3) << __func__ << " failed to create new stream: " << this;
    return nullptr;
  }

  auto hqStream = createStreamTransport(quicStreamId.value());

  if (hqStream) {
    // DestructorGuard dg(this);
    hqStream->txn_.setHandler(CHECK_NOTNULL(handler));
    setNewTransactionPauseState(&hqStream->txn_);
    sock_->setReadCallback(quicStreamId.value(), this);
    return &hqStream->txn_;
  } else {
    VLOG(3) << __func__ << "Failed to create new transaction on "
            << quicStreamId.value();
    abortStream(HTTPException::Direction::INGRESS_AND_EGRESS,
                quicStreamId.value(),
                HTTP3::ErrorCode::HTTP_INTERNAL_ERROR);
    return nullptr;
  }
}

void HQSession::startNow() {
  VLOG(4) << __func__ << " sess=" << *this;
  CHECK(!started_);
  CHECK(sock_);
  started_ = true;
  transportInfo_.secure = true;
  transportInfo_.validTcpinfo = true;
  transportStart_ = getCurrentTime();
  // TODO: invoke socket.start() here
  resetTimeout();
}

void HQSession::HQStreamTransportBase::checkForDetach() {
  if (detached_ && readBuf_.empty() && writeBuf_.empty() && !pendingEOM_ &&
      !queueHandle_.isStreamTransportEnqueued()) {
    session_.detachStreamTransport(this);
  }
}

bool HQSession::HQStreamTransportBase::getCurrentTransportInfo(
    wangle::TransportInfo* tinfo) {
  VLOG(4) << __func__ << " txn=" << txn_;
  CHECK(quicStreamProtocolInfo_.get());
  bool success = session_.getCurrentTransportInfo(tinfo);

  // Save connection-level protocol fields in the HQStreamTransport-level
  // protocol info.
  if (success) {
    auto connectionTransportInfo =
        dynamic_cast<QuicProtocolInfo*>(tinfo->protocolInfo.get());
    if (connectionTransportInfo) {
      // NOTE: slicing assignment; stream-level fields of
      // quicStreamProtocolInfo_ are not changed while the connection
      // level fields are overwritten.
      *quicStreamProtocolInfo_ = *connectionTransportInfo;
    }
  }

  // Update the HQStreamTransport-level protocol info with the
  // stream info from the the QUIC transport
  if (hasIngressStreamId() || hasEgressStreamId()) {
    session_.getCurrentStreamTransportInfo(quicStreamProtocolInfo_.get(),
                                           getStreamId());
  }

  // Set the transport info query result to the HQStreamTransport protocol
  // info
  tinfo->protocolInfo = quicStreamProtocolInfo_;

  return success;
}

HTTPTransaction::Transport::Type
HQSession::HQStreamTransportBase::getSessionType() const noexcept {
  return session_.getType();
}

void HQSession::detachStreamTransport(HQStreamTransportBase* hqStream) {
  // Special case - streams that dont have either ingress stream id
  // or egress stream id dont need to be actually detached
  // prior to being erased
  if (hqStream->hasIngressStreamId() || hqStream->hasEgressStreamId()) {
    auto streamId = hqStream->getStreamId();
    VLOG(4) << __func__ << " streamID=" << streamId;
    CHECK(findStream(streamId));
    if (sock_ && hqStream->hasIngressStreamId()) {
      clearStreamCallbacks(streamId);
    }
    eraseStream(streamId);
  } else {
    VLOG(4) << __func__ << " streamID=NA";
    eraseUnboundStream(hqStream);
  }

  // If there are no established streams left, close the connection
  if (getNumStreams() == 0) {
    cleanupPendingStreams();
    if (infoCallback_) {
      infoCallback_->onDeactivateConnection(*this);
    }
    if (getConnectionManager()) {
      getConnectionManager()->onDeactivated(*this);
    }
    resetTimeout();
  } else {
    if (infoCallback_) {
      infoCallback_->onTransactionDetached(*this);
    }
  }
}

void HQSession::HQControlStream::processReadData() {
  bool isControl = (*type_ == hq::UnidirectionalStreamType::H1Q_CONTROL ||
                        *type_ == hq::UnidirectionalStreamType::CONTROL);
  std::unique_ptr<HTTPCodec> savedCodec;
  HQUnidirectionalCodec* ingressCodecPtr = ingressCodec_.get();
  if (isControl) {
    // We need ingressCodec_ to be realCodec_, to correctly wire up the filter
    // chain callbacks
    savedCodec = std::move(realCodec_);
    realCodec_.reset(static_cast<HQControlCodec*>(ingressCodec_.release()));
    CHECK(!ingressCodec_);
  }
  auto g1 = folly::makeGuard([&] {
      if (!isControl) {
        return;
      }
      CHECK(!ingressCodec_);
      ingressCodec_.reset(static_cast<HQControlCodec*>(realCodec_.release()));
      realCodec_ = std::move(savedCodec);
    });
  auto g = folly::makeGuard(setActiveCodec(__func__));
  if (isControl) {
    // Now ingressCodec_ has been pushed onto the codec stack.  Restore the
    // egress codec, in case an ingress callback triggers egress
    CHECK(!realCodec_);
    realCodec_ = std::move(savedCodec);
  }
  auto g2 = folly::makeGuard([&] {
      if (!isControl) {
        return;
      }
      savedCodec = std::move(realCodec_);
    });

  CHECK(ingressCodecPtr->isIngress());
  auto initialLength = readBuf_.chainLength();
  if (initialLength > 0) {
    auto ret = ingressCodecPtr->onUnidirectionalIngress(readBuf_.move());
    VLOG(4) << "streamID=" << getIngressStreamId() << " parsed bytes="
            << static_cast<int>(initialLength - readBuf_.chainLength())
            << " from readBuf remain=" << readBuf_.chainLength()
            << " eof=" << readEOF_;
    readBuf_.append(std::move(ret));
  }
  if (readEOF_ && readBuf_.chainLength() == 0) {
    ingressCodecPtr->onUnidirectionalIngressEOF();
  }
}

bool HQSession::HQStreamTransportBase::processReadData() {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  if (eomGate_.get(EOMType::CODEC) && readBuf_.chainLength() > 0) {
    // why are we calling processReadData with no data?
    VLOG(3) << " Received data after HTTP EOM for txn=" << txn_
            << ", len=" << static_cast<int>(readBuf_.chainLength());
    HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS,
                     "Unexpected data after request");
    errorOnTransaction(std::move(ex));
    return false;
  }
  while (!ingressError_ && readBuf_.chainLength() > 0) {
    // Skip any 0 length buffers before invoking the codec. Since readBuf_ is
    // not empty, we are guaranteed to find a non-empty buffer.
    while (readBuf_.front()->length() == 0) {
      readBuf_.pop_front();
    }
    size_t bytesParsed = codecFilterChain->onIngress(*readBuf_.front());
    VLOG(4) << "streamID=" << getStreamId()
            << " parsed bytes=" << static_cast<int>(bytesParsed)
            << " from readBuf remain=" << readBuf_.chainLength()
            << " eof=" << readEOF_;
    if (bytesParsed == 0) {
      break;
    }
    readBuf_.trimStart(bytesParsed);
  }
  if (ingressError_) {
    abortIngress();
  }
  return (readBuf_.chainLength() > 0);
}

void HQSession::HQStreamTransportBase::processPeekData(
    const folly::Range<quic::QuicSocket::PeekIterator>& peekData) {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  for (auto& item : peekData) {
    auto streamOffset = item.offset;
    const auto& chain = item.data;
    auto bodyOffset =
        session_.versionUtils_->onIngressPeekDataAvailable(streamOffset);
    if (bodyOffset.hasError()) {
      if (bodyOffset.error() != UnframedBodyOffsetTrackerError::NO_ERROR) {
        LOG(ERROR) << "peek: " << bodyOffset.error();
      }
    } else {
      CHECK(chain.front()) << "Got peek data for an empty chain.";
      txn_.onIngressBodyPeek(*bodyOffset, *chain.front());
    }
  }
}

void HQSession::HQStreamTransportBase::processDataExpired(
    uint64_t streamOffset) {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  auto bodyOffset = session_.versionUtils_->onIngressDataExpired(streamOffset);
  if (bodyOffset.hasError()) {
    VLOG(4) << __func__ << ": got an invalid (possibly stale) skip offset: "
            << bodyOffset.error();
  } else {
    txn_.onIngressBodySkipped(*bodyOffset);
  }
}

void HQSession::HQStreamTransportBase::processDataRejected(
    uint64_t streamOffset) {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  auto bodyOffset = session_.versionUtils_->onIngressDataRejected(streamOffset);
  if (bodyOffset.hasError()) {
    VLOG(4) << __func__ << ": got an invalid (possibly stale) reject offset:"
            << bodyOffset.error();
  } else {
    txn_.onIngressBodyRejected(*bodyOffset);
  }
}

// This method can be invoked via several paths:
//  - last header in the response has arrived
//  - triggered by QPACK
//  - push promise has arrived
//  - 1xx information header (e.g. 100 continue)
// The method is safe to use in all the above scenarios
// see specific comments in the method body
void HQSession::HQStreamTransportBase::onHeadersComplete(
    HTTPCodec::StreamID streamID, std::unique_ptr<HTTPMessage> msg) {
  VLOG(4) << __func__ << " txn=" << txn_;
  msg->dumpMessage(3);
  // TODO: the codec will set this for non-H1Q
  msg->setAdvancedProtocolString(session_.alpn_);
  msg->setSecure(true);
  CHECK(codecStreamId_);
  CHECK_EQ(streamID, *codecStreamId_);

  //  setupOnHeadersComplete is only implemented
  //  in the HQDownstreamSession, which does not
  //  receive push promises. Will only be called once
  session_.setupOnHeadersComplete(&txn_, msg.get());
  if (!txn_.getHandler()) {
    txn_.sendAbort();
    return;
  }

  // for h1q-fb-v1 start draining on receipt of a Connection:: close header
  // if we are getting a response, transportReady has been called!
  DCHECK(session_.versionUtils_);
  session_.versionUtils_->headersComplete(msg.get());

  // onHeadersComplete can be triggered by data from a different stream ID
  // - specifically, the QPACK encoder stream.  If that's true, then there may
  // be unparsed data in HQStreamTransport.  Add this stream's id to the
  // read set and schedule a loop callback to restart it.
  if (session_.pendingProcessReadSet_.find(getStreamId()) ==
          session_.pendingProcessReadSet_.end() &&
      !readBuf_.empty()) {
    session_.pendingProcessReadSet_.insert(getStreamId());
    session_.scheduleLoopCallback();
  }

  // Tell the HTTPTransaction to start processing the message now
  // that the full ingress headers have arrived.
  // Depending on the push promise latch, the message is delivered to
  // the current transaction (no push promise) or to a freshly created
  // pushed transaction. The latter is done via "onPushPromiseHeadersComplete"
  // callback
  if (ingressPushId_) {
    onPushPromiseHeadersComplete(*ingressPushId_, streamID, std::move(msg));
    ingressPushId_ = folly::none;
  } else {
    txn_.onIngressHeadersComplete(std::move(msg));
  }

  auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - createdTime);
  auto sock = session_.sock_;
  auto streamId = getStreamId();
  if (sock && sock->getState() && sock->getState()->qLogger) {
    sock->getState()->qLogger->addStreamStateUpdate(
      streamId, quic::kOnHeaders, timeDiff);
  }
}

void HQSession::HQStreamTransportBase::transactionTimeout(
    HTTPTransaction* txn) noexcept {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  VLOG(4) << __func__ << " txn=" << txn_;
  DCHECK(txn == &txn_);

  if (txn->isPushed()) {
    if (!hasIngressStreamId()) {
      // This transaction has not been assigned a stream id yet.
      // Do not attempt to close the stream but do invoke
      // the timeout on the txn
      VLOG(3) << "Transaction timeout on pushedTxn pushId=" << txn->getID();
      txn_.onIngressTimeout();
      return;
    }
  }
  // Verify that the transaction has egress or ingress stream
  DCHECK(hasIngressStreamId() || hasEgressStreamId())
      << "Timeout on transaction without stream id txnID=" << txn->getID()
      << " isPushed=" << txn->isPushed();
  // A transaction has timed out.  If the transaction does not have
  // a Handler yet, because we haven't yet received the full request
  // headers, we give it a DirectResponseHandler that generates an
  // error page.
  VLOG(3) << "Transaction timeout for streamID=" << getStreamId();

  if (!codecStreamId_) {
    // transactionTimeout before onMessageBegin
    codecStreamId_ = codecFilterChain->createStream();
  }

  if (!txn_.getHandler() &&
      txn_.getEgressState() == HTTPTransactionEgressSM::State::Start) {
    VLOG(4) << " Timed out receiving headers. " << this;
    if (session_.infoCallback_) {
      session_.infoCallback_->onIngressError(session_, kErrorTimeout);
    }

    VLOG(4) << " creating direct error handler. " << this;
    auto handler = session_.getTransactionTimeoutHandler(&txn_);
    txn_.setHandler(handler);
  }

  // There may be unparsed ingress.  Discard it.
  abortIngress();

  // Tell the transaction about the timeout.  The transaction will
  // communicate the timeout to the handler, and the handler will
  // decide how to proceed.
  if (hasIngressStreamId()) {
    session_.abortStream(HTTPException::Direction::INGRESS,
                         getIngressStreamId(),
                         HTTP3::ErrorCode::HTTP_INTERNAL_ERROR);
  }

  txn_.onIngressTimeout();
}

void HQSession::abortStream(HTTPException::Direction dir,
                            quic::StreamId id,
                            HTTP3::ErrorCode err) {
  CHECK(sock_);
  if (dir != HTTPException::Direction::EGRESS &&
      (sock_->isBidirectionalStream(id) || isPeerUniStream(id))) {
    // Any INGRESS abort generates a QPACK cancel
    versionUtils_->abortStream(id);
    sock_->stopSending(id, err);
  }
  if (dir != HTTPException::Direction::INGRESS &&
      (sock_->isBidirectionalStream(id) || isSelfUniStream(id))) {
    sock_->resetStream(id, err);
  }
}

void HQSession::HQVersionUtils::abortStream(quic::StreamId id) {
  auto sock = session_.sock_;
  if (sock && sock->getState() && sock->getState()->qLogger) {
    sock->getState()->qLogger->addStreamStateUpdate(
        id, quic::kAbort, folly::none);
  }
  auto cancel = qpackCodec_.encodeCancelStream(id);
  auto QPACKDecoderStream =
      session_.findControlStream(hq::UnidirectionalStreamType::QPACK_DECODER);
  DCHECK(QPACKDecoderStream);
  QPACKDecoderStream->writeBuf_.append(std::move(cancel));
  session_.scheduleWrite();
}

void HQSession::HQStreamTransportBase::sendHeaders(HTTPTransaction* txn,
                                                   const HTTPMessage& headers,
                                                   HTTPHeaderSize* size,
                                                   bool includeEOM) noexcept {
  VLOG(4) << __func__ << " txn=" << txn_;
  CHECK(hasEgressStreamId()) << __func__ << " invoked on stream without egress";
  DCHECK(txn == &txn_);

  if (session_.versionUtils_) {
    // for h1q-fb-v1 initiate shutdown when sending a request
    // a good client should always wait for onTransportReady before sending
    // data
    session_.versionUtils_->checkSendingGoaway(headers);
  }

  // If this is a push promise, send it on the parent stream.
  // The accounting will happen in the nested context
  if (headers.isRequest() && txn->getAssocTxnId()) {
    sendPushPromise(txn, folly::none, headers, size, includeEOM);
    return;
  }

  const uint64_t oldOffset = streamWriteByteOffset();
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(codecStreamId_)
      << "Trying to send headers on an half open stream isRequest="
      << headers.isRequest()
      << "; assocTxnId=" << txn->getAssocTxnId().value_or(-1)
      << "; txn=" << txn->getID();
  codecFilterChain->generateHeader(
      writeBuf_, *codecStreamId_, headers, includeEOM, size);

  const uint64_t newOffset = streamWriteByteOffset();
  egressHeadersStreamOffset_ = newOffset;
  if (size) {
    VLOG(4) << "sending headers, size=" << size->compressed
            << ", uncompressedSize=" << size->uncompressed << " txn=" << txn_;
  }

  // only do it for downstream now to bypass handling upstream reuse cases
  if (/* session_.direction_ == TransportDirection::DOWNSTREAM && */
      headers.isResponse() && newOffset > oldOffset &&
      // catch 100-ish response?
      !txn->testAndSetFirstHeaderByteSent()) {
    byteEventTracker_.addFirstHeaderByteEvent(newOffset, txn);
  }

  if (includeEOM) {
    CHECK_GE(newOffset, oldOffset);
    session_.handleLastByteEvents(&byteEventTracker_,
                                  &txn_,
                                  newOffset - oldOffset,
                                  streamWriteByteOffset(),
                                  true);
  }

  pendingEOM_ = includeEOM;
  // Headers can be empty for a 0.9 response
  if (writeBuf_.chainLength() > 0 || pendingEOM_) {
    notifyPendingEgress();
  }

  auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - createdTime);
  auto sock = session_.sock_;
  auto streamId = getStreamId();
  if (sock && sock->getState() && sock->getState()->qLogger) {
    sock->getState()->qLogger->addStreamStateUpdate(
      streamId, quic::kHeaders, timeDiff);
  }
  if (includeEOM) {
    if (sock && sock->getState() && sock->getState()->qLogger) {
      sock->getState()->qLogger->addStreamStateUpdate(
        streamId, quic::kEOM, timeDiff);
    }
  }

  // If partial reliability is enabled, enable the callbacks.
  if (session_.isPartialReliabilityEnabled() && headers.isPartiallyReliable()) {
    // For requests, enable right away.
    // For responses, enable only if response code is >= 200.
    if (headers.isRequest() ||
        (headers.isResponse() && headers.getStatusCode() >= 200)) {
      session_.setPartiallyReliableCallbacks(*codecStreamId_);
    }
  }

  if ((newOffset > 0) &&
      (headers.isRequest() ||
       (headers.isResponse() && headers.getStatusCode() >= 200))) {
    // Track last egress header and notify the handler when the receiver acks
    // the headers.
    // We need to track last byte sent offset, so substract one here.
    armEgressHeadersAckCb(newOffset - 1);
  }
}

size_t HQSession::HQStreamTransportBase::sendEOM(
    HTTPTransaction* txn, const HTTPHeaders* trailers) noexcept {
  VLOG(4) << __func__ << " txn=" << txn_;
  CHECK(hasEgressStreamId()) << __func__ << " invoked on stream without egress";
  DCHECK(txn == &txn_);
  auto g = folly::makeGuard(setActiveCodec(__func__));

  size_t encodedSize = 0;

  CHECK(codecStreamId_);
  if (trailers) {
    encodedSize = codecFilterChain->generateTrailers(
        writeBuf_, *codecStreamId_, *trailers);
  }

  encodedSize += codecFilterChain->generateEOM(writeBuf_, *codecStreamId_);

  // This will suppress the call to onEgressBodyLastByte in
  // handleLastByteEvents, since we're going to add a last byte event anyways.
  // This safely keeps the txn open until we egress the FIN to the transport.
  // At that point, the deliveryCallback should also be registered.
  // Note: even if the byteEventTracker_ is already at streamWriteByteOffset(),
  // it is still invoked with the same offset after egressing the FIN.
  bool pretendPiggybacked = (encodedSize == 0);
  session_.handleLastByteEvents(&byteEventTracker_,
                                &txn_,
                                encodedSize,
                                streamWriteByteOffset(),
                                pretendPiggybacked);
  if (pretendPiggybacked) {
    byteEventTracker_.addLastByteEvent(txn, streamWriteByteOffset());
  }
  // For H1 without chunked transfer-encoding, generateEOM is a no-op
  // We need to make sure writeChain(eom=true) gets called
  pendingEOM_ = true;
  notifyPendingEgress();
  auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - createdTime);
  auto sock = session_.sock_;
  auto streamId = getStreamId();
  if (sock && sock->getState() && sock->getState()->qLogger) {
    sock->getState()->qLogger->addStreamStateUpdate(
      streamId, quic::kEOM, timeDiff);
  }
  return encodedSize;
}

size_t HQSession::HQStreamTransportBase::sendAbort(
    HTTPTransaction* txn, ErrorCode errorCode) noexcept {
  return sendAbortImpl(toHTTP3ErrorCode(errorCode),
                       folly::to<std::string>("Application aborts, errorCode=",
                                              getErrorCodeString(errorCode),
                                              " txnID=",
                                              txn->getID(),
                                              " isPushed=",
                                              txn->isPushed()));
}

size_t HQSession::HQStreamTransportBase::sendAbortImpl(HTTP3::ErrorCode code,
                                                       std::string errorMsg) {
  VLOG(4) << __func__ << " txn=" << txn_;

  // If the HQ stream is bound to a transport stream, abort it.
  if (hasStreamId()) {
    session_.abortStream(getStreamDirection(), getStreamId(), code);
  }

  if (hasEgressStreamId()) {
    abortEgress(true);
  }
  // NOTE: What about the streams that only `hasIngressStreamId()` ?
  // At the time being, the only case of ingress-only transport stream
  // is an ingress push stream. The essential procedure for aborting
  // the ingress push streams is the same as above - abort the stream via
  // sending the "stop sending" frame on the control stream.
  //
  // Additional logic that is specific to the ingress push stream, such as
  // sending `CANCEL_PUSH` message, does not belong to `HQSession` level, but
  // to `HQUpstreamSesssion::HQIngressPushStream::sendAbort`, which
  // invokes this method.

  // We generated 0 application bytes so return 0?
  return 0;
}

void HQSession::HQStreamTransportBase::abortIngress() {
  VLOG(4) << "Aborting ingress for " << txn_;
  ingressError_ = true;
  readBuf_.move();
  codecFilterChain->setParserPaused(true);
}

void HQSession::HQStreamTransportBase::abortEgress(bool checkForDetach) {
  VLOG(4) << "Aborting egress for " << txn_;
  byteEventTracker_.drainByteEvents();
  writeBuf_.move();
  pendingEOM_ = false;
  if (queueHandle_.isStreamTransportEnqueued()) {
    VLOG(4) << "clearPendingEgress for " << txn_;
    session_.txnEgressQueue_.clearPendingEgress(queueHandle_.getHandle());
  }
  if (checkForDetach) {
    HTTPTransaction::DestructorGuard dg(&txn_);
  }
}

void HQSession::HQControlStream::onError(HTTPCodec::StreamID streamID,
                                         const HTTPException& error,
                                         bool /* newTxn */) {
  // All the errors on the control stream are to be considered session errors
  // anyway, so just use the ingress stream id
  if (streamID == kSessionStreamId) {
    streamID = getIngressStreamId();
  }
  session_.handleSessionError(
      CHECK_NOTNULL(session_.findControlStream(streamID)),
      StreamDirection::INGRESS,
      toHTTP3ErrorCode(error),
      kErrorConnection);
}

void HQSession::HQStreamTransportBase::onError(HTTPCodec::StreamID streamID,
                                               const HTTPException& error,
                                               bool /* newTxn */) {
  VLOG(4) << __func__ << " (from Codec) txn=" << txn_ << " err=" << error;
  // Codec must either call onMessageComplete or onError, but not both
  // I think.  The exception might be if stream has more than one HTTP
  // message on it.
  CHECK(!eomGate_.get(EOMType::CODEC));
  ingressError_ = true;

  if (streamID == kSessionStreamId) {
    session_.handleSessionError(this,
                                StreamDirection::INGRESS,
                                toHTTP3ErrorCode(error),
                                kErrorConnection);
    return;
  }

  if (!codecStreamId_ && error.hasHttpStatusCode() && streamID != 0) {
    // onError before onMessageBegin
    codecStreamId_ = streamID;
  }

  if (!txn_.getHandler() &&
      txn_.getEgressState() == HTTPTransactionEgressSM::State::Start) {
    session_.handleErrorDirectly(&txn_, error);
    return;
  }

  txn_.onError(error);
  auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - createdTime);
  auto sock = session_.sock_;
  auto streamId = getStreamId();
  if (sock && sock->getState() && sock->getState()->qLogger) {
    sock->getState()->qLogger->addStreamStateUpdate(
      streamId, quic::kOnError, timeDiff);
  }
}

void HQSession::HQStreamTransportBase::onResetStream(HTTP3::ErrorCode errorCode,
                                                     HTTPException ex) {
  // kErrorStreamAbort prevents HTTPTransaction from calling sendAbort in reply.
  // We use this code and manually call sendAbort here for appropriate cases
  HTTP3::ErrorCode replyError;
  if (session_.direction_ == TransportDirection::UPSTREAM) {
    // Upstream ingress closed - cancel this request.
    replyError = HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED;
  } else if (!txn_.isIngressStarted()) {
    // Downstream ingress closed with no ingress yet, we can send REJECTED
    // It's actually ok if we've received headers but not made any
    // calls to the handler, but there's no API for that.
    replyError = HTTP3::ErrorCode::HTTP_REQUEST_REJECTED;
  } else {
    // Downstream ingress closed but we've received some ingress.
    // TODO: This can be HTTP_REQUEST_CANCELLED also after the next release
    // Does it require hq-04 to prevent clients from retrying accidentally?
    replyError = HTTP3::ErrorCode::HTTP_NO_ERROR;
  }

  if (errorCode == HTTP3::ErrorCode::HTTP_REQUEST_REJECTED) {
    VLOG_IF(2, session_.direction_ == TransportDirection::DOWNSTREAM)
        << "RST_STREAM/REJECTED should not be sent by clients txn=" << txn_;
    // kErrorStreamUnacknowledged signals that this is safe to retry
    ex.setProxygenError(kErrorStreamUnacknowledged);
  } else {
    ex.setProxygenError(kErrorStreamAbort);
  }
  if (errorCode == HTTP3::ErrorCode::GIVEUP_ZERO_RTT) {
    // This error code comes from application who wants to error out all
    // transactions over hqsession because QUIC lost race with TCP. Passing this
    // error back to transactions through onError so that they can be retried.
    ex.setProxygenError(kErrorEarlyDataFailed);
  }
  // TODO: set Quic error when quic is OSS
  ex.setErrno(uint32_t(errorCode));
  auto msg = ex.what();
  errorOnTransaction(std::move(ex));
  sendAbortImpl(replyError, msg);
}

void HQSession::HQStreamTransportBase::notifyPendingEgress() noexcept {
  VLOG(4) << __func__ << " txn=" << txn_;
  CHECK(hasEgressStreamId()) << __func__ << " invoked on stream without egress";
  signalPendingEgressImpl();
  session_.scheduleWrite();
}

size_t HQSession::HQStreamTransportBase::sendBody(
    HTTPTransaction* txn,
    std::unique_ptr<folly::IOBuf> body,
    bool includeEOM,
    bool /* trackLastByteFlushed */) noexcept {
  VLOG(4) << __func__ << " len=" << body->computeChainDataLength()
          << " eof=" << includeEOM << " txn=" << txn_;
  CHECK(hasEgressStreamId()) << __func__ << " invoked on stream without egress";
  DCHECK(txn == &txn_);
  uint64_t offset = streamWriteByteOffset();

  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(codecStreamId_);
  size_t encodedSize = codecFilterChain->generateBody(writeBuf_,
                                                      *codecStreamId_,
                                                      std::move(body),
                                                      HTTPCodec::NoPadding,
                                                      includeEOM);
  if (encodedSize > 0 && !txn->testAndSetFirstByteSent()) {
    byteEventTracker_.addFirstBodyByteEvent(offset + 1, txn);
  }

  if (includeEOM) {
    session_.handleLastByteEvents(
        &byteEventTracker_, &txn_, encodedSize, streamWriteByteOffset(), true);
    VLOG(3) << "sending EOM in body for streamID=" << getStreamId()
            << " txn=" << txn_;
    pendingEOM_ = true;
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - createdTime);
    auto sock = session_.sock_;
    auto streamId = getStreamId();
    if (sock && sock->getState() && sock->getState()->qLogger) {
      sock->getState()->qLogger->addStreamStateUpdate(
        streamId, quic::kEOM, timeDiff);
    }
  }
  notifyPendingEgress();
  return encodedSize;
}

size_t HQSession::HQStreamTransportBase::sendChunkHeader(
    HTTPTransaction* txn, size_t length) noexcept {
  VLOG(4) << __func__ << " txn=" << txn_;
  CHECK(hasEgressStreamId()) << __func__ << " invoked on stream without egress";
  DCHECK(txn == &txn_);
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(codecStreamId_);
  size_t encodedSize =
      codecFilterChain->generateChunkHeader(writeBuf_, *codecStreamId_, length);
  notifyPendingEgress();
  return encodedSize;
}

size_t HQSession::HQStreamTransportBase::sendChunkTerminator(
    HTTPTransaction* txn) noexcept {
  VLOG(4) << __func__ << " txn=" << txn_;
  CHECK(hasEgressStreamId()) << __func__ << " invoked on stream without egress";
  DCHECK(txn == &txn_);
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(codecStreamId_);
  size_t encodedSize =
      codecFilterChain->generateChunkTerminator(writeBuf_, *codecStreamId_);
  notifyPendingEgress();
  return encodedSize;
}

void HQSession::HQStreamTransportBase::onMessageBegin(
    HTTPCodec::StreamID streamID, HTTPMessage* /* msg */) {
  VLOG(4) << __func__ << " txn=" << txn_ << " streamID=" << streamID
          << " ingressPushId=" << ingressPushId_.value_or(-1);

  if (ingressPushId_) {
    constexpr auto error =
        "Received onMessageBegin in the middle of push promise";
    LOG(ERROR) << error << " streamID=" << streamID << " session=" << session_;
    session_.dropConnectionAsync(
        std::make_pair(HTTP3::ErrorCode::HTTP_MALFORMED_FRAME_PUSH_PROMISE,
                       error),
        kErrorDropped);
    return;
  }

  if (session_.infoCallback_) {
    session_.infoCallback_->onRequestBegin(session_);
  }

  // NOTE: for H2 this is where we create a new stream and transaction.
  // for HQ there is nothing to do here, except caching the codec streamID
  codecStreamId_ = streamID;

  // Reset the pending pushID, since the subsequent invocation of
  // `onHeadersComplete` won't be associated with a push
  ingressPushId_ = folly::none;
}

// Partially reliable transport callbacks.

void HQSession::HQStreamTransportBase::onUnframedBodyStarted(
    HTTPCodec::StreamID streamID, uint64_t streamOffset) {
  CHECK(session_.isPartialReliabilityEnabled())
      << ": received " << __func__ << " but partial reliability is not enabled";
  session_.setPartiallyReliableCallbacks(streamID);
  txn_.onIngressUnframedBodyStarted(streamOffset);
}

folly::Expected<folly::Unit, ErrorCode> HQSession::HQStreamTransportBase::peek(
    HTTPTransaction::PeekCallback peekCallback) {
  if (!codecStreamId_) {
    LOG(ERROR) << "codec streamId is not set yet";
    return folly::makeUnexpected(ErrorCode::PROTOCOL_ERROR);
  }

  auto cb = [&](quic::StreamId streamId,
                const folly::Range<quic::QuicSocket::PeekIterator>& range) {
    for (const auto& entry : range) {
      peekCallback(streamId, entry.offset, *entry.data.front());
    }
  };
  auto res = session_.sock_->peek(*codecStreamId_, std::move(cb));
  if (res.hasError()) {
    return folly::makeUnexpected(ErrorCode::INTERNAL_ERROR);
  }
  return folly::unit;
}

folly::Expected<folly::Unit, ErrorCode>
HQSession::HQStreamTransportBase::consume(size_t amount) {
  if (!codecStreamId_) {
    LOG(ERROR) << "codec streamId is not set yet";
    return folly::makeUnexpected(ErrorCode::PROTOCOL_ERROR);
  }

  auto res = session_.sock_->consume(*codecStreamId_, amount);
  if (res.hasError()) {
    return folly::makeUnexpected(ErrorCode::INTERNAL_ERROR);
  }
  return folly::unit;
}

uint64_t HQSession::HQStreamTransportBase::trimPendingEgressBody(
    uint64_t trimOffset) {
  const auto bytesCommited = streamEgressCommittedByteOffset();
  if (bytesCommited > trimOffset) {
    VLOG(3)
        << __func__ << ": trim offset requested = " << trimOffset
        << " is below bytes already committed  to the wire = " << bytesCommited;
    return 0;
  }

  auto trimBytes = trimOffset - bytesCommited;
  if (trimBytes > 0) {
    writeBuf_.trimStartAtMost(trimBytes);
    VLOG(3) << __func__ << ": discarding " << trimBytes
            << " from egress buffer on stream " << getEgressStreamId();
  }

  return trimBytes;
}

folly::Expected<folly::Optional<uint64_t>, ErrorCode>
HQSession::HQStreamTransportBase::skipBodyTo(HTTPTransaction* txn,
                                             uint64_t nextBodyOffset) {
  DCHECK(txn == &txn_);
  if (!session_.isPartialReliabilityEnabled()) {
    LOG(ERROR) << "PR not supported";
    return folly::makeUnexpected(ErrorCode::PROTOCOL_ERROR);
  }

  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  auto streamOffset = session_.versionUtils_->onEgressBodySkip(nextBodyOffset);
  if (streamOffset.hasError()) {
    LOG(ERROR) << "skipBodyTo: " << streamOffset.error();
    HTTPException ex(HTTPException::Direction::EGRESS, "failed to send a skip");
    errorOnTransaction(std::move(ex));
    return folly::makeUnexpected(ErrorCode::INTERNAL_ERROR);
  }

  bytesSkipped_ += trimPendingEgressBody(*streamOffset);

  CHECK(codecStreamId_);
  auto res = session_.sock_->sendDataExpired(*codecStreamId_, *streamOffset);
  if (res.hasValue()) {
    return *res;
  } else {
    return folly::makeUnexpected(ErrorCode::INTERNAL_ERROR);
  }
}

folly::Expected<folly::Optional<uint64_t>, ErrorCode>
HQSession::HQStreamTransportBase::rejectBodyTo(HTTPTransaction* txn,
                                               uint64_t nextBodyOffset) {
  VLOG(4) << __func__ << " txn=" << txn_;
  DCHECK(txn == &txn_);
  if (!session_.isPartialReliabilityEnabled()) {
    return folly::makeUnexpected(ErrorCode::PROTOCOL_ERROR);
  }

  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  auto streamOffset =
      session_.versionUtils_->onEgressBodyReject(nextBodyOffset);
  if (streamOffset.hasError()) {
    LOG(ERROR) << "rejectBodyTo: " << streamOffset.error();
    HTTPException ex(HTTPException::Direction::EGRESS,
                     "failed to send a reject");
    errorOnTransaction(std::move(ex));
    return folly::makeUnexpected(ErrorCode::INTERNAL_ERROR);
  }

  CHECK(codecStreamId_);
  auto res = session_.sock_->sendDataRejected(*codecStreamId_, *streamOffset);
  if (res.hasValue()) {
    return *res;
  } else {
    return folly::makeUnexpected(ErrorCode::INTERNAL_ERROR);
  }
}

void HQSession::HQStreamTransportBase::trackEgressBodyDelivery(
    uint64_t bodyOffset) {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  uint64_t streamOffset = egressHeadersStreamOffset_ + bodyOffset;
  // We need to track last byte sent offset, so substract one here.
  auto offset = streamOffset - 1;
  armEgressBodyAckCb(offset);
  VLOG(4) << __func__ << ": armed body delivery callback for offset=" << offset
          << "; last egress headers offset=" << egressHeadersStreamOffset_
          << "; txn=" << txn_;
}

void HQSession::HQStreamTransportBase::armStreamAckCb(uint64_t streamOffset) {
  auto res = session_.sock_->registerDeliveryCallback(
      getEgressStreamId(), streamOffset, this);
  if (res.hasError()) {
    auto errStr = folly::to<std::string>(
      "failed to register delivery callback: ", toString(res.error()));
    LOG(ERROR) << errStr;
    HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS, errStr);
    ex.setProxygenError(kErrorNetwork);
    errorOnTransaction(std::move(ex));
    return;
  }
  numActiveDeliveryCallbacks_++;

  // Increment pending byte events so the transaction won't detach until we get
  // and ack/cancel from transport here.
  txn_.incrementPendingByteEvents();

  VLOG(4) << __func__
          << ": registered ack callback for offset = " << streamOffset
          << "; sess=" << session_ << "; txn=" << txn_;
}

void HQSession::HQStreamTransportBase::armEgressHeadersAckCb(
    uint64_t streamOffset) {
  VLOG(4) << __func__ << ": registering headers delivery callback for offset = "
          << streamOffset << "; sess=" << session_ << "; txn=" << txn_;
  armStreamAckCb(streamOffset);
  egressHeadersAckOffset_ = streamOffset;
}

void HQSession::HQStreamTransportBase::armEgressBodyAckCb(
    uint64_t streamOffset) {
  VLOG(4) << __func__ << ": registering body delivery callback for offset = "
          << streamOffset << "; sess=" << session_ << "; txn=" << txn_;
  armStreamAckCb(streamOffset);
  egressBodyAckOffsets_.insert(streamOffset);
}

void HQSession::HQStreamTransportBase::handleHeadersAcked(
    uint64_t streamOffset) {
  CHECK(egressHeadersAckOffset_);
  if (*egressHeadersAckOffset_ != streamOffset) {
    LOG(ERROR) << ": bad offset for egress headers ack: e="
               << *egressHeadersAckOffset_ << ", r=" << streamOffset;
    return;
  }

  VLOG(4) << __func__
          << ": got delivery ack for egress headers, stream offset = "
          << streamOffset << "; sess=" << session_ << "; txn=" << txn_;

  resetEgressHeadersAckOffset();
  txn_.onLastEgressHeaderByteAcked();
}

void HQSession::HQStreamTransportBase::handleBodyAcked(uint64_t streamOffset) {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  CHECK_GE(streamOffset, egressHeadersStreamOffset_);
  uint64_t bodyOffset = streamOffset - egressHeadersStreamOffset_;

  VLOG(4) << __func__
          << ": got delivery ack for egress body, bodyOffset = " << bodyOffset
          << "; sess=" << session_ << "; txn=" << txn_;

  resetEgressBodyAckOffset(streamOffset);
  txn_.onEgressBodyBytesAcked(bodyOffset);
}

void HQSession::HQStreamTransportBase::handleBodyCancelled(
    uint64_t streamOffset) {
  auto g = folly::makeGuard(setActiveCodec(__func__));
  CHECK(session_.versionUtils_);

  CHECK_GE(streamOffset, egressHeadersStreamOffset_);
  uint64_t bodyOffset = streamOffset - egressHeadersStreamOffset_;

  resetEgressBodyAckOffset(streamOffset);
  txn_.onEgressBodyDeliveryCanceled(bodyOffset);
}

void HQSession::HQStreamTransportBase::onDeliveryAck(
    quic::StreamId /* id */,
    uint64_t offset,
    std::chrono::microseconds /* rtt */) {
  VLOG(4) << __func__ << ": got delivery ack for offset = " << offset
          << "; sess=" << session_ << "; txn=" << txn_;

  DCHECK_GT(numActiveDeliveryCallbacks_, 0);
  numActiveDeliveryCallbacks_--;
  txn_.decrementPendingByteEvents();

  if (egressHeadersAckOffset_) {
    handleHeadersAcked(offset);
    return;
  }

  if (egressBodyAckOffsets_.find(offset) != egressBodyAckOffsets_.end()) {
    handleBodyAcked(offset);
    return;
  }

  LOG(DFATAL) << __func__
             << ": received an unexpected onDeliveryAck event at offset "
             << offset << "; sess=" << session_ << "; txn=" << txn_;
}

void HQSession::HQStreamTransportBase::onCanceled(quic::StreamId id,
                                                  uint64_t offset) {
  VLOG(3) << __func__ << ": data cancelled on stream = " << id
          << ", offset = " << offset << "; sess=" << session_
          << "; txn=" << txn_;
  DCHECK_GT(numActiveDeliveryCallbacks_, 0);
  numActiveDeliveryCallbacks_--;
  txn_.decrementPendingByteEvents();

  if (egressHeadersAckOffset_) {
    resetEgressHeadersAckOffset();
    return;
  }

  if (egressBodyAckOffsets_.find(offset) != egressBodyAckOffsets_.end()) {
    handleBodyCancelled(offset);
    return;
  }

  LOG(DFATAL) << __func__
             << ": received an unexpected onCanceled event at offset "
             << offset;
}

// Methods specific to StreamTransport subclasses
void HQSession::HQStreamTransportBase::onPushMessageBegin(
    HTTPCodec::StreamID pushID,
    HTTPCodec::StreamID assocStreamID,
    HTTPMessage* /* msg */) {
  VLOG(4) << __func__ << " txn=" << txn_ << " streamID=" << getIngressStreamId()
          << " assocStreamID=" << assocStreamID
          << " ingressPushId=" << ingressPushId_.value_or(-1);

  if (ingressPushId_) {
    constexpr auto error =
        "Received onPushMessageBegin in the middle of push promise";
    LOG(ERROR) << error;
    session_.dropConnectionAsync(
        std::make_pair(HTTP3::ErrorCode::HTTP_MALFORMED_FRAME_PUSH_PROMISE,
                       error),
        kErrorDropped);
    return;
  }

  if (session_.infoCallback_) {
    session_.infoCallback_->onRequestBegin(session_);
  }

  // Notify the testing callbacks
  if (session_.serverPushLifecycleCb_) {
    session_.serverPushLifecycleCb_->onPushPromiseBegin(
        assocStreamID, static_cast<hq::PushId>(pushID));
  }

  ingressPushId_ = static_cast<hq::PushId>(pushID);
}

// Methods specific to StreamTransport subclasses
//
//

// Request-stream implementation of the "sendPushPromise"
// HQEgressPushStream::sendPushPromise calls this
void HQSession::HQStreamTransport::sendPushPromise(
    HTTPTransaction* txn,
    folly::Optional<hq::PushId> pushId,
    const HTTPMessage& headers,
    HTTPHeaderSize* size,
    bool includeEOM) {
  CHECK(txn);

  CHECK(pushId.has_value()) << " Request stream impl expects pushID to be set";
  const uint64_t oldOffset = streamWriteByteOffset();
  auto g = folly::makeGuard(setActiveCodec(__func__));

  codecFilterChain->generatePushPromise(
      writeBuf_, *codecStreamId_, headers, pushId.value(), includeEOM, size);

  const uint64_t newOffset = streamWriteByteOffset();
  if (size) {
    VLOG(4) << "sending push promise, size=" << size->compressed
            << ", uncompressedSize=" << size->uncompressed << " txn=" << txn_;
  }

  if (includeEOM) {
    CHECK_GE(newOffset, oldOffset);
    session_.handleLastByteEvents(&byteEventTracker_,
                                  &txn_,
                                  newOffset - oldOffset,
                                  streamWriteByteOffset(),
                                  true);
  }

  pendingEOM_ = includeEOM;
  notifyPendingEgress();

  auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - createdTime);
  auto sock = session_.sock_;
  auto streamId = getStreamId();
  if (sock && sock->getState() && sock->getState()->qLogger) {
    sock->getState()->qLogger->addStreamStateUpdate(
      streamId, quic::kPushPromise, timeDiff);
  }
  if (includeEOM) {
    if (sock && sock->getState() && sock->getState()->qLogger) {
      sock->getState()->qLogger->addStreamStateUpdate(
        streamId, quic::kEOM, timeDiff);
    }
  }
}

HTTPTransaction* FOLLY_NULLABLE
HQSession::HQStreamTransport::newPushedTransaction(
    HTTPCodec::StreamID parentRequestStreamId,
    HTTPTransaction::PushHandler* handler,
    ProxygenError* error) noexcept {

  CHECK_EQ(parentRequestStreamId, txn_.getID());

  return session_.newPushedTransaction(
      parentRequestStreamId, // stream id of the egress push stream
      handler,
      error);
}

void HQSession::HQStreamTransport::onPushPromiseHeadersComplete(
    hq::PushId pushID,
    HTTPCodec::StreamID assocStreamID,
    std::unique_ptr<HTTPMessage> msg) {
  VLOG(4) << "processing new Push Promise msg=" << msg.get()
          << " streamID=" << assocStreamID << " maybePushID=" << pushID
          << ", txn= " << txn_;

  // Notify the testing callbacks
  if (session_.serverPushLifecycleCb_) {
    session_.serverPushLifecycleCb_->onPushPromise(
        assocStreamID, pushID, msg.get());
  }

  // Create ingress push stream (will also create the transaction)
  // If a corresponding nascent push stream is ready, it will be
  // bound to the newly created stream.
  // virtual function call into UpstreamSession.  This will crash if it happens
  // downstream.
  auto pushStream = session_.createIngressPushStream(assocStreamID, pushID);
  CHECK(pushStream);

  // Notify the *parent* transaction that the *pushed* transaction has been
  // successfully created.
  txn_.onPushedTransaction(&pushStream->txn_);

  // Notify the *pushed* transaction on the push promise headers
  // This has to be called AFTER "onPushedTransaction" upcall
  pushStream->txn_.onIngressHeadersComplete(std::move(msg));
}

std::ostream& operator<<(std::ostream& os, const HQSession& session) {
  session.describe(os);
  return os;
}

std::ostream& operator<<(std::ostream& os, HQSession::DrainState drainState) {
  switch (drainState) {
    case HQSession::DrainState::NONE:
      os << "none";
      break;
    case HQSession::DrainState::PENDING:
      os << "pending";
      break;
    case HQSession::DrainState::CLOSE_SENT:
      os << "close_sent";
      break;
    case HQSession::DrainState::CLOSE_RECEIVED:
      os << "close_recvd";
      break;
    case HQSession::DrainState::FIRST_GOAWAY:
      os << "first_goaway";
      break;
    case HQSession::DrainState::SECOND_GOAWAY:
      os << "second_goaway";
      break;
    case HQSession::DrainState::DONE:
      os << "done";
      break;
    default:
      folly::assume_unreachable();
  }
  return os;
}

} // namespace proxygen
