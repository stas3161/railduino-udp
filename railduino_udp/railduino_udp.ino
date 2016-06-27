/*
    Copyright (C) 2016  Dusan Zatkovsky
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    
 */

#define oneWireSleepRatio 2

#include <OneWire.h>
#include <Dhcp.h>
#include <Dns.h>
#include <Ethernet.h>
#include <EthernetClient.h>
#include <EthernetServer.h>
#include <EthernetUdp.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0 }; // last byte will be derived from board address
unsigned int listenPort = 44444; 
unsigned int sendPort = 55554; 
unsigned int loxonePort = 55555; 
IPAddress listenIpAddress;
IPAddress sendIpAddress(255, 255, 255, 255);

#define inputPacketBufferSize UDP_TX_PACKET_MAX_SIZE
char inputPacketBuffer[UDP_TX_PACKET_MAX_SIZE];

#define outputPacketBufferSize 100
char outputPacketBuffer[outputPacketBufferSize];

#define dbg(x) Serial.println(x);
//#define dbg(x) ;

EthernetUDP udpRecv;
EthernetUDP udpSend;

#define numOfRelays 12
int relayPins[] = {39,41,43,45,47,49,23,25,27,29,31,33};

#define numOfInputs 24
int inputPins[] = {36,34,48,46,69,68,67,66,44,42,40,38,6,5,3,2,14,15,16,17,24,26,28,30};
int inputStatus[numOfInputs];

int boardAddress = 0;
String boardAddressStr;

String relayOnCommands[numOfRelays];
String relayOffCommands[numOfRelays];

void setup() {
  Serial.begin(9600);

 /* COMMON settings */
  for(int i = 0; i < numOfRelays; i++) {
    pinMode(relayPins[i], OUTPUT);  
  }
  pinMode(32, OUTPUT);  // LED
  
  /* digital inputs settings */
  for(int i = 0; i < numOfInputs; i++) {
    pinMode(inputPins[i], INPUT_PULLUP);   
    inputStatus[i] = 0;
  }
  
  /* DIP SWITCH settings */
  pinMode(57, INPUT);  
  pinMode(56, INPUT);
  pinMode(55, INPUT);
  pinMode(54, INPUT);
  
  /* ANALOG INPUTS */  
  pinMode(A4, INPUT);
  pinMode(A5, INPUT);
  pinMode(A8, INPUT);
  
  /* PWM OUTPUTS */
  pinMode(7, OUTPUT);  
  pinMode(11, OUTPUT);
  pinMode(12, OUTPUT);
  pinMode(13, OUTPUT);

  // read address from dip switch
  if (!digitalRead(54)) {boardAddress |= 1;}
  if (!digitalRead(55)) {boardAddress |= 2;}
  if (!digitalRead(56)) {boardAddress |= 4;}
  if (!digitalRead(57)) {boardAddress |= 8;}
  
  boardAddressStr = String(boardAddress);

  // generate relay on/off command templates (speedup matching when receiving udp packet)
  for(int i = 0; i < numOfRelays; i++) {
     relayOnCommands[i] = "rail" + boardAddressStr + " r" + String(i + 1, DEC) + " on";
     relayOffCommands[i] = "rail" + boardAddressStr + " r" + String(i + 1, DEC) + " off";
  }

   // initialize ethernet
   mac[5] = (0xED + boardAddress);
   Ethernet.begin(mac, listenIpAddress);
   udpRecv.begin(listenPort);    
   udpSend.begin(sendPort);    
   listenIpAddress = IPAddress(192, 168, 150, 150 + boardAddress);

  dbg("Railduino address: ");
  dbg(boardAddressStr);
  dbg(listenIpAddress);

  for(int i = 0; i < numOfRelays; i++) {
    setRelay(i, 0);
  }
    
}

void loop() {

  // read inputs and send updates
  readInputs();
  
  // process relay commands
  processCommands();

  // process onewire
  processOnewire();
  
  delay(10);

}


OneWire  ds(9);
byte oneWireData[12];
byte oneWireAddr[8];
enum OneWireConversationState {SEARCH, INIT, READ};
OneWireConversationState oneWireConversationState = SEARCH;
unsigned long nextOneWireNextTriggerTime = millis();

void oneWireSleep(unsigned long timeMs) {
  nextOneWireNextTriggerTime = millis() + timeMs * oneWireSleepRatio;  
}

String oneWireAddressToString(byte addr[]) {
  String s = "";
  for(int i = 0; i < 8; i++) {
    s += String(addr[i], HEX);
  }
  return s;
}

void processOnewire() {

  // handle "sleeps"
  if( millis() < nextOneWireNextTriggerTime ) {
    return; 
  }

  switch(oneWireConversationState) {
    case SEARCH:
      if ( !ds.search(oneWireAddr) )  {
        ds.reset_search();
        oneWireSleep(250);
        return;
      }
      oneWireConversationState = INIT;
      return;

    case INIT:
      ds.reset();
      ds.select(oneWireAddr);
      ds.write(0x44, 1);        // start conversion, with parasite power on at the end
      oneWireConversationState = READ;
      oneWireSleep(1000);
      return;

    case READ:
      byte type_s;
      byte present = ds.reset();
      ds.select(oneWireAddr);    
      ds.write(0xBE);                     // Read Scratchpad
      for ( int i = 0; i < 9; i++) {          // we need 9 bytes
          oneWireData[i] = ds.read();
      }      
      if ( OneWire::crc8(oneWireData, 8) == oneWireData[8] ) {
        switch (oneWireAddr[0]) {
          case 0x10:  // DS18S20/DS1820
            type_s = 1;
            break;
          case 0x28:  // DS18B20
            type_s = 0;
            break;
          case 0x22:  // DS1822
            type_s = 0;
            break;
          default:
            dbg("Unknown 1wire device");
            return;
        } 
      
        int16_t raw = (oneWireData[1] << 8) | oneWireData[0];
        if (type_s) {
          raw = raw << 3; // 9 bit resolution default
          if (oneWireData[7] == 0x10) {
            // "count remain" gives full 12 bit resolution
            raw = (raw & 0xFFF0) + 12 - oneWireData[6];
          }
        } else {
          byte cfg = (oneWireData[4] & 0x60);
          // at lower res, the low bits are undefined, so let's zero them
          if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
          else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
          else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
          //// default is 12 bit resolution, 750 ms conversion time
        }
        float celsius = (float)raw / 16.0;
        sendUDP("rail" + boardAddressStr + " 1wire " + oneWireAddressToString(oneWireAddr) + " " + String(celsius, 2));
      }
      oneWireConversationState = SEARCH;
      oneWireSleep(1000);
      return;
  }
  
}


void readInputs() {

  for(int i = 0; i < numOfInputs; i++) {
    int pin = inputPins[i];
    int value = digitalRead(pin);
    int oldValue = inputStatus[i];
    inputStatus[i] = value;

    // note values are inverted due to pullup
    if(value < oldValue) {
      sendInputOn(i+1);
    }
    if(value > oldValue) {
      sendInputOff(i+1);
    }
  }
}

void sendInputOn(int input) {
  sendUDP("rail" + boardAddressStr + " i" + String(input, DEC) + " 1");
}

void sendInputOff(int input) {
  sendUDP("rail" + boardAddressStr + " i" + String(input, DEC) + " 0");
}

void sendUDP(String message) {
    udpSend.beginPacket(sendIpAddress, loxonePort);
    message.toCharArray(outputPacketBuffer, outputPacketBufferSize);
    udpSend.write(outputPacketBuffer, message.length());
    udpSend.endPacket();
    dbg(message);
}

void setRelay(int relay, int value) {
  if(relay > numOfRelays) {
    return;
  }
  dbg("Writing to relay " + String(relay) + " value " + String(value));
  digitalWrite(relayPins[relay], value);
}

String receivePacket() {
  int packetSize = udpRecv.parsePacket();
  if (packetSize) {
    memset(inputPacketBuffer, 0, sizeof(inputPacketBuffer));
    udpRecv.read(inputPacketBuffer, inputPacketBufferSize);
    String cmd = String(inputPacketBuffer);
    dbg(cmd);
    return cmd;
  }
  return "";
}

void processCommands() {
    String cmd = receivePacket();
    if(cmd != "") {
      for(int i = 0; i < numOfRelays; i++) {
        if(cmd == relayOnCommands[i]) {
          setRelay(i, 1);
        } else if(cmd == relayOffCommands[i]) {
          setRelay(i, 0);
        }
      }
    }
  }


