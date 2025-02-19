/*
 *  © 2022, Peter Cole. All rights reserved.
 *  
 *  This file is part of EX-IOExpander.
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
* EX-IOExpander is a native DCC-EX I/O expander utilising a microcontroller's digital and analogue I/O pins
* to expand the I/O capability of an EX-CommandStation.
*
* All digital I/O pins are available as digital inputs or outputs.
* Analogue I/O pins are available as digital inputs or outputs or analogue inputs (depending on architecture).
*/

#include <Arduino.h>
#include "SupportedDevices.h"

#ifdef CPU_TYPE_ERROR
#error Unsupported microcontroller architecture detected, you need to use a type listed in defines.h
#endif

/*
If we haven't got a custom config.h, use the example.
*/
#if __has_include ("myConfig.h")
  #include "myConfig.h"
#else
  #warning myConfig.h not found. Using defaults from myConfig.example.h
  #include "myConfig.example.h"
#endif

/*
* Struct to define the digital pin parameters and keep state.
*/
typedef struct {
  bool direction;       // 0 = output, 1 = input
  bool pullup;          // 0 = no pullup, 1 = pullup (input only)
  bool state;           // stores current state, 0 = LOW, 1 = HIGH
} digitalConfig;

/*
* Struct to define the analogue pin assignment and keep state
*/
typedef struct {
  byte valueLSB;        // Least significant byte of analogue value
  byte valueMSB;        // Most significant byte of analogue value
  bool enable;          // Flag if it's enabled (1) or not (0)
} analogueConfig;

/*
* Create digitalConfig array using struct.
* Size of the array needs to be able to include analogue pins as well.
*/
digitalConfig digitalPins[NUMBER_OF_DIGITAL_PINS + NUMBER_OF_ANALOGUE_PINS];

/*
* Create array for analogue pin assignments.
*/
analogueConfig analoguePins[NUMBER_OF_ANALOGUE_PINS];

/*
* Global variables here
*/
uint8_t numDigitalPins = NUMBER_OF_DIGITAL_PINS;    // Init with default, will be overridden by config
uint8_t numAnaloguePins = NUMBER_OF_ANALOGUE_PINS;  // Init with default, will be overridden by config
int digitalPinBytes;  // Used for configuring and sending/receiving digital pins
int analoguePinBytes; // Used for enabling/disabling analogue pins
bool setupComplete = false;   // Flag when initial configuration/setup has been received
uint8_t outboundFlag;   // Used to determine what data to send back to the CommandStation
byte analogueOutBuffer[2];  // Array to send requested LSB/MSB of the analogue value to the CommandStation
byte digitalOutBuffer[1];   // Array to send digital value to CommandStation
#ifdef DIAG
unsigned long lastPinDisplay = 0;   // Last time in millis we displayed DIAG input states
#endif

/*
* If for some reason the I2C address isn't defined, define our default here.
*/
#ifndef I2C_ADDRESS
#define I2C_ADDRESS 0x65
#endif

/*
* Include required files and libraries.
*/
#include "version.h"
#include <Wire.h>

/*
* Main setup function here.
*/
void setup() {
  Serial.begin(115200);
  Serial.print(F("DCC-EX EX-IOExpander version "));
  Serial.println(VERSION);
  Serial.print(F("Detected device: "));
  Serial.println(BOARD_TYPE);
  Serial.print(F("Available at I2C address 0x"));
  Serial.println(I2C_ADDRESS, HEX);
  Wire.begin(I2C_ADDRESS);
  // Need to intialise every pin in INPUT mode (no pull ups) for safe start
  for (uint8_t pin = 0; pin < NUMBER_OF_DIGITAL_PINS; pin++) {
    pinMode(digitalPinMap[pin], INPUT);
  }
  for (uint8_t pin = 0; pin < NUMBER_OF_ANALOGUE_PINS; pin++) {
    pinMode(analoguePinMap[pin], INPUT);
  }
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
}

/*
* Main loop here, just processes our inputs and updates the writeBuffer.
*/
void loop() {
  if (setupComplete) {
    for (uint8_t dPin = 0; dPin < numDigitalPins; dPin++) {
      if (digitalPins[dPin].direction == 1) {
        if (digitalPins[dPin].pullup == 1) {
          pinMode(digitalPinMap[dPin], INPUT_PULLUP);
        } else {
          pinMode(digitalPinMap[dPin], INPUT);
        }
        bool currentState = digitalRead(digitalPinMap[dPin]);
        if (digitalPins[dPin].pullup) currentState = !currentState;
        digitalPins[dPin].state = currentState;
      }
    }
    for (uint8_t aPin = 0; aPin < numAnaloguePins; aPin++) {
      if (analoguePins[aPin].enable == 1) {
        uint16_t value = analogRead(analoguePinMap[aPin]);
        analoguePins[aPin].valueLSB = value & 0xFF;
        analoguePins[aPin].valueMSB = value >> 8;
      }
    }
  }
  #ifdef DIAG
  displayPins();
#endif
}

/*
* Function triggered when CommandStation is sending data to this device.
*/
void receiveEvent(int numBytes) {
  if (numBytes == 0) {
    return;
  }
  byte buffer[numBytes];
  for (uint8_t byte = 0; byte < numBytes; byte++) {
    buffer[byte] = Wire.read();   // Read all received bytes into our buffer array
  }
  switch(buffer[0]) {
    // Initial configuration start, must be 3 bytes
    case EXIOINIT:
      if (numBytes == 3) {
        numDigitalPins = buffer[1];
        numAnaloguePins = buffer[2];
        if (numDigitalPins + numAnaloguePins == NUMBER_OF_DIGITAL_PINS + NUMBER_OF_ANALOGUE_PINS) {
          // Calculate number of bytes required to cover pins
          digitalPinBytes = (numDigitalPins + 7) / 8;
          analoguePinBytes = (numAnaloguePins + 7) / 8;
          for (uint8_t pin = 0; pin < NUMBER_OF_DIGITAL_PINS + NUMBER_OF_ANALOGUE_PINS; pin++) {
            digitalPins[pin].direction = 0;
            digitalPins[pin].pullup = 0;
            digitalPins[pin].state = 0;
          }
          for (uint8_t pin = 0; pin < NUMBER_OF_ANALOGUE_PINS; pin++) {
            analoguePins[pin].enable = 0;
            analoguePins[pin].valueLSB = 0;
            analoguePins[pin].valueMSB = 0;
          }
#ifdef DIAG
          Serial.print(F("Configured pins (digital|analogue): "));
          Serial.print(numDigitalPins);
          Serial.print(F("|"));
          Serial.println(numAnaloguePins);
#endif
          setupComplete = true;
        } else {
          Serial.print(F("ERROR: Invalid pins sent by device driver! (Digital|Analogue): "));
          Serial.print(numDigitalPins);
          Serial.print(F("|"));
          Serial.println(numAnaloguePins);
          setupComplete = false;
        }
        outboundFlag = EXIOINIT;
      } else {
#ifdef DIAG
        Serial.println(F("EXIOINIT received with incorrect data"));
#endif
      }
      break;
    // Flag to set digital pin pullups, 0 disabled, 1 enabled
    case EXIODPUP:
      if (numBytes == 3) {
        uint8_t pin = buffer[1];
        digitalPins[pin].direction = 1;   // Must be an input if we got a pullup config
        digitalPins[pin].pullup = buffer[2];
        if (digitalPins[pin].pullup == 1) {
          pinMode(digitalPinMap[pin], INPUT_PULLUP);
        } else {
          pinMode(digitalPinMap[pin], INPUT);
        }
        digitalPins[pin].state = digitalRead(digitalPinMap[pin]);
        // }
      } else {
#ifdef DIAG
      Serial.println(F("EXIODPUP received with incorrect number of bytes"));
#endif
      }
      break;
    case EXIORDAN:
      if (numBytes == 2) {
        outboundFlag = EXIORDAN;
        uint8_t aPin = buffer[1] - NUMBER_OF_DIGITAL_PINS;
        if (analoguePins[aPin].enable == 0) {
          analoguePins[aPin].enable = 1;
          uint16_t value = analogRead(analoguePinMap[aPin]);
          analoguePins[aPin].valueLSB = value & 0xFF;
          analoguePins[aPin].valueMSB = value >> 8;
        }
        analogueOutBuffer[0] = analoguePins[aPin].valueLSB;
        analogueOutBuffer[1] = analoguePins[aPin].valueMSB;
      }
      break;
    case EXIOWRD:
      if (numBytes == 3) {
        uint8_t dPin = buffer[1];
        bool state = buffer[2];
        digitalPins[dPin].direction = 0;
        digitalPins[dPin].state = state;
        digitalWrite(digitalPinMap[dPin], state);
      }
      break;
    case EXIORDD:
      if (numBytes == 3) {
        uint8_t dPin = buffer[1];
        outboundFlag = EXIORDD;
        digitalOutBuffer[0] = digitalPins[dPin].state;
      }
      break;
    default:
      break;
  }
}

/*
* Function triggered when CommandStation polls for inputs on this device.
*/
void requestEvent() {
  switch(outboundFlag) {
    case EXIOINIT:
      if (setupComplete) {
        Wire.write(EXIORDY);
      } else {
        Wire.write(0);
      }
      break;
    case EXIORDAN:
      Wire.write(analogueOutBuffer, 2);
      break;
    case EXIORDD:
      Wire.write(digitalOutBuffer, 1);
      break;
    default:
      break;
  }
}

/*
* Function to display pin configuratin and states when DIAG enabled
*/
#ifdef DIAG
void displayPins() {
  if (millis() > lastPinDisplay + DIAG_CONFIG_DELAY) {
    lastPinDisplay = millis();
    Serial.println(F("Digital Pin|Direction|Pullup|State:"));
    for (uint8_t pin = 0; pin < NUMBER_OF_DIGITAL_PINS + NUMBER_OF_ANALOGUE_PINS; pin++) {
      Serial.print(digitalPinMap[pin]);
      Serial.print(F("|"));
      Serial.print(digitalPins[pin].direction);
      Serial.print(F("|"));
      Serial.print(digitalPins[pin].pullup);
      Serial.print(F("|"));
      if (pin == NUMBER_OF_DIGITAL_PINS + NUMBER_OF_ANALOGUE_PINS - 1 || (pin % 15 == 0 && pin > 0)) {
        Serial.println(digitalPins[pin].state);
      } else {
        Serial.print(digitalPins[pin].state);
        Serial.print(F(","));
      }
    }
    Serial.println(F("Analogue Pin|Enable|Value|LSB|MSB:"));
    for (uint8_t pin = 0; pin < NUMBER_OF_ANALOGUE_PINS; pin++) {
      Serial.print(analoguePinMap[pin]);
      Serial.print(F("|"));
      Serial.print(analoguePins[pin].enable);
      Serial.print(F("|"));
      Serial.print((analoguePins[pin].valueMSB << 8) + analoguePins[pin].valueLSB);
      Serial.print(F("|"));
      Serial.print(analoguePins[pin].valueLSB);
      Serial.print(F("|"));
      if (pin == NUMBER_OF_ANALOGUE_PINS - 1) {
        Serial.println(analoguePins[pin].valueMSB);
      } else {
        Serial.print(analoguePins[pin].valueMSB);
        Serial.print(F(","));
      }
    }
  }
}
#endif