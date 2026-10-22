# Medooze mediaserver 1.12.2

This release brings real-time text over WebSocket to the conference API and extends the player's transcoding to Opus

- Real-time text (T.140) over WebSocket for a conference participant: `ConfigureParticipantMediaConnection` (conference API) switches a participant's text leg from RTP to WebSocket and returns the full URL `ws(s)://host:port/mcu/<confId>/<token>` in one call — scheme follows the server's TLS setting, host from `--websocket-host` else `--public-ip`
- The bridge sits at the text-mixer seam and carries WSEndpoint's policies: bounded pending buffer (32 frames / 5 s) replayed on connect, U+FFFD toward the surviving side on close, lone-BOM keepalives filtered, reconnection on the same token replaces the previous socket
- `StartSending`/`StartReceiving`(TEXT) are refused while the participant's text is on WebSocket
- Played files transcoded toward the peer can now target Opus, honouring the negotiated bounds (`useinbandfec`, `maxaveragebitrate`…) instead of encoder defaults; the bounds are re-applied on rewind
- Control API documentation updated (MCU-API.md)
