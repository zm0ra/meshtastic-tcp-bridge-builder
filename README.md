# meshtastic-tcp-bridge-builder

Patch set for `meshtastic/firmware` that adds a raw TCP bridge on port 4404,
alongside the existing UDP multicast bridge. Mirrors outgoing traffic to
connected TCP clients and accepts framed packets back for injection into
the mesh.

Same idea as [meshcore-xiao-wifi-serial2tcp](https://github.com/zm0ra/meshcore-xiao-wifi-serial2tcp):
don't fork, patch upstream and build with PlatformIO.

## Why not just use port 4403

4403 is the phone/admin API. A connected client can only send as its own
node identity (`MeshService.cpp` zeroes `from` on anything coming through
that path). 4404 behaves like the UDP multicast transport instead: it
accepts arbitrary `from`, same as UDP already does today, gated by the same
two checks (`isFromUs`, valid encrypted payload).

## Build

```bash
./build.sh --env heltec-v3 --build
./build.sh --env seeed-xiao-s3 --build
./build.sh --env heltec-v3 --build --upload
```

Built and flashable on both `heltec-v3` and `seeed-xiao-s3` against current
`develop`.

## Wire format

```
[len:4 bytes, big-endian][MeshPacket protobuf]
```

Same `meshtastic_MeshPacket` schema as 4403/UDP, nanopb on both ends.

## Files

- `TcpBridgeHandler.h` -- the bridge itself
- `patches/` -- diffs against `src/main.cpp`, `src/main.h`, `src/mesh/Router.cpp`,
  `src/mesh/wifi/WiFiAPClient.cpp`, `variants/esp32/esp32-common.ini`
- `build.sh` -- clone, patch, build, upload
