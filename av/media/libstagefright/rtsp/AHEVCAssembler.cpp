/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "AHEVCAssembler"
#include <utils/Log.h>

#include "AHEVCAssembler.h"

#include "ARTPSource.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <include/HevcUtils.h>
#include <media/stagefright/foundation/hexdump.h>

#include <stdint.h>

#define H265_NALU_MASK 0x3F
#define H265_NALU_VPS 0x20
#define H265_NALU_SPS 0x21
#define H265_NALU_PPS 0x22
#define H265_NALU_AP 0x30
#define H265_NALU_FU 0x31
#define H265_NALU_PACI 0x32


namespace android {

const double JITTER_MULTIPLE = 1.5f;

// static
AHEVCAssembler::AHEVCAssembler(const sp<AMessage> &notify)
    : mNotifyMsg(notify),
      mAccessUnitRTPTime(0),
      mNextExpectedSeqNoValid(false),
      mNextExpectedSeqNo(0),
      mAccessUnitDamaged(false),
      mFirstIFrameProvided(false),
      mLastIFrameProvidedAtMs(0),
      mWidth(0),
      mHeight(0) {

      ALOGV("Constructor");
}

AHEVCAssembler::~AHEVCAssembler() {
}

int32_t AHEVCAssembler::addNack(
        const sp<ARTPSource> &source) {
    List<sp<ABuffer>> *queue = source->queue();
    int32_t nackCount = 0;

    List<sp<ABuffer> >::iterator it = queue->begin();

    if (it == queue->end()) {
        return nackCount /* 0 */;
    }

    uint16_t queueHeadSeqNum = (*it)->int32Data();

    // move to the packet after which RTCP:NACK was sent.
    for (; it != queue->end(); ++it) {
        int32_t seqNum = (*it)->int32Data();
        if (seqNum >= source->mHighestNackNumber) {
            break;
        }
    }

    int32_t nackStartAt = -1;

    while (it != queue->end()) {
        int32_t seqBeforeLast = (*it)->int32Data();
        // increase iterator.
        if ((++it) == queue->end()) {
            break;
        }

        int32_t seqLast = (*it)->int32Data();

        if ((seqLast - seqBeforeLast) < 0) {
            ALOGD("addNack: found end of seqNum from(%d) to(%d)", seqBeforeLast, seqLast);
            source->mHighestNackNumber = 0;
        }

        // missed packet found
        if (seqLast > (seqBeforeLast + 1) &&
            // we didn't send RTCP:NACK for this packet yet.
            (seqLast - 1) > source->mHighestNackNumber) {
            source->mHighestNackNumber = seqLast -1;
            nackStartAt = seqBeforeLast + 1;
            break;
        }

    }

    if (nackStartAt != -1) {
        nackCount = source->mHighestNackNumber - nackStartAt + 1;
        ALOGD("addNack: nackCount=%d, nackFrom=%d, nackTo=%d", nackCount,
            nackStartAt, source->mHighestNackNumber);

        uint16_t mask = (uint16_t)(0xffff) >> (16 - nackCount + 1);
        source->setSeqNumToNACK(nackStartAt, mask, queueHeadSeqNum);
    }

    return nackCount;
}

ARTPAssembler::AssemblyStatus AHEVCAssembler::addNALUnit(
        const sp<ARTPSource> &source) {
    List<sp<ABuffer> > *queue = source->queue();
    const uint32_t firstRTPTime = source->mFirstRtpTime;

    if (queue->empty()) {
        return NOT_ENOUGH_DATA;
    }

    sp<ABuffer> buffer = *queue->begin();
    buffer->meta()->setObject("source", source);

    int64_t rtpTime = findRTPTime(firstRTPTime, buffer);

    const int64_t startTimeMs = source->mFirstSysTime / 1000;
    const int64_t nowTimeMs = ALooper::GetNowUs() / 1000;
    const int64_t staticJbTimeMs = source->getStaticJitterTimeMs();
    const int64_t dynamicJbTimeMs = source->getDynamicJitterTimeMs();
    const int64_t clockRate = source->mClockRate;

    int64_t playedTimeMs = nowTimeMs - startTimeMs;
    int64_t playedTimeRtp = source->mFirstRtpTime + MsToRtp(playedTimeMs, clockRate);

    /**
     * Based on experience in real commercial network services,
     * 300 ms is a maximum heuristic jitter buffer time for video RTP service.
     */

    /**
     * The static(base) jitter is a kind of expected propagation time that we desire.
     * We can drop packets if it doesn't meet our standards.
     * If it gets shorter we can get faster response but can lose packets.
     * Expecting range : 50ms ~ 1000ms (But 300 ms would be practical upper bound)
     */
    const int64_t baseJbTimeRtp = MsToRtp(staticJbTimeMs, clockRate);
    /**
     * Dynamic jitter is a variance of interarrival time as defined in the 6.4.1 of RFC 3550.
     * We can regard this as a tolerance of every moments.
     * Expecting range : 0ms ~ 150ms (Not to over 300 ms practically)
     */
    const int64_t dynamicJbTimeRtp =                        // Max 150
            std::min(MsToRtp(dynamicJbTimeMs, clockRate), MsToRtp(150, clockRate));
    const int64_t jitterTimeRtp = baseJbTimeRtp + dynamicJbTimeRtp; // Total jitter time

    int64_t expiredTimeRtp = rtpTime + jitterTimeRtp;       // When does this buffer expire ? (T)
    int64_t diffTimeRtp = playedTimeRtp - expiredTimeRtp;
    bool isExpired = (diffTimeRtp >= 0);                    // It's expired if T is passed away
    bool isFirstLineBroken = (diffTimeRtp > jitterTimeRtp); // (T + jitter) is a standard tolerance

    int64_t finalMargin = dynamicJbTimeRtp * JITTER_MULTIPLE;
    bool isSecondLineBroken = (diffTimeRtp > jitterTimeRtp + finalMargin); // The Maginot line

    if (mShowQueueCnt < 20) {
        showCurrentQueue(queue);
        printNowTimeMs(startTimeMs, nowTimeMs, playedTimeMs);
        printRTPTime(rtpTime, playedTimeRtp, expiredTimeRtp, isExpired);
        mShowQueueCnt++;
    }

    AHEVCAssembler::addNack(source);

    if (!isExpired) {
        ALOGV("buffering in jitter buffer.");
        return NOT_ENOUGH_DATA;
    }

    if (isFirstLineBroken) {
        if (isSecondLineBroken) {
            ALOGW("buffer too late ... \t Diff in Jb=%lld \t "
                    "Seq# %d \t ExpSeq# %d \t"
                    "JitterMs %lld + (%lld * %.3f)",
                    (long long)(diffTimeRtp),
                    buffer->int32Data(), mNextExpectedSeqNo,
                    (long long)staticJbTimeMs, (long long)dynamicJbTimeMs, JITTER_MULTIPLE + 1);
            printNowTimeMs(startTimeMs, nowTimeMs, playedTimeMs);
            printRTPTime(rtpTime, playedTimeRtp, expiredTimeRtp, isExpired);

            mNextExpectedSeqNo = pickProperSeq(queue, firstRTPTime, playedTimeRtp, jitterTimeRtp);
        }  else {
            ALOGW("=== WARNING === buffer arrived after %lld + %lld = %lld ms === WARNING === ",
                    (long long)staticJbTimeMs, (long long)dynamicJbTimeMs,
                    (long long)RtpToMs(jitterTimeRtp, clockRate));
        }
    }

    if (mNextExpectedSeqNoValid) {
        int32_t size = queue->size();
        int32_t cntRemove = deleteUnitUnderSeq(queue, mNextExpectedSeqNo);

        if (cntRemove > 0) {
            source->noticeAbandonBuffer(cntRemove);
            ALOGW("delete %d of %d buffers", cntRemove, size);
        }

        if (queue->empty()) {
            return NOT_ENOUGH_DATA;
        }
    }

    buffer = *queue->begin();

    if (!mNextExpectedSeqNoValid) {
        mNextExpectedSeqNoValid = true;
        mNextExpectedSeqNo = (uint32_t)buffer->int32Data();
    } else if ((uint32_t)buffer->int32Data() != mNextExpectedSeqNo) {
        ALOGV("Not the sequence number I expected");

        return WRONG_SEQUENCE_NUMBER;
    }

    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    if (size < 1 || (data[0] & 0x80)) {
        // Corrupt.

        ALOGV("Ignoring corrupt buffer.");
        queue->erase(queue->begin());

        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }

    unsigned nalType = (data[0] >> 1) & H265_NALU_MASK;
    if (nalType > 0 && nalType < H265_NALU_AP) {
        addSingleNALUnit(buffer);
        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return OK;
    } else if (nalType == H265_NALU_FU) {
        // FU-A
        return addFragmentedNALUnit(queue);
    } else if (nalType == H265_NALU_AP) {
        // STAP-A
        bool success = addSingleTimeAggregationPacket(buffer);
        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;

        return success ? OK : MALFORMED_PACKET;
    } else if (nalType == 0) {
        ALOGV("Ignoring undefined nal type.");

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;

        return OK;
    } else {
        ALOGV("Ignoring unsupported buffer (nalType=%d)", nalType);

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;

        return MALFORMED_PACKET;
    }
}

void AHEVCAssembler::checkSpsUpdated(const sp<ABuffer> &buffer) {
    if (buffer->size() == 0) {
        return;
    }
    const uint8_t *data = buffer->data();
    HevcParameterSets paramSets;
    unsigned nalType = (data[0] >> 1) & H265_NALU_MASK;
    if (nalType == H265_NALU_SPS) {
        int32_t width = 0, height = 0;
        paramSets.FindHEVCDimensions(buffer, &width, &height);
        ALOGV("existing resolution (%u x %u)", mWidth, mHeight);
        if (width != mWidth || height != mHeight) {
            mFirstIFrameProvided = false;
            mWidth = width;
            mHeight = height;
            ALOGD("found a new resolution (%u x %u)", mWidth, mHeight);
        }
    }
}

void AHEVCAssembler::checkIFrameProvided(const sp<ABuffer> &buffer) {
    if (buffer->size() == 0) {
        return;
    }
    const uint8_t *data = buffer->data();
    unsigned nalType = (data[0] >> 1) & H265_NALU_MASK;
    if (nalType > 0x0F && nalType < 0x18) {
        mLastIFrameProvidedAtMs = ALooper::GetNowUs() / 1000;
        if (!mFirstIFrameProvided) {
            mFirstIFrameProvided = true;
            uint32_t rtpTime;
            CHECK(buffer->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));
            ALOGD("got First I-frame to be decoded. rtpTime=%d, size=%zu", rtpTime, buffer->size());
        }
    }
}

bool AHEVCAssembler::dropFramesUntilIframe(const sp<ABuffer> &buffer) {
    if (buffer->size() == 0) {
        return false;
    }
    const uint8_t *data = buffer->data();
    unsigned nalType = (data[0] >> 1) & H265_NALU_MASK;
    return !mFirstIFrameProvided && nalType < 0x10;
}

void AHEVCAssembler::addSingleNALUnit(const sp<ABuffer> &buffer) {
    ALOGV("addSingleNALUnit of size %zu", buffer->size());
#if !LOG_NDEBUG
    hexdump(buffer->data(), buffer->size());
#endif
    checkSpsUpdated(buffer);
    checkIFrameProvided(buffer);

    uint32_t rtpTime;
    CHECK(buffer->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));

    if (dropFramesUntilIframe(buffer)) {
        sp<ARTPSource> source = nullptr;
        buffer->meta()->findObject("source", (sp<android::RefBase>*)&source);
        if (source != nullptr) {
            ALOGD("Issued FIR to get the I-frame");
            source->onIssueFIRByAssembler();
        }
        ALOGD("drop P-frames till an I-frame provided. rtpTime %u", rtpTime);
        return;
    }

    if (!mNALUnits.empty() && rtpTime != mAccessUnitRTPTime) {
        submitAccessUnit();
    }
    mAccessUnitRTPTime = rtpTime;

    mNALUnits.push_back(buffer);
}

bool AHEVCAssembler::addSingleTimeAggregationPacket(const sp<ABuffer> &buffer) {
    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    if (size < 3) {
        ALOGV("Discarding too small STAP-A packet.");
        return false;
    }

    ++data;
    --size;
    while (size >= 2) {
        size_t nalSize = (data[0] << 8) | data[1];

        if (size < nalSize + 2) {
            ALOGV("Discarding malformed STAP-A packet.");
            return false;
        }

        sp<ABuffer> unit = new ABuffer(nalSize);
        memcpy(unit->data(), &data[2], nalSize);

        CopyTimes(unit, buffer);

        addSingleNALUnit(unit);

        data += 2 + nalSize;
        size -= 2 + nalSize;
    }

    if (size != 0) {
        ALOGV("Unexpected padding at end of STAP-A packet.");
    }

    return true;
}

ARTPAssembler::AssemblyStatus AHEVCAssembler::addFragmentedNALUnit(
        List<sp<ABuffer> > *queue) {
    CHECK(!queue->empty());

    sp<ABuffer> buffer = *queue->begin();
    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    CHECK(size > 0);
    /*   H265 payload header is 16 bit
        0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |F|     Type  |  Layer ID | TID |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */
    unsigned indicator = (data[0] >> 1);

    CHECK((indicator & H265_NALU_MASK) == H265_NALU_FU);

    if (size < 3) {
        ALOGV("Ignoring malformed FU buffer (size = %zu)", size);

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }

    if (!(data[2] & 0x80)) {
        // Start bit not set on the first buffer.

        ALOGV("Start bit not set on first buffer");

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }

    /*  FU INDICATOR HDR
        0 1 2 3 4 5 6 7
       +-+-+-+-+-+-+-+-+
       |S|E|   Type    |
       +-+-+-+-+-+-+-+-+
     */
    uint32_t nalType = data[2] & H265_NALU_MASK;
    uint32_t tid = data[1] & 0x7;
    ALOGV("nalType =%u, tid =%u", nalType, tid);

    uint32_t expectedSeqNo = (uint32_t)buffer->int32Data() + 1;
    size_t totalSize = size - 3;
    size_t totalCount = 1;
    bool complete = false;

    uint32_t rtpTimeStartAt;
    CHECK(buffer->meta()->findInt32("rtp-time", (int32_t *)&rtpTimeStartAt));
    uint32_t startSeqNo = buffer->int32Data();
    bool pFrame = (nalType < 0x10);

    if (data[2] & 0x40) {
        // Huh? End bit also set on the first buffer.

        ALOGV("Grrr. This isn't fragmented at all.");

        complete = true;
    } else {
        List<sp<ABuffer> >::iterator it = ++queue->begin();
        int32_t connected = 1;
        bool snapped = false;
        while (it != queue->end()) {
            ALOGV("sequence length %zu", totalCount);

            const sp<ABuffer> &buffer = *it;

            const uint8_t *data = buffer->data();
            size_t size = buffer->size();

            if ((uint32_t)buffer->int32Data() != expectedSeqNo) {
                ALOGV("sequence not complete, expected seqNo %u, got %u, nalType %u",
                     expectedSeqNo, (uint32_t)buffer->int32Data(), nalType);
                snapped = true;

                if (!pFrame) {
                    return WRONG_SEQUENCE_NUMBER;
                }
            }

            if (!snapped) {
                connected++;
            }

            uint32_t rtpTime;
            CHECK(buffer->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));
            if (size < 3
                    || ((data[0] >> 1) & H265_NALU_MASK) != indicator
                    || (data[2] & H265_NALU_MASK) != nalType
                    || (data[2] & 0x80)
                    || rtpTime != rtpTimeStartAt) {
                ALOGV("Ignoring malformed FU buffer.");

                // Delete the whole start of the FU.

                mNextExpectedSeqNo = expectedSeqNo + 1;
                deleteUnitUnderSeq(queue, mNextExpectedSeqNo);

                return MALFORMED_PACKET;
            }

            totalSize += size - 3;
            ++totalCount;

            expectedSeqNo = (uint32_t)buffer->int32Data() + 1;

            if (data[2] & 0x40) {
                if (pFrame && !recycleUnit(startSeqNo, expectedSeqNo,
                        connected, totalCount, 0.5f)) {
                    mNextExpectedSeqNo = expectedSeqNo;
                    deleteUnitUnderSeq(queue, mNextExpectedSeqNo);

                    return MALFORMED_PACKET;
                }
                // This is the last fragment.
                complete = true;
                break;
            }

            ++it;
        }
    }

    if (!complete) {
        return NOT_ENOUGH_DATA;
    }

    mNextExpectedSeqNo = expectedSeqNo;

    // We found all the fragments that make up the complete NAL unit.

    // Leave room for the header. So far totalSize did not include the
    // header byte.
    totalSize += 2;

    sp<ABuffer> unit = new ABuffer(totalSize);
    CopyTimes(unit, *queue->begin());

    unit->data()[0] = (nalType << 1);
    unit->data()[1] = tid;

    size_t offset = 2;
    int32_t cvo = -1;
    List<sp<ABuffer> >::iterator it = queue->begin();
    for (size_t i = 0; i < totalCount; ++i) {
        const sp<ABuffer> &buffer = *it;

        ALOGV("piece #%zu/%zu", i + 1, totalCount);
#if !LOG_NDEBUG
        hexdump(buffer->data(), buffer->size());
#endif

        memcpy(unit->data() + offset, buffer->data() + 3, buffer->size() - 3);
        buffer->meta()->findInt32("cvo", &cvo);
        offset += buffer->size() - 3;

        it = queue->erase(it);
    }

    unit->setRange(0, totalSize);

    if (cvo >= 0) {
        unit->meta()->setInt32("cvo", cvo);
    }

    addSingleNALUnit(unit);

    ALOGV("successfully assembled a NAL unit from fragments.");

    return OK;
}

void AHEVCAssembler::submitAccessUnit() {
    CHECK(!mNALUnits.empty());

    ALOGV("Access unit complete (%zu nal units)", mNALUnits.size());

    size_t totalSize = 0;
    for (List<sp<ABuffer> >::iterator it = mNALUnits.begin();
         it != mNALUnits.end(); ++it) {
        totalSize += 4 + (*it)->size();
    }

    sp<ABuffer> accessUnit = new ABuffer(totalSize);
    size_t offset = 0;
    int32_t cvo = -1;
    for (List<sp<ABuffer> >::iterator it = mNALUnits.begin();
         it != mNALUnits.end(); ++it) {
        memcpy(accessUnit->data() + offset, "\x00\x00\x00\x01", 4);
        offset += 4;

        sp<ABuffer> nal = *it;
        memcpy(accessUnit->data() + offset, nal->data(), nal->size());
        offset += nal->size();
        nal->meta()->findInt32("cvo", &cvo);
    }

    CopyTimes(accessUnit, *mNALUnits.begin());

#if 0
    printf(mAccessUnitDamaged ? "X" : ".");
    fflush(stdout);
#endif
    if (cvo >= 0) {
        accessUnit->meta()->setInt32("cvo", cvo);
    }

    if (mAccessUnitDamaged) {
        accessUnit->meta()->setInt32("damaged", true);
    }

    mNALUnits.clear();
    mAccessUnitDamaged = false;

    sp<AMessage> msg = mNotifyMsg->dup();
    msg->setBuffer("access-unit", accessUnit);
    msg->post();
}

int32_t AHEVCAssembler::pickProperSeq(const Queue *queue,
        uint32_t first, int64_t play, int64_t jit) {
    sp<ABuffer> buffer = *(queue->begin());
    int32_t nextSeqNo = buffer->int32Data();

    Queue::const_iterator it = queue->begin();
    while (it != queue->end()) {
        int64_t rtpTime = findRTPTime(first, *it);
        // if pkt in time exists, that should be the next pivot
        if (rtpTime + jit >= play) {
            nextSeqNo = (*it)->int32Data();
            break;
        }
        it++;
    }
    return nextSeqNo;
}

bool AHEVCAssembler::recycleUnit(uint32_t start, uint32_t end,  uint32_t connected,
         size_t avail, float goodRatio) {
    float total = end - start;
    float valid = connected;
    float exist = avail;
    bool isRecycle = (valid / total) >= goodRatio;

    ALOGV("checking p-frame losses.. recvBufs %f valid %f diff %f recycle? %d",
            exist, valid, total, isRecycle);

    return isRecycle;
}

int32_t AHEVCAssembler::deleteUnitUnderSeq(Queue *queue, uint32_t seq) {
    int32_t initSize = queue->size();
    Queue::iterator it = queue->begin();
    while (it != queue->end()) {
        if ((uint32_t)(*it)->int32Data() >= seq) {
            break;
        }
        it++;
    }
    queue->erase(queue->begin(), it);
    return initSize - queue->size();
}

ARTPAssembler::AssemblyStatus AHEVCAssembler::assembleMore(
        const sp<ARTPSource> &source) {
    AssemblyStatus status = addNALUnit(source);
    if (status == MALFORMED_PACKET) {
        uint64_t msecsSinceLastIFrame = (ALooper::GetNowUs() / 1000) - mLastIFrameProvidedAtMs;
        if (msecsSinceLastIFrame > 1000) {
            ALOGV("request FIR to get a new I-Frame, time after "
                    "last I-Frame in %llu ms", (unsigned long long)msecsSinceLastIFrame);
            source->onIssueFIRByAssembler();
        }
    }
    return status;
}

void AHEVCAssembler::packetLost() {
    CHECK(mNextExpectedSeqNoValid);
    ALOGD("packetLost (expected %u)", mNextExpectedSeqNo);

    ++mNextExpectedSeqNo;
}

void AHEVCAssembler::onByeReceived() {
    sp<AMessage> msg = mNotifyMsg->dup();
    msg->setInt32("eos", true);
    msg->post();
}

}  // namespace android
