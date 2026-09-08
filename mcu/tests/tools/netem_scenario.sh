#!/bin/bash
# Injecte les trois scenarios de degradation (docs/RATE-CONTROL.md) et
# journalise chaque changement avec son horodatage, pour que bwe_report.py puisse
# recouper les traces BWE du mediaserver avec ce qui a reellement ete fait au
# reseau.
#
#   ./netem_scenario.sh -i eth0 -s escalier -m escalier.tsv
#   ./netem_scenario.sh -i eth0 -s pertes   -m pertes.tsv   --ingress
#   ./netem_scenario.sh -i eth0 -s gigue    -m gigue.tsv
#
# SENS DU TRAFIC. netem ne faconne que l'EMISSION d'une interface. L'estimateur
# qu'on mesure ici est celui de la RECEPTION : il faut donc degrader ce qui
# ARRIVE au mediaserver.
#   - machine en coupure (ou poste client) : lancer le script dessus, sur
#     l'interface qui emet vers le mediaserver. C'est le montage de reference.
#   - sur le mediaserver lui-meme : ajouter --ingress, qui detourne le trafic
#     entrant vers une interface ifb et lui applique netem. Pratique, mais le
#     faconnage s'applique alors a TOUT ce qui entre par l'interface.
#
# PROFONDEUR DE FILE. netem garde par defaut 1000 paquets, soit 19 s de backlog
# a 500 kb/s : le lien injecte se comporte alors en entrepot, pas en lien. Mesure
# du 2026-08-18 : au relachement de la marche basse, l'entrant est reste 8 s a
# l'ancien plafond puis a burste 4 s a 1789 kb/s le temps que la file se vide,
# ce qui faussait le chrono de re-montee et la dispersion. Un lien d'acces reel
# tient 100 a 300 ms ; -l fixe cette profondeur (40 paquets = ~200 ms a 2 Mb/s).
#
# Toute sortie (fin normale, Ctrl-C, kill) restaure les qdisc d'origine.

set -u

IFACE=""
SCENARIO=""
MARKERS=""
RATE_KBPS=2000
PHASE=60
LIMIT_PKTS=40
INGRESS=0
IFB="ifb-mcu"
IFB_CREATED=0
TARGET=""

usage() {
	cat <<'EOF'
Usage: netem_scenario.sh -i IFACE -s SCENARIO [options]

  -i, --iface IFACE     interface reseau a degrader (obligatoire)
  -s, --scenario NOM    escalier | pertes | gigue (obligatoire)
  -m, --markers FICHIER journal des marqueurs (defaut : marqueurs-SCENARIO.tsv)
  -r, --rate KBPS       debit du lien sain, en kb/s (defaut : 2000)
  -d, --phase SECONDES  duree de chaque phase (defaut : 60)
  -l, --limit PAQUETS   profondeur de la file netem (defaut : 40, ~200 ms a
                        2 Mb/s ; le defaut netem de 1000 vaut 19 s a 500 kb/s)
      --ingress         faconner le trafic ENTRANT via une interface ifb
  -h, --help            cette aide

Scenarios (duree totale = 3 a 4 phases) :
  escalier  lien sain -> lien divise par 4 -> retour au lien sain
  pertes    lien sain -> 2 % de perte -> 10 % de perte -> lien sain
  gigue     lien sain -> delai 50 ms +/- 30 ms -> lien sain
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		-i|--iface)    IFACE="$2"; shift 2;;
		-s|--scenario) SCENARIO="$2"; shift 2;;
		-m|--markers)  MARKERS="$2"; shift 2;;
		-r|--rate)     RATE_KBPS="$2"; shift 2;;
		-d|--phase)    PHASE="$2"; shift 2;;
		-l|--limit)    LIMIT_PKTS="$2"; shift 2;;
		--ingress)     INGRESS=1; shift;;
		-h|--help)     usage; exit 0;;
		*) echo "option inconnue : $1" >&2; usage; exit 2;;
	esac
done

[ -n "$IFACE" ] && [ -n "$SCENARIO" ] || { usage; exit 2; }
[ -n "$MARKERS" ] || MARKERS="marqueurs-$SCENARIO.tsv"

command -v tc >/dev/null 2>&1 || { echo "tc introuvable (dnf install iproute-tc)" >&2; exit 3; }
[ "$(id -u)" = "0" ] || { echo "a lancer en root" >&2; exit 3; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "interface $IFACE inconnue" >&2; exit 3; }

marque() {
	# epoch(ms) <TAB> label <TAB> cle=valeur ...
	printf '%s\t%s\t%s\n' "$(date +%s.%3N)" "$1" "${2:-}" >> "$MARKERS"
	echo "  [$(date +%H:%M:%S)] $1 ${2:-}"
}

NETTOYE=0
nettoyer() {
	[ "$NETTOYE" = "1" ] && return 0
	NETTOYE=1
	[ -n "$TARGET" ] && tc qdisc del dev "$TARGET" root 2>/dev/null
	if [ "$INGRESS" = "1" ]; then
		tc qdisc del dev "$IFACE" ingress 2>/dev/null
		[ "$IFB_CREATED" = "1" ] && ip link del "$IFB" 2>/dev/null
	fi
	marque restore ""
	echo "qdisc restaurees sur $IFACE."
}

preparer() {
	if [ "$INGRESS" = "1" ]; then
		modprobe ifb numifbs=0 2>/dev/null
		if ! ip link show "$IFB" >/dev/null 2>&1; then
			ip link add "$IFB" type ifb || exit 3
			IFB_CREATED=1
		fi
		ip link set "$IFB" up || exit 3
		tc qdisc del dev "$IFACE" ingress 2>/dev/null
		tc qdisc add dev "$IFACE" handle ffff: ingress || exit 3
		tc filter add dev "$IFACE" parent ffff: protocol all u32 \
			match u32 0 0 action mirred egress redirect dev "$IFB" || exit 3
		TARGET="$IFB"
	else
		TARGET="$IFACE"
	fi
	tc qdisc del dev "$TARGET" root 2>/dev/null
}

appliquer() {
	# $1 : arguments netem, $2 : label du marqueur, $3 : parametres du marqueur
	tc qdisc replace dev "$TARGET" root netem limit "$LIMIT_PKTS" $1 || exit 4
	marque "$2" "$3"
}

trap 'nettoyer; exit 130' INT TERM
trap 'nettoyer' EXIT

: > "$MARKERS"
preparer
marque start "scenario=$SCENARIO iface=$IFACE target=$TARGET phase_s=$PHASE limit_pkts=$LIMIT_PKTS"

case "$SCENARIO" in
	escalier)
		BAS=$(( RATE_KBPS / 4 ))
		appliquer "rate ${RATE_KBPS}kbit"  cap "rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		appliquer "rate ${BAS}kbit"        cap "rate_kbps=$BAS"
		sleep "$PHASE"
		appliquer "rate ${RATE_KBPS}kbit"  cap "rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		;;
	pertes)
		appliquer "rate ${RATE_KBPS}kbit"            clean "rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		appliquer "rate ${RATE_KBPS}kbit loss 2%"    loss  "pct=2 rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		appliquer "rate ${RATE_KBPS}kbit loss 10%"   loss  "pct=10 rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		appliquer "rate ${RATE_KBPS}kbit"            clean "rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		;;
	gigue)
		appliquer "rate ${RATE_KBPS}kbit" clean "rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		appliquer "rate ${RATE_KBPS}kbit delay 50ms 30ms distribution normal" \
			jitter "delay_ms=50 jitter_ms=30 rate_kbps=$RATE_KBPS"
		sleep $(( PHASE * 2 ))
		appliquer "rate ${RATE_KBPS}kbit" clean "rate_kbps=$RATE_KBPS"
		sleep "$PHASE"
		;;
	*)
		echo "scenario inconnu : $SCENARIO" >&2
		exit 2
		;;
esac

marque stop ""
echo "Termine. Marqueurs : $MARKERS"
