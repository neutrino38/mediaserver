# Ordre des verrous du chemin média JSR-309

Ce document dit dans quel ordre les verrous du chemin média JSR-309 doivent
être pris. Un ordre inversé, même sur un chemin rare, est un interblocage.

## Les acteurs

Trois threads touchent aux mêmes objets.

| Thread | Ce qu'il fait | Verrou qu'il prend en premier |
|---|---|---|
| Démultiplexage (`RTPEndpoint::MultiplexLoop`) | livre les paquets aux écouteurs | `RTPMultiplexer::mutex` du port source |
| XML-RPC | `Attach`, `Dettach`, `SetCodec`, création et destruction d'objets | `MediaSession::mutex` |
| Balayeur de files d'événements | détruit les sessions dont la file n'est plus lue | `JSR309Manager::mutex` |

## L'ordre

Du plus extérieur au plus intérieur :

```
JSR309Manager::mutex
  → MediaSession::mutex
      → RTPMultiplexer::mutex   (le port : source ou puits)
          → verrous d'émission de RTPSession
```

Et, en dehors de cette chaîne :

```
MediaSession::eventContextsMutex
```

`eventContextsMutex` est un verrou **feuille** : il ne protège que la table des
contextes d'événement, et rien n'est appelé pendant qu'il est tenu. On peut donc
le prendre à tout moment, y compris sous `MediaSession::mutex` ou sous le verrou
d'un port. L'inverse est interdit : ne jamais prendre un autre verrou en le
tenant.

## Pourquoi ce verrou séparé

Un écouteur peut demander une intra à sa source depuis le chemin des paquets :
`VideoTranscoder::RequestSourceFPU`, ou le décodeur vidéo sur une perte. Il est
alors **sous le verrou du port source**.

Si la source est un `RTPEndpoint` dont la propriété `useExtFIR` est vraie, cette
demande publie un événement :

```
Port(source).mutex  →  Joinable::Update()  →  PostEvent
                    →  JSR309Manager::PostEvent  →  résolution du contexte
```

Pendant ce temps, le thread XML-RPC exécute un `Dettach` :

```
MediaSession::mutex  →  Port::Detach()  →  RemoveListener  →  Port(source).mutex
```

Les deux ordres sont opposés. Si la résolution du contexte prenait
`MediaSession::mutex`, le cycle serait fermé et les deux threads bloqueraient.
C'est pourquoi `MediaSession::GetEventContext` prend `eventContextsMutex`, et
lui seul.

Test de non-régression : `mcu/tests/test_jsr309_event_deadlock.cpp`. Il
reproduit les deux threads et **tue le processus** si l'un des deux ne rend pas
la main en 5 s.

## Ce que le verrou du port garantit

`RTPMultiplexer::Multiplex` tient son verrou pendant **tout** l'appel aux
écouteurs. `RemoveListener` prend le même verrou. Donc, quand `RemoveListener`
rend la main, plus aucun `onRTPPacket` de cette source n'est en vol.

C'est une **barrière**, et le code s'appuie dessus. Règle qui en découle :
retirer l'écouteur de la source **avant** de détruire ce dont il se sert (un
codec, par exemple), jamais après.
