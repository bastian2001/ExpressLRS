#include <Arduino.h>
#include "targets.h"
#include "utils.h"
#include "common.h"
#include "LowPassFilter.h"
#include "LoRaRadioLib.h"
#include "CRSF_RX.h"
#include "FHSS.h"
#include "rx_LinkQuality.h"
#include "HwTimer.h"
#include "HwSerial.h"
#include "debug.h"
#include "helpers.h"
#include "rc_channels.h"

//// CONSTANTS ////
#define SEND_LINK_STATS_TO_FC_INTERVAL 100

///////////////////

CRSF_RX crsf(CrsfSerial); //pass a serial port object to the class for it to use
RcChannels rc_ch;

volatile connectionState_e connectionState = STATE_disconnected;
static volatile uint8_t NonceRXlocal = 0; // nonce that we THINK we are up to.
static volatile bool alreadyFHSS = false;
static volatile uint32_t tlm_check_ratio = 0;
static volatile uint8_t rx_buffer[8];
static volatile uint8_t rx_buffer_handle = 0;

///////////////////////////////////////////////
////////////////  Filters  ////////////////////
static LPF LPF_Offset(3);
static LPF LPF_FreqError(5);
static LPF LPF_UplinkRSSI(5);

//////////////////////////////////////////////////////////////
////// Variables for Telemetry and Link Quality //////////////

static volatile uint32_t LastValidPacket = 0; //Time the last valid packet was recv
static uint32_t SendLinkStatstoFCintervalNextSend = SEND_LINK_STATS_TO_FC_INTERVAL;

///////////////////////////////////////////////////////////////
///////////// Variables for Sync Behaviour ////////////////////
static uint32_t RFmodeNextCycle = 0;
static volatile uint8_t scanIndex = 0;
static volatile uint8_t tentative_cnt = 0;

static inline void RfModeNextCycleCalc(void)
{
    RFmodeNextCycle = millis() +
                      ExpressLRS_currAirRate->RFmodeCycleInterval +
                      /* ((connectionState == STATE_tentative) ? ExpressLRS_currAirRate->RFmodeCycleAddtionalTime : 0);*/
                      ExpressLRS_currAirRate->RFmodeCycleAddtionalTime;
}

///////////////////////////////////////

static inline void TimerAdjustment(uint32_t us)
{
    /* Adjust timer only if sync ok */
    uint32_t interval = ExpressLRS_currAirRate->interval;
    us -= TxTimer.LastCallbackMicrosTick;
    int32_t HWtimerError = us % interval;
    HWtimerError -= (interval >> 1);
    int32_t Offset = LPF_Offset.update(HWtimerError); //crude 'locking function' to lock hardware timer to transmitter, seems to work well enough
    TxTimer.phaseShift(uint32_t((Offset >> 4) + timerOffset));
}

///////////////////////////////////////

void ICACHE_RAM_ATTR getRFlinkInfo()
{
    int8_t LastRSSI = Radio.LastPacketRSSI;
    crsf.ChannelsPacked.ch15 = UINT10_to_CRSF(MAP(LastRSSI, -100, -50, 0, 1023));
    crsf.ChannelsPacked.ch14 = UINT10_to_CRSF(MAP_U16(linkQuality, 0, 100, 0, 1023));
#if 1
    int32_t rssiDBM = LPF_UplinkRSSI.update(LastRSSI);
    // our rssiDBM is currently in the range -128 to 98, but BF wants a value in the range
    // 0 to 255 that maps to -1 * the negative part of the rssiDBM, so cap at 0.
    if (rssiDBM > 0)
        rssiDBM = 0;
    crsf.LinkStatistics.uplink_RSSI_1 = -1 * rssiDBM; // to match BF
#else
    uint8_t rssi = LPF_UplinkRSSI.update(Radio.LastPacketRssiRaw);
    crsf.LinkStatistics.uplink_RSSI_1 = (rssi > 127) ? 127 : rssi;
#endif
    crsf.LinkStatistics.uplink_SNR = Radio.LastPacketSNR * 10;
    crsf.LinkStatistics.uplink_Link_quality = linkQuality;
    //DEBUG_PRINTLN(crsf.LinkStatistics.uplink_RSSI_1);
}

uint8_t ICACHE_RAM_ATTR HandleFHSS()
{
    uint8_t modresult = NonceRXlocal % ExpressLRS_currAirRate->FHSShopInterval;
    if (connectionState > STATE_disconnected) // don't hop if we lost
    {
        if (modresult == 0)
        {
            //DEBUG_PRINT("F");
            linkQuality = getRFlinkQuality();
            Radio.RXnb(FHSSgetNextFreq()); // 260us => 148us => ??
        }
        NonceRXlocal++;
    }
    return modresult;
}

void ICACHE_RAM_ATTR HandleSendTelemetryResponse()
{
    //DEBUG_PRINT("T");

    uint32_t __tx_buffer[2]; // esp requires aligned buffer
    uint8_t *tx_buffer = (uint8_t *)__tx_buffer;

    tx_buffer[0] = DEIVCE_ADDR_GENERATE(DeviceAddr) + TLM_PACKET; // address + tlm packet

    crsf.LinkStatisticsPack(&tx_buffer[1]);

    uint8_t crc = CalcCRC(tx_buffer, 7) + CRCCaesarCipher;
    tx_buffer[7] = crc;
    Radio.TXnb(tx_buffer, 8, FHSSgetCurrFreq());
    addPacketToLQ(); // Adds packet to LQ otherwise an artificial drop in LQ is seen due to sending TLM.
}

void ICACHE_RAM_ATTR HWtimerCallback(uint32_t us)
{
    //DEBUG_PRINT("H");
    if (!alreadyFHSS)
        HandleFHSS();
    alreadyFHSS = false;

    // Check if connected before sending tlm resp
    if (STATE_disconnected < connectionState)
    {
        incrementLQArray();
        if (tlm_check_ratio && (NonceRXlocal & tlm_check_ratio) == 0)
        {
            HandleSendTelemetryResponse();
        }
    }
}

void ICACHE_RAM_ATTR LostConnection()
{
    if (connectionState <= STATE_disconnected)
    {
        return; // Already disconnected
    }

    connectionState = STATE_disconnected; //set lost connection
    LPF_FreqError.init(0);
    scanIndex = 0;

    led_set_state(1);                     // turn off led
    Radio.SetFrequency(GetInitialFreq()); // in conn lost state we always want to listen on freq index 0
    DEBUG_PRINTLN("lost conn");

    platform_connection_state(connectionState);
}

void ICACHE_RAM_ATTR TentativeConnection()
{
    tentative_cnt = 0;
    connectionState = STATE_tentative;
    DEBUG_PRINTLN("tentative conn");
}

void ICACHE_RAM_ATTR GotConnection()
{
    connectionState = STATE_connected; //we got a packet, therefore no lost connection

    led_set_state(0); // turn on led
    DEBUG_PRINTLN("got conn");

    platform_connection_state(connectionState);
}

void ICACHE_RAM_ATTR ProcessRFPacket(volatile uint8_t *buff)
{
    //DEBUG_PRINT("I");

    volatile_memcpy(rx_buffer, buff, sizeof(rx_buffer));

    uint32_t current_us;
    uint8_t calculatedCRC = CalcCRC(rx_buffer, 7) + CRCCaesarCipher;
    uint8_t address = rx_buffer[0];

    if (rx_buffer[7] != calculatedCRC)
    {
        //DEBUG_PRINTLN("CRC error on RF packet");
        //DEBUG_PRINT("!CRC");
        DEBUG_PRINT("!C");
        return;
    }
    else if (DEIVCE_ADDR_GET(address) != DeviceAddr)
    {
        //DEBUG_PRINTLN("Wrong device address on RF packet");
        DEBUG_PRINT("!A");
        return;
    }

    rx_buffer_handle = 1;

    getRFlinkInfo();

    switch (TYPE_GET(address))
    {
        case RC_DATA_PACKET: //Standard RC Data Packet
            rx_buffer_handle = 1;
            break;

#if !defined(SEQ_SWITCHES) && !defined(HYBRID_SWITCHES_8)
        case SWITCH_DATA_PACKET: // Switch Data Packet
            // extra layer of protection incase the crc and addr headers fail us.
            if ((rx_buffer[3] == rx_buffer[1]) &&
                (rx_buffer[4] == rx_buffer[2]))
            {
                rx_buffer_handle = 1;
                NonceRXlocal = rx_buffer[5];
                FHSSsetCurrIndex(rx_buffer[6]);
            }
            break;
#endif

        case TLM_PACKET: //telemetry packet from master
            rx_buffer_handle = 1;
            break;

        case SYNC_PACKET: //sync packet from master
            if (rx_buffer[4] == UID[3] &&
                rx_buffer[5] == UID[4] &&
                rx_buffer[6] == UID[5])
            {
                if (connectionState == STATE_disconnected)
                {
                    TentativeConnection();
                }
                else if (connectionState == STATE_tentative)
                {
                    if (NonceRXlocal == rx_buffer[2] &&
                        FHSSgetCurrIndex() == rx_buffer[1])
                    {
                        GotConnection();
                    }
                    else if (2 < (tentative_cnt++))
                    {
                        LostConnection();
                    }
                }

                FHSSsetCurrIndex(rx_buffer[1]);
                NonceRXlocal = rx_buffer[2];
            }
            break;

        default: // code to be executed if n doesn't match any cases
            break;
    }

    addPacketToLQ();

    current_us = micros();
    //TimerAdjustment(Radio.LastPacketIsrMicros);
    TimerAdjustment(current_us);

    LastValidPacket = current_us / 1000U;
    alreadyFHSS = true;

    // Do freq correction after FHSS
    if (HandleFHSS() == 1)
    {
#if !NEW_FREQ_CORR
        int32_t freqerror = LPF_FreqError.update(Radio.GetFrequencyError()); // get freq error = 90us
        //DEBUG_PRINT(freqerror);
        //DEBUG_PRINT(" : ");

        if (freqerror > 0)
        {
            if (FreqCorrection < FreqCorrectionMax)
            {
                FreqCorrection += FreqCorrectionStep;
            }
            else
            {
                FreqCorrection = FreqCorrectionMax;
                DEBUG_PRINT("FREQ_MAX");
            }
        }
        else
        {
            if (FreqCorrection > FreqCorrectionMin)
            {
                FreqCorrection -= FreqCorrectionStep;
            }
            else
            {
                FreqCorrection = FreqCorrectionMin;
                DEBUG_PRINT("FREQ_MIN");
            }
        }

        Radio.setPPMoffsetReg(FreqCorrection);
#else
        // Do freq error correction
        int32_t freqerror = Radio.GetFrequencyError();
        //freqerror = LPF_FreqError.update(freqerror);
        if ((freqerror < (-FreqCorrectionStep)) ||
            (FreqCorrectionStep < freqerror))
        {
            FreqCorrection += freqerror;
            Radio.setPPMoffsetReg(freqerror, 0);
        }
#endif
        //DEBUG_PRINTLN(FreqCorrection);
    }
    //DEBUG_PRINT("E");
}

void forced_start(void)
{
    DEBUG_PRINTLN("Manual Start");
    Radio.RXnb(GetInitialFreq());
}

void forced_stop(void)
{
    Radio.StopContRX();
    TxTimer.stop();
}

void SetRFLinkRate(uint8_t rate) // Set speed of RF link (hz)
{
    const expresslrs_mod_settings_s *const config = get_elrs_airRateConfig(rate);
    if (config == ExpressLRS_currAirRate)
        return; // No need to modify, rate is same
    ExpressLRS_currAirRate = config;

    DEBUG_PRINT("Set RF rate: ");
    DEBUG_PRINTLN(rate);
    //DEBUG_PRINTLN(config->interval);

    tlm_check_ratio = 0;
    if (TLM_RATIO_NO_TLM < config->TLMinterval)
    {
        tlm_check_ratio = TLMratioEnumToValue(ExpressLRS_currAirRate->TLMinterval) - 1;
    }

    /* TODO:
     * 1. timer stop and start!
     * 2. reset LPF_Offset, LPF_FreqError, LPF_UplinkRSSI,
     * 3. reset
     */

    Radio.StopContRX();
    Radio.Config(config->bw, config->sf, config->cr, GetInitialFreq(), 0);
    Radio.SetPreambleLength(config->PreambleLen);
    TxTimer.updateInterval(config->interval);
    //LPF_Offset.init(0);
    crsf.LinkStatistics.uplink_RSSI_2 = 0;
    crsf.LinkStatistics.rf_Mode = RATE_MAX - config->enum_rate;
    Radio.RXnb();
}

void tx_done_cb(void)
{
    Radio.RXnb();
}

void setup()
{
    platform_setup();

    CrsfSerial.Begin(CRSF_RX_BAUDRATE);

    DEBUG_PRINTLN("Module Booting...");

#ifdef Regulatory_Domain_AU_915
    DEBUG_PRINTLN("Setting 915MHz Mode");
    Radio.RFmodule = RFMOD_SX1276; //define radio module here
#elif defined Regulatory_Domain_FCC_915
    DEBUG_PRINTLN("Setting 915MHz Mode");
    Radio.RFmodule = RFMOD_SX1276; //define radio module here
#elif defined Regulatory_Domain_EU_868
    DEBUG_PRINTLN("Setting 868MHz Mode");
    Radio.RFmodule = RFMOD_SX1276; //define radio module here
#elif defined Regulatory_Domain_AU_433 || defined Regulatory_Domain_EU_433
    DEBUG_PRINTLN("Setting 433MHz Mode");
    Radio.RFmodule = RFMOD_SX1278; //define radio module here
#else
#error No regulatory domain defined, please define one in common.h
#endif

    Radio.SetPins(GPIO_PIN_RST, GPIO_PIN_DIO0, GPIO_PIN_DIO1);
    Radio.SetSyncWord(getSyncWord());
    Radio.Begin();

    FHSSrandomiseFHSSsequence();
    //Radio.SetFrequency(GetInitialFreq());

    Radio.SetOutputPower(0b1111); //default is max power (17dBm for RX)

    int16_t RFnoiseFloor = MeasureNoiseFloor();
    DEBUG_PRINT("RF noise floor: ");
    DEBUG_PRINT(RFnoiseFloor);
    DEBUG_PRINTLN("dBm");
    (void)RFnoiseFloor;

    Radio.RXdoneCallback1 = &ProcessRFPacket;

    Radio.TXdoneCallback1 = &tx_done_cb;

    TxTimer.callbackTock = &HWtimerCallback;
    TxTimer.init();
    //TxTimer.start(); // start tlm timer

    LQreset();
    scanIndex = RATE_DEFAULT;
    SetRFLinkRate(RATE_DEFAULT);
    RfModeNextCycleCalc();

    crsf.Begin();
}

void loop()
{
    uint32_t now = millis();

    if (connectionState == STATE_disconnected)
    {
        if (now > RFmodeNextCycle)
        {
            //Radio.SetFrequency(GetInitialFreq());
            SetRFLinkRate(((++scanIndex) % RATE_MAX)); //switch between rates
            LQreset();
            led_toggle();

            if (RATE_MAX < scanIndex)
                platform_connection_state(STATE_search_iteration_done);

            RfModeNextCycleCalc();
        }
    }
    else if (connectionState > STATE_disconnected)
    {
        // check if we lost conn.
        if (now > (LastValidPacket + ExpressLRS_currAirRate->RFmodeCycleAddtionalTime))
        {
            LostConnection();
        }
        else if (connectionState == STATE_connected)
        {
            if (rx_buffer_handle)
            {
                switch (TYPE_GET(rx_buffer[0]))
                {
                    case RC_DATA_PACKET:     // Standard RC Data Packet
                    case SWITCH_DATA_PACKET: // Switch Data Packet
                        rc_ch.channels_extract(rx_buffer, crsf.ChannelsPacked);
                        crsf.sendRCFrameToFC();
                        break;

                    case TLM_PACKET:
                        // not implimented yet
                        break;

                    case SYNC_PACKET: // sync packet is handled in ISR
                    default:
                        break;
                }
            }

            if (now > SendLinkStatstoFCintervalNextSend)
            {
                crsf.LinkStatisticsSend();
                SendLinkStatstoFCintervalNextSend = now + SEND_LINK_STATS_TO_FC_INTERVAL;
            }
        }
    }

    crsf.handleUartIn();

    platform_loop(connectionState);
    platform_wd_feed();
}
