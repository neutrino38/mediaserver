# Medooze mediaserver 1.19.1

Un hôte double pile annonce ses deux adresses publiques. speech-server passe sur les modèles Voxtral 3 de Mistral et impose la langue à la reconnaissance vocale

La version publiée est la **release 2** (`1.19.1-2`). Elle apporte le passage à Voxtral 3.

## Mise à jour : ce qui change pour l'exploitant

- **`--public-ip` se répète une fois par famille**, comme `--internal-ip` : une adresse IPv4 et une adresse IPv6. Exemple : `--public-ip 203.0.113.10 --public-ip 2001:db8::10`. Avant, l'option ne gardait que sa dernière valeur. Un hôte double pile perdait donc son profil IPv6 public sans aucun message
- **Mettre à jour `speech-config.json` à la main.** Le fichier est en `%config(noreplace)` : une mise à jour du paquet garde le fichier en place, avec les anciens modèles. Remplacer dans `/etc/mediaserver/speech-config.json` :
  - `voxtral-mini-tts-2603` par `voxtral-mini-tts-3-0`
  - `voxtral-mini-transcribe-realtime-2602` par `voxtral-mini-transcribe-realtime-3-0`

  puis lancer `systemctl reload speech-server`. Les anciens modèles répondent encore, mais sans les apports ci-dessous
- **La reconnaissance vocale impose la langue.** Le paramètre `lang` du flux `asr` vaut `fr` par défaut. Il est maintenant transmis à Mistral. Avant, la langue était détectée seule. Un appelant qui parle une autre langue sur un flux ouvert sans `lang` est donc transcrit comme du français

## speech-server sur Voxtral 3

- Modèles par défaut : `voxtral-mini-tts-3-0` (synthèse) et `voxtral-mini-transcribe-realtime-3-0` (reconnaissance)
- `asr` : la langue demandée (`lang`) est imposée à Mistral. Elle doit tenir en deux lettres (`fr`, `en`). Un code de trois lettres est refusé avant l'ouverture du flux (400). Le message `ready` porte `lang` à la place de `langIgnored`
- `asr` : le paramètre `delay` accepte de 80 à 1200 ms, ou 2400 exactement. Ce sont les seules valeurs que Voxtral 3 accepte. Une autre valeur, par exemple 1500, fermait la session chez Mistral. Elle est maintenant refusée avant l'ouverture du flux (400). Le défaut reste 1000 ms
- `tts` : la voix part dans le champ `voice_id`, le seul que Mistral documente. Les voix clonées gardent leur identifiant
- Le format audio ne change pas : Voxtral TTS 3 rend toujours du PCM float32 à 24 kHz, converti en 16 bits à 16 kHz

Documentation exploitant mise à jour : `docs/SPEECH-SERVER.md`.

## Documentation

- L'hôte de l'URL WebSocket rendue par `ConfigureParticipantMediaConnection` est `--websocket-host`, sinon l'adresse annoncée du profil par défaut (`--default-profile`)

# Limitations

- Le TTS temps réel de Mistral (`voxtral-mini-tts-realtime-3-0`, texte envoyé au fil de la frappe) n'est pas utilisé. Il n'est pas ouvert à notre compte, et son protocole n'est pas publié. Le texte reste donc découpé en phrases avant la synthèse
- Voxtral 3 apporte la détection de fin de tour (`turn_detection`), quatre voix françaises prédéfinies et un réglage d'expressivité. speech-server ne s'en sert pas encore
- La qualité de Voxtral 3 n'a pas été comparée à celle de Voxtral 2 en appel réel
