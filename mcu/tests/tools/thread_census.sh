#!/bin/bash
# thread_census.sh — recensement des threads et du CPU du mediaserver.
#
# Mesure de référence du chantier « transcodeurs sans thread »
# (jsr309_transcode_sans_thread.md, lot 0 point 5) : à lancer AVANT les lots 1
# à 4, puis après, sur le même scénario d'appel.
#
# Usage :
#   mcu/tests/tools/thread_census.sh [pid] [duree_s]
#
# Sans pid, le script cherche le processus `mediaserver` (ou `mcu`).
# Sans durée, il prend une seule photo ; avec, il échantillonne chaque seconde
# et rend le minimum, le maximum et la moyenne.
#
# Il ne crée aucun appel : c'est à l'opérateur d'en établir un pendant la
# mesure, et de noter lequel.

set -u

pid=${1:-}
duration=${2:-0}

if [ -z "$pid" ]; then
	pid=$(pgrep -x mediaserver || pgrep -x mcu) || {
		echo "Aucun processus mediaserver/mcu trouve. Donnez un pid." >&2
		exit 1
	}
	pid=$(echo "$pid" | head -1)
fi

if [ ! -d "/proc/$pid" ]; then
	echo "Pas de processus $pid." >&2
	exit 1
fi

# Noms des threads, regroupes et comptes : c'est la photo qui dit OU sont les
# threads, pas seulement combien.
census() {
	echo "--- threads par nom ---"
	for t in /proc/"$pid"/task/*/comm; do
		[ -r "$t" ] && cat "$t"
	done | sort | uniq -c | sort -rn
	echo "--- total : $(ls /proc/"$pid"/task 2>/dev/null | wc -l) threads"
}

# CPU cumule du processus, en jiffies (utime + stime du champ /proc/pid/stat).
cpu_jiffies() {
	awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null || echo 0
}

thread_count() {
	ls /proc/"$pid"/task 2>/dev/null | wc -l
}

echo "mediaserver pid=$pid  $(date -Is)"
census

if [ "$duration" -le 0 ]; then
	exit 0
fi

hz=$(getconf CLK_TCK)
start_cpu=$(cpu_jiffies)
min=$(thread_count)
max=$min
sum=0
n=0

for _ in $(seq "$duration"); do
	sleep 1
	c=$(thread_count)
	[ "$c" -lt "$min" ] && min=$c
	[ "$c" -gt "$max" ] && max=$c
	sum=$((sum + c))
	n=$((n + 1))
done

end_cpu=$(cpu_jiffies)

echo
echo "--- sur ${duration}s ---"
echo "threads   : min=$min max=$max moyenne=$((sum / n))"
echo "cpu       : $(awk -v a="$start_cpu" -v b="$end_cpu" -v hz="$hz" -v d="$duration" \
	'BEGIN { printf "%.1f %% d un coeur", 100 * (b - a) / hz / d }')"
echo
census
