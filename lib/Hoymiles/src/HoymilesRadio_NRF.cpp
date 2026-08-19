// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "HoymilesRadio_NRF.h"
#include "Hoymiles.h"
#include "Utils.h"
#include "commands/RequestFrameCommand.h"
#include <Every.h>
#include <FunctionalInterrupt.h>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

static constexpr uint32_t TIME_SLOT = 2596; // slot time in microseconds
static constexpr uint32_t SLOTS = 2;        // number of slots per channel
static constexpr uint32_t CHANNEL_SLOT = SLOTS * TIME_SLOT; // channel time in microseconds

// technical background why I think we can use 2 seconds for sync lifetime
// if we assume +/- 100ppm clock tolerance on inverter and DTU
// in worst case scenario we have 200ppm between both clocks
// on 2 seconds the maximum failure would be 400us, about 20% of the TIME_SLOT
static constexpr uint32_t SYNC_LIFETIME_MAX = 2 * 1000 * 1000; // maximum sync time in microseconds


void HoymilesRadio_NRF::init(SPIClass* initialisedSpiBus, const uint8_t pinCE, const uint8_t pinIRQ)
{
    _dtuSerial.u64 = 0;

    _spiPtr.reset(initialisedSpiBus);
    _radio.reset(new RF24(pinCE, initialisedSpiBus->pinSS()));

    _radio->begin(_spiPtr.get());

    _radio->setDataRate(RF24_250KBPS);
    _radio->enableDynamicPayloads();
    _radio->setCRCLength(RF24_CRC_16);
    _radio->setAddressWidth(5);
    _radio->setRetries(0, 0);
    _radio->setStatusFlags(RF24_RX_DR); // enable only receiving interrupts
    if (!_radio->isChipConnected()) {
        ESP_LOGE(TAG, "NRF: Connection error!!");
        return;
    }
    ESP_LOGI(TAG, "NRF: Connection successful");

    attachInterrupt(digitalPinToInterrupt(pinIRQ), std::bind(&HoymilesRadio_NRF::handleIntr, this), FALLING);

    openReadingPipe();
    _radio->startListening();
    _isInitialized = true;
}

void HoymilesRadio_NRF::loop()
{
    if (!_isInitialized) {
        return;
    }

    if (_packetReceived) {
        ESP_LOGV(TAG, "Interrupt received");
        while (_radio->available()) {
            if (_rxBuffer.size() > FRAGMENT_BUFFER_SIZE) {
                ESP_LOGE(TAG, "NRF: Buffer full");
                _radio->flush_rx();
                continue;
            }

            fragment_t f;
            memset(f.fragment, 0xcc, MAX_RF_PAYLOAD_SIZE);
            f.len = std::min<uint8_t>(_radio->getDynamicPayloadSize(), MAX_RF_PAYLOAD_SIZE);
            f.channel = _radio->getChannel();
            f.rssi = _radio->testRPD() ? -30 : -80;
            _radio->read(f.fragment, f.len);
            _rxBuffer.push(f);

            // when we receive a packet, we can synchronize the reference time.
            // Due to our often too late channel switching, we always synchronize with the second time slot to improve performance.
            if (_packetMicros != 0) {
                uint32_t airTime = (16 + f.len) * 32 + 260 + 20 + 8; // approximate time in microseconds for 250kbps
                uint32_t newRefMicros = _packetMicros - airTime - TIME_SLOT;
                ESP_LOGV(TAG, "Reference time shift: %.3f ms", (static_cast<float>(newRefMicros) - static_cast<float>(_refMicros)) / 1000.0f);
                _refMicros = newRefMicros;
                _inSync = true;
                _syncLifetime = _packetMicros;
                _packetMicros = 0;
            }
        }
        _packetReceived = false;
    }

    // time slot statistics, we count how many times we are in the first, second or later half of the time slot
    uint32_t elapsedMicros = micros() - _refMicros;
    if (elapsedMicros >= CHANNEL_SLOT) {
        uint32_t lateMicros = elapsedMicros - CHANNEL_SLOT;
        if (lateMicros < TIME_SLOT / 2) {
            _inPercentCounter[0]++;
        } else if (lateMicros < TIME_SLOT) {
            _inPercentCounter[1]++;
        } else {
            _inPercentCounter[2]++;
        }
        uint32_t allCounter = _inPercentCounter[0] + _inPercentCounter[1] + _inPercentCounter[2];
        if (allCounter > 50000) {
            for (uint8_t i = 0; i < MAX_COUNTER; i++) { _inPercentCounter[i] /= 2; }
        }
    }

    switchRxCh(); // first check

    while (!_rxBuffer.empty()) {
        fragment_t f = _rxBuffer.front();
        if (checkFragmentCrc(f)) {
            std::shared_ptr<InverterAbstract> inv = Hoymiles.getInverterByFragment(f);

            if (nullptr != inv) {
                // Save packet in inverter rx buffer
                ESP_LOGD(TAG, "RX Channel: %" PRIu8 " --> %s | %" PRId8 " dBm",
                    f.channel, Utils::dumpArray(f.fragment, f.len).c_str(), f.rssi);

                inv->addRxFragment(f.fragment, f.len, f.rssi);
            } else {
                ESP_LOGE(TAG, "Inverter Not found!");
            }

        } else {
            ESP_LOGW(TAG, "Frame kaputt");
        }

        // Remove packet from buffer even it was corrupted
        _rxBuffer.pop();
    }

    handleReceivedPackage();

    switchRxCh(); // second check
}

void HoymilesRadio_NRF::setPALevel(const rf24_pa_dbm_e paLevel)
{
    if (!_isInitialized) {
        return;
    }
    _radio->setPALevel(paLevel);
}

void HoymilesRadio_NRF::setDtuSerial(const uint64_t serial)
{
    HoymilesRadio::setDtuSerial(serial);

    if (!_isInitialized) {
        return;
    }
    openReadingPipe();
}

bool HoymilesRadio_NRF::isConnected() const
{
    if (!_isInitialized) {
        return false;
    }
    return _radio->isChipConnected();
}

bool HoymilesRadio_NRF::isPVariant() const
{
    if (!_isInitialized) {
        return false;
    }
    return _radio->isPVariant();
}

void HoymilesRadio_NRF::openReadingPipe()
{
    const serial_u s = convertSerialToRadioId(_dtuSerial);
    _radio->openReadingPipe(1, s.b);
}

void HoymilesRadio_NRF::openWritingPipe(const serial_u serial)
{
    const serial_u s = convertSerialToRadioId(serial);
    _radio->stopListening(s.b);
}

void ARDUINO_ISR_ATTR HoymilesRadio_NRF::handleIntr()
{
    _packetReceived = true;
    if (_packetMicros == 0) { _packetMicros = micros(); }
}

uint8_t HoymilesRadio_NRF::getTxNxtChannel()
{
    uint8_t addCh = 1;          // default not in sync
    uint8_t chIdx = _txChIdx;   // default not in sync
    uint32_t nowMicros = micros();

    if (_inSync) {
        // we are in sync, we start transmitting at begin of the next Rx channel slot.
        addCh = (nowMicros - _refMicros) / CHANNEL_SLOT + 1;
        chIdx = _rxChIdx;
        _refMicros = _refMicros + addCh * CHANNEL_SLOT;

        uint32_t delayMicros = _refMicros - nowMicros;
        if (delayMicros < CHANNEL_SLOT) {
            delayMicroseconds(delayMicros); // delay to the next channel slot
        }
    } else {
        // we are not in sync, we do not have to wait for the next channel slot, we use the next Tx channel
        _refMicros = nowMicros;
    }

    _txChIdx = (chIdx + addCh) % MAX_CHANNELS;
    return _chLst[_txChIdx];
}

void HoymilesRadio_NRF::switchRxCh(bool const ignoreTime)
{
    // channel hopping should be kept as precise as possible, even if the function has not been called for
    // a longer period of time or if the function is called multiple times in the same channel timeslot.
    // only after transmitting a packet, the time is ignored (ignoreTime = true), because we want to switch back to receiving mode
    uint32_t diffMicros = micros() - _refMicros;

    if ((diffMicros > CHANNEL_SLOT) || ignoreTime) {
        uint8_t addCh = diffMicros / CHANNEL_SLOT;
        _refMicros = _refMicros + addCh * CHANNEL_SLOT;
        _rxChIdx = (_rxChIdx + addCh) % MAX_CHANNELS;

        _radio->stopListening();
        _radio->setChannel(_chLst[_rxChIdx]);
        _radio->startListening();
    }
}

void HoymilesRadio_NRF::sendEsbPacket(CommandAbstract& cmd)
{
    cmd.incrementSendCount();

    cmd.setRouterAddress(DtuSerial().u64);

    serial_u s;
    s.u64 = cmd.getTargetAddress();

    // we can only be in sync with one inverter at a time and if the max sync lifetime is not exceeded
    if (s.u64 != _syncInverterSerial) {
        _inSync = false;
        _syncInverterSerial = s.u64;
    } else {
        _inSync = ((micros() - _syncLifetime) <= SYNC_LIFETIME_MAX) ? true : false;
    }

    if (_inSync) {
        ESP_LOGV(TAG, "In Sync: Yes, Sync Time: %ums", static_cast<uint32_t>((micros() - _syncLifetime) / 1000));
    } else {
        ESP_LOGV(TAG, "In Sync: No");
    }

    _radio->stopListening();
    _radio->setChannel(getTxNxtChannel());
    openWritingPipe(s);
    _radio->setRetries(2, 0); // ARD = 2 (750us), ARC = 0 (no retry)

    // to keep the timing as precise as possible, we do not use the automatic retransmission feature of the NRF24L01.
    // Instead, we manually calculate the timing and use a loop.
    uint32_t spiTime = (cmd.getDataSize() * 8u) / 10u; // time in microseconds to send the payload over SPI (10MHz) to the NRF24L01
    auto newRefMicros = _refMicros;
    uint8_t arcUsed = 0;
    auto result = false;
    for (uint8_t attempt = 0; attempt < (SLOTS * MAX_CHANNELS); ++attempt) {

        // we send the packet and wait for acknowledgment. If we get acknowledgment or if this is the last attempt, we are done.
        result = _radio->write(cmd.getDataPayload(), cmd.getDataSize());
        if (result || (attempt == (SLOTS * MAX_CHANNELS) - 1)) { break; }

        // if we did not get acknowledgment, we wait for the next time slot to retry
        uint32_t diffMicros = micros() - newRefMicros + spiTime;
        if (diffMicros < TIME_SLOT) {
            delayMicroseconds(TIME_SLOT - diffMicros);
        }
        newRefMicros += TIME_SLOT;
        ++arcUsed;
    }

    if (result) {
        // we got acknowledgment, we can sync the rx channel to the tx channel
        _syncLifetime = micros();
        _inSync = true;
        _rxChIdx = _txChIdx;
        _refMicros = newRefMicros;
    } else {
        // we did not get acknowledgment, we are not in sync anymore and we keep the tx channel
        _inSync = false;
    }

    _radio->setRetries(0, 0);
    openReadingPipe();
    switchRxCh(true); // switch back to receive mode to be ready for the response.
    _busyFlag = true;
    _rxTimeout.set(cmd.getTimeout());

    // now we can log the transmission result and statistics
    // we do this after switching back to receive mode, because the logging can take some time and negatively influence the timing.
    ESP_LOGD(TAG, "TX %s Channel: %" PRIu8 " --> %s",
        cmd.getCommandName().c_str(), _chLst[_txChIdx], cmd.dumpDataPayload().c_str());

    _txCounter++;
    if (!result) { _txFailCounter++; }
    if (_txCounter > 50000) { _txCounter /= 2; _txFailCounter /= 2; }

    ESP_LOGV(TAG, "Acknowledge: %s, Acknowledge performance: %0.2f%%, ARC used: %u, PayLoad: %u",
        result ? "Yes" : "No", (_txCounter - _txFailCounter) * 100.0f / _txCounter, arcUsed, cmd.getDataSize());

    uint32_t allCounter = _inPercentCounter[0] + _inPercentCounter[1] + _inPercentCounter[2];
    if (allCounter > 10) {
        auto percent0 = _inPercentCounter[0] * 100 / allCounter;
        auto percent1 = _inPercentCounter[1] * 100 / allCounter;
        auto percent2 = 100 - (percent0 + percent1);
        ESP_LOGV(TAG, "Channel switching timing performance: < 0.5 TS: %u%%, 0.5-1 TS: %u%%, > 1 TS: %u%%",
            percent0, percent1, percent2);
    }
}
