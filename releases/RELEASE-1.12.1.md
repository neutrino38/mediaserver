# Medooze mediaserver 1.12.1

This release makes the media server authoritative for codec negotiation, adds media supervision, and repairs real-time text over WebSocket

- Codec negotiation is delegated to the media server on both control APIs: `StartReceiving` (conference) and `EndpointStartReceiving` (JSR-309) now take the SDP offer's fmtp per payload type and return the accepted payload types with the exact fmtp to advertise
- H.264 negotiation follows RFC 6184 §8.2.2 and resolves per payload type — a browser offer enumerating H.264 under several payload types no longer collapses to a single answer
- Opus fmtp (RFC 7587 §7) and AV1 fmtp are ingested; AV1 emission is clamped to the level declared by the peer
- The negotiated bounds (profile, packetization-mode, bitrate) now reach the emitting encoder, and a live encoder is restarted when they change
- RTP inactivity watchdog on the conference API (`StartRTPTimeout`), with two new events: ParticipantMediaTimeout and ParticipantMediaConnected (the latter also proving the DTLS handshake completed on a secure leg)
- Real-time text (T.140) over WebSocket is reachable and lossless again: WebSocket URL parsing fixed, `ConfigureMediaConnection` no longer reports a fault on success, the announced scheme follows the TLS setting (ws/wss), and text arriving before the browser connects is queued and replayed
- The composite is letterboxed to the encoder's resolution instead of being stretched
- Transcoder attach/detach XML-RPC calls no longer report an error on success
- Control API documentation updated (MCU-API.md, xmlrpc_jsr309_api.md)
