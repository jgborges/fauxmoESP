/*

FAUXMO ESP 2.0.0

Copyright (C) 2016 by Xose Pérez <xose dot perez at gmail dot com>

The MIT License (MIT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "fauxmoESP.h"

void fauxmoESP::_sendUDPResponse() {

    --_roundsLeft;
    DEBUG_MSG_FAUXMO("[FAUXMO] UDP Responses round #%d\n", MAX_DISCOVERY_ROUNDS - _roundsLeft);

    unsigned int sent = 0;
    char response[strlen(UDP_TEMPLATE) + 40];

    #if COMPATIBLE_2_3_0
        WiFiUDP udpClient;
    #else
        AsyncUDP udpClient;
    #endif

    for (unsigned int i = 0; i < _devices.size(); i++) {

        fauxmoesp_device_t device = _devices[i];
        if (device.hit) continue;

        sprintf_P(response, UDP_TEMPLATE,
            WiFi.localIP().toString().c_str(),
            _base_port + i, device.uuid, device.uuid
        );

        DEBUG_MSG_FAUXMO("[FAUXMO] UDP Response from device #%d (%s)\n", i, device.name);

        #if COMPATIBLE_2_3_0
            udpClient.beginPacket(_remoteIP, _remotePort);
            udpClient.write(response);
            udpClient.endPacket();
        #else
            if (udpClient.connect(_remoteIP, _remotePort)) {
                udpClient.print(response);
            }
        #endif

        ++sent;

    }

    if (sent == 0) {
        DEBUG_MSG_FAUXMO("[FAUXMO] Nothing to do...\n");
        _roundsLeft = 0;
    }

}

void fauxmoESP::_handleUDPPacket(IPAddress remoteIP, unsigned int remotePort, uint8_t *data, size_t len) {

    if (!_enabled) return;

    //DEBUG_MSG_FAUXMO("[FAUXMO] Got UDP packet from %s\n", remoteIP.toString().c_str());

    data[len] = 0;
    String content = String((char *) data);

    if (content.indexOf(UDP_SEARCH_PATTERN) == 0) {
        if (content.indexOf(UDP_DEVICE_PATTERN) > 0) {

            DEBUG_MSG_FAUXMO("[FAUXMO] Search request from %s\n", remoteIP.toString().c_str());

            // Set hits to false
            for (unsigned int i = 0; i < _devices.size(); i++) {
                _devices[i].hit = false;
            }

            // Send responses
            _remoteIP = remoteIP;
            _remotePort = remotePort;
            _roundsLeft = MAX_DISCOVERY_ROUNDS;

            // We are throwing here the first UDP responses
            // Calling the handle() method (optional) will retry several times
            // until all devices have been requested back
            _lastTick = millis();
            _sendUDPResponse();

        }
    }

}

void fauxmoESP::_handleSetup(AsyncWebServerRequest *request, unsigned int device_id) {

    if (!_enabled) return;

    DEBUG_MSG_FAUXMO("[FAUXMO] Device #%d /setup.xml\n", device_id);
    _devices[device_id].hit = true;

    fauxmoesp_device_t device = _devices[device_id];
    char response[strlen(SETUP_TEMPLATE) + 50];
    sprintf_P(response, SETUP_TEMPLATE, device.name, device.uuid);
    request->send(200, "text/xml", response);

}

void fauxmoESP::_handleContent(AsyncWebServerRequest *request, unsigned int device_id, String content) {

    if (!_enabled) return;

    DEBUG_MSG_FAUXMO("[FAUXMO] Device #%d /upnp/control/basicevent1\n", device_id);
    fauxmoesp_device_t device = _devices[device_id];

    if (content.indexOf("<BinaryState>0</BinaryState>") > 0) {
        if (_callback) _callback(device_id, device.name, false);
    }

    if (content.indexOf("<BinaryState>1</BinaryState>") > 0) {
        if (_callback) _callback(device_id, device.name, true);
    }

    request->send(200);

}

void fauxmoESP::addDevice(const char * device_name) {

    fauxmoesp_device_t new_device;
    unsigned int device_id = _devices.size();

    // Copy name
    new_device.name = strdup(device_name);

    // Create UUID
    char uuid[15];
    sprintf(uuid, "444556%06X%02X\0", ESP.getChipId(), device_id); // "DEV" + CHIPID + DEV_ID
    new_device.uuid = strdup(uuid);

    // TCP Server
    new_device.server = new AsyncWebServer(_base_port + device_id);
    new_device.server->on("/setup.xml", HTTP_GET, [this, device_id](AsyncWebServerRequest *request){
        _handleSetup(request, device_id);
    });
    new_device.server->onRequestBody([this, device_id](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        char * tmp = (char *) data;
        tmp[len] = 0;
        String content = String(tmp);
        _handleContent(request, device_id, content);
    });

    new_device.server->begin();

    // Attach
    _devices.push_back(new_device);

}

void fauxmoESP::handle() {

    #if COMPATIBLE_2_3_0
    int len = _udp.parsePacket();
    if (len > 0) {
        IPAddress remoteIP = _udp.remoteIP();
        unsigned int remotePort = _udp.remotePort();
        uint8_t data[len];
        _udp.read(data, len);
        _handleUDPPacket(remoteIP, remotePort, data, len);
    }
    #endif

    if (_roundsLeft > 0) {
        if (millis() - _lastTick > UDP_RESPONSES_INTERVAL) {
            _lastTick = millis();
            _sendUDPResponse();
        }
    }

}

fauxmoESP::fauxmoESP(unsigned int port) {

    _base_port = port;

    // UDP Server
    #if COMPATIBLE_2_3_0
        _udp.beginMulticast(WiFi.localIP(), UDP_MULTICAST_IP, UDP_MULTICAST_PORT);
    #else
        if (_udp.listenMulticast(UDP_MULTICAST_IP, UDP_MULTICAST_PORT)) {
            _udp.onPacket([this](AsyncUDPPacket packet) {
                _handleUDPPacket(packet.remoteIP(), packet.remotePort(), packet.data(), packet.length());
            });
        }
    #endif
}
