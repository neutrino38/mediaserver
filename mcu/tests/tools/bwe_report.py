#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Depouillement des traces BWE du mediaserver (lot 3 du chantier de controle de debit).

Lit un /var/log/mcu.log produit avec les traces de debug actives (option -d),
en extrait les series du controle de debit, les recoupe avec le journal de
marqueurs ecrit par netem_scenario.sh, et rend :

  - bwe.csv      : la serie d'estimation, une ligne par trace "BWE: estimation"
  - events.csv   : detections, feedback emis, changements d'etat, pertes, RTT
  - bwe.svg      : un graphe par patte (estimation vs debit entrant + marqueurs)
  - un verdict par critere d'acceptation du lot 3, sur la sortie standard
  - avec --markdown, le bloc pret a coller en annexe D de rate-control.md

Bibliotheque standard uniquement (python >= 3.6) : la machine de mesure n'a ni
matplotlib ni gnuplot, et on ne veut rien y installer.

Usage :
    ./bwe_report.py /var/log/mcu.log --markers scenario-escalier.tsv --out ./depouillement
    ./bwe_report.py mcu.log --stream 'sip:alice@ives.fr' --markdown
"""

import argparse
import csv
import math
import os
import re
import sys
from datetime import datetime

# Bornes de l'estimateur, cf. rate_control_plan.md lot 1.3 (kb/s).
DEFAULT_MAX_KBPS = 30000
DEFAULT_MIN_KBPS = 16

# Criteres d'acceptation, lot 3 du plan. Ceux que le plan ne chiffre pas
# ("pas d'oscillation entretenue") portent ici une valeur par defaut explicite,
# revisable en annexe D : mieux vaut un seuil discutable qu'un jugement au doigt.
SETTLE_S = 15.0          # temps laisse a la boucle avant de juger un regime etabli
TOLERANCE = 0.25         # +/- 25 % du debit effectif en regime etabli
REACTION_MAX_S = 3.0     # reaction a une marche descendante
RECOVERY_RATIO = 0.80    # part du lien a retrouver apres une marche montante
RECOVERY_MAX_S = 30.0    # ... et en combien de temps
FLIPS_PER_MIN_MAX = 6.0  # bascules Increase<->Decrease tolerees en regime etabli
FLIPS_MIN_WINDOW_S = 30.0  # en deca, un taux par minute n'est qu'une extrapolation
COV_MAX = 0.20           # coefficient de variation tolere en regime etabli
CLIP_MAX_S = 5.0         # temps cumule tolere en ecretage au plafond
STUCK_MAX_S = 30.0       # duree au-dela de laquelle une hypothese est dite gelee
LONG_RUN_S = 600.0       # les "10 minutes" du critere de stabilite

# ---------------------------------------------------------------------------
# Lecture du journal
# ---------------------------------------------------------------------------

# [0x7f8e4b7fe700][1755292800.123][DBG]...   (cf. mcu/include/log.h)
RE_DBG = re.compile(r'^\[0x(?P<tid>[0-9a-fA-F]+)\]\[(?P<ts>\d+\.\d+)\]\[DBG\](?P<body>.*)$')
# [0x7f8e4b7fe700][2026-08-17T14:23:45.123][LOG]...
RE_LOG = re.compile(r'^\[0x(?P<tid>[0-9a-fA-F]+)\]\[(?P<ts>\d{4}-\d{2}-\d{2}T[\d:.]+)\]\[LOG\](?P<body>.*)$')

# "stream=" est optionnel : un journal capture avant son ajout reste lisible.
RE_ESTIM = re.compile(
    r'^BWE: estimation (?:stream=(?P<stream>.*?) )?state=(?P<state>\S+) region=(?P<region>\S+) '
    r'usage=(?P<usage>\S+) currentBitRate=(?P<cur>\S+) current=(?P<raw>\S+) '
    r'incoming=(?P<inc>\S+) min=(?P<min>\S+) max=(?P<max>\S+)')
RE_OVERUSE = re.compile(
    r'^BWE:\s+Overusing bitrate:(?P<bitrate>\S+) max:\S+ min:\S+ T:(?P<t>\S+),threshold:(?P<th>\S+)')
RE_CANDIDATE = re.compile(r'^BWE:\s+Overusing candidate (?P<n>\d+)/3 ')
RE_USAGE = re.compile(r'^BWE:\s+(?P<usage>Normal|UnderUsing)\s+bitrate:(?P<bitrate>\S+)')
RE_STATE = re.compile(r'^BWE: ChangeState from:(?P<src>\S+) to:(?P<dst>\S+)')
RE_REGION = re.compile(r'^BWE: Change region to:(?P<region>\S+)')
RE_RATE = re.compile(r'^BWE: (?P<dir>Increase|Decrease) rate to current = (?P<kbps>\d+) kbps')
RE_LOST = re.compile(
    r'^BWE: UpdateLost lost:(?P<lost>\d+) hi?pothesis:(?P<hyp>[^,]+),packets:(?P<packets>\S+),lost:(?P<lostf>\S+)')
RE_RTT = re.compile(r'^BWE: UpdateRTT rtt:(?P<rtt>\d+)ms hi?pothesis:(?P<hyp>\S+)')
RE_COV = re.compile(r'^BWE: covariance no longer positive semi-definite')
RE_REMB = re.compile(r'^-RTPSession::SendReceiverEstimatedMaxBitrate \[(?P<bps>-?\d+)\] on (?P<media>\S+) stream')
RE_TMMBR = re.compile(r'^-RTPSession::SendTempMaxMediaStreamBitrateRequest \[(?P<bps>-?\d+)\] on (?P<media>\S+) stream')
RE_TARGET = re.compile(
    r'^-RTPSession::onTargetBitrateRequested\(\) mode (?P<mode>-?\d+), bitrate \[(?P<in>-?\d+)\] -> '
    r'\[(?P<out>-?\d+)\] send (?P<send>-?\d+) for (?P<media>\S+) stream (?P<session>\S+?)\.')
RE_MAXLIMIT = re.compile(r'^-RemoteRateEstimator::SetTemporalMaxLimit\(\)\s*(?P<what>ignored |maximized )?(?P<kbps>\d+)')

FEEDBACK_KINDS = ('REMB', 'TMMBR')


def to_float(text):
    """Convertit une valeur imprimee par printf, y compris nan/-nan/inf."""
    try:
        return float(text)
    except (TypeError, ValueError):
        low = (text or '').strip().lower()
        if 'nan' in low:
            return float('nan')
        if 'inf' in low:
            return float('-inf') if low.startswith('-') else float('inf')
        return float('nan')


def iso_to_epoch(text):
    try:
        return datetime.strptime(text, '%Y-%m-%dT%H:%M:%S.%f').timestamp()
    except ValueError:
        return None


class Leg(object):
    """Une patte RTP : un thread de RTPSession, donc une serie d'estimation."""

    def __init__(self, tid):
        self.tid = tid
        self.stream = ''
        self.estim = []      # dicts : t, state, region, usage, kbps, raw, incoming, min, max
        self.events = []     # dicts : t, kind, detail, value

    @property
    def label(self):
        return self.stream if self.stream else ('tid 0x%s' % self.tid)


def parse_log(path, stream_filter=None):
    legs = {}
    nan_lines = 0
    parsed = 0

    def leg_of(tid):
        if tid not in legs:
            legs[tid] = Leg(tid)
        return legs[tid]

    with open(path, 'r', errors='replace') as handle:
        for line in handle:
            line = line.rstrip('\r\n')
            match = RE_DBG.match(line)
            if match:
                epoch = float(match.group('ts'))
            else:
                match = RE_LOG.match(line)
                if not match:
                    continue
                epoch = iso_to_epoch(match.group('ts'))
                if epoch is None:
                    continue
            tid = match.group('tid')
            body = match.group('body')
            if 'BWE:' not in body and 'RTPSession::' not in body and 'RemoteRateEstimator::' not in body:
                continue

            hit = RE_ESTIM.match(body)
            if hit:
                leg = leg_of(tid)
                name = hit.group('stream')
                if name and not leg.stream:
                    leg.stream = name
                values = dict(
                    t=epoch,
                    state=hit.group('state'),
                    region=hit.group('region'),
                    usage=hit.group('usage'),
                    kbps=to_float(hit.group('cur')),
                    raw=to_float(hit.group('raw')),
                    incoming=to_float(hit.group('inc')),
                    min=to_float(hit.group('min')),
                    max=to_float(hit.group('max')),
                )
                if any(math.isnan(values[k]) for k in ('kbps', 'raw', 'incoming')):
                    nan_lines += 1
                leg.estim.append(values)
                parsed += 1
                continue

            kind = detail = None
            value = None
            for regex, name, fields in (
                    (RE_CANDIDATE, 'candidate', ('n',)),
                    (RE_OVERUSE, 'detect', ('bitrate', 't', 'th')),
                    (RE_USAGE, 'detect', ('usage', 'bitrate')),
                    (RE_STATE, 'state', ('src', 'dst')),
                    (RE_REGION, 'region', ('region',)),
                    (RE_RATE, 'rate', ('dir', 'kbps')),
                    (RE_LOST, 'lost', ('lost', 'hyp', 'packets', 'lostf')),
                    (RE_RTT, 'rtt', ('rtt', 'hyp')),
                    (RE_COV, 'covariance', ()),
                    (RE_REMB, 'REMB', ('bps', 'media')),
                    (RE_TMMBR, 'TMMBR', ('bps', 'media')),
                    (RE_TARGET, 'target', ('mode', 'in', 'out', 'send', 'media')),
                    (RE_MAXLIMIT, 'maxlimit', ('what', 'kbps')),
            ):
                hit = regex.match(body)
                if not hit:
                    continue
                kind = name
                detail = ' '.join('%s=%s' % (f, hit.group(f)) for f in fields if hit.group(f) is not None)
                if name in FEEDBACK_KINDS:
                    value = to_float(hit.group('bps')) / 1000.0
                elif name == 'rate':
                    value = to_float(hit.group('kbps'))
                elif name == 'maxlimit':
                    value = to_float(hit.group('kbps')) / 1000.0
                break
            if kind is None:
                continue
            if 'nan' in (detail or '').lower():
                nan_lines += 1
            leg_of(tid).events.append(dict(t=epoch, kind=kind, detail=detail or '', value=value))
            parsed += 1

    ordered = sorted(legs.values(), key=lambda leg: (leg.stream, leg.tid))
    if stream_filter:
        ordered = [leg for leg in ordered if stream_filter in leg.label]
    # Une patte sans aucune estimation n'a rien a montrer (thread voisin qui a
    # seulement emis un feedback relaye, par exemple).
    ordered = [leg for leg in ordered if leg.estim or leg.events]
    return ordered, nan_lines, parsed


# ---------------------------------------------------------------------------
# Marqueurs de scenario
# ---------------------------------------------------------------------------

class Marker(object):
    def __init__(self, t, label, params):
        self.t = t
        self.label = label
        self.params = params

    @property
    def cap_kbps(self):
        if 'rate_kbps' in self.params:
            return to_float(self.params['rate_kbps'])
        return None

    def __str__(self):
        extra = ' '.join('%s=%s' % kv for kv in sorted(self.params.items()))
        return ('%s %s' % (self.label, extra)).strip()

    def short(self):
        """Etiquette courte pour le graphe : le parametre qui change, pas tout."""
        if self.label == 'cap' and self.cap_kbps is not None:
            return '%g kb/s' % self.cap_kbps
        if self.label == 'loss':
            return 'perte %s %%' % self.params.get('pct', '?')
        if self.label == 'jitter':
            return 'gigue %s/%s ms' % (self.params.get('delay_ms', '?'), self.params.get('jitter_ms', '?'))
        return self.label


def parse_markers(path):
    markers = []
    if not path:
        return markers
    with open(path, 'r', errors='replace') as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) < 2:
                parts = line.split(None, 2)
            if len(parts) < 2:
                continue
            try:
                epoch = float(parts[0])
            except ValueError:
                continue
            params = {}
            if len(parts) > 2:
                for token in parts[2].split():
                    if '=' in token:
                        key, val = token.split('=', 1)
                        params[key] = val
            markers.append(Marker(epoch, parts[1], params))
    markers.sort(key=lambda m: m.t)
    return markers


# ---------------------------------------------------------------------------
# Criteres
# ---------------------------------------------------------------------------

def median(values):
    clean = sorted(v for v in values if not math.isnan(v))
    if not clean:
        return float('nan')
    mid = len(clean) // 2
    if len(clean) % 2:
        return clean[mid]
    return (clean[mid - 1] + clean[mid]) / 2.0


def samples_between(leg, start, end):
    return [s for s in leg.estim if start <= s['t'] < end]


def fin_transitoire(samples):
    """Instant ou l'estimation cesse sa rampe monotone d'entree de phase.

    Une marche laisse l'estimation grimper (ou chuter) strictement de facon
    monotone pendant tout le transitoire : y calculer une dispersion mesure la
    pente, pas une oscillation. On coupe donc au premier renversement de pente.
    Rend None si la phase est monotone de bout en bout — il n'y a alors aucun
    regime etabli a juger.
    """
    sens = 0
    for prev, cur in zip(samples, samples[1:]):
        if math.isnan(prev['kbps']) or math.isnan(cur['kbps']):
            continue
        delta = cur['kbps'] - prev['kbps']
        if delta == 0:
            continue
        signe = 1 if delta > 0 else -1
        if sens == 0:
            sens = signe
        elif signe != sens:
            return cur['t']
    return None


def fenetre_etablie(leg, start, end, settle):
    """Fenetre sur laquelle un regime etabli se juge, et le motif de son choix.

    La garde reste le plancher ; le transitoire ne fait que le repousser. Le
    motif est rendu pour etre affiche : une fenetre choisie en silence rend le
    verdict illisible en annexe D.
    """
    garde = start + settle
    phase = samples_between(leg, start, end)
    if len(phase) < 3:
        return [], 'aucun echantillon dans la phase (mauvaise patte, ou journal decale ?)'
    stable = fin_transitoire(phase)
    if stable is None:
        return [], 'estimation monotone sur toute la phase'
    depart = max(garde, stable)
    window = samples_between(leg, depart, end)
    if len(window) < 3:
        window = samples_between(leg, garde, end)
        return window, 'garde %gs (transitoire trop long pour en ecarter plus)' % settle
    motif = 'garde %gs' % settle if depart == garde else 'transitoire ecarte jusqu a %+.1f s' % (depart - start)
    return window, motif


# Une file profonde se reconnait a son PLATEAU : le debit entrant reste plaque a
# l'ancien plafond apres le tc, le temps de la vidange. Un simple depassement de
# seuil ne suffit pas a la reconnaitre — avec une file courte, l'entrant part de
# plus bas et remonte franchement, et prendre son premier depassement pour une
# vidange retranche du chrono la rampe de la source, qui est du signal.
PLATEAU_BAS = 0.80       # bande autour de l'ancien plafond, ou l'entrant est plaque
PLATEAU_HAUT = 1.15      # ... et qu'il ne quitte pas tant que la file se vide
PLATEAU_BURST = 2.00     # une file vidée rend BEAUCOUP plus que le plafond ; en
                         # deca, l'entrant ne fait que monter, et c'est du signal
PLATEAU_MIN_S = 2.0      # duree minimale du plateau pour parler de vidange


def debut_libere(leg, marker_t, end, previous_cap):
    """Instant ou le lien relache devient observable, si une file l'a retarde.

    Le plateau est une BANDE autour de l'ancien plafond : on n'en sort par le
    haut qu'au burst de rattrapage, et le burst lui-meme ne doit pas compter
    comme du plateau. Rend None si l'entrant quitte la bande par le bas, ou tout
    de suite : il n'y a alors rien a retrancher et le chrono part du marqueur.
    """
    if not previous_cap:
        return None
    bas = previous_cap * PLATEAU_BAS
    haut = previous_cap * PLATEAU_HAUT
    burst = previous_cap * PLATEAU_BURST
    debut = None
    for sample in leg.estim:
        if sample['t'] < marker_t or sample['t'] >= end:
            continue
        inc = sample['incoming']
        if math.isnan(inc) or inc < bas:
            return None
        if inc > haut:
            # Sortie par le haut : rattrapage de file seulement si l'entrant
            # SAUTE loin au-dessus du plafond. Une sortie modeste est une source
            # qui remonte et traverse la bande — mesure du 2026-08-18 : 435 ->
            # 683 kb/s en 8 s pour un plafond de 500, pris a tort pour 8,6 s de
            # vidange, ce qui retranchait de la rampe legitime au chrono.
            if inc <= burst:
                return None
            if debut is None or sample['t'] - marker_t < PLATEAU_MIN_S:
                return None
            return sample['t']
        debut = sample['t'] if debut is None else debut
    return None


class Verdict(object):
    def __init__(self, status, title, detail):
        self.status = status  # 'OK', 'KO' ou '--' (non evaluable)
        self.title = title
        self.detail = detail

    def line(self):
        return '[%2s] %-46s %s' % (self.status, self.title, self.detail)


def judge_segments(leg, markers, args):
    """Marches d'escalier : regime etabli, reaction a la baisse, re-montee."""
    verdicts = []
    # Un palier se termine au marqueur SUIVANT, quel qu'il soit : dans le
    # scenario de pertes, les phases saines portent le debit du lien et une
    # phase degradee s'intercale entre deux d'entre elles — la fenetre de
    # jugement ne doit pas l'enjamber.
    caps = []
    for index, marker in enumerate(markers):
        if marker.cap_kbps is None:
            continue
        end = markers[index + 1].t if index + 1 < len(markers) else float('inf')
        caps.append((marker, end))
    for index, (marker, end) in enumerate(caps):
        cap = marker.cap_kbps
        window, motif = fenetre_etablie(leg, marker.t, end, args.settle)
        title = 'palier %g kb/s : regime etabli' % cap
        if len(window) < 3:
            verdicts.append(Verdict('--', title, '%s : pas de regime etabli a juger' % motif))
        else:
            est = median([s['kbps'] for s in window])
            inc = median([s['incoming'] for s in window])
            ecart = abs(est - cap) / cap if cap else float('nan')
            status = 'OK' if ecart <= TOLERANCE else 'KO'
            verdicts.append(Verdict(status, title,
                                    'estimation mediane %.0f kb/s (entrant %.0f) soit %+.0f %% du lien'
                                    ' [%s]'
                                    % (est, inc, 100.0 * (est - cap) / cap if cap else float('nan'), motif)))
            # Oscillation : bascules d'etat et dispersion sur la meme fenetre.
            flips = 0
            previous = None
            for sample in window:
                if sample['state'] in ('Increase', 'Decrease'):
                    if previous and sample['state'] != previous:
                        flips += 1
                    previous = sample['state']
            duree_min = max((window[-1]['t'] - window[0]['t']) / 60.0, 1e-9)
            taux = flips / duree_min
            valeurs = [s['kbps'] for s in window if not math.isnan(s['kbps'])]
            moyenne = sum(valeurs) / len(valeurs) if valeurs else float('nan')
            ecart_type = math.sqrt(sum((v - moyenne) ** 2 for v in valeurs) / len(valeurs)) if valeurs else float('nan')
            cov = ecart_type / moyenne if moyenne else float('nan')
            duree = window[-1]['t'] - window[0]['t']
            if duree < FLIPS_MIN_WINDOW_S:
                status = 'OK' if cov <= COV_MAX else 'KO'
                mesure = ('coef. variation %.2f (max %.2f) sur %.0f s seulement :'
                          ' %d bascule(s) non extrapolee(s) [%s]'
                          % (cov, COV_MAX, duree, flips, motif))
            else:
                status = 'OK' if (taux <= FLIPS_PER_MIN_MAX and cov <= COV_MAX) else 'KO'
                mesure = ('%.1f bascule/min (max %.0f), coef. variation %.2f (max %.2f)'
                          ' sur %.0f s [%s]'
                          % (taux, FLIPS_PER_MIN_MAX, cov, COV_MAX, duree, motif))
            verdicts.append(Verdict(status, 'palier %g kb/s : pas d oscillation' % cap, mesure))

        if index == 0:
            continue
        previous_cap = caps[index - 1][0].cap_kbps
        after = [s for s in leg.estim if marker.t <= s['t'] < end]
        if previous_cap is None or not after:
            continue
        if cap < previous_cap:
            cible = cap * (1.0 + TOLERANCE)
            atteint = next((s for s in after if s['kbps'] <= cible), None)
            title = 'marche descendante %g -> %g kb/s : reaction' % (previous_cap, cap)
            if atteint is None:
                verdicts.append(Verdict('KO', title, 'jamais redescendue sous %.0f kb/s' % cible))
            else:
                delai = atteint['t'] - marker.t
                verdicts.append(Verdict('OK' if delai <= REACTION_MAX_S else 'KO', title,
                                        '%.1f s pour passer sous %.0f kb/s (max %.0f s)'
                                        % (delai, cible, REACTION_MAX_S)))
        elif cap > previous_cap:
            cible = cap * RECOVERY_RATIO
            atteint = next((s for s in after if s['kbps'] >= cible), None)
            title = 'marche montante %g -> %g kb/s : re-montee' % (previous_cap, cap)
            if atteint is None:
                verdicts.append(Verdict('KO', title, 'jamais remontee a %.0f kb/s' % cible))
            else:
                libere = debut_libere(leg, marker.t, end, previous_cap)
                origine = libere if libere is not None else marker.t
                delai = atteint['t'] - origine
                if libere is None:
                    detail = ('%.1f s pour atteindre %.0f %% du lien (max %.0f s)'
                              ' [depuis le marqueur : vidange de file non observee]'
                              % (delai, 100 * RECOVERY_RATIO, RECOVERY_MAX_S))
                else:
                    detail = ('%.1f s pour atteindre %.0f %% du lien (max %.0f s)'
                              ' [lien libere a +%.1f s du marqueur, %.1f s bruts]'
                              % (delai, 100 * RECOVERY_RATIO, RECOVERY_MAX_S,
                                 libere - marker.t, atteint['t'] - marker.t))
                verdicts.append(Verdict('OK' if delai <= RECOVERY_MAX_S else 'KO', title, detail))
    return verdicts


def judge_impairments(leg, markers, args):
    """Pertes et gigue : on rapporte, on ne prononce que l'evident."""
    verdicts = []
    reference = None
    for index, marker in enumerate(markers):
        end = markers[index + 1].t if index + 1 < len(markers) else float('inf')
        window = samples_between(leg, marker.t + args.settle, end)
        etabli, motif = fenetre_etablie(leg, marker.t, end, args.settle)
        est = median([s['kbps'] for s in etabli]) if etabli else float('nan')
        # Le label commande : un marqueur "jitter" porte lui aussi rate_kbps et
        # aucun pct, donc un test de reference place avant lui l'absorbait et son
        # critere n'etait jamais prononce (silencieusement).
        if marker.label not in ('loss', 'jitter'):
            if not math.isnan(est):
                reference = est
            continue
        if marker.label == 'loss':
            pct = marker.params.get('pct', '?')
            if math.isnan(est) or reference is None:
                verdicts.append(Verdict('--', 'pertes %s %% : estimation' % pct,
                                        'pas de reference exploitable (%s)' % motif))
                continue
            part = est / reference if reference else float('nan')
            status = 'KO' if part < 0.25 else 'OK'
            verdicts.append(Verdict(status, 'pertes %s %% : pas d effondrement' % pct,
                                    'estimation %.0f kb/s = %.0f %% de la reference %.0f kb/s [%s]'
                                    % (est, 100 * part, reference, motif)))
        elif marker.label == 'jitter':
            gigue = '%s+/-%s ms' % (marker.params.get('delay_ms', '?'), marker.params.get('jitter_ms', '?'))
            if not window:
                verdicts.append(Verdict('--', 'gigue %s : faux positifs' % gigue, 'pas d echantillon'))
                continue
            over = [s for s in window if s['usage'] != 'Normal']
            part = float(len(over)) / len(window)
            status = 'OK' if part <= 0.10 else 'KO'
            verdicts.append(Verdict(status, 'gigue %s : faux positifs' % gigue,
                                    '%.0f %% des echantillons hors Normal (max 10 %%), estimation mediane %.0f kb/s'
                                    % (100 * part, median([s['kbps'] for s in window]))))
    return verdicts


def judge_stability(leg, args, nan_lines):
    verdicts = []
    if not leg.estim:
        return [Verdict('--', 'stabilite', 'aucune trace d estimation')]
    duree = leg.estim[-1]['t'] - leg.estim[0]['t']
    verdicts.append(Verdict('OK' if duree >= LONG_RUN_S else '--',
                            'duree de capture >= 10 min',
                            '%.1f min' % (duree / 60.0)))
    verdicts.append(Verdict('OK' if nan_lines == 0 else 'KO', 'aucun NaN dans les traces',
                            '%d ligne(s) portant un NaN' % nan_lines))

    # Hypothese gelee : plus longue plage continue hors Normal.
    pire = 0.0
    debut = None
    courant = None
    for sample in leg.estim:
        if sample['usage'] != courant:
            if courant is not None and courant != 'Normal' and debut is not None:
                pire = max(pire, sample['t'] - debut)
            courant = sample['usage']
            debut = sample['t']
    if courant is not None and courant != 'Normal' and debut is not None:
        pire = max(pire, leg.estim[-1]['t'] - debut)
    verdicts.append(Verdict('OK' if pire <= STUCK_MAX_S else 'KO', 'aucune hypothese gelee',
                            'plus longue plage hors Normal : %.1f s (max %.0f s)' % (pire, STUCK_MAX_S)))

    # Ecretage : temps cumule au plafond et au plancher.
    clip_haut = clip_bas = 0.0
    for index in range(1, len(leg.estim)):
        delta = leg.estim[index]['t'] - leg.estim[index - 1]['t']
        if delta <= 0 or delta > 5.0:
            continue
        if leg.estim[index]['kbps'] >= args.max_kbps:
            clip_haut += delta
        if leg.estim[index]['kbps'] <= args.min_kbps:
            clip_bas += delta
    verdicts.append(Verdict('OK' if clip_haut <= CLIP_MAX_S else 'KO', 'pas d ecretage permanent au plafond',
                            '%.1f s a %g kb/s (max %.0f s)' % (clip_haut, args.max_kbps, CLIP_MAX_S)))
    if clip_bas > CLIP_MAX_S:
        verdicts.append(Verdict('KO', 'pas d ecretage permanent au plancher',
                                '%.1f s a %g kb/s' % (clip_bas, args.min_kbps)))
    covariance = [e for e in leg.events if e['kind'] == 'covariance']
    verdicts.append(Verdict('OK' if not covariance else 'KO', 'covariance semi-definie positive',
                            '%d avertissement(s)' % len(covariance)))
    return verdicts


# ---------------------------------------------------------------------------
# Graphe SVG
# ---------------------------------------------------------------------------

PANEL_H = 300
PANEL_W = 1100
MARGIN_L = 70
MARGIN_R = 210
MARGIN_T = 96  # de quoi loger les etiquettes de marqueur, ecrites a la verticale
MARGIN_B = 54
BAND_H = 14

USAGE_COLOR = {'Normal': '#2f8f4e', 'OverUsing': '#c0392b', 'UnderUsing': '#c8892a'}


def escape(text):
    return (str(text).replace('&', '&amp;').replace('<', '&lt;')
            .replace('>', '&gt;').replace('"', '&quot;'))


def nice_ticks(maximum, count=5):
    if maximum <= 0 or math.isnan(maximum):
        return [0]
    brut = maximum / float(count)
    exposant = math.floor(math.log10(brut)) if brut > 0 else 0
    base = 10 ** exposant
    for facteur in (1, 2, 2.5, 5, 10):
        pas = facteur * base
        if brut <= pas:
            break
    ticks = []
    valeur = 0.0
    while valeur <= maximum + pas * 0.5:
        ticks.append(valeur)
        valeur += pas
    return ticks


def render_svg(legs, markers, t0, path):
    width = MARGIN_L + PANEL_W + MARGIN_R
    height = len(legs) * (PANEL_H + MARGIN_T + MARGIN_B)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 %d %d" font-family="DejaVu Sans, sans-serif" font-size="12">'
           % (width, height, width, height),
           '<rect width="%d" height="%d" fill="#ffffff"/>' % (width, height)]

    duree = 1.0
    for leg in legs:
        for sample in leg.estim:
            duree = max(duree, sample['t'] - t0)
    for marker in markers:
        duree = max(duree, marker.t - t0)

    for index, leg in enumerate(legs):
        top = index * (PANEL_H + MARGIN_T + MARGIN_B) + MARGIN_T
        plafond = 1.0
        for sample in leg.estim:
            for key in ('kbps', 'incoming'):
                if not math.isnan(sample[key]):
                    plafond = max(plafond, sample[key])
        for event in leg.events:
            if event['kind'] in FEEDBACK_KINDS and event['value'] and not math.isnan(event['value']):
                plafond = max(plafond, event['value'])
        plafond *= 1.12

        def x_of(t):
            return MARGIN_L + PANEL_W * (t - t0) / duree

        def y_of(v):
            v = 0.0 if math.isnan(v) else min(max(v, 0.0), plafond)
            return top + PANEL_H - PANEL_H * v / plafond

        out.append('<text x="%d" y="%d" font-size="14" font-weight="bold">%s</text>'
                   % (MARGIN_L, top - MARGIN_T + 18, escape('patte : ' + leg.label)))
        out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#fbfbfb" stroke="#d0d0d0"/>'
                   % (MARGIN_L, top, PANEL_W, PANEL_H))

        for tick in nice_ticks(plafond):
            y = y_of(tick)
            out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e6e6e6"/>'
                       % (MARGIN_L, y, MARGIN_L + PANEL_W, y))
            out.append('<text x="%d" y="%.1f" text-anchor="end" fill="#555">%s</text>'
                       % (MARGIN_L - 6, y + 4, escape('%g' % tick)))
        out.append('<text x="%d" y="%d" text-anchor="end" fill="#555">kb/s</text>' % (MARGIN_L - 6, top - 6))

        pas = max(nice_ticks(duree, 8)[1] if len(nice_ticks(duree, 8)) > 1 else duree, 1)
        seconde = 0.0
        while seconde <= duree:
            x = x_of(t0 + seconde)
            out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#e6e6e6"/>'
                       % (x, top, x, top + PANEL_H))
            out.append('<text x="%.1f" y="%d" text-anchor="middle" fill="#555">%s</text>'
                       % (x, top + PANEL_H + 16, escape('%gs' % seconde)))
            seconde += pas

        for marker in markers:
            x = x_of(marker.t)
            if x < MARGIN_L or x > MARGIN_L + PANEL_W:
                continue
            out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#8e44ad" '
                       'stroke-dasharray="4,3"/>' % (x, top - 6, x, top + PANEL_H))
            # Etiquette a la verticale AU-DESSUS du panneau : dedans elle
            # traverserait les courbes, dessous elle mangerait le panneau suivant.
            out.append('<text x="%.1f" y="%d" fill="#8e44ad" text-anchor="end" '
                       'transform="rotate(-90 %.1f %d)">%s</text>'
                       % (x + 4, top - 10, x + 4, top - 10, escape(marker.short())))

        # Bandeau d'hypothese sous le panneau.
        for pos in range(len(leg.estim)):
            sample = leg.estim[pos]
            fin = leg.estim[pos + 1]['t'] if pos + 1 < len(leg.estim) else sample['t']
            largeur = max(x_of(fin) - x_of(sample['t']), 1.0)
            out.append('<rect x="%.1f" y="%d" width="%.1f" height="%d" fill="%s" fill-opacity="0.75"/>'
                       % (x_of(sample['t']), top + PANEL_H + 22, largeur, BAND_H,
                          USAGE_COLOR.get(sample['usage'], '#999999')))

        def polyline(points, color, width_px, dash=''):
            if len(points) < 2:
                return
            coords = ' '.join('%.1f,%.1f' % pt for pt in points)
            out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%s" %s/>'
                       % (coords, color, width_px, ('stroke-dasharray="%s"' % dash) if dash else ''))

        polyline([(x_of(s['t']), y_of(s['incoming'])) for s in leg.estim], '#7f8c8d', '1.2')
        polyline([(x_of(s['t']), y_of(s['kbps'])) for s in leg.estim], '#1f6fb4', '2')

        # Feedback emis : une marche par valeur annoncee.
        fb = [(e['t'], e['value']) for e in leg.events
              if e['kind'] in FEEDBACK_KINDS and e['value'] is not None and not math.isnan(e['value'])]
        marche = []
        for pos, (t, value) in enumerate(fb):
            marche.append((x_of(t), y_of(value)))
            fin = fb[pos + 1][0] if pos + 1 < len(fb) else (leg.estim[-1]['t'] if leg.estim else t)
            marche.append((x_of(fin), y_of(value)))
        polyline(marche, '#e07b16', '1.4', dash='6,3')

        legend = [('#1f6fb4', 'estimation'),
                  ('#7f8c8d', 'debit entrant'),
                  ('#e07b16', 'feedback emis')]
        for pos, (color, texte) in enumerate(legend):
            y = top + 14 + pos * 18
            out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="2"/>'
                       % (MARGIN_L + PANEL_W + 12, y, MARGIN_L + PANEL_W + 40, y, color))
            out.append('<text x="%d" y="%d" fill="#333">%s</text>'
                       % (MARGIN_L + PANEL_W + 46, y + 4, escape(texte)))
        for pos, (usage, color) in enumerate(sorted(USAGE_COLOR.items())):
            y = top + 14 + (len(legend) + pos) * 18
            out.append('<rect x="%d" y="%d" width="28" height="10" fill="%s"/>'
                       % (MARGIN_L + PANEL_W + 12, y - 6, color))
            out.append('<text x="%d" y="%d" fill="#333">%s</text>'
                       % (MARGIN_L + PANEL_W + 46, y + 4, escape(usage)))

    out.append('</svg>')
    with open(path, 'w') as handle:
        handle.write('\n'.join(out))


# ---------------------------------------------------------------------------
# Sorties
# ---------------------------------------------------------------------------

def write_csv(legs, t0, out_dir):
    chemin = os.path.join(out_dir, 'bwe.csv')
    with open(chemin, 'w', newline='') as handle:
        writer = csv.writer(handle)
        writer.writerow(['t_rel', 'epoch', 'tid', 'stream', 'state', 'region', 'usage',
                         'estimation_kbps', 'raw_kbps', 'incoming_kbps', 'min_kbps', 'max_kbps'])
        for leg in legs:
            for sample in leg.estim:
                writer.writerow(['%.3f' % (sample['t'] - t0), '%.3f' % sample['t'], '0x' + leg.tid, leg.label,
                                 sample['state'], sample['region'], sample['usage'],
                                 '%.0f' % sample['kbps'], '%.0f' % sample['raw'], '%.0f' % sample['incoming'],
                                 '%.0f' % sample['min'], '%.0f' % sample['max']])
    chemin_ev = os.path.join(out_dir, 'events.csv')
    with open(chemin_ev, 'w', newline='') as handle:
        writer = csv.writer(handle)
        writer.writerow(['t_rel', 'epoch', 'tid', 'stream', 'kind', 'value_kbps', 'detail'])
        for leg in legs:
            for event in leg.events:
                writer.writerow(['%.3f' % (event['t'] - t0), '%.3f' % event['t'], '0x' + leg.tid, leg.label,
                                 event['kind'],
                                 '' if event['value'] is None else '%.0f' % event['value'],
                                 event['detail']])
    return chemin, chemin_ev


def print_markdown(legs, verdicts_par_patte, markers, t0):
    print('')
    print('### Depouillement (genere par `mcu/tests/tools/bwe_report.py`)')
    print('')
    if markers:
        print('| t (s) | marqueur |')
        print('|---|---|')
        for marker in markers:
            print('| %.1f | %s |' % (marker.t - t0, str(marker)))
        print('')
    for leg in legs:
        print('**Patte `%s`**' % leg.label)
        print('')
        print('| critere | verdict | mesure |')
        print('|---|---|---|')
        for verdict in verdicts_par_patte[leg.label]:
            marque = {'OK': 'OK', 'KO': '**KO**', '--': 'n/a'}[verdict.status]
            print('| %s | %s | %s |' % (verdict.title, marque, verdict.detail))
        print('')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('log', help='journal du mediaserver (mcu.log), traces -d actives')
    parser.add_argument('--markers', help='journal de marqueurs ecrit par netem_scenario.sh')
    parser.add_argument('--out', default='.', help='repertoire de sortie (defaut : .)')
    parser.add_argument('--stream', help='ne garder que les pattes dont le nom contient cette chaine')
    parser.add_argument('--settle', type=float, default=SETTLE_S,
                        help='garde avant de juger un regime etabli, en s (defaut : %g)' % SETTLE_S)
    parser.add_argument('--max-kbps', type=float, default=DEFAULT_MAX_KBPS,
                        help='plafond de l estimateur, kb/s (defaut : %d)' % DEFAULT_MAX_KBPS)
    parser.add_argument('--min-kbps', type=float, default=DEFAULT_MIN_KBPS,
                        help='plancher de l estimateur, kb/s (defaut : %d)' % DEFAULT_MIN_KBPS)
    parser.add_argument('--no-svg', action='store_true', help='ne pas produire le graphe')
    parser.add_argument('--markdown', action='store_true', help='emettre le bloc a coller en annexe D')
    args = parser.parse_args(argv)

    legs, nan_lines, parsed = parse_log(args.log, args.stream)
    markers = parse_markers(args.markers)
    if not legs:
        print('Aucune trace BWE trouvee dans %s.' % args.log, file=sys.stderr)
        print('Le mediaserver tourne-t-il avec -d (OPTIONS de /etc/sysconfig/mediaserver) ?', file=sys.stderr)
        return 2

    tous = [s['t'] for leg in legs for s in leg.estim] + [e['t'] for leg in legs for e in leg.events]
    t0 = min(tous + [m.t for m in markers]) if markers else min(tous)

    os.makedirs(args.out, exist_ok=True)
    chemin_csv, chemin_ev = write_csv(legs, t0, args.out)
    chemin_svg = os.path.join(args.out, 'bwe.svg')
    if not args.no_svg:
        render_svg(legs, markers, t0, chemin_svg)

    print('%d ligne(s) retenue(s), %d patte(s), %s'
          % (parsed, len(legs), 'aucun marqueur' if not markers else '%d marqueur(s)' % len(markers)))
    if len(legs) > 1 and markers and not args.stream:
        # Les marqueurs decrivent le lien degrade ; les juger contre une patte
        # qui n'est pas sur ce lien produit des KO qui ne veulent rien dire.
        print('  NB : %d pattes et un seul lien degrade — utiliser --stream pour ne juger'
              % len(legs))
        print('       que celle qui est en coupure (%s)' % ', '.join(leg.label for leg in legs))
    print('  %s' % chemin_csv)
    print('  %s' % chemin_ev)
    if not args.no_svg:
        print('  %s' % chemin_svg)

    verdicts_par_patte = {}
    for leg in legs:
        verdicts = []
        verdicts += judge_segments(leg, markers, args)
        verdicts += judge_impairments(leg, markers, args)
        verdicts += judge_stability(leg, args, nan_lines)
        verdicts_par_patte[leg.label] = verdicts
        print('')
        print('== patte %s (%d echantillons) ==' % (leg.label, len(leg.estim)))
        for verdict in verdicts:
            print('  ' + verdict.line())

    if args.markdown:
        print_markdown(legs, verdicts_par_patte, markers, t0)

    echecs = sum(1 for verdicts in verdicts_par_patte.values() for v in verdicts if v.status == 'KO')
    print('')
    print('%d critere(s) en echec.' % echecs)
    return 1 if echecs else 0


if __name__ == '__main__':
    sys.exit(main())
