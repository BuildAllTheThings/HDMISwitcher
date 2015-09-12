// tl;dr Project synopsis
// I bought some cheap HDMI switchers and want to have an API to arbitrarily switch the inputs but they came with cheap IR remotes.
// Conveniently, though, they came with IR receivers that I can spoof the signal from so I'm going to pretend to receive IR commands
// when the Spark receives an API call.
// 
// 
// 
// Wiring instructions
// For switching generic HDMI 5 in / 1 out switches obtained from Amazon by sending simulated infrared codes over the TRS (audio jack) connection in lieu of the included IR receiver
// The HDMI switcher uses the sleeve as ground, and provides 3.3v over the tip to drive the electronics of the receiver
// The ring is attached to 3.3v using a pullup resistor, and the IR receiver is expected to pull it low to signal receipt of infrared codes
//
// Usage instructions:
// As an example, to switch the selector wired to pin 1 to input 4, send an HTTPS request like this:
// POST https://api.particle.io/v1/devices/<device id>/selectInput
// Content-Type: application/x-www-form-urlencoded
// 
// access_token=<your access token>&command=1,4
// 
// 
// 
// Long form investigation narrative:
// 
// I have captured some signals using my Saleae logic analyzer to confirm some theories
// The smallest "off" interval and the smallest "on" interval are about 590 microseconds
// There is a significant lead-in low pulse of about 9 milliseconds before what appears
// to be each data burst; other than that, low pulses are always 590 microseconds.
// The high pulses appear to come in a few more flavors; there is a longish one after the
// very long low pulse, and then there are two main varieties interleaved among the short
// low pulses.
// The pulse width of the high pulses appears to be the meaningful signal with the short
// low pulses the separator.  The high pulses between short low pulses measure either
// 590 microseconds or 1.67 milliseconds (approximately 1:3 ratio).  It seems likely this
// is intended to be a more precise multiple on paper but is probably accepted without
// complaint by the decoder on the other side so most likely the generating electronics
// could afford some imprecision.
// 
// Based on my readings this is a standard NEC generic infrared signal sent raw over the TRS line,
// except that the concept of high/low is inverted from the typical description (the IR receiver
// is pulling the signal line low when receiving a protocol high).
// 
// In such a signal, the "mark" is sent to indicate a new bit and the width of the
// "space" determines which bit is being transmitted; note that the standard implementation
// dictates a mark:space ratio of 1:1 for a low bit, 1:3 for a high bit.
// From here on out I am now reading the NEC infrared spec first and confirming with measurements
// taken from the logic analyzer instead of reverse engineering, since I'm pretty sure I'm on the
// right path.
// This document is quite decent:
// http://techdocs.altium.com/display/FPGA/NEC+Infrared+Transmission+Protocol
// Many documents describe NEC code as being a 38khz signal with the smallest unit of data being
// 22 cycles of the carrier wave; however, I have also found documents describing the smallest unit
// of data being 564 microseconds (which would put it at 39khz), and my own observations appear to
// put it around 590 microseconds (which would make it a 37.3khz carrier signal).  I am fairly
// certain most chips are therefore flexible in their interpretation and that they're forgiving given
// the lossiness of the medium, so we don't need incredible timing for this.
// Because I'm pretty sure the chip in the Spark core is overpowered I'm just going to start with
// bitbanging this, and if I need to go to PWM I will get fancier later.
// Based on my interpretation of the captured results of pressing "button 1" on my remote it sends:
// 0000 0001 1111 1110 0100 0000 1011 1111 
// and then a single low interval followed by a 40ms high interval before sending some more data in
// a separate burst.
// 
// Given the documents on NEC code I believe this is:
// - address byte 0x01
// - inverted address byte
// - command byte 0x40
// - inverted command byte
// - stop bit
// 
// Given that capture and interpretation of my remote's "button 1" I assumed that the address would always be 1
// and simply brute forced the remainder of the codes instead of capturing them.

#define STANDARD_DURATION 564
#define ACTIVE_BIT 0x80000000
#define BYTE_MASK 0xFF

void sendMark(char pin, unsigned int duration) {
    digitalWrite(pin, LOW);
    delayMicroseconds(duration);
}

void sendSpace(char pin, unsigned int duration) {
    digitalWrite(pin, HIGH);
    delayMicroseconds(duration);
}

void sendOne(char pin) {
    sendMark(pin, STANDARD_DURATION);
    sendSpace(pin, 3 * STANDARD_DURATION);
}

void sendZero(char pin) {
    sendMark(pin, STANDARD_DURATION);
    sendSpace(pin, STANDARD_DURATION);
}

void send(char pin, unsigned long data) {
    sendMark(pin, 16 * STANDARD_DURATION);
    sendSpace(pin, 8 * STANDARD_DURATION);
    // 32 bits
    for (int i = 0; i < 32; i++) {
        if (data & ACTIVE_BIT) {
            sendOne(pin);
        } else {
            sendZero(pin);
        }
        data <<= 1;
    }
    // Stop bit
    sendMark(pin, STANDARD_DURATION);
    // Return to rest
    digitalWrite(pin, HIGH);
}

void send(uint16_t pin, char address, char command) {
    unsigned long data = 0x00000000;
    data |= (address & BYTE_MASK) << 24;
    data |= ((address ^ BYTE_MASK) & BYTE_MASK) << 16;
    data |= (command & BYTE_MASK) << 8;
    data |= (command ^ BYTE_MASK) & BYTE_MASK;
    send(pin, data);
}



const char SELECTOR_PINS[] = { (char)A2, (char)A1, (char)A0, (char)A3 };
const char INPUT_COMMANDS[] = { 0x40, 0x60, 0x10, 0x50, 0xB0 };

int sendCommand(String args) {
    // Command is a string of the format "<selector>,<command>", indexing from 1 for the selector
    // We also accept "<selector>,<address>,<command>"
    int separatorIndex = args.indexOf(",");
    int selector = args.substring(0, separatorIndex).toInt();
    int nextSeparatorIndex = args.indexOf(",", separatorIndex + 1);
    int address = 1;
    if (nextSeparatorIndex > 0) {
        address = args.substring(separatorIndex + 1, nextSeparatorIndex).toInt();
        separatorIndex = nextSeparatorIndex;
    }
    int command = args.substring(separatorIndex + 1).toInt();
    int numSelectorPins = sizeof(SELECTOR_PINS) / sizeof(*SELECTOR_PINS);
    if (selector > 0 && selector <= numSelectorPins) {
        send(SELECTOR_PINS[selector - 1], address, command);
        return 0;
    }
    
    return 1;
}

int selectInput(String args) {
    // Command is a string of the format "<selector>,<input>", indexing from 1
    int separatorIndex = args.indexOf(",");
    int selector = args.substring(0, separatorIndex).toInt();
    int input = args.substring(separatorIndex + 1).toInt();
    int numSelectorPins = sizeof(SELECTOR_PINS) / sizeof(*SELECTOR_PINS);
    int numInputCommands = sizeof(INPUT_COMMANDS) / sizeof(*INPUT_COMMANDS);
    if (selector > 0 && selector <= numSelectorPins) {
        if (input > 0 && input <= numInputCommands) {
            send(SELECTOR_PINS[selector - 1], 0x01, INPUT_COMMANDS[input - 1]);
            
            return 0;
        }
    }
    
    return 1;
}



void setup() {
    Serial.begin(9600);
  
    // Set the pins we'll use to send our IR codes as outputs
    pinMode(A0, OUTPUT);
    pinMode(A1, OUTPUT);
    pinMode(A2, OUTPUT);
    // This pin is expected to be high when not in use
    digitalWrite(A0, HIGH);
    digitalWrite(A1, HIGH);
    digitalWrite(A2, HIGH);
    
    // Intended to be the principal workhorse function
    Spark.function("selectInput", selectInput);
    // Useful for brute force discovering other commands and for reusing this project to explore other devices
    Spark.function("sendCommand", sendCommand);
}

void loop() {
    // Nothing to do, seriously.
}

