/*
// App listens on UDP port for frames,
// parse them and route to spi/pwm outputs
// through 2 switches
//
*/

#include <atomic>
#include <iostream>
#include <map>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <chrono>

#include "UdpManager.h"
#include "rpi_ws281x/ws2811.h"
#include "spi/SpiOut.h"
#include <wiringPi.h>

#include "easylogging++.h"

using microseconds = std::chrono::microseconds; 
using milliseconds = std::chrono::milliseconds; 
using namespace std::chrono_literals;

// WS281X lib options
#define GPIO_PIN_1 12
#define GPIO_PIN_2 13
#define DMA 10
#define STRIP_TYPE WS2811_STRIP_RGB // WS2812/SK6812RGB integrated chip+leds
//#define STRIP_TYPE SK6812_STRIP_RGBW // SK6812RGBW

constexpr size_t MAX_CHANNELS = 2;
constexpr size_t LED_COUNT_WS = 1000;
constexpr size_t LED_COUNT_SPI = 2000;
constexpr size_t MAX_SENDBUFFER_SIZE = 4096 * 3; // 2 SPI channels RGB

constexpr int FRAME_IN_PORT = 3001;
constexpr int STRIP_TYPE_PORT = 3002;

std::atomic<bool> continue_looping{ true };
int clear_on_exit = 0;

static const int PIN_SWITCH_1 = 5;
static const int PIN_SWITCH_2 = 6;
static const int PIN_SWITCH_SPI = 24;

static std::map<int, bool> s_gpioSwitches
    = { { PIN_SWITCH_1, false }, // false(LOW) - send ws to chan 1
          { PIN_SWITCH_2, false }, // false(LOW) - send ws to chan 2
          { PIN_SWITCH_SPI, true } }; // send spi to chan 1 (true) or chan 2 (false)

static const std::string s_spiDevice = "/dev/spidev0.0";

enum { TYPE_WS281X, TYPE_SK9822 };

static std::map<std::string, int> s_ledTypeToEnum
    = { { "WS281X", TYPE_WS281X }, { "SK9822", TYPE_SK9822 } };

bool initGPIO()
{
    if (wiringPiSetupGpio() != 0) {
        LOG(ERROR) << "Failed to init GPIO";
        return false;
    }
    for (auto &gpio : s_gpioSwitches) {
        pinMode(gpio.first, OUTPUT);
        LOG(INFO) << "Pin #" << std::to_string(gpio.first) << " -> "
                  << (gpio.second ? "HIGH" : "LOW");
        digitalWrite(gpio.first, (gpio.second ? HIGH : LOW));
    }

    LOG(INFO) << "GPIO Inited";
    return true;
}

bool initWS(ws2811_t &ledstring)
{
    ledstring.freq = WS2811_TARGET_FREQ;
    ledstring.dmanum = DMA;
    /// channel params sequence must fit its arrangement in ws2811_channel_t
    ledstring.channel[0] = {
        GPIO_PIN_1, // gpionum
        0, // invert
        LED_COUNT_WS, // count
        STRIP_TYPE, // strip_type
        nullptr,
        255, // brightness
    };
    ledstring.channel[1] = {
        GPIO_PIN_2, // gpionum
        0, // invert
        LED_COUNT_WS, // count
        STRIP_TYPE, // strip_type
        nullptr,
        255, // brightness
    };

    ws2811_return_t ret;
    if ((ret = ws2811_init(&ledstring)) != WS2811_SUCCESS) {
        LOG(ERROR) << "ws2811_init failed: \n" << ws2811_get_return_t_str(ret);
        return false;
    }
    return true;
};

struct GpioOutSwitcher {
    GpioOutSwitcher()
        : m_isWs(false)
    {
        switchWsOut(true);
    }

    void switchWsOut(bool isWS)
    {
        if (m_isWs == isWS)
            return;
        LOG(DEBUG) << "switch to WS = " << isWS;
        m_isWs = isWS;
        digitalWrite(PIN_SWITCH_1, m_isWs ? LOW : HIGH);
        digitalWrite(PIN_SWITCH_2, m_isWs ? LOW : HIGH);
        std::this_thread::sleep_for(milliseconds(500));
    }
    bool m_isWs;
};

void stop_program(int sig)
{
    /* Ignore the signal */
    signal(sig, SIG_IGN);
    /* stop the looping */
    continue_looping.store(false);
    /* Put the ctrl-c to default action in case something goes wrong */
    signal(sig, SIG_DFL);
}
///
/// Main : init SPI, GPIO and WS interfaces, create lister on localhost:FRAME_IN_PORT
/// receive UDP frames and route them to LEDs through right outputs on Shield
///
int main()
{
    /// WS (one wire) output setup
    ws2811_t wsOut;
    ws2811_return_t wsReturnStat;
    if (!initWS(wsOut)) {
        exit(1);
    }

    SpiOut spiOut;
    if (!spiOut.init(s_spiDevice))
        exit(1);
    /// add two channels to spi out,
    /// further can select kind of channel for different ICs
    spiOut.addChannel(LED_COUNT_SPI);
    spiOut.addChannel(LED_COUNT_SPI);

    if (!initGPIO())
        exit(1);

    /// UDP listeners setup
    LedMapper::UdpSettings udpConf;
    udpConf.receiveOn(FRAME_IN_PORT);
    udpConf.receiveBufferSize = MAX_SENDBUFFER_SIZE;
    auto frameInput = LedMapper::UdpManager();
    if (!frameInput.Setup(udpConf)) {
        LOG(ERROR) << "Failed to bind to port=" << FRAME_IN_PORT;
        exit(1);
    }

    /// Init Gpio Multiplexer Switcher, LED Type selection listener thread and atomic isWS flag init
    GpioOutSwitcher gpioSwitcher;
    std::atomic<bool> isWS{ gpioSwitcher.m_isWs };

    std::thread typeListener([&isWS]() {
        LedMapper::UdpSettings udpConf;
        udpConf.receiveOn(STRIP_TYPE_PORT);
        auto typeInput = LedMapper::UdpManager();
        if (!typeInput.Setup(udpConf)) {
            LOG(ERROR) << "Failed to bind to port=" << STRIP_TYPE_PORT;
            exit(1);
        }
        std::string currentType{ "" };
        char message[6];
        while (continue_looping.load()) {
            if (typeInput.Receive(message, 6) < 6)
                continue;
            std::string type(message, 6);
            if (currentType != type) {
                currentType = type;
                LOG(DEBUG) << "Got new type " << type;
                isWS.store(s_ledTypeToEnum[type] == TYPE_WS281X, std::memory_order_release);
            }
        };
    });

    LOG(INFO) << "Inited ledMapper Listener";

    /// break while loops on termination
    signal(SIGINT, &stop_program);

    size_t received = 0;
    size_t i = 0;
    size_t total_leds_num = 0, max_leds_in_chan;
    size_t chan_cntr = 0, curChannel;
    size_t headerByteOffset = 0, chanPixelOffset = 0;
    uint16_t ledsInChannel[] = { 0, 0, 0, 0, 0, 0 };
    char *pixels;
    char message[MAX_SENDBUFFER_SIZE];

    while (continue_looping.load()) {
        /// update output route based on atomic bool changed in typeListener thread
        gpioSwitcher.switchWsOut(isWS.load(std::memory_order_acquire));

        /// wait for frames with min size 4 bytes which are header
        if ((received = frameInput.PeekReceive()) > 4) {
            if (frameInput.Receive(message, received) <= 0)
                continue;

            chan_cntr = 0;
            max_leds_in_chan = 0;
            /// parse header to get number of leds to read per each channel
            /// header end is sequence of two 0xff chars
            while (chan_cntr + 1 < received
                && (message[chan_cntr * 2] != 0xff && message[chan_cntr * 2 + 1] != 0xff)) {
                ledsInChannel[chan_cntr] = message[chan_cntr * 2 + 1] << 8 | message[chan_cntr * 2];
                // LOG(DEBUG) << "chan #" << chan_cntr << "has leds=" << ledsInChannel[chan_cntr];
                if (ledsInChannel[chan_cntr] > max_leds_in_chan)
                    max_leds_in_chan = ledsInChannel[chan_cntr];
                ++chan_cntr;
            }

            headerByteOffset = chan_cntr * 2 + 2;
            /// pixels pointer stores point to pixel data in message starting after offset
            pixels = message + headerByteOffset;

            if (chan_cntr > MAX_CHANNELS)
                chan_cntr = MAX_CHANNELS;

            total_leds_num = (received - headerByteOffset) / 3;

            /// For each channel fill output buffers with pixels data
            chanPixelOffset = 0;
            for (curChannel = 0; curChannel < chan_cntr; ++curChannel) {
                for (i = chanPixelOffset;
                     i < ledsInChannel[curChannel] + chanPixelOffset && i < total_leds_num; ++i) {

                    // printf("%i : %i -> %d  %d  %d\n", curChannel, i - chanPixelOffset,
                    //        pixels[i * 3 + 0], pixels[i * 3 + 1],
                    //        pixels[i * 3 + 2]);

                    if (isWS) {
                        wsOut.channel[curChannel].leds[i - chanPixelOffset]
                            = (pixels[i * 3 + 0] << 16) | (pixels[i * 3 + 1] << 8)
                            | pixels[i * 3 + 2];
                    }
                    else {
                        spiOut.writeLed(curChannel, i - chanPixelOffset, pixels[i * 3 + 0],
                            pixels[i * 3 + 1], pixels[i * 3 + 2]);
                    }
                }
                chanPixelOffset += ledsInChannel[curChannel];
            }

            if (isWS) {
                wsReturnStat = ws2811_render(&wsOut);
                if (wsReturnStat != WS2811_SUCCESS) {
                    LOG(ERROR) << "ws2811_render failed: " << ws2811_get_return_t_str(wsReturnStat);
                    break;
                }
                std::this_thread::sleep_for(microseconds(30 * max_leds_in_chan));
            }
            else {
                for (curChannel = 0; curChannel < chan_cntr; ++curChannel) {
                    if (ledsInChannel[curChannel] == 0)
                        continue;
                    digitalWrite(PIN_SWITCH_SPI, curChannel == 0 ? HIGH : LOW);
                    spiOut.send(curChannel, ledsInChannel[curChannel]);
                }
                std::this_thread::sleep_for(microseconds(max_leds_in_chan));
            }
        }
    }

    LOG(INFO) << "Exit from loop";

    typeListener.join();

    ws2811_fini(&wsOut);

    return 0;
}
