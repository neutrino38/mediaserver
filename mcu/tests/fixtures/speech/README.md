# Rejeux Mistral du lot 0

Ces fichiers sont les réponses brutes de Mistral AI, enregistrées le
2026-10-13 par la sonde du lot 0. La SPEC les décrit au §4.2
(`docs/conception/SPEECH-SERVER/SPEC.md`). Ils font foi pour le faux Mistral
des tests des lots 2 à 4.

Anonymisation : les `request_id` sont remplacés par des zéros. Aucune clé
n'y figure. Les `id` de `voices_all.json` sont ceux des voix publiques de
Mistral.

| Fichier | Contenu |
|---|---|
| `asr_binary_d<délai>_l<langue>_s<silence>.jsonl` | ASR Voxtral Realtime, audio en trames binaires. Une ligne par événement, avec son instant `t` en secondes et sa nature (`kind` : `tx`, `rx`, `open`…). `d` est le `target_streaming_delay_ms`, `l` la langue forcée. `s` est l'attente muette après l'audio, en secondes : à 0, la sonde envoie `input_audio.flush` puis `input_audio.end` ; sinon, elle n'envoie plus rien |
| `asr_json_*.jsonl` | même chose, audio en `input_audio.append` base64 |
| `tts16k.wav` | l'audio envoyé à l'ASR : la phrase de test dite par `en_paul_neutral`, 16 kHz mono |
| `tts.jsonl` | TTS, mesures de chaque essai |
| `tts_stream_raw.bin` | TTS en flux : le corps SSE brut, octet pour octet |
| `tts_stream_chunks.json` | les morceaux reçus de ce corps : instant, longueur, début du contenu |
| `comp.jsonl`, `comp2.jsonl` | COMP : requête, réponse, latence et tokens facturés. `comp.jsonl` demande des positions (abandonné), `comp2.jsonl` des mots |
| `voices_all.json` | `GET /v1/audio/voices` : les dix voix fournies |
