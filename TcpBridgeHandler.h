#pragma once
#if HAS_TCP_BRIDGE
#include "configuration.h"
#include "main.h"
#include "concurrency/OSThread.h"
#include "mesh/Router.h"

#if HAS_ETHERNET && defined(ARCH_ESP32)
#include <ETH.h>
#endif // HAS_ETHERNET

#include <WiFi.h>

#ifndef TCP_BRIDGE_DEFAULT_PORT
#define TCP_BRIDGE_DEFAULT_PORT 4404
#endif

#ifndef TCP_BRIDGE_MAX_CLIENTS
#define TCP_BRIDGE_MAX_CLIENTS 4
#endif

// [len:4 BE][MeshPacket protobuf] -- TCP has no datagram boundaries so frames
// need an explicit length prefix.
#define TCP_BRIDGE_MAX_FRAME_SIZE meshtastic_MeshPacket_size

// TCP counterpart to UdpMulticastHandler. Same hooks (Router::send for TX,
// router->enqueueReceivedMessage() for RX), same isFromUs()/encrypted checks.
class TcpBridgeHandler final : private concurrency::OSThread
{
  public:
    TcpBridgeHandler()
        : concurrency::OSThread("TcpBridge"), isRunning(false), port(TCP_BRIDGE_DEFAULT_PORT), server(nullptr)
    {
        for (int i = 0; i < TCP_BRIDGE_MAX_CLIENTS; i++) {
            clients[i] = nullptr;
        }
    }

    void start(int listenPort = TCP_BRIDGE_DEFAULT_PORT)
    {
        if (isRunning) {
            LOG_DEBUG("TCP bridge already running");
            return;
        }
        port = listenPort;
        server = new WiFiServer(port);
        server->begin();
        for (int i = 0; i < TCP_BRIDGE_MAX_CLIENTS; i++) {
            rxLen[i] = 0;
        }
        isRunning = true;
        LOG_INFO("TCP bridge listening on port %d", port);
    }

    void stop()
    {
        if (!isRunning)
            return;
        for (int i = 0; i < TCP_BRIDGE_MAX_CLIENTS; i++) {
            freeClient(i);
        }
        if (server) {
            server->end();
            delete server;
            server = nullptr;
        }
        isRunning = false;
        LOG_DEBUG("Stopping TCP bridge");
    }

    // Called from Router::send(), same spot as mqtt->onSend()/udpHandler->onSend().
    bool onSend(const meshtastic_MeshPacket *mp)
    {
        if (!isRunning || !mp)
            return false;
        if (WiFi.status() != WL_CONNECTED)
            return false;

        uint8_t payload[TCP_BRIDGE_MAX_FRAME_SIZE];
        size_t payloadLen = pb_encode_to_bytes(payload, sizeof(payload), &meshtastic_MeshPacket_msg, mp);
        if (payloadLen == 0)
            return false;

        uint8_t header[4] = {
            (uint8_t)((payloadLen >> 24) & 0xFF),
            (uint8_t)((payloadLen >> 16) & 0xFF),
            (uint8_t)((payloadLen >> 8) & 0xFF),
            (uint8_t)(payloadLen & 0xFF),
        };

        bool wroteAny = false;
        for (int i = 0; i < TCP_BRIDGE_MAX_CLIENTS; i++) {
            if (!clients[i] || !clients[i]->connected())
                continue;
            clients[i]->write(header, sizeof(header));
            clients[i]->write(payload, payloadLen);
            wroteAny = true;
        }
        return wroteAny;
    }

  protected:
    int32_t runOnce() override
    {
        if (!isRunning || !server)
            return 100;

        acceptNewClients();

        for (int i = 0; i < TCP_BRIDGE_MAX_CLIENTS; i++) {
            if (!clients[i])
                continue;
            if (!clients[i]->connected()) {
                freeClient(i);
                continue;
            }
            pumpClientInput(i, *clients[i]);
        }
        return 20;
    }

  private:
    void freeClient(int i)
    {
        if (!clients[i])
            return;
        clients[i]->stop();
        delete clients[i];
        clients[i] = nullptr;
        rxLen[i] = 0;
    }

    // A WiFiClient copy-constructed from a named local still closes the
    // underlying socket when that local's destructor runs at the end of this
    // function, even though the heap copy looks "connected" right after
    // accept -- the socket is dead by the time the next poll tries to read
    // it. Constructing the heap object directly from server->available()'s
    // return value keeps this to a single construction (guaranteed copy
    // elision), so no intermediate object's destructor ever runs.
    void acceptNewClients()
    {
        if (!server->hasClient())
            return;
        for (int i = 0; i < TCP_BRIDGE_MAX_CLIENTS; i++) {
            if (clients[i])
                continue;
            clients[i] = new WiFiClient(server->available());
            if (!*clients[i]) {
                delete clients[i];
                clients[i] = nullptr;
                return;
            }
            rxLen[i] = 0;
            LOG_DEBUG("TCP bridge client %d connected", i);
            return;
        }
        LOG_DEBUG("TCP bridge: max clients reached, rejecting connection");
        server->available().stop();
    }

    // Fills rxBuf[i] until a full frame is in, then dispatches it. Per-client
    // state so a frame split across reads still assembles.
    //
    // The frame-complete check has to run in the same pass that reads the
    // last needed byte, not at the top of a fresh while-iteration -- if the
    // whole frame arrives in a single TCP segment, available() drops to 0
    // the instant the last byte is consumed, so a check gated behind
    // "while (client.available() > 0)" never runs again and the fully
    // buffered frame sits there forever.
    void pumpClientInput(int i, WiFiClient &client)
    {
        while (client.available() > 0) {
            int b = client.read();
            if (b < 0)
                return;
            rxBuf[i][rxLen[i]++] = (uint8_t)b;

            if (rxLen[i] < 4)
                continue;

            uint32_t frameLen = ((uint32_t)rxBuf[i][0] << 24) | ((uint32_t)rxBuf[i][1] << 16) |
                                 ((uint32_t)rxBuf[i][2] << 8) | (uint32_t)rxBuf[i][3];
            if (frameLen > TCP_BRIDGE_MAX_FRAME_SIZE) {
                LOG_WARN("TCP bridge client %d sent oversized frame (%u), disconnecting", i, frameLen);
                client.stop();
                rxLen[i] = 0;
                return;
            }

            size_t haveBody = rxLen[i] - 4;
            if (haveBody < frameLen)
                continue;

            handleFrame(i, rxBuf[i] + 4, frameLen);
            rxLen[i] = 0;
        }
    }

    void handleFrame(int clientIdx, const uint8_t *frameBytes, size_t frameLen)
    {
        meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
        if (!pb_decode_from_bytes(frameBytes, frameLen, &meshtastic_MeshPacket_msg, &mp)) {
            LOG_WARN("TCP bridge client %d sent an undecodable MeshPacket, dropping", clientIdx);
            return;
        }
        if (mp.which_payload_variant != meshtastic_MeshPacket_encrypted_tag) {
            LOG_WARN("TCP bridge client %d sent a non-encrypted MeshPacket, dropping", clientIdx);
            return;
        }
        // matches UdpMulticastHandler::onReceive()
        if (isFromUs(&mp)) {
            LOG_WARN("TCP bridge client %d sent a packet spoofing our own from=0x%08x, dropping", clientIdx, mp.from);
            return;
        }

        if (!router)
            return;

        mp.transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MULTICAST_UDP;
        UniquePacketPoolPacket p = packetPool.allocUniqueCopy(mp);
        p->rx_snr = 0;
        p->rx_rssi = 0;
        router->enqueueReceivedMessage(p.release());
    }

    bool isRunning;
    int port;
    WiFiServer *server;
    WiFiClient *clients[TCP_BRIDGE_MAX_CLIENTS];
    uint8_t rxBuf[TCP_BRIDGE_MAX_CLIENTS][4 + TCP_BRIDGE_MAX_FRAME_SIZE];
    size_t rxLen[TCP_BRIDGE_MAX_CLIENTS];
};
#endif // HAS_TCP_BRIDGE
